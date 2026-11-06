# CTForge

CTForge 是一个基于 eBPF 的 Linux 安全框架，提供非侵入式动态防护与集中化控制能力。项目保留 eBPF payload 动态加载、SM2 签名等核心能力，同时集成 Web 控制台、远程终端、文件管理、告警通知等完整运维功能。

## 功能特性

| 类别 | 功能 |
|------|------|
| **eBPF 防护** | payload 模块动态加载/卸载、SM2 签名验证、内核态攻击缓解 |
| **主机监控** | CPU / 内存 / 磁盘 / 负载实时趋势，10s 自动刷新 |
| **远程终端** | 浏览器内 xterm.js 全双工终端，支持 vim / top 等全屏程序 |
| **文件管理** | 目录浏览 / 上传 / 下载 / 删除 / 重命名 / 新建目录 |
| **系统运维** | systemd 服务管理、软件包更新、安全审计、系统账户管理 |
| **网络管理** | 接口级流量监控、连接状态分布、端口转发 |
| **命令执行** | 远程一次性命令下发，返回 stdout/stderr/退出码/耗时 |
| **告警引擎** | 离线/CPU/内存/磁盘/流量/SSH 爆破检测 + 邮件/Webhook 通知 |
| **SSH 跳板** | `ssh -p 2222` 直接登录纳管主机，密码/公钥认证 |
| **定时任务** | cron 表达式批量执行命令，记录执行历史 |
| **AI 分析** | OpenAI 兼容 API 自动诊断并给出优化建议 |
| **权限管理** | RBAC 三级角色（admin/operator/viewer），26 个权限点 |
| **CTForge 管理** | Web 界面直接管理 eBPF payload 模块（加载/卸载/状态查询） |

---

## 运行方式

CTForge 提供三种运行方式，按推荐程度排序：

---

### 方式一：Docker Compose（推荐）

适合：快速部署、生产环境、隔离运行。

**前置要求：** Docker 20.10+ 与 Docker Compose v2。

```bash
# 1. 克隆或进入项目目录
cd ctforge-web

# 2. （可选）创建 .env 配置注册令牌等参数
cat > .env << 'EOF'
# Agent 注册令牌（留空则不校验，生产环境必须设置）
CTF_TOKEN=your-secure-token-here
# 局域网发现（0=关闭）
CTF_DISCOVER=9001
# SSH 跳板端口（空=关闭）
CTF_SSH=:2222
EOF

# 3. 构建并启动
docker compose up -d --build

# 4. 查看状态
docker compose ps
docker compose logs -f ctforge-server

# 5. 访问 Web 控制台
# 默认地址：http://localhost:8080
# 默认账号：admin / admin123（首次登录强制改密）
```

**常用运维命令：**

```bash
# 停止
docker compose down

# 重启
docker compose restart

# 更新（拉新代码后重建）
git pull && docker compose up -d --build

# 进入容器调试
docker exec -it ctforge-server sh

# 查看持久化数据（账户、证书）
docker volume inspect ctforge-web_ctforge-data
docker volume inspect ctforge-web_ctforge-certs
```

**端口说明：**

| 端口 | 协议 | 用途 |
|------|------|------|
| 8080 | TCP | Web 控制台 + REST API + WebSocket |
| 9000 | TCP | Agent 控制通道（TLS 1.2+） |
| 9001 | UDP | 局域网发现广播 |
| 2222 | TCP | SSH 跳板机（可选） |

---

### 方式二：裸机直接运行

适合：开发调试、无 Docker 环境、单节点快速验证。

**前置要求：** Go 1.21+、Node.js 18+、gcc、openssl（可选 clang/libbpf 用于 eBPF 组件）。

