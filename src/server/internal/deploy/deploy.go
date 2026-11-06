// Package deploy 生成跨发行版的一键安装脚本。
// 目标：在任何 Linux 上以管理员身份执行一行命令即可完成：
//
//	下载 agent 二进制 → 写入配置 → 注册 systemd → 启动 → 自动纳管
package deploy

import (
	"fmt"
	"strings"
)

// ScriptParams 生成安装脚本所需参数
type ScriptParams struct {
	ServerHost   string   // 服务端主机（IP 或域名），批准管理中可自定义
	ServerIPs    []string // 服务端所有非回环 IP（自动枚举，agent 自动切换）
	ServerPort   int      // 控制通道端口（默认 9000）
	RegKey       string   // 注册令牌
	Fingerprint  string   // TLS 证书指纹
	DownloadURL  string   // agent 二进制下载地址（默认走服务端 /download/agent）
	ServerID     string
	DiscoverPort int
	UseTLS       bool
}

// GenScript 生成一键安装 shell 脚本
func GenScript(p *ScriptParams) string {
	port := p.ServerPort
	if port == 0 {
		port = 9000
	}
	discPort := p.DiscoverPort
	if discPort == 0 {
		discPort = 9001
	}
	/* 默认 HTTP（use_tls=0），服务端支持 TLS 时可通过配置开启 */
	useTLS := 0
	if p.UseTLS {
		useTLS = 1
	}
	dl := p.DownloadURL
	if dl == "" {
		/* 默认 HTTP:8080，不通才回退 HTTPS:9443 */
		dl = fmt.Sprintf("http://%s:8080/download/agent", p.ServerHost)
	}

	serverHosts := p.ServerHost
	if len(p.ServerIPs) > 0 {
		serverHosts = strings.Join(p.ServerIPs, ",")
	}
	return fmt.Sprintf(`#!/bin/sh
# ctforge agent 一键安装脚本
# 支持发行版：Fedora/RHEL/CentOS/Debian/Ubuntu/openSUSE/Alpine
# 要求：root 或 sudo
set -e

echo "==> CTForge Agent Installer"

# ---- 权限检查 ----
if [ "$(id -u)" != "0" ]; then
    echo "错误：请以 root 或 sudo 运行本脚本"
    echo "用法：curl -fsSL <url> | sudo sh"
    exit 1
fi

# ---- 幂等检测：已在运行则跳过（避免重复安装/重复进程）----
if [ "${CTF_FORCE:-0}" != "1" ]; then
    if [ -f /etc/ctforge/agent.conf ] && grep -q '^server_host=' /etc/ctforge/agent.conf 2>/dev/null; then
        for b in /usr/local/bin/ctforge-agent /usr/bin/ctforge-agent /bin/ctforge-agent /opt/bin/ctforge-agent; do
            if [ -x "$b" ]; then
                echo "==> 已检测到 CTForge Agent：$b"
                echo "    配置：/etc/ctforge/agent.conf"
                # 尝试确保服务在运行
                if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
                    systemctl is-active ctforge-agent >/dev/null 2>&1 || systemctl restart ctforge-agent >/dev/null 2>&1 || true
                elif [ -x /etc/init.d/ctforge-agent ]; then
                    /etc/init.d/ctforge-agent start >/dev/null 2>&1 || true
                fi
                echo "==> 跳过安装（幂等）。如需强制重装：sudo CTF_FORCE=1 sh <script>"
                exit 0
            fi
        done
    fi
fi

# ---- 架构检测 ----
ARCH=$(uname -m)
case "$ARCH" in
    x86_64|amd64)   BIN_ARCH="amd64" ;;
    aarch64|arm64)  BIN_ARCH="arm64" ;;
    *)              echo "错误：不支持的架构 $ARCH"; exit 1 ;;
esac

# ---- 下载 agent 二进制 ----
echo "==> 下载 agent 二进制..."
TMPBIN=$(mktemp /tmp/ctforge-agent.XXXXXX)
trap 'rm -f "$TMPBIN"' EXIT

HTTP_URL="%s?arch=$BIN_ARCH"
# 默认 HTTP:8080，不通回退 HTTPS:9443
HTTPS_URL=$(echo "$HTTP_URL" | sed -e 's|^http://|https://|' -e 's|:8080|:9443|')
echo "    架构: $BIN_ARCH  地址: $HTTP_URL"
DL_OK=0
if command -v curl >/dev/null 2>&1; then
    if curl -fsSL -o "$TMPBIN" "$HTTP_URL" 2>/dev/null; then
        DL_OK=1
    elif curl -fsSL -o "$TMPBIN" "$HTTPS_URL" 2>/dev/null; then
        echo "    HTTP 不通，回退 HTTPS: $HTTPS_URL"
        DL_OK=1
    fi
elif command -v wget >/dev/null 2>&1; then
    if wget -q -O "$TMPBIN" "$HTTP_URL" 2>/dev/null; then
        DL_OK=1
    elif wget -q -O "$TMPBIN" "$HTTPS_URL" 2>/dev/null; then
        echo "    HTTP 不通，回退 HTTPS: $HTTPS_URL"
        DL_OK=1
    fi
fi
if [ "$DL_OK" != "1" ]; then
    echo "错误：下载失败（HTTP 与 HTTPS 均不通）"
    exit 1
fi

chmod +x "$TMPBIN"

# ---- 下载 ctforged eBPF 守护进程（可选，失败不阻断 agent 安装） ----
CTFD_TMPBIN=$(mktemp /tmp/ctforged.XXXXXX)
CTFD_URL="$(echo "$HTTP_URL" | sed 's|/download/agent|/download/ctforged|')"
CTFD_DL_OK=0
if command -v curl >/dev/null 2>&1; then
    if curl -fsSL -o "$CTFD_TMPBIN" "$CTFD_URL" 2>/dev/null; then
        CTFD_DL_OK=1
    fi
elif command -v wget >/dev/null 2>&1; then
    if wget -q -O "$CTFD_TMPBIN" "$CTFD_URL" 2>/dev/null; then
        CTFD_DL_OK=1
    fi
fi
if [ "$CTFD_DL_OK" = "1" ]; then
    chmod +x "$CTFD_TMPBIN"
    mkdir -p /usr/sbin 2>/dev/null
    if cp "$CTFD_TMPBIN" /usr/sbin/ctforged 2>/dev/null; then
        chmod 755 /usr/sbin/ctforged
        echo "==> ctforged 已安装到 /usr/sbin/ctforged"
        # 写默认配置
        if [ ! -f /etc/ctforge/ctforge.cfg ]; then
            mkdir -p /etc/ctforge /usr/share/ctforge/payload /var/log/ctforge
            cat > /etc/ctforge/ctforge.cfg <<'CFGEOF'
{
    "listen": "127.0.0.1:7000",
    "payload_dir": "/usr/share/ctforge/payload",
    "log_dir": "/var/log/ctforge",
    "private_key": "/etc/ctforge/private.pem",
    "public_key": "/etc/ctforge/public.pem"
}
CFGEOF
            chmod 644 /etc/ctforge/ctforge.cfg
        fi
    fi
fi
rm -f "$CTFD_TMPBIN"

# ---- 确定安装目录并安装二进制 ----
# OpenWrt / 精简系统的 /usr/local/bin 可能不存在，需逐级回退
BIN_PATH=""
for d in /usr/local/bin /usr/bin /bin /opt/bin; do
    [ -d "$d" ] || continue
    if cp "$TMPBIN" "$d/ctforge-agent" 2>/dev/null; then
        chmod 755 "$d/ctforge-agent" 2>/dev/null && BIN_PATH="$d/ctforge-agent" && break
    fi
done

# 若所有目录都不存在，尝试创建 /usr/local/bin
if [ -z "$BIN_PATH" ]; then
    mkdir -p /usr/local/bin 2>/dev/null
    if cp "$TMPBIN" /usr/local/bin/ctforge-agent 2>/dev/null; then
        chmod 755 /usr/local/bin/ctforge-agent && BIN_PATH="/usr/local/bin/ctforge-agent"
    fi
fi

if [ -z "$BIN_PATH" ]; then
    echo "错误：无法安装二进制到任何标准目录"
    echo "提示：请检查 /usr/local/bin 或 /usr/bin 是否可写"
    rm -f "$TMPBIN"
    exit 1
fi

# ---- 清理其他目录的旧版本，避免 systemd 指向过期二进制 ----
for d in /usr/local/bin /usr/bin /bin /opt/bin; do
    f="$d/ctforge-agent"
    [ "$f" = "$BIN_PATH" ] && continue
    if [ -f "$f" ]; then
        cmp -s "$TMPBIN" "$f" 2>/dev/null || { rm -f "$f" && echo "    清理旧版本：$f"; }
    fi
done

echo "==> 已安装到 $BIN_PATH"
rm -f "$TMPBIN"

# ---- 写入配置 ----
echo "==> 写入配置 /etc/ctforge/agent.conf..."
mkdir -p /etc/ctforge
cat > /etc/ctforge/agent.conf <<'EOF'
server_host=%s
server_port=%d
use_tls=%d
server_fingerprint=%s
token=%s
discover_enable=1
discover_port=%d
metrics_interval=30
heartbeat_interval=10
log_level=1
# 批准制：agent 会主动广播，服务端批准后才会正式接入
# 绑定的服务端 ID（批准后写入，agent 重连时校验）
bound_server_id=
# 服务端候选地址列表（逗号分隔，agent 自动切换）
# server_host 为空时，agent 会依次尝试此列表
EOF

# ---- 按 init 系统注册自启动服务 ----
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    echo "==> 注册 systemd 服务..."
    cat > /etc/systemd/system/ctforge-agent.service <<'UNIT'
[Unit]
Description=ctforge monitoring agent
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=__BIN_PATH__ -c /etc/ctforge/agent.conf
Restart=always
RestartSec=5
Nice=19
CPUQuota=5%%
MemoryLimit=64M
ProtectSystem=full
NoNewPrivileges=false

[Install]
WantedBy=multi-user.target
UNIT
    sed -i "s|__BIN_PATH__|$BIN_PATH|g" /etc/systemd/system/ctforge-agent.service
    systemctl daemon-reload
    systemctl enable ctforge-agent 2>/dev/null || true
    systemctl restart ctforge-agent
    echo "    状态查询：systemctl status ctforge-agent"

elif [ -f /sbin/procd ] || [ -d /etc/init.d ] && [ -f /sbin/init ]; then
    # OpenWrt procd
    echo "==> 注册 OpenWrt procd 服务..."
    cat > /etc/init.d/ctforge-agent <<'UNIT'
#!/bin/sh /etc/rc.common
START=99
STOP=10
USE_PROCD=1

start_service() {
    procd_open_instance
    procd_set_param command __BIN_PATH__ -c /etc/ctforge/agent.conf
    procd_set_param respawn 3600 5 5
    procd_set_param stdout 1
    procd_set_param stderr 1
    procd_set_param nice 19
    procd_close_instance
}
UNIT
    sed -i "s|__BIN_PATH__|$BIN_PATH|g" /etc/init.d/ctforge-agent
    chmod +x /etc/init.d/ctforge-agent
    /etc/init.d/ctforge-agent enable
    /etc/init.d/ctforge-agent start
    echo "    状态查询：/etc/init.d/ctforge-agent status"

elif [ -d /etc/init.d ]; then
    # SysV init
    echo "==> 注册 SysV init 服务..."
    cat > /etc/init.d/ctforge-agent <<'UNIT'
#!/bin/sh
# chkconfig: 2345 99 10
### BEGIN INIT INFO
# Provides:          ctforge-agent
# Required-Start:    $network
# Required-Stop:     $network
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Description:       ctforge monitoring agent
### END INIT INFO
case "$1" in
  start)   __BIN_PATH__ -c /etc/ctforge/agent.conf & echo $! > /var/run/ctforge-agent.pid ;;
  stop)    [ -f /var/run/ctforge-agent.pid ] && kill $(cat /var/run/ctforge-agent.pid) ;;
  restart) $0 stop; sleep 1; $0 start ;;
  status)  [ -f /var/run/ctforge-agent.pid ] && kill -0 $(cat /var/run/ctforge-agent.pid) && echo running || echo stopped ;;
  *) echo "usage: $0 {start|stop|restart|status}"; exit 1 ;;
esac
UNIT
    sed -i "s|__BIN_PATH__|$BIN_PATH|g" /etc/init.d/ctforge-agent
    chmod +x /etc/init.d/ctforge-agent
    chkconfig --add ctforge-agent 2>/dev/null || update-rc.d ctforge-agent defaults 2>/dev/null || true
    /etc/init.d/ctforge-agent start
else
    # 无 init 系统（容器等）：直接后台起
    echo "==> 未检测到 init 系统，直接后台启动..."
    nohup __BIN_PATH__ -c /etc/ctforge/agent.conf > /var/log/ctforge-agent.log 2>&1 &
    echo $! > /var/run/ctforge-agent.pid
    echo "    已作为后台进程启动（PID: $(cat /var/run/ctforge-agent.pid)）"
fi

echo ""
echo "==> 安装完成！"
echo "    服务端将在控制台的【批准管理】中看到本机（%s），批准后开始监控。"
echo "    日志查看：journalctl -u ctforge-agent -f 或 tail -f /var/log/ctforge-agent.log"
`,
		dl,
		serverHosts, port, useTLS, p.Fingerprint, p.RegKey, discPort, p.ServerHost)
}

// GenCurlCommand 生成一行 curl 命令（用户直接粘贴执行）
func GenCurlCommand(p *ScriptParams) string {
	url := p.DownloadURL
	if url == "" {
		url = fmt.Sprintf("http://%s:8080/api/deploy/script.sh", p.ServerHost)
	}
	return fmt.Sprintf("curl -fsSL %s | sudo sh", url)
}
