# CrossDesk 自托管部署速查

CrossDesk 客户端**不含**中继/信令服务，自托管需要单独部署
[`kunkundi/crossdesk-server`](https://github.com/kunkundi/crossdesk-server)（WSS 信令 + Coturn + Web 管理后台）。

## 组件与端口

| 组件 | 容器 | 端口 | 协议 | 用途 |
| --- | --- | --- | --- | --- |
| 信令服务 | `crossdesk_server` | `9099` | TCP | WSS 信令、`/stats`、`/admin` |
| Coturn | `crossdesk_coturn` | `3478` | TCP/UDP | STUN / TURN |
| Coturn 媒体 | 同上 | `50000-60000` | UDP | P2P 失败时中继媒体 |

> Compose 使用 `network_mode: host`，**不做端口映射**。端口必须在宿主机上唯一占用，
> 云厂商安全组和本机防火墙都要放通（尤其 `50000-60000/UDP`，漏了会出现"能连上但黑屏"）。

## 前置条件

- Linux 服务器（amd64 / arm64），Docker Engine + **Compose 插件**（`docker compose version` 可用）
- 公网 IP；若在 NAT 后：`EXTERNAL_IP` 填公网、`INTERNAL_IP` 填网卡实际 IP
- 生产环境建议固定镜像 tag，和客户端版本配套升级

## 一键部署

```bash
scp deploy.sh root@your-server:/root/
ssh root@your-server
chmod +x deploy.sh
./deploy.sh --tag v1.1.38                       # 固定版本（推荐）
# ./deploy.sh                                   # 自动取最新 Release
# ./deploy.sh --domain desk.example.com         # 用域名 + 自签 SAN 证书
# ./deploy.sh --dir /srv/crossdesk --port 443   # 换目录/端口
```

脚本会：下载与版本匹配的 `compose.yaml` / `env.example` → 探测 IP → 生成两个独立密钥 →
写 `.env`（已存在则保留）→ 放通 ufw/firewalld → `compose up -d` → 打印根证书路径。

## 手工部署（官方步骤）

```bash
mkdir -p /opt/crossdesk && cd /opt/crossdesk
TAG=v1.1.38
curl -fLO https://github.com/kunkundi/crossdesk-server/releases/download/$TAG/compose.yaml
curl -fLO https://github.com/kunkundi/crossdesk-server/releases/download/$TAG/env.example
cp env.example .env && chmod 600 .env

openssl rand -hex 32   # → COTURN_AUTH_SECRET
openssl rand -hex 32   # → COTURN_STATELESS_NONCE_SECRET
vi .env                # EXTERNAL_IP / INTERNAL_IP / 两个密钥 / CROSSDESK_IMAGE 固定 tag

docker compose config -q
docker compose pull
docker compose up -d --no-build
docker compose ps

curl --fail --cacert /var/lib/crossdesk/certs/api.crossdesk.cn_root.crt \
  https://<公网IP>:9099/stats
```

必填项：

| 变量 | 说明 |
| --- | --- |
| `EXTERNAL_IP` | 公网 IP，同时用于自动签发证书的 SAN 和 TURN 对外地址 |
| `INTERNAL_IP` | 宿主机网卡 IP，Coturn 监听/中继绑定它，不能填错 |
| `CROSSDESK_IMAGE` | 固定发布 tag，别用 `latest` |
| `COTURN_AUTH_SECRET` | 信令与 Coturn 共享密钥，**不下发给客户端** |
| `COTURN_STATELESS_NONCE_SECRET` | 独立 nonce 密钥，重启不变可避免 438 重认证 |
| `CROSSDESK_DATA_DIR` / `CROSSDESK_LOG_DIR` | 证书、SQLite、日志的持久化目录 |

## 证书三种方案

| 方案 | 做法 | 客户端 |
| --- | --- | --- |
| 自签 + IP（默认） | 首次启动 `start.sh` 自动签发，只含 `EXTERNAL_IP` 的 SAN | 客户端填 IP；每端导入 `api.crossdesk.cn_root.crt` |
| 自签 + 域名 | `./deploy.sh --domain desk.example.com` 预生成含 `DNS:x` + `IP:y` 的证书 | 客户端可填域名；仍需导入根证书 |
| CA 证书（最省事） | 用受信任证书链覆盖 `${DATA_DIR}/certs/` 下固定文件名 | 客户端填对应域名，**无需导入任何根证书** |

CA 方案要放的文件（文件名固定，与真实域名无关）：

```
${CROSSDESK_DATA_DIR}/certs/api.crossdesk.cn_bundle.crt   # 服务器证书 + 中间链
${CROSSDESK_DATA_DIR}/certs/api.crossdesk.cn.key          # 私钥，0600
${CROSSDESK_DATA_DIR}/certs/api.crossdesk.cn_root.crt     # 根证书（自签时才有）
```

放入后 `docker compose up -d --no-build` 重启（`restart` 不会重读环境变量）。

导入根证书：

```powershell
# Windows 管理员 PowerShell
certutil -addstore "Root" "C:\path\to\api.crossdesk.cn_root.crt"
```

```bash
# Ubuntu / Debian
sudo cp api.crossdesk.cn_root.crt /usr/local/share/ca-certificates/ && sudo update-ca-certificates
```

```bash
# macOS
sudo security add-trusted-cert -d -r trustRoot -k /Library/Keychains/System.keychain api.crossdesk.cn_root.crt
```

## 客户端配置

**桌面端（控制端和被控端都要改）**

1. `☰ → 设置 → 自托管配置 → 编辑`
2. 服务器地址：`1.2.3.4` 或 `desk.example.com`（**不填协议、路径、端口**）
3. 信令端口 `9099`，中继端口 `3478` → 确认
4. 打开「自托管配置」开关 → 保存 → 等两端都显示"已连接服务器"
5. 切换服务器后**重新核对被控端 ID**（设备身份绑定服务端，旧 ID 不可复用）

**iOS**：设置 → 服务器，填信令主机 / 信令端口 / STUN-TURN 端口，自签根证书需在设备上安装并信任。

**Web**：改 `crossdesk-web-client` 的 `web_client.js` 里 `signalingUrl`（如 `wss://1.2.3.4:9099`）
和 STUN 地址（`stun:1.2.3.4:3478`）。浏览器要信任证书，不要关 TLS 校验。

## 管理后台

`.env` 里同时填 `ADMIN_USERNAME` 和 `ADMIN_PASSWORD`（只填一个不生效），然后
`docker compose up -d --no-build`，访问 `https://<地址>:9099/admin`。
可看在线设备、会话、累计时长，并强制断开指定会话。登录态在内存，重启需重登。

可选地域分布：`CROSSDESK_GEOIP_LOOKUP=1` + `CROSSDESK_GEOIP_KEY`（IP2Location），默认关闭，关闭时不会外发客户端 IP。

## 运维

```bash
cd /opt/crossdesk
docker compose ps
docker compose logs --tail 100 crossdesk-server coturn
docker compose up -d --no-build     # 改 .env 后必须这样重建，restart 不生效

# 备份（含密钥与设备库，注意保管）
docker compose stop
tar -czf crossdesk-backup-$(date +%F).tar.gz .env compose.yaml \
  -C /var/lib/crossdesk certs db
docker compose up -d --no-build
```

## 排障顺序

| 现象 | 检查 |
| --- | --- |
| 未连接服务器 / TLS 错误 | 端口、证书有效期、证书 SAN 是否含所填地址、根证书是否已导入 |
| 已连服务器但设备离线 | 两端是否同一服务器、被控端是否运行、是否复制了当前 ID |
| 能连上但黑屏 / 无画面 | `50000-60000/UDP` 是否放通、Coturn 状态、客户端版本是否配套 |
| `config -q` 校验失败 | `EXTERNAL_IP`/`INTERNAL_IP` 或两个密钥为空，或 compose 与 env 不是同一 Release |
| Coturn 起不来 | `INTERNAL_IP` 不属于本机网卡、`3478` 被占用 |
| 后台打不开 | 两个管理变量是否都设了、是否重建过容器、是否走 HTTPS |
| 改配置没反应 | 用了 `restart`；应 `up -d --no-build` |

## 不用 Docker 的裸机方式

服务端是单个静态二进制，Release 里也有 `crossdesk_server-linux-amd64` / `-arm64`：

```bash
install -Dm755 crossdesk_server-linux-amd64 /usr/local/bin/crossdesk_server
# 需要自己准备 /var/lib/crossdesk/certs、db、日志目录，二进制不会自动签发证书
crossdesk_server 9099
```

裸机方式需自行用 systemd 托管并单独跑 Coturn，官方发布流程只维护 Docker 镜像，一般不推荐。