```bash
# 1. 构建全部组件
make all

# 2. 安装到系统（可选，默认 PREFIX=/usr/local）
sudo make install

# 3. 准备运行时目录与配置
sudo mkdir -p /var/lib/ctforge /etc/ctforge
sudo cp src/clients/ctforge-agent.conf.example /etc/ctforge/agent.conf

# 4. 启动服务端（方式 A：直接前台运行，Ctrl+C 停止）
sudo /usr/local/bin/ctforge-server \
  -http :8080 \
  -sock :9000 \
  -static src/frontend/dist \
  -data /var/lib/ctforge \
  -accounts /var/lib/ctforge/accounts.json \
  -tls-cert /etc/ctforge/server.crt \
  -tls-key  /etc/ctforge/server.key

# 5. 或使用环境变量（等效）
sudo CTF_HTTP=:8080 \
     CTF_SOCK=:9000 \
     CTF_STATIC=src/frontend/dist \
     CTF_DATA=/var/lib/ctforge \
     /usr/local/bin/ctforge-server
```

**服务端启动参数说明：**

| 参数 | 环境变量 | 默认值 | 说明 |
|------|----------|--------|------|
| `-http` | `CTF_HTTP` | `:8080` | HTTP/WebSocket 监听地址 |
| `-sock` | `CTF_SOCK` | `:9000` | Agent 控制通道监听地址 |
| `-static` | `CTF_STATIC` | 空 | 前端 dist 目录（空=不托管前端） |
| `-token` | `CTF_TOKEN` | 空 | Agent 注册令牌（空=不校验） |
| `-tls-cert` | `CTF_TLS_CERT` | `server.crt` | TLS 证书路径 |
| `-tls-key` | `CTF_TLS_KEY` | `server.key` | TLS 密钥路径 |
| `-tls-san` | `CTF_TLS_SAN` | 空 | 额外证书 SAN（逗号分隔） |
| `-accounts` | `CTF_ACCOUNTS` | `accounts.json` | 账户文件路径 |
| `-data` | `CTF_DATA` | `.` | 运行时数据目录 |
| `-ssh` | `CTF_SSH` | `:2222` | SSH 跳板监听地址（空=关闭） |
| `-discover` | `CTF_DISCOVER` | `9001` | UDP 发现端口（0=关闭） |
| `-agent-bin` | `CTF_AGENT_BIN` | 空 | Agent 二进制路径（用于 `/download/agent`） |
| `-no-tls` | `CTF_NO_TLS=1` | 关闭 | 禁用 Agent 通道 TLS（仅调试用） |

---

### 方式三：Systemd 服务（生产推荐）

适合：生产服务器、需要开机自启和崩溃自动重启的场景。

**服务端 systemd unit：**

```bash
# 1. 构建并安装
make all && sudo make install

# 2. 创建 systemd unit
sudo tee /etc/systemd/system/ctforge-server.service << 'EOF'
[Unit]
Description=CTForge Server
Documentation=https://github.com/ctforge/ctforge
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
User=ctforge
Group=ctforge

ExecStart=/usr/local/bin/ctforge-server \
  -http :8080 \
  -sock :9000 \
  -static /usr/local/share/ctforge/static \
  -data /var/lib/ctforge \
  -accounts /var/lib/ctforge/accounts.json \
  -tls-cert /etc/ctforge/server.crt \
  -tls-key /etc/ctforge/server.key

Restart=always
RestartSec=10
LimitNOFILE=65536

# 安全加固
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
ReadWritePaths=/var/lib/ctforge /etc/ctforge

[Install]
WantedBy=multi-user.target
EOF

# 3. 创建运行用户与目录
sudo useradd -r -s /usr/sbin/nologin ctforge 2>/dev/null || true
sudo mkdir -p /var/lib/ctforge /etc/ctforge
sudo chown -R ctforge:ctforge /var/lib/ctforge /etc/ctforge

# 4. 复制前端静态文件
sudo mkdir -p /usr/local/share/ctforge/static
sudo cp -r src/frontend/dist/* /usr/local/share/ctforge/static/

# 5. 启用并启动
sudo systemctl daemon-reload
sudo systemctl enable --now ctforge-server

# 6. 查看状态
sudo systemctl status ctforge-server
sudo journalctl -u ctforge-server -f
```

**eBPF 守护进程（ctforged）systemd unit：**

```bash
# 安装 ctforged（需要 clang + libbpf 编译）
sudo make -C src/ctforged install

# 启用服务（项目已附带 unit 文件）
sudo systemctl enable --now ctforged

# 查看状态
sudo systemctl status ctforged
sudo journalctl -u ctforged -f
```

