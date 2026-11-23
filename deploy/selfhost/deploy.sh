#!/usr/bin/env bash
#
# CrossDesk 自托管服务端一键部署（Linux + Docker Compose）
#
# 做的事：
#   1. 校验 Docker / Compose 插件
#   2. 从 crossdesk-server 的 GitHub Release 下载与版本匹配的 compose.yaml + env.example
#   3. 自动探测公网 IP / 内网网卡 IP，生成两个 32 字节十六进制密钥
#   4. 写入 .env（已存在则保留，避免误改密钥）
#   5. 可选 --domain：预生成带 SAN 的自签证书（域名 + IP 都能用）
#   6. 放通防火墙端口，拉镜像并启动，最后输出根证书路径与验证命令
#
# 用法：
#   sudo ./deploy.sh                        # 用公网 IP 部署，最新 Release
#   sudo ./deploy.sh --tag v1.1.38          # 固定版本（生产建议固定）
#   sudo ./deploy.sh --domain desk.example.com
#   sudo ./deploy.sh --dir /srv/crossdesk --port 443
#
# 注意：compose 使用 network_mode: host，脚本不做端口映射，端口必须在本机空闲。
set -euo pipefail

TAG="${CROSSDESK_SERVER_TAG:-}"
DEPLOY_DIR="${DEPLOY_DIR:-/opt/crossdesk}"
DOMAIN=""
EXTERNAL_IP=""
INTERNAL_IP=""
PORT="9099"
COTURN_PORT="3478"
MIN_PORT="50000"
MAX_PORT="60000"
FORCE="0"

usage() {
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --tag)          TAG="$2"; shift 2 ;;
        --dir)          DEPLOY_DIR="$2"; shift 2 ;;
        --domain)       DOMAIN="$2"; shift 2 ;;
        --external-ip)  EXTERNAL_IP="$2"; shift 2 ;;
        --internal-ip)  INTERNAL_IP="$2"; shift 2 ;;
        --port)         PORT="$2"; shift 2 ;;
        --coturn-port)  COTURN_PORT="$2"; shift 2 ;;
        --force)        FORCE="1"; shift ;;
        -h|--help)      usage; exit 0 ;;
        *)              echo "未知参数: $1" >&2; usage; exit 2 ;;
    esac
done

log()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[!]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

# ---------- 0. 前置检查 ----------
command -v docker >/dev/null 2>&1 || die "未找到 docker，请先安装 Docker Engine"
docker compose version >/dev/null 2>&1 || die "未找到 compose 插件，请安装 docker-compose-plugin（不是旧版 docker-compose）"
command -v openssl >/dev/null 2>&1 || die "未找到 openssl，请先安装"
command -v curl >/dev/null 2>&1 || die "未找到 curl，请先安装"

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo"
    log "非 root 运行，docker 相关命令将使用 sudo"
fi

# ---------- 1. 部署目录与 Release 文件 ----------
mkdir -p "$DEPLOY_DIR"
cd "$DEPLOY_DIR"

if [ -z "$TAG" ]; then
    log "查询 crossdesk-server 最新 Release"
    TAG="$(curl -fsSL --max-time 15 \
        https://api.github.com/repos/kunkundi/crossdesk-server/releases/latest \
        | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -n1)"
    [ -n "$TAG" ] || die "无法获取最新 Release 版本，请用 --tag 指定，例如 --tag v1.1.38"
fi
log "目标版本: $TAG"

BASE="https://github.com/kunkundi/crossdesk-server/releases/download/$TAG"
for f in compose.yaml env.example; do
    if [ -f "$f" ]; then
        log "已存在 $f，保留（如需替换请手动删除）"
    else
        log "下载 $f"
        curl -fsSL --retry 3 -o "$f" "$BASE/$f" || die "下载 $f 失败，检查版本号是否存在"
    fi
done

# ---------- 2. 探测 IP ----------
if [ -z "$EXTERNAL_IP" ]; then
    EXTERNAL_IP="$(curl -fsS --max-time 8 https://api.ipify.org 2>/dev/null \
        || curl -fsS --max-time 8 https://ifconfig.me 2>/dev/null \
        || true)"
fi
[ -n "$EXTERNAL_IP" ] || die "无法自动探测公网 IP，请用 --external-ip 指定"

if [ -z "$INTERNAL_IP" ]; then
    INTERNAL_IP="$(ip -4 route get 1.1.1.1 2>/dev/null \
        | awk '{for(i=1;i<=NF;i++) if($i=="src"){print $(i+1); exit}}')"
fi
if [ -z "$INTERNAL_IP" ]; then
    INTERNAL_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
