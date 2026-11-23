# 用 GitHub Actions 构建「证书提示」版客户端

本机编译被 xmake 的包管理环境卡住（第三方包安装时会重建 PATH，dav1d 的 meson 始终找不到
nasm），因此改用项目自带的 CI 工作流出包。CI 环境是官方验证过的，成功率接近 100%。

改动只涉及 4 个已有文件 + 2 个新文件，全部在 `tls-cert-prompt.patch` 里。

## 改完后的行为

1. 客户端连自建服务器，TLS 校验失败时（自签名根不在信任库），弹出模态对话框：
   「服务器证书不受信任」+ 说明 + 服务器地址。
2. 点「自动安装证书」：
   - 客户端自己对该端点做一次不校验的 TLS 握手，取回服务端下发的证书链；
   - 找出其中的自签名根证书（不是自签名则提示检查系统时间/联系管理员）；
   - 导出 PEM 到 `%LOCALAPPDATA%\CrossDesk\certs\crossdesk_server_root.crt`；
   - 用 `CertAddEncodedCertificateToStore` 写入**当前用户**的「受信任的根证书颁发机构」
     （HKCU，**不需要管理员，不弹 UAC**）；
   - 保存成功后自动重建信令客户端，立刻重连，连上后对话框自动关闭。
3. 点「稍后」：本次运行不再提示（换服务器会重新提示）。
4. 安装失败时对话框显示原因，并给出导出的证书路径，可双击手动安装。

## 方案 A：你提供 GitHub token，我全自动完成

给我一个 **classic PAT**（scope 勾 `repo` + `workflow`），我会用它：

```bash
gh auth login --with-token
gh repo fork kunkundi/crossdesk --clone=false
# 本地打补丁 → push 到 fork 的 feature 分支 → 等 Actions → 下载 artifact
```

产物会放到 `K:\crossdesk\deploy\selfhost\dist\`。

## 方案 B：自己动手（约 10 分钟 + 40~60 分钟等构建）

前提：有自己的 GitHub 账号，装了 Git。

### 1. fork 并克隆

打开 https://github.com/kunkundi/crossdesk → 右上角 Fork。然后：

```bash
git clone --recurse-submodules https://github.com/<你的用户名>/crossdesk.git
cd crossdesk
```

### 2. 打补丁

把本目录的 `tls-cert-prompt.patch` 放到仓库根目录，然后：

```bash
git apply --stat tls-cert-prompt.patch   # 先看改了什么
git apply -3 tls-cert-prompt.patch
```

如果上游代码有更新导致冲突，`git apply -3` 会做三方合并；仍冲突就告诉我，我按新的基线重做补丁。

### 3. push 触发构建

```bash
git checkout -b feature/tls-cert-prompt
git add -A
git commit -m "prompt and auto-install the server root certificate on TLS failure"
git push -u origin feature/tls-cert-prompt
```

工作流 `.github/workflows/build.yml` 的触发条件是 `push: branches: "**"`，推送后会自动跑
Linux / macOS / iOS / Android / Windows 全部任务。**只需要等 Windows 那个**：
`Build on Windows x64`，约 40~60 分钟（首次没有依赖缓存，slint/Rust 编译最慢）。

### 4. 下载产物

Actions → 左侧选这次运行 → 页面底部 **Artifacts**：

| 产物 | 用途 |
|---|---|
| `crossdesk-win-x64-1.7.6` | NSIS 安装包（未签名，SmartScreen 会拦一次，选「仍要运行」） |
| `crossdesk-win-x64-portable-1.7.6` | 便携版 zip，解压即用，适合测试 |

建议先用便携版验证：解压 → 运行 `CrossDesk.exe` → 设置里填自托管地址 → 应该弹证书对话框。

## 验证清单

1. 先**删掉**已导入的根证书，模拟新机器：
   ```powershell
   certutil -user -delstore Root "8.138.12.16"
   ```
2. 启动便携版，`☰ → 设置 → 自托管配置`：地址 `8.138.12.16`、信令端口 `9099` **已预置且默认开启**，
   中继端口 `3478` 在服务端。
   注意：该行的「编辑」「开关」都要密码 `11223344`（关掉也要密码，取消则回到原状态）。
3. 应弹出「服务器证书不受信任」。
4. 点「自动安装证书」→ 对话框变为「证书已安装，正在重新连接服务器…」→ 状态栏变「已连接服务器」。
5. 复核证书确实进了库：
   ```powershell
   certutil -user -store Root | Select-String "8.138.12.16"
   ```
6. **注意**：这只解决当前用户。如果要用「登录前远控」（`crossdesk_service.exe` 以 LocalSystem 运行），
   仍需管理员把同一张证书装进机器级库：
   ```powershell
   certutil -addstore Root "%LOCALAPPDATA%\CrossDesk\certs\crossdesk_server_root.crt"
   ```

## 改动清单

| 文件 | 内容 |
|---|---|
| `apps/desktop/src/gui/features/settings/tls_certificate_bootstrap.{h,cpp}` | 新增。取证书链、找自签名根、导出 PEM、写 HKCU ROOT。Windows 实现，其他平台返回 unsupported。 |
| `apps/desktop/src/gui/ui/main_window.slint` | 新增 3 个属性 + 2 个 callback + 模态对话框（仿 offline-warning 的实现）。 |
| `apps/desktop/src/gui/application/gui_application.{h,cpp}` | TLS 错误边沿触发弹窗、按端点记忆状态、工作线程执行安装、成功后重建信令客户端。 |
| `apps/desktop/src/gui/ui/ui_localization.h` | 接入新文案。 |
| `localization/catalog.json` | 新增 7 条文案（10 种语言），`localization_data.h` 已用 generate.py 重新生成。 |
| `apps/desktop/xmake/targets.lua` | gui target 的 Windows syslinks 补 `Crypt32`。 |
| `apps/desktop/src/config_center/config_center.{h,cpp}` | 预置自托管端点 `8.138.12.16:9099` 并默认开启；`self_hosted_preset_version` 做一次性灌入，之后用户关掉不会被下次启动改回。 |
| `apps/desktop/src/gui/ui/main_window.slint` | 自托管开关改成"草稿值 + 密码提交"，新增密码模态框（`InputType.password`）。 |
| `apps/desktop/src/gui/application/gui_application.cpp` | 固定密码校验回调；保存失败重开编辑器也走密码；启动时把落盘的自托管状态灌进 UI。 |

## 已验证 / 未验证

- 已验证：`tls_certificate_bootstrap.cpp` 用 `clang-cl /W4 /std:c++20` 单独编译**零警告通过**
  （项目开 `/WX`，警告即错误）。
- 已验证：整份 UI 用官方 `slint-compiler` v1.17.1（与 `add_requires("slint 1.17.1")` 一致）
  编译 `crossdesk_ui.slint` **零报错**，生成的 C++ 里 `on_verify_self_host_password`、
  `set_self_host_password_*`、`set_self_hosted_draft` 等接口名均已核对存在。
- 未验证：整包链接、运行时行为——本机依赖装不出来，必须靠 CI 出包后实测。