---

## Agent 部署

### Agent 安装（被纳管主机）

**方式 A：一键脚本（推荐）**

在 CTForge Web 控制台 → 主机列表 → 点击「部署 Agent」按钮，生成一键安装命令，在目标主机以 root 执行：

```bash
# 由 Web 控制台生成，示例：
curl -fsSL http://<server-ip>:8080/api/deploy/script.sh | sudo sh
```

**方式 B：手动安装**

```bash
# 1. 下载 agent 二进制（从服务端）
curl -fsSL http://<server-ip>:8080/download/agent?arch=amd64 -o /usr/local/bin/ctforge-agent
chmod +x /usr/local/bin/ctforge-agent

# 2. 创建配置
cat > /etc/ctforge/agent.conf << 'EOF'
server_host=<server-ip>
server_port=9000
token=<与 CTF_TOKEN 一致>
use_tls=1
metrics_interval=30
heartbeat_interval=10
log_level=1
EOF

# 3. 创建 systemd unit
cat > /etc/systemd/system/ctforge-agent.service << 'EOF'
[Unit]
Description=CTForge Agent
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
ExecStart=/usr/local/bin/ctforge-agent -c /etc/ctforge/agent.conf
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF

# 4. 启动
systemctl daemon-reload
systemctl enable --now ctforge-agent
```

**Agent 配置项说明：**

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `server_host` | `127.0.0.1` | 服务端地址 |
| `server_port` | `9000` | 服务端 Agent 通道端口 |
| `token` | 空 | 注册令牌（与服务端 `CTF_TOKEN` 一致） |
| `use_tls` | `1` | TLS 加密连接（0=明文，仅调试用） |
| `server_fingerprint` | 空 | 证书 SHA-256 指纹（pinning 防 MITM） |
| `tls_skip_verify` | `0` | 完全不校验证书（不推荐） |
| `discover_enable` | `1` | UDP 广播发现 |
| `discover_port` | `9001` | 发现广播端口 |
| `metrics_interval` | `30` | 指标采集周期（秒） |
| `heartbeat_interval` | `10` | 心跳周期（秒） |
| `log_level` | `1` | 0=debug 1=info 2=warn 3=error |

---

## 从旧服务迁移

如果之前运行着旧版监控服务（占用 8080/9000/9001/2222 端口），请先关闭再启动 CTForge：

```bash
# 1. 停止旧服务（根据实际服务名调整）
sudo systemctl stop legacy-monitor-server  # systemd 方式
docker compose -f /path/to/old/docker-compose.yml down  # Docker 方式

# 2. 确认端口已释放
ss -tlnp | grep -E ':8080|:9000|:2222' || echo "端口已释放"

# 3. 启动 CTForge（任选上述三种方式之一）
docker compose up -d --build
```

---

## 目录结构

```
ctforge-web/
├── docker-compose.yml   # Docker Compose 部署（推荐）
├── Makefile             # 顶层构建入口
├── README.md
├── doc/                 # 研发文档
│   └── README.md
└── src/
    ├── server/          # Go 服务端
    │   ├── cmd/server/  # 入口
    │   ├── internal/    # 20 个内部模块
    │   └── Dockerfile   # 多阶段构建镜像
    ├── clients/         # C Agent（采集 + 命令执行 + ctforge 桥接）
    │   ├── src/ctforge.c # eBPF 桥接模块
    │   └── include/
    ├── frontend/        # Vue3 Web 控制台
    ├── ctforged/        # eBPF 守护进程
    ├── ctforgectl/      # 本地管理 CLI
    ├── payload/         # eBPF payload 模块
    ├── key/             # SM2 密钥生成
    ├── configs/         # 配置文件
    └── scripts/         # systemd service 文件
```

## 部署架构

```
浏览器 ──HTTPS──► ctforge-server ──TCP/TLS──► ctforge-agent ──JSON-RPC──► ctforged(eBPF)
                      │
                      ├─ :8080  Web 控制台 + REST API + WebSocket
                      ├─ :9000  Agent 控制通道（TLS）
                      ├─ :9001  UDP 局域网发现
                      └─ :2222  SSH 跳板机（可选）
```

## License

MIT