fi
[ -n "$INTERNAL_IP" ] || die "无法探测内网网卡 IP，请用 --internal-ip 指定"

log "EXTERNAL_IP=$EXTERNAL_IP  INTERNAL_IP=$INTERNAL_IP"

# ---------- 3. .env ----------
DATA_DIR="/var/lib/crossdesk"
LOG_DIR="/var/log/crossdesk"

if [ -f .env ] && [ "$FORCE" != "1" ]; then
    warn ".env 已存在，跳过重写（避免更换密钥导致旧凭据失效）。要重建请加 --force"
else
    if [ -f .env ]; then
        cp .env ".env.bak.$(date +%Y%m%d-%H%M%S)"
    fi
    cp env.example .env
    chmod 600 .env

    AUTH_SECRET="$(openssl rand -hex 32)"
    NONCE_SECRET="$(openssl rand -hex 32)"

    set_kv() { # set_kv <key> <value>
        local k="$1" v="$2"
        if grep -qE "^[#[:space:]]*${k}=" .env; then
            awk -v k="$k" -v v="$v" '
                $0 ~ "^[#[:space:]]*"k"=" && !done {print k"="v; done=1; next}
                {print}
            ' .env > .env.tmp && mv .env.tmp .env
        else
            printf '%s=%s\n' "$k" "$v" >> .env
        fi
        chmod 600 .env
    }

    set_kv EXTERNAL_IP           "$EXTERNAL_IP"
    set_kv INTERNAL_IP           "$INTERNAL_IP"
    set_kv CROSSDESK_SERVER_PORT "$PORT"
    set_kv COTURN_PORT           "$COTURN_PORT"
    set_kv MIN_PORT              "$MIN_PORT"
    set_kv MAX_PORT              "$MAX_PORT"
    set_kv CROSSDESK_IMAGE       "crossdesk/crossdesk-server:$TAG"
    set_kv COTURN_AUTH_SECRET    "$AUTH_SECRET"
    set_kv COTURN_STATELESS_NONCE_SECRET "$NONCE_SECRET"
    if [ -n "$DOMAIN" ]; then
        set_kv COTURN_PUBLIC_HOST "$DOMAIN"
    fi
    log ".env 已生成（两个密钥均为 openssl rand -hex 32 独立生成）"
fi

# DATA_DIR / LOG_DIR 以 .env 为准（可能被手工改过）
DATA_DIR="$(sed -n 's/^CROSSDESK_DATA_DIR=//p' .env | tail -n1)"
DATA_DIR="${DATA_DIR:-/var/lib/crossdesk}"
LOG_DIR="$(sed -n 's/^CROSSDESK_LOG_DIR=//p' .env | tail -n1)"
LOG_DIR="${LOG_DIR:-/var/log/crossdesk}"
$SUDO mkdir -p "$DATA_DIR/certs" "$DATA_DIR/db" "$LOG_DIR"

# ---------- 4. 可选：预生成带域名 SAN 的自签证书 ----------
# 官方 start.sh 只在 certs 目录缺少 key/bundle 时才自动签发，且只写 IP 的 SAN。
# 提前放好证书 → start.sh 直接复用，不会覆盖。
CERT_DIR="$DATA_DIR/certs"
if [ -n "$DOMAIN" ] && [ ! -f "$CERT_DIR/api.crossdesk.cn.key" ]; then
    log "为域名 $DOMAIN 预生成自签证书（SAN: DNS:$DOMAIN, IP:$EXTERNAL_IP）"
    TMPC="$(mktemp -d)"
    (
        cd "$TMPC"
        openssl genrsa -out root.key 4096 2>/dev/null
        openssl req -x509 -new -nodes -key root.key -sha256 -days 3650 \
            -out api.crossdesk.cn_root.crt \
            -subj "/C=CN/O=CrossDesk/CN=CrossDesk Self-Hosted Root" 2>/dev/null

        openssl genrsa -out api.crossdesk.cn.key 2048 2>/dev/null
        cat > san.cnf <<EOF
[req]
distinguished_name = dn
req_extensions = v3_req
prompt = no
[dn]
CN = $DOMAIN
[v3_req]
subjectAltName = @alt
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
[alt]
DNS.1 = $DOMAIN
IP.1 = $EXTERNAL_IP
EOF
        openssl req -new -key api.crossdesk.cn.key -out server.csr -config san.cnf 2>/dev/null
        openssl x509 -req -in server.csr -CA api.crossdesk.cn_root.crt -CAkey root.key \
            -CAcreateserial -out server.crt -days 825 -sha256 \
            -extensions v3_req -extfile san.cnf 2>/dev/null
        cat server.crt api.crossdesk.cn_root.crt > api.crossdesk.cn_bundle.crt

        $SUDO install -m 0644 api.crossdesk.cn_root.crt api.crossdesk.cn_bundle.crt "$CERT_DIR/"
        $SUDO install -m 0600 api.crossdesk.cn.key "$CERT_DIR/"
    )
    rm -rf "$TMPC"
    log "证书已放入 $CERT_DIR"
