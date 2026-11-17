# CTForge 使用手册

> 面向**运维/使用方**的完整手册。研发/架构细节请看 [研发文档](README.md)。
> 最后更新：2026-11-17，对应 master @ `5fcc785` 之前的修订版（含本仓库 Makefile 修复）

---

## 目录

1. [TL;DR — 5 分钟跑起来](#1-tldr--5-分钟跑起来)
2. [环境依赖矩阵](#2-环境依赖矩阵)
3. [三种拉起方式](#3-三种拉起方式)
   - 3.1 [Docker Compose（推荐）](#31-docker-compose-推荐生产首选)
   - 3.2 [裸机直接运行（开发调试）](#32-裸机直接运行开发调试)
   - 3.3 [systemd 服务（生产服务器）](#33-systemd-服务生产服务器)
4. [首次登录与账户](#4-首次登录与账户)
5. [纳管主机（Agent 接入）](#5-纳管主机agent-接入)
6. [Web 控制台功能地图](#6-web-控制台功能地图)
7. [RBAC 权限模型](#7-rbac-权限模型)
8. [告警与通知](#8-告警与通知)
9. [SSH 跳板机](#9-ssh-跳板机)
10. [定时任务](#10-定时任务)
11. [AI 智能分析](#11-ai-智能分析)
12. [CTForge eBPF 管理](#12-ctforge-ebpf-管理)
13. [服务端 / Agent 配置参考](#13-服务端--agent-配置参考)
14. [备份 / 升级 / 排障](#14-备份--升级--排障)
15. [FAQ](#15-faq)

---

## 1. TL;DR — 5 分钟跑起来

前提：Linux 主机，已装 Docker 20.10+ / Docker Compose v2，宿主机有 Node.js ≥ 22.12 与 Go ≥ 1.26。

```bash
git clone <repo-url> ctforge-web && cd ctforge-web

# 1. 构建宿主机产物（compose 挂载这些文件，不构建会跑旧版/404）
cd src/frontend && npm install && npm run build && cd ../..
cd src/server   && CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o bin/ctforge-server ./cmd/server && cd ../..

# 2. （推荐）配置 .env
cat > .env <<'EOF'
CTF_TOKEN=请换成强随机串
CTF_SSH=:2222
CTF_DISCOVER=9001
EOF

# 3. 启动
docker compose up -d

# 4. 验证
curl http://localhost:8080/api/health   # 期望 {"code":0,...,"status":"up"}
open http://localhost:8080              # 默认 admin / admin123，首次登录强制改密
```

---

## 2. 环境依赖矩阵

| 用途 | 必需 | 推荐 / 可选 |
|------|------|------------|
| **Docker Compose 部署** | Docker 20.10+，Compose v2 | Node.js ≥ 22.12（前端构建），Go ≥ 1.26（服务端构建） |
| **裸机运行服务端** | Go ≥ 1.26，Node.js ≥ 22.12（前端构建） | gcc + OpenSSL（构建 agent）；`docker`（可选，仅用来跑 compose） |
| **构建 C agent** | gcc，OpenSSL 头文件（`openssl-devel` / `libssl-dev`） | `build-static.sh` 编译的静态 OpenSSL（产物零依赖） |
| **构建 ctforged (eBPF)** | clang，libbpf，libelf，libuv，json-c，openssl，liblzma（开发包） | bpftool（部分 payload 签名验证用） |
| **前端开发** | Node.js ≥ 22.12，npm | pnpm/yarn 也可以 |
| **被纳管主机 (Linux)** | glibc，systemd（或 init），curl/wget | OpenSSL 运行时（若 agent 是动态链接） |

Fedora/RHEL 一键装构建依赖：
```bash
sudo dnf install -y golang nodejs gcc openssl-devel clang libbpf-devel \
     elfutils-libelf-devel libuv-devel json-c-devel xz-devel
```

Debian/Ubuntu：
```bash
sudo apt update && sudo apt install -y golang nodejs gcc libssl-dev clang \
     libbpf-dev libelf-dev libuv1-dev libjson-c-dev liblzma-dev make
```

---

## 3. 三种拉起方式

### 3.1 Docker Compose（推荐，生产首选）

> 本项目 `docker-compose.yml` 使用「宿主机产物挂载」模式：`src/frontend/dist`、`src/server/bin/ctforge-server`、`src/server/bin` 被只读挂进容器。
> 好处：改代码 `docker compose restart` 就生效，不用重打镜像。
> **代价：必须先在本机构建这些产物**。直接 `docker compose up -d` 看到的是旧版本（甚至 404）。

```bash
cd ctforge-web

# (1) 构建前端产物
cd src/frontend && npm install && npm run build && cd ../..

# (2) 构建服务端（必须 CGO_ENABLED=0，容器是 alpine/musl）
cd src/server && CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" \
     -o bin/ctforge-server ./cmd/server && cd ../..

# (3) （可选但推荐）构建 agent + ctforged 二进制，供 /download/* 与一键脚本使用
#     agent：默认找 /tmp/openssl-3.0.15 静态库，没有会自动回退到系统 OpenSSL（产物是动态链接）
make -C src/clients
cp src/clients/ctforge-agent src/server/bin/ctforge-agent-amd64

# ctforged 需要 SM2/ECDSA 头文件；首次需生成 key
mkdir -p src/key/CTFORGED
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out src/key/CTFORGED/private.pem
openssl pkey -in src/key/CTFORGED/private.pem -pubout -out src/key/CTFORGED/public.pem
cd src/key && gcc -O2 -std=c99 -o user_sm2_test user_sm2_test.c -lssl -lcrypto && ./user_sm2_test
cp sm2_signature.h CTFORGED/ && cd ../../ctforged && make && cd ..
cp src/ctforged/ctforged src/server/bin/ctforged-amd64

# (4) .env
cat > .env <<'EOF'
CTF_TOKEN=强随机串
CTF_SSH=:2222
CTF_DISCOVER=9001
EOF

# (5) 启动
docker compose up -d
docker compose ps          # 期望 Up (healthy)
docker compose logs -f ctforge-server

# (6) 访问
# HTTP  : http://<host>:8080
# HTTPS : https://<host>:9443（自签证书）
# 默认   : admin / admin123（首次登录强制改密）
```

**端口说明：**

| 端口 | 协议 | 用途 | compose 是否映射 |
|------|------|------|-----------------|
| 8080 | TCP | Web 控制台 + REST API + WebSocket | ✅ |
| 9443 | TCP | Web 控制台 HTTPS（自签证书） | ✅ |
| 9000 | TCP | Agent 控制通道（TLS 1.2+） | ✅ |
| 9001 | UDP | 局域网发现广播 | ✅ |
| 2222 | TCP | SSH 跳板机 | ✅（已加上，2026-11-17） |

### 3.2 裸机直接运行（开发调试）

适合开发、调试、不想用 Docker 的场景。会同时跑前后端，监听本机端口。

```bash
cd ctforge-web

# 1. 一键构建全部组件（前端、服务端、agent、ctforged、ctforgectl）
make all

# 2. 准备运行目录
sudo mkdir -p /var/lib/ctforge /etc/ctforge

# 3. 直接前台跑（Ctrl+C 退出）
sudo ./src/server/bin/ctforge-server \
  -http     :8080 \
  -https    :9443 \
  -sock     :9000 \
  -static   src/frontend/dist \
  -data     /var/lib/ctforge \
  -accounts /var/lib/ctforge/accounts.json \
  -tls-cert /etc/ctforge/server.crt \
  -tls-key  /etc/ctforge/server.key \
  -agent-bin src/server/bin/ctforge-agent-amd64

# 或使用环境变量（与 flag 等效）
sudo CTF_HTTP=:8080 CTF_HTTPS=:9443 CTF_SOCK=:9000 \
     CTF_STATIC=src/frontend/dist \
     CTF_DATA=/var/lib/ctforge \
     ./src/server/bin/ctforge-server
```

注意：
- 默认会读取 `.env` 吗？**不会**，`.env` 只被 `docker-compose.yml` 读取。裸机跑要么传 flag，要么 `export CTF_*=...`。
- agent 二进制路径：默认服务端在 `CTF_AGENT_BIN_DIR`（默认 `bin/`）下找 `ctforge-agent-amd64/arm64`。`make all` 后已拷到 `src/server/bin/`，请把当前目录设为这个路径，或用 `-agent-bin` 直接指定绝对路径。

### 3.3 systemd 服务（生产服务器）

适合：要开机自启、崩溃自动重启、统一日志（journald）。

**服务端 unit：**

```bash
# 1. 构建并安装（PREFIX 默认 /usr/local）
make all && sudo make install

# 2. 创建运行用户与目录
sudo useradd -r -s /usr/sbin/nologin ctforge 2>/dev/null || true
sudo mkdir -p /var/lib/ctforge /etc/ctforge /usr/local/share/ctforge/static
sudo chown -R ctforge:ctforge /var/lib/ctforge /etc/ctforge

# 3. 拷贝前端产物
sudo cp -r src/frontend/dist/* /usr/local/share/ctforge/static/

# 4. 写 unit
sudo tee /etc/systemd/system/ctforge-server.service <<'EOF'
[Unit]
Description=CTForge Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=ctforge
Group=ctforge
ExecStart=/usr/local/bin/ctforge-server \
  -http :8080 -https :9443 -sock :9000 \
  -static /usr/local/share/ctforge/static \
  -data /var/lib/ctforge \
  -accounts /var/lib/ctforge/accounts.json \
  -tls-cert /etc/ctforge/server.crt \
  -tls-key /etc/ctforge/server.key \
  -agent-bin /usr/local/bin/ctforge-agent
Restart=always
RestartSec=10
LimitNOFILE=65536
NoNewPrivileges=true
ProtectSystem=strict
ReadWritePaths=/var/lib/ctforge /etc/ctforge

[Install]
WantedBy=multi-user.target
EOF

# 5. 启用并启动
sudo systemctl daemon-reload
sudo systemctl enable --now ctforge-server
sudo systemctl status ctforge-server --no-pager
```

**eBPF 守护进程 unit（可选，仅在需要 eBPF 防护的主机装）：**

```bash
# 在目标主机（被纳管端）执行
cd ctforge-web

# 1. 生成 ECDSA key + 头文件
mkdir -p src/key/CTFORGED
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out src/key/CTFORGED/private.pem
openssl pkey -in src/key/CTFORGED/private.pem -pubout -out src/key/CTFORGED/public.pem
cd src/key && gcc -O2 -std=c99 -o user_sm2_test user_sm2_test.c -lssl -lcrypto && ./user_sm2_test
cp sm2_signature.h CTFORGED/ && cd ../..

# 2. 构建并安装
make -C src/ctforged install      # 装 ctforged + ctforged.service 到 /etc/systemd/system/
make -C src/ctforgectl install    # 装 ctforgectl

# 3. 启用
sudo systemctl daemon-reload
sudo systemctl enable --now ctforged
sudo systemctl status ctforged --no-pager
sudo journalctl -u ctforged -f
```

---

## 4. 首次登录与账户

| 项 | 值 |
|----|---|
| 默认账号 | `admin` |
| 默认密码 | `admin123` |
| 首次登录 | **强制跳改密页**（账户 `MustChange=true`） |

**改密路径：** 右上角头像 → 个人中心 / 修改密码 → 输入旧密码 + 新密码 → 提交后 `MustChange` 自动清除。

**新增账户：** 安全与管理 → 账户管理 → 新增账户。新账户同样默认 `MustChange=true`。

**角色速查：**

| 角色 | 能做什么 |
|------|----------|
| viewer | 看主机、指标、日志、告警；不能改 |
| operator | viewer + 批准主机、执行命令、远程终端、文件、处理告警、端口转发 |
| admin | 全部权限（账户、通知、AI、审计、系统设置） |

更细粒度的机器级授权见 [第 7 节](#7-rbac-权限模型)。

---

## 5. 纳管主机（Agent 接入）

### 5.1 一键脚本（推荐）

Web 控制台 → 主机列表 → 「部署 Agent」生成命令，到目标主机以 root 执行：

```bash
curl -fsSL http://<server-ip>:8080/api/deploy/script.sh | sudo sh
```

脚本会：下载匹配架构的 agent → 写 `/etc/ctforge/agent.conf` → systemd 托管 → （可选）装 ctforged。

参数：
- `--ctforged-only`：只装 ctforged，不装 agent
- `--force-agent`：强制重装 agent（升级场景）

### 5.2 手动安装

```bash
curl -fsSL 'http://<server-ip>:8080/download/agent?arch=amd64' -o /usr/local/bin/ctforge-agent
chmod +x /usr/local/bin/ctforge-agent

sudo mkdir -p /etc/ctforge
sudo tee /etc/ctforge/agent.conf <<'EOF'
server_host=<server-ip>
server_port=9000
token=<与服务端 CTF_TOKEN 一致>
use_tls=1
metrics_interval=30
heartbeat_interval=10
log_level=1
EOF

sudo tee /etc/systemd/system/ctforge-agent.service <<'EOF'
[Unit]
Description=CTForge Agent
After=network-online.target

[Service]
Type=simple
ExecStart=/usr/local/bin/ctforge-agent -c /etc/ctforge/agent.conf
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload && sudo systemctl enable --now ctforge-agent
```

### 5.3 批准接入

Agent 握手后处于 **pending** 状态。路径：**安全与管理 → 批准管理** → 选主机 → 批准 / 拒绝。批准后 agent 才上报指标，且与当前 `server_id` 绑定。

### 5.4 局域网自动发现

- 服务端：UDP 9001 广播
- Agent：`discover_enable=1` 时自动尝试握手
- 关闭：服务端 `CTF_DISCOVER=0`，或 agent `discover_enable=0`

### 5.5 心跳与指标

- 心跳：每 `heartbeat_interval`（默认 10s）
- 指标：每 `metrics_interval`（默认 30s）
- 离线判定：连续 3 次心跳缺失 → 标记离线 + 触发离线告警

---

## 6. Web 控制台功能地图

| 一级菜单 | 二级页面 | 主要功能 |
|---------|----------|----------|
| **主机监控** | 主机列表 | 在线状态、CPU/内存/磁盘、OS、心跳；备注、卸载 |
| | 主机总览 | 全局健康度、告警、Top N |
| | 硬件信息 | CPU/主板/内存/磁盘/网卡 |
| | 网络监控 | 网卡速率、连接状态、TCP 状态 |
| | 日志分析 | 关键字检索、异常聚类 |
| | CTForge 管理 | eBPF payload 加载/卸载/状态（详见第 12 节） |
| | 系统日志 | syslog/journal 查看 |
| **运维操作** | 远程终端 | xterm.js 全双工、持久会话、多标签 |
| | 文件管理 | 浏览/上传/下载/删除/重命名/新建目录 |
| | 系统服务 | systemd unit 启停/自启 |
| | 软件包 | 检查更新、升级 |
| | Docker | 容器/镜像列表、启停、日志 |
| | 端口转发 | TCP 端口转发 |
| | 系统账户 | 用户增删改密（仅 admin） |
| | 命令执行 | 批量下发命令，stdout/stderr/exit/耗时 |
| | 客户端配置 | 在线修改 agent 配置并下发 |
| **安全与管理** | 安全审计 | 基线检查：弱密码、可疑 SSH key、高危端口 |
| | 告警中心 | 当前/历史告警、处理标记 |
| | 定时任务 | cron 批量执行、执行历史 |
| | 统计视图 | CPU/内存/磁盘/流量趋势 |
| | 网络拓扑 | 可视化主机连接 |
| | 批准管理 | 待批准主机 |
| | 系统设置 | 服务端显示名等 |
| | 分组与授权 | 机器分组 + 细粒度授权（见第 7 节） |
| | 账户管理 | 控制台用户 CRUD |
| | 通知配置 | 邮件/Webhook 渠道、告警规则、AI 配置 |
| | 操作审计 | 谁在何时对哪台主机做了什么 |

---

## 7. RBAC 权限模型

三层结构：
```
账户角色 (admin/operator/viewer)
        │
功能权限点 (26 个，agent.terminal / system.alert ...)
        │
机器范围 (全部 / 分组 / 指定主机)
```

**26 个权限点（节选）：**

| 分组 | 权限点 |
|------|--------|
| 只读 | `agent.view` `agent.metrics` `agent.logs` `agent.syslog` |
| 运维 | `agent.terminal` `agent.file.view` `agent.file.edit` `agent.cmd.exec` `agent.service` `agent.pkg` `agent.sysuser` `agent.security` `agent.portfwd` `agent.upgrade` `agent.uninstall` |
| 管理 | `system.deploy` `system.approval` `system.task` `system.alert` `system.notify` `system.accounts` `system.audit` `system.stats` `system.topology` `system.ai` `system.group` |
| 超管 | `system.all`（所有权限并集） |

**典型场景：** 给运维小张在 `db-prod` 分组 3 台主机上授予终端权限，有效期 30 天。
路径：分组与授权 → 新建分组（加入主机）→ 新建授权（用户/权限点/范围=分组/有效期）。

---

## 8. 告警与通知

### 8.1 默认告警规则

| 告警 | 默认阈值 |
|------|----------|
| 主机离线 | 连续 3 次心跳缺失 |
| CPU 过高 | > 90% 持续 5min |
| 内存过高 | > 90% 持续 5min |
| 磁盘过高 | > 85%（任一挂载点） |
| 流量异常 | 突增 5 倍基线 |
| SSH 爆破 | 1min 内 5 次失败 |

阈值在控制台 **通知配置 → 告警规则**，或服务端 `rules.json`（`-data` 目录）中调整。

### 8.2 通知渠道

| 渠道 | 必填字段 |
|------|----------|
| email | SMTP 主机、端口、账号、密码、收件人 |
| webhook | URL（企业微信/钉钉/飞书/自定义） |

保存后点 **测试** 发送测试消息。

### 8.3 告警生命周期

```
触发 → 未处理 → 已确认（人工） → 自动恢复（条件消失）
```

---

## 9. SSH 跳板机

服务端监听 `:2222`（compose 已映射）。用法：

```bash
ssh <控制台用户名>@<server-ip> -p 2222
# 密码 = 控制台密码；或先在 个人中心 上传 SSH 公钥
```

登录后会列出主机菜单，输入序号直达。

**关闭跳板机：** 服务端 `CTF_SSH=""` 或 `-ssh ""`，重启生效。

---

## 10. 定时任务

路径：**安全与管理 → 定时任务 → 新增任务**

| 字段 | 示例 |
|------|------|
| 任务名 | `daily-cleanup` |
| cron | `0 3 * * *`（标准 5 段） |
| 目标 | 选主机或分组 |
| 命令 | `docker system prune -af`（支持多行 shell） |

**执行历史：** 任务列表 → 点击任务 → 历史记录，含每台主机 stdout/stderr/exit。

---

## 11. AI 智能分析

### 11.1 配置

路径：**通知配置 → AI 配置**

```json
{
  "enabled": true,
  "base_url": "https://api.openai.com/v1",
  "api_key": "sk-...",
  "model": "gpt-4o-mini",
  "max_tokens": 4096
}
```

任何 OpenAI 兼容端点均可（DeepSeek、Moonshot、自建 vLLM/ollama）。

### 11.2 触发

主机详情页 → **AI 分析**。返回：一句话结论 / 异常项列表 / 优化建议。结果缓存 5min。

---

## 12. CTForge eBPF 管理

### 12.1 前置条件

目标主机需运行 `ctforged`（默认监听 `127.0.0.1:7000`）。安装步骤见 [3.3 节](#33-systemd-服务生产服务器)。

### 12.2 原理

```
浏览器 ──HTTP──► ctforge-server ──MSG_CTF_*──► ctforge-agent ──JSON-RPC──► ctforged
```

agent 内置桥接模块，从 `/etc/ctforge/ctforge.cfg` 读 `listen`，把服务端指令翻译成 JSON-RPC 发给本地 `ctforged`。**方法白名单**：`lsmod/insmod/rmmod/mod/status`。

### 12.3 Web 操作

路径：**主机监控 → CTForge 管理**
- 守护进程状态卡片
- 已加载模块表格（可卸载）
- 加载模块表单
- 子命令执行表单

### 12.4 API 调用

```bash
TOKEN=***
H="Authorization: Bearer ***"
B="http://<server>:8080"
ID=<agent-id>

curl -X POST -H "$H" $B/api/agents/$ID/ctforge/status
curl -X POST -H "$H" $B/api/agents/$ID/ctforge/lsmod
curl -X POST -H "$H" -H 'Content-Type: application/json' -d '{"module":"kprobe_exec"}' $B/api/agents/$ID/ctforge/insmod
curl -X POST -H "$H" -H 'Content-Type: application/json' -d '{"module":"kprobe_exec"}' $B/api/agents/$ID/ctforge/rmmod
```

---

## 13. 服务端 / Agent 配置参考

### 13.1 服务端启动参数

| flag | 环境变量 | 默认 | 说明 |
|------|----------|------|------|
| `-http` | `CTF_HTTP` | `:8080` | HTTP 监听 |
| `-https` | `CTF_HTTPS` | `:9443` | HTTPS（空=关闭） |
| `-sock` | `CTF_SOCK` | `:9000` | Agent 通道 |
| `-static` | `CTF_STATIC` | 空 | 前端 dist 目录 |
| `-token` | `CTF_TOKEN` | 空 | Agent 注册令牌 |
| `-tls-cert` | `CTF_TLS_CERT` | `server.crt` | TLS 证书 |
| `-tls-key` | `CTF_TLS_KEY` | `server.key` | TLS 私钥 |
| `-tls-san` | `CTF_TLS_SAN` | 空 | 额外 SAN |
| `-no-tls` | `CTF_NO_TLS=1` | 关 | 关闭 Agent TLS（仅调试） |
| `-accounts` | `CTF_ACCOUNTS` | `accounts.json` | 账户文件 |
| `-data` | `CTF_DATA` | `.` | 运行时数据目录 |
| `-name` | `CTF_NAME` | 主机名 | 显示名 |
| `-ssh` | `CTF_SSH` | `:2222` | SSH 跳板（空=关闭） |
| `-discover` | `CTF_DISCOVER` | `9001` | UDP 发现（0=关闭） |
| `-agent-bin` | `CTF_AGENT_BIN` | 空 | `/download/agent` 路径 |

### 13.2 Agent 配置 `/etc/ctforge/agent.conf`

| 配置项 | 默认 | 说明 |
|--------|------|------|
| `server_host` | `127.0.0.1` | 服务端地址 |
| `server_port` | `9000` | 服务端 Agent 通道端口 |
| `token` | 空 | 注册令牌 |
| `agent_id` | 主机名 | 唯一标识 |
| `name` | 主机名 | 显示名 |
| `use_tls` | `1` | TLS（0=明文） |
| `server_fingerprint` | 空 | 证书 SHA-256 指纹（pinning） |
| `tls_skip_verify` | `0` | 完全不校验证书 |
| `discover_enable` | `1` | UDP 发现 |
| `discover_port` | `9001` | 发现端口 |
| `metrics_interval` | `30` | 指标周期（秒） |
| `heartbeat_interval` | `10` | 心跳周期（秒） |
| `log_level` | `1` | 0=debug 1=info 2=warn 3=error |

---

## 14. 备份 / 升级 / 排障

### 14.1 备份

```bash
# Docker 卷
docker run --rm -v ctforge-web_ctforge-data:/data -v $(pwd):/backup alpine \
  tar czf /backup/ctforge-data-$(date +%F).tar.gz -C /data .
docker run --rm -v ctforge-web_ctforge-certs:/data -v $(pwd):/backup alpine \
  tar czf /backup/ctforge-certs-$(date +%F).tar.gz -C /data .

# 裸机
sudo tar czf ctforge-backup-$(date +%F).tar.gz /var/lib/ctforge /etc/ctforge
```

### 14.2 升级

```bash
git pull
make all                 # 重新构建所有产物
docker compose restart   # compose 部署（产物已挂载进容器）
# 或
sudo systemctl restart ctforge-server   # systemd 部署
```

升级 agent：到每台目标主机执行 `curl -fsSL http://<server>:8080/api/deploy/script.sh | sudo sh -s -- --force-agent`。

### 14.3 健康检查

```bash
curl http://localhost:8080/api/health
# 期望 {"code":0,"data":{"status":"up","time":...},"message":"success"}
```

### 14.4 日志

```bash
# Docker
docker compose logs -f ctforge-server --tail=200

# systemd
sudo journalctl -u ctforge-server -f --since today

# Agent（被管主机）
sudo journalctl -u ctforge-agent -f
```

---

## 15. FAQ

**Q1：改了前端代码，容器里还是旧页面？**
A：`docker-compose.yml` 把 `src/frontend/dist` 挂载进容器。改完前端必须 `npm run build`，然后 `docker compose restart ctforge-server`。

**Q2：Agent 一直「待批准」，但批准管理里看不到？**
A：看 agent 日志 `journalctl -u ctforge-agent -f`：
- `pending approval from administrator` → 等待批准
- `connection refused` → 服务端 9000 不通，查防火墙
- `bad token` → 服务端 `CTF_TOKEN` 与 agent `token` 不一致
- `certificate verify failed` → `server_fingerprint` 不匹配

**Q3：HTTPS 9443 报证书错误？**
A：服务端用自签证书。生产可：① 把 `/etc/ctforge/server.crt` 换成正式证书；② 或前置 nginx/caddy 反代。

**Q4：忘记 admin 密码？**
A：删账户文件重启即重建 `admin/admin123`：
```bash
docker compose stop ctforge-server
docker run --rm -v ctforge-web_ctforge-data:/data alpine sh -c 'rm -f /data/accounts.json'
docker compose start ctforge-server
```

**Q5：远程终端连不上 / 闪断？**
A：确认目标主机有 `/bin/sh` 或 `/bin/bash`；确认 8080 可达（WebSocket 走 8080）；容器 NAT 环境下 agent 需能直连服务端 IP。

**Q6：SSH 跳板连不上？**
A：compose 已映射 2222；确认 `CTF_SSH` 配置、防火墙放行；登录账号必须是控制台已存在账户。

**Q7：一台主机能同时被多个服务端管吗？**
A：不能。批准后 agent 与 `server_id` 绑定。要切服务端需先卸载 agent（或删 `/etc/ctforge/server_id`）再重装。

**Q8：Agent 能装在 Windows / macOS 吗？**
A：不能。当前 agent 是 C 写的 Linux-only，依赖 `/proc`、systemd、glibc。

**Q9：eBPF 功能加载失败？**
A：确认内核 ≥ 4.19 且开启 `CONFIG_BPF`/`CONFIG_BPF_SYSCALL`；查 `journalctl -u ctforged`；SM2/ECDSA 密钥对必须先生成（见 3.3）。

**Q10：生产环境加固清单？**
A：① 强 `CTF_TOKEN`；② admin 强密码；③ 用 HTTPS 9443；④ agent 配 `server_fingerprint`；⑤ 防火墙只放必需端口；⑥ 定期备份 `ctforge-data` 卷。

**Q11：`make all` 报 `libssl.a: No such file or directory`？**
A：`src/clients/Makefile` 默认找 `/tmp/openssl-3.0.15` 下的静态 OpenSSL。没有会自动回退到系统 OpenSSL（产物动态链接，目标机需装 `libssl`）。要完全静态，先跑 `src/clients/build-static.sh` 编译静态 OpenSSL。

**Q12：`make ctforged` 报 `sm2_signature.h: No such file or directory`？**
A：需要先按 3.3 节生成 ECDSA key 并跑 `src/key/user_sm2_test` 生成头文件，再 `make ctforged`。

---

**维护提示：** 本文档应与代码同步更新；改动部署流程、端口、依赖时请先更新本节。