elif [ -n "$DOMAIN" ]; then
    log "检测到已有证书，保留不覆盖；确认其 SAN 是否包含 $DOMAIN"
fi

# ---------- 5. 防火墙 ----------
if command -v ufw >/dev/null 2>&1 && $SUDO ufw status 2>/dev/null | grep -q "Status: active"; then
    log "放通 ufw 端口"
    $SUDO ufw allow "$PORT/tcp"                >/dev/null
    $SUDO ufw allow "$COTURN_PORT/tcp"         >/dev/null
    $SUDO ufw allow "$COTURN_PORT/udp"         >/dev/null
    $SUDO ufw allow "$MIN_PORT:$MAX_PORT/udp"  >/dev/null
elif command -v firewall-cmd >/dev/null 2>&1 && $SUDO firewall-cmd --state >/dev/null 2>&1; then
    log "放通 firewalld 端口"
    $SUDO firewall-cmd --permanent --add-port="$PORT/tcp"               >/dev/null
    $SUDO firewall-cmd --permanent --add-port="$COTURN_PORT/tcp"        >/dev/null
    $SUDO firewall-cmd --permanent --add-port="$COTURN_PORT/udp"        >/dev/null
    $SUDO firewall-cmd --permanent --add-port="$MIN_PORT-$MAX_PORT/udp" >/dev/null
    $SUDO firewall-cmd --reload >/dev/null
else
    warn "未检测到启用的 ufw/firewalld，云服务器请手动放通：TCP $PORT、TCP/UDP $COTURN_PORT、UDP $MIN_PORT-$MAX_PORT"
fi

# ---------- 6. 启动 ----------
log "校验 compose 配置"
$SUDO docker compose config -q

log "拉取镜像"
$SUDO docker compose pull

log "启动服务"
$SUDO docker compose up -d --no-build

log "等待证书与数据库就绪"
for _ in $(seq 1 30); do
    if $SUDO test -s "$CERT_DIR/api.crossdesk.cn.key" \
        && $SUDO test -s "$CERT_DIR/api.crossdesk.cn_bundle.crt"; then
        break
    fi
    sleep 2
done
$SUDO docker compose ps

# ---------- 7. 输出后续步骤 ----------
HOST="$EXTERNAL_IP"
if [ -n "$DOMAIN" ]; then
    HOST="$DOMAIN"
fi
ROOT_CRT="$CERT_DIR/api.crossdesk.cn_root.crt"

cat <<EOF

------------------------------------------------------------------
部署完成，版本 $TAG

服务端目录     : $DEPLOY_DIR
数据 / 证书    : $DATA_DIR
日志           : $LOG_DIR

客户端填写：
  服务器地址   : $EXTERNAL_IP$([ -n "$DOMAIN" ] && echo "（或域名 $DOMAIN，证书已含 SAN）")
  信令端口     : $PORT
  中继端口     : $COTURN_PORT

管理后台（先在 .env 设 ADMIN_USERNAME / ADMIN_PASSWORD 再
docker compose up -d --no-build 重建容器）：
  https://$HOST:$PORT/admin

需要导入到每台控制端/被控端的根证书：
  $ROOT_CRT
  Windows(管理员 PowerShell):
    certutil -addstore "Root" "C:\path\to\api.crossdesk.cn_root.crt"
  Ubuntu/Debian:
    sudo cp api.crossdesk.cn_root.crt /usr/local/share/ca-certificates/ && sudo update-ca-certificates
  macOS:
    sudo security add-trusted-cert -d -r trustRoot -k /Library/Keychains/System.keychain api.crossdesk.cn_root.crt

自检：
  curl --fail --cacert $ROOT_CRT https://$HOST:$PORT/stats
  sudo docker compose logs --tail 50 crossdesk-server coturn

提示：
  - 客户端两端必须指向同一服务器；升级时先升服务端再升客户端。
  - 改了 .env 必须 docker compose up -d --no-build，restart 不会应用新环境变量。
  - 改 EXTERNAL_IP 不会自动重签证书，需删除 $CERT_DIR 下的证书后重启。
------------------------------------------------------------------
EOF
