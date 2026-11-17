# CTForge 使用手册

本手册面向**运维用户**，覆盖从安装部署、首次登录、纳管主机，到日常运维操作的完整流程。研发/架构细节请参考 [研发文档](README.md)。

---

## 目录

1. [快速上手（5 分钟）](#1-快速上手5-分钟)
2. [安装部署](#2-安装部署)
3. [首次登录与账户管理](#3-首次登录与账户管理)
4. [纳管主机（Agent 接入）](#4-纳管主机agent-接入)
5. [Web 控制台功能详解](#5-web-控制台功能详解)
6. [RBAC 权限模型](#6-rbac-权限模型)
7. [告警与通知](#7-告警与通知)
8. [SSH 跳板机](#8-ssh-跳板机)
9. [定时任务](#9-定时任务)
10. [AI 智能分析](#10-ai-智能分析)
11. [CTForge eBPF 管理](#11-ctforge-ebpf-管理)
12. [服务端与 Agent 配置参考](#12-服务端与-agent-配置参考)
13. [日常运维（备份/升级/排障）](#13-日常运维备份升级排障)
14. [FAQ 常见问题](#14-faq-常见问题)

---

## 1. 快速上手（5 分钟）

前提：一台装了 Docker 的 Linux 主机。

```bash
# 1. 进入项目
cd ctforge-web

# 2. 构建前端产物（compose 会把 dist 挂载进容器）
cd src/frontend && npm install && npm run build && cd ../..

# 3. 构建服务端二进制（compose 挂载此文件，免重建镜像）
cd src/server && go build -o bin/ctforge-server ./cmd/server && cd ../..

# 4. 启动
docker compose up -d

# 5. 打开浏览器
#    http://<主机IP>:8080   （HTTP）
#    https://<主机IP>:9443  （HTTPS，自签证书）
# 默认账号：admin / admin123 —— 登录后系统会强制要求修改密码
```

> 💡 为什么先 `npm run build` 和 `go build`？
> `docker-compose.yml` 为了支持"宿主机改代码 → 容器内直接生效"，把 `src/frontend/dist` 和 `src/server/bin/ctforge-server` 以只读卷方式挂进容器。宿主机构建产物不新鲜时，容器里跑的就是旧版本。

---

## 2. 安装部署

### 2.1 三种部署方式对比

| 方式 | 适用场景 | 启动命令 |
|------|----------|----------|
| **Docker Compose**（推荐） | 大多数环境 | `docker compose up -d` |
| **裸机运行** | 开发调试、不想用 Docker | `ctforge-server -http :8080 ...` |
| **systemd** | 生产服务器，需开机自启 | `systemctl enable --now ctforge-server` |

### 2.2 Docker Compose 部署（推荐）

```bash
# 进入项目根目录
cd ctforge-web

# （推荐）创建 .env 文件配置注册令牌，避免任何 agent 都能接入
cat > .env << 'EOF'
# Agent 注册令牌，agent 配置里要填同样的值；生产环境必配
CTF_TOKEN=change-me-to-a-long-random-string
# SSH 跳板机端口（默认 :2222，填 0 或空关闭）
CTF_SSH=:2222
# UDP 局域网发现端口（默认 9001，0=关闭）
CTF_DISCOVER=9001
# 额外证书 SAN，逗号分隔（例：10.0.0.5,ct.example.com）
CTF_TLS_SAN=
EOF

# 构建并启动
docker compose up -d

# 查看状态（healthy 即就绪）
docker compose ps
docker compose logs -f ctforge-server
```

**端口说明：**

| 端口 | 协议 | 用途 | 是否必须 |
|------|------|------|----------|
| `8080` | TCP | Web 控制台（HTTP）+ REST API + WebSocket | 必须 |
| `9443` | TCP | Web 控制台（HTTPS，自签证书） | 推荐 |
| `9000` | TCP | Agent 控制通道（TLS 1.2+） | 必须 |
| `9001` | UDP | 局域网自动发现广播 | 可选 |
| `2222` | TCP | SSH 跳板机 | 可选 |

**持久化数据：**

| 卷 | 内容 |
|----|------|
| `ctforge-data` | 账户、定时任务、告警规则、server_id |
| `ctforge-certs` | TLS 证书与私钥（首次启动自动生成） |

### 2.3 裸机运行（开发/调试）

依赖：Go ≥ 1.26、Node.js ≥ 22.12、gcc、OpenSSL 开发头文件。

```bash
# 1. 构建全部组件
make all

# 2. 准备运行目录
sudo mkdir -p /var/lib/ctforge /etc/ctforge

# 3. 启动服务端
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
```

### 2.4 systemd 部署（生产推荐）

```bash
# 1. 构建并安装到 /usr/local/bin
make all && sudo make install

# 2. 创建运行用户
sudo useradd -r -s /usr/sbin/nologin ctforge 2>/dev/null || true
sudo mkdir -p /var/lib/ctforge /etc/ctforge
sudo chown -R ctforge:ctforge /var/lib/ctforge /etc/ctforge

# 3. 拷贝前端静态文件
sudo mkdir -p /usr/local/share/ctforge/static
sudo cp -r src/frontend/dist/* /usr/local/share/ctforge/static/

# 4. 写入 unit 文件
sudo tee /etc/systemd/system/ctforge-server.service << 'EOF'
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
  -tls-key /etc/ctforge/server.key
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
sudo systemctl status ctforge-server
```

### 2.5 防火墙放行

```bash
# firewalld（Fedora/RHEL/CentOS）
sudo firewall-cmd --permanent --add-port={8080,9443,9000,2222}/tcp --add-port=9001/udp
sudo firewall-cmd --reload

# ufw（Debian/Ubuntu）
sudo ufw allow 8080,9443,9000,2222/tcp
sudo ufw allow 9001/udp
```

---

## 3. 首次登录与账户管理

### 3.1 默认账户

| 项 | 值 |
|----|---|
| 默认账号 | `admin` |
| 默认密码 | `admin123` |
| 首次登录 | 系统会强制跳转到修改密码页面（`must_change` 标记） |

**修改密码流程：** 登录后点击右上角头像 → **个人中心 / 修改密码**，输入旧密码和新密码。服务端校验通过后 `MustChange` 标记自动清除。

### 3.2 添加其他账户

路径：**安全与管理 → 账户管理 → 新增账户**

- 角色三选一：`admin`（超管）/ `operator`（运维）/ `viewer`（只读）
- 新账户默认带 `must_change` 标记，首次登录同样强制改密
- 删除账户不影响已下发的授权与历史审计记录

### 3.3 角色权限速查

| 角色 | 能做什么 |
|------|----------|
| `viewer` | 看主机、看指标、看告警、看日志；不能改任何东西 |
| `operator` | 在 `viewer` 基础上：批准/拒绝主机、执行命令、管文件、远程终端、处理告警、配置端口转发 |
| `admin` | 全部权限，包括账户管理、通知配置、AI 配置、审计日志 |

更细粒度的机器级授权见 [第 6 节 RBAC 权限模型](#6-rbac-权限模型)。

---

## 4. 纳管主机（Agent 接入）

### 4.1 方式 A：一键脚本（推荐）

Web 控制台 → **主机列表 → 部署 Agent**，复制生成的命令到目标主机以 root 执行：

```bash
curl -fsSL http://<server-ip>:8080/api/deploy/script.sh | sudo sh
```

脚本会自动完成：
1. 下载与目标主机架构匹配的 agent 二进制
2. 写入 `/etc/ctforge/agent.conf`
3. 安装并启动 systemd service
4. （可选）同时安装 `ctforged` eBPF 守护进程

脚本支持的可选参数：

| 参数 | 作用 |
|------|------|
| `--ctforged-only` | 只装 ctforged，不装 agent |
| `--force-agent` | 强制重装 agent 二进制（用于升级） |

### 4.2 方式 B：手动安装

```bash
# 1. 下载 agent（amd64 / arm64）
curl -fsSL 'http://<server-ip>:8080/download/agent?arch=amd64' -o /usr/local/bin/ctforge-agent
chmod +x /usr/local/bin/ctforge-agent

# 2. 写入配置
sudo mkdir -p /etc/ctforge
sudo tee /etc/ctforge/agent.conf << 'EOF'
server_host=<server-ip>
server_port=9000
token=<与服务端 CTF_TOKEN 一致>
use_tls=1
metrics_interval=30
heartbeat_interval=10
log_level=1
EOF

# 3. systemd 托管
sudo tee /etc/systemd/system/ctforge-agent.service << 'EOF'
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

sudo systemctl daemon-reload
sudo systemctl enable --now ctforge-agent
```

### 4.3 批准接入

出于安全考虑，agent 首次握手后处于 **pending**（待批准）状态：

1. 控制台左侧 **安全与管理 → 批准管理**
2. 找到目标主机 → 点 **批准**（或 **拒绝**）
3. 批准后 agent 才会开始上报指标和接受指令
4. 批准后 agent 会与当前 `server_id` 绑定，防止被另一台服务端"劫持"

### 4.4 局域网自动发现

服务端默认在 UDP `9001` 端口广播自身存在。开启了 `discover_enable=1` 的 agent 收到广播后会自动尝试握手，无需手动填服务端 IP。

关闭方式：服务端 `CTF_DISCOVER=0`，或 agent `discover_enable=0`。

### 4.5 在线状态与心跳

- 心跳周期：`heartbeat_interval`（默认 10s）
- 指标采集：`metrics_interval`（默认 30s）
- 超过 3 个心跳周期无响应，控制台标记主机为 **离线**，并可触发离线告警

---

## 5. Web 控制台功能详解

控制台左侧菜单按使用频率分三组：**主机监控**、**运维操作**、**安全与管理**。

### 5.1 主机监控

| 页面 | 功能 |
|------|------|
| **主机列表** | 所有纳管主机一览：在线状态、CPU/内存/磁盘百分比、操作系统、最近心跳；支持备注、卸载 |
| **主机总览** | 全局健康度：在线率、告警数量、Top N 资源占用 |
| **硬件信息** | CPU 型号、主板、内存条、磁盘、网卡等硬件清单 |
| **网络监控** | 各网卡实时收发速率、连接状态分布、TCP 状态统计 |
| **日志分析** | 基于关键字的日志检索与异常聚类 |
| **CTForge 管理** | eBPF payload 模块加载/卸载/状态查询（详见第 11 节） |
| **系统日志** | 查看主机上的 syslog/journal |

### 5.2 运维操作

| 页面 | 功能 |
|------|------|
| **远程终端** | 浏览器内 xterm.js 全双工终端，支持 vim/top 全屏；多标签、持久会话（断开重连回到原 shell） |
| **文件管理** | 目录浏览、上传、下载、删除、重命名、新建目录、权限查看 |
| **系统服务** | systemd unit 启停/重启/开机自启 |
| **软件包** | 检查可更新包、执行升级（自动识别 apt/dnf/yum/pacman） |
| **Docker** | 容器/镜像列表、容器启停、日志查看 |
| **端口转发** | 把被管主机的某端口转发出来，支持 TCP |
| **系统账户** | 增删系统用户、改密码、改 shell（仅 admin） |
| **命令执行** | 一次性下发命令到一台或多台主机，返回 stdout/stderr/退出码/耗时 |
| **客户端配置** | 在线修改 agent 配置并下发 |

### 5.3 安全与管理

| 页面 | 功能 |
|------|------|
| **安全审计** | 主机基线检查：弱密码、可疑 SSH 公钥、特权文件、开启的高危端口 |
| **告警中心** | 当前活跃告警与历史告警，支持手动"处理"标记 |
| **定时任务** | cron 表达式批量执行命令，含执行历史 |
| **统计视图** | CPU/内存/磁盘/流量趋势图，支持时间区间筛选 |
| **网络拓扑** | 可视化各主机的连接关系 |
| **批准管理** | 待批准主机列表（见 4.3） |
| **系统设置** | 服务端显示名、登录超时等 |
| **分组与授权** | 机器分组 + 细粒度权限点下发（见第 6 节） |
| **账户管理** | 控制台用户增删改查 |
| **通知配置** | 邮件/Webhook 通知渠道与告警规则 |
| **操作审计** | 谁在什么时间对哪台主机做了什么 |

---

## 6. RBAC 权限模型

CTForge 的权限由三层组成：

```
账户角色 (admin/operator/viewer)
        │
        ▼
功能权限点 (agent.terminal、agent.file.edit、system.alert ...)
        │
        ▼
机器范围 (全部主机 / 指定分组 / 指定主机)
```

### 6.1 26 个权限点

| 分组 | 权限点 | 说明 |
|------|--------|------|
| 只读 | `agent.view` `agent.metrics` `agent.logs` `agent.syslog` | 看主机、看指标、看日志 |
| 运维 | `agent.terminal` `agent.file.view` `agent.file.edit` `agent.cmd.exec` `agent.service` `agent.pkg` `agent.sysuser` `agent.security` `agent.portfwd` `agent.upgrade` `agent.uninstall` | 日常运维动作 |
| 管理 | `system.deploy` `system.approval` `system.task` `system.alert` `system.notify` `system.accounts` `system.audit` `system.stats` `system.topology` `system.ai` `system.group` | 平台级管理 |
| 超管 | `system.all` | 所有权限的并集 |

### 6.2 典型授权示例

**场景：把"运维组"中 3 台数据库主机的终端权限授给运维小张，有效期 1 个月**

1. **分组与授权 → 新建分组**：名 `db-prod`，把 3 台主机加入
2. **分组与授权 → 新建授权**：
   - 用户名：`xiaozhang`
   - 权限点：勾选 `agent.terminal`、`agent.view`
   - 机器范围：分组 → 选 `db-prod`
   - 有效期：30 天
3. 保存后小张登录只能看到这 3 台主机，且只能用终端功能

授权过期自动失效，无需手动回收。

---

## 7. 告警与通知

### 7.1 告警规则

默认开启的告警项：

| 告警 | 默认阈值 | 说明 |
|------|----------|------|
| 主机离线 | 连续 3 次心跳缺失 | 网络断/agent 死/主机关机 |
| CPU 过高 | > 90% 持续 5 分钟 | |
| 内存过高 | > 90% 持续 5 分钟 | |
| 磁盘过高 | > 85% | 任一挂载点 |
| 流量异常 | 突增 5 倍基线 | |
| SSH 爆破 | 1 分钟内 5 次失败 | 基于 `/var/log/secure` 或 auth.log |

阈值在 `rules.json`（`-data` 目录下）或控制台 **通知配置 → 告警规则** 中调整。

### 7.2 通知渠道

路径：**通知配置 → 渠道管理 → 新增渠道**

| 渠道 | 必填字段 |
|------|----------|
| `email` | SMTP 主机、端口、账号、密码、收件人列表 |
| `webhook` | URL（企业微信/钉钉/飞书机器人均可） |

保存后点 **测试** 按钮发送一条测试消息，确认链路通畅。

### 7.3 告警生命周期

```
触发 → 未处理 → 已确认（人工点处理） → 自动恢复（条件消失）
```

告警一旦触发会记入 `history`，即使后来自动恢复也能在告警中心 → 历史标签页看到。

---

## 8. SSH 跳板机

服务端默认监听 `:2222`，提供一个无需登录 Web 也能 SSH 直达纳管主机的通道。

```bash
# 用法：ssh <控制台用户名>@<服务端IP> -p 2222
ssh admin@<server-ip> -p 2222
# 密码即控制台密码，或先在 个人中心 上传 SSH 公钥走密钥认证

# 登录后会看到主机选择菜单，输入序号直达对应主机
```

**公钥认证：** 控制台右上角头像 → 个人中心 → SSH 公钥 → 粘贴 `~/.ssh/id_rsa.pub` 内容。

**关闭跳板机：** 服务端 `CTF_SSH=""`（或 `-ssh ""`）并重启。

---

## 9. 定时任务

路径：**安全与管理 → 定时任务 → 新增任务**

字段说明：

| 字段 | 例子 | 说明 |
|------|------|------|
| 任务名 | `daily-cleanup` | |
| cron 表达式 | `0 3 * * *` | 每天 03:00（标准 5 段式） |
| 目标主机 | 选主机或分组 | |
| 命令 | `docker system prune -af` | 支持多行 shell |

**查看执行历史：** 任务列表 → 点击任务 → 历史记录，含每台主机的 stdout/stderr/退出码。

---

## 10. AI 智能分析

### 10.1 配置

路径：**通知配置 → AI 配置**（或调用 API `PUT /api/ai/config`）

```json
{
  "enabled": true,
  "base_url": "https://api.openai.com/v1",
  "api_key": "sk-...",
  "model": "gpt-4o-mini",
  "max_tokens": 4096
}
```

任何 OpenAI 兼容端点都可以（DeepSeek、Moonshot、自建 vLLM/ollama 等）。

### 10.2 触发分析

主机详情页 → **AI 分析** 按钮。系统会把最近 5 分钟的指标、告警、日志摘要打包发给 LLM，返回：

- **一句话结论**：主机当前是否健康
- **异常项列表**：具体哪个指标异常
- **优化建议**：可执行的修复命令

分析结果缓存 5 分钟，重复点击返回缓存。

---

## 11. CTForge eBPF 管理

### 11.1 前置条件

目标主机上需运行 `ctforged` 守护进程（默认监听 `127.0.0.1:7000`）。

```bash
# 在目标主机安装 ctforged（编译依赖：clang libbpf libelf libuv json-c openssl liblzma）
cd ctforge-web && sudo make -C src/ctforged install

# 写入配置 /etc/ctforge/ctforge.cfg
sudo tee /etc/ctforge/ctforge.cfg << 'EOF'
{
  "listen": "127.0.0.1:7000",
  "payload_dir": "/usr/share/ctforge/payload",
  "log_dir": "/var/log/ctforge",
  "private_key": "/etc/ctforge/private.pem",
  "public_key": "/etc/ctforge/public.pem"
}
EOF

# 生成 SM2 密钥对
sudo /usr/local/bin/ctforgectl genkey

# 启动
sudo systemctl enable --now ctforged
```

### 11.2 工作原理

```
浏览器 ──HTTP──► ctforge-server ──MSG_CTF_*──► ctforge-agent ──JSON-RPC──► ctforged
```

agent 内置桥接模块（`ctforge.c`），从 `/etc/ctforge/ctforge.cfg` 读 `listen` 地址，把服务端的指令翻译成 JSON-RPC 请求发给本地 `ctforged`，再把结果透传回来。**方法白名单**只允许 `lsmod/insmod/rmmod/mod/status`，防止被滥用执行任意 RPC。

### 11.3 Web 操作

路径：**主机监控 → CTForge 管理**

- **守护进程状态**卡片：ctforged 是否在线、已加载模块数
- **已加载模块**表格：列出所有 payload 模块，可勾选卸载
- **加载模块**表单：输入模块名（不带 `.o` 后缀），一键加载
- **子命令执行**表单：对指定模块执行子命令（如 `show_config`），返回 JSON

### 11.4 API 调用

```bash
TOKEN=<登录后获取>

# 查状态
curl -X POST -H "Authorization: Bearer $TOKEN" \
  http://<server>:8080/api/agents/<agent-id>/ctforge/status

# 列出模块
curl -X POST -H "Authorization: Bearer $TOKEN" \
  http://<server>:8080/api/agents/<agent-id>/ctforge/lsmod

# 加载模块
curl -X POST -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"module":"kprobe_exec"}' \
  http://<server>:8080/api/agents/<agent-id>/ctforge/insmod

# 卸载模块
curl -X POST -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"module":"kprobe_exec"}' \
  http://<server>:8080/api/agents/<agent-id>/ctforge/rmmod
```

---

## 12. 服务端与 Agent 配置参考

### 12.1 服务端启动参数

| 参数 | 环境变量 | 默认值 | 说明 |
|------|----------|--------|------|
| `-http` | `CTF_HTTP` | `:8080` | HTTP 监听 |
| `-https` | `CTF_HTTPS` | `:9443` | HTTPS 监听（空字符串=关闭） |
| `-sock` | `CTF_SOCK` | `:9000` | Agent 控制通道 |
| `-static` | `CTF_STATIC` | 空 | 前端 dist 目录 |
| `-token` | `CTF_TOKEN` | 空 | Agent 注册令牌（生产必配） |
| `-tls-cert` | `CTF_TLS_CERT` | `server.crt` | TLS 证书 |
| `-tls-key` | `CTF_TLS_KEY` | `server.key` | TLS 私钥 |
| `-tls-san` | `CTF_TLS_SAN` | 空 | 额外 SAN（逗号分隔） |
| `-no-tls` | `CTF_NO_TLS=1` | 关 | 禁用 Agent 通道 TLS（仅调试） |
| `-accounts` | `CTF_ACCOUNTS` | `accounts.json` | 账户文件 |
| `-data` | `CTF_DATA` | `.` | 运行时数据目录 |
| `-name` | `CTF_NAME` | 主机名 | 服务端显示名 |
| `-ssh` | `CTF_SSH` | `:2222` | SSH 跳板监听（空=关闭） |
| `-discover` | `CTF_DISCOVER` | `9001` | UDP 发现端口（0=关闭） |
| `-agent-bin` | `CTF_AGENT_BIN` | 空 | agent 二进制路径（`/download/agent`） |

### 12.2 Agent 配置文件

路径：`/etc/ctforge/agent.conf`

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `server_host` | `127.0.0.1` | 服务端地址 |
| `server_port` | `9000` | 服务端 Agent 通道端口 |
| `token` | 空 | 注册令牌（与服务端 `CTF_TOKEN` 一致） |
| `agent_id` | 主机名 | 本机唯一标识 |
| `name` | 主机名 | 显示名 |
| `use_tls` | `1` | TLS 加密（0=明文，仅调试） |
| `server_fingerprint` | 空 | 服务端证书 SHA-256 指纹（pinning） |
| `tls_skip_verify` | `0` | 完全不校验证书（不推荐） |
| `discover_enable` | `1` | UDP 广播发现 |
| `discover_port` | `9001` | 发现广播端口 |
| `metrics_interval` | `30` | 指标采集周期（秒） |
| `heartbeat_interval` | `10` | 心跳周期（秒） |
| `log_level` | `1` | 0=debug 1=info 2=warn 3=error |

### 12.3 API 一览（部分）

所有受保护接口都需要 `Authorization: Bearer <token>`，token 通过 `POST /api/auth/login` 获得。

| Method | Path | 最低角色 | 说明 |
|--------|------|----------|------|
| POST | `/api/auth/login` | 公开 | 登录 |
| POST | `/api/auth/logout` | 公开 | 登出 |
| GET  | `/api/auth/me` | 登录 | 当前用户信息 |
| POST | `/api/auth/password` | 登录 | 修改自己密码 |
| GET/POST/DELETE | `/api/accounts[/{name}]` | admin | 账户管理 |
| GET | `/api/health` | 公开 | 健康检查 |
| GET | `/api/agents` | viewer | 主机列表 |
| GET/POST/... | `/api/agents/{id}/...` | viewer+ | 单主机操作（指标/终端/文件/CTForge） |
| POST | `/api/agents/{id}/ctforge/{lsmod,insmod,rmmod,mod,status}` | operator | eBPF payload 管理 |
| GET | `/api/approvals` | viewer | 待批准主机 |
| POST | `/api/approvals/{id}/{approve,reject}` | operator | 审批动作 |
| GET/POST/DELETE | `/api/tasks[/{id}]` | viewer+ | 定时任务 |
| GET | `/api/alerts` | viewer | 告警列表 |
| POST | `/api/alerts/{id}/resolve` | operator | 处理告警 |
| GET/PUT | `/api/notify/config` | admin | 通知配置 |
| POST | `/api/notify/test` | admin | 测试通知 |
| GET | `/api/perms/meta` | viewer | 权限点元信息 |
| GET/POST/DELETE | `/api/groups[/{id}]` | viewer/admin | 机器分组 |
| GET/POST | `/api/grants[/{user}]` | admin | 授权 |
| GET | `/api/history` | viewer | 上下线历史 |
| GET/POST | `/api/portforwards` | operator | 端口转发 |
| GET/PUT | `/api/ai/config` | admin | AI 配置 |
| GET | `/api/ai/status` | viewer | AI 可用性 |
| GET | `/api/stats/{overview,agents}` | viewer | 统计 |
| GET | `/api/topology` | viewer | 拓扑 |
| GET | `/api/terminals` | viewer | 持久终端会话列表 |
| WS  | `/ws/terminal` | 登录 | 终端 WebSocket |
| GET | `/api/deploy/script.sh` | 公开 | 一键部署脚本 |
| GET | `/download/agent?arch={amd64,arm64}` | 公开 | agent 二进制下载 |

---

## 13. 日常运维（备份/升级/排障）

### 13.1 备份

```bash
# Docker 卷备份
docker run --rm -v ctforge-web_ctforge-data:/data -v $(pwd):/backup alpine \
  tar czf /backup/ctforge-data-$(date +%F).tar.gz -C /data .
docker run --rm -v ctforge-web_ctforge-certs:/data -v $(pwd):/backup alpine \
  tar czf /backup/ctforge-certs-$(date +%F).tar.gz -C /data .

# 裸机部署备份
sudo tar czf ctforge-backup-$(date +%F).tar.gz /var/lib/ctforge /etc/ctforge
```

### 13.2 升级

```bash
cd ctforge-web
git pull

# Docker 部署（宿主机产物挂载方式）
cd src/frontend && npm install && npm run build && cd ../..
cd src/server && go build -o bin/ctforge-server ./cmd/server && cd ../..
docker compose restart   # 无需 down，卷挂载的二进制直接生效

# 升级 agent（可选）：到每台目标主机执行
curl -fsSL http://<server>:8080/api/deploy/script.sh | sudo sh -s -- --force-agent
```

### 13.3 健康检查

```bash
curl http://localhost:8080/api/health
# 期望：{"code":0,"data":{"status":"up","time":...},"message":"success"}
```

### 13.4 查看日志

```bash
# Docker
docker compose logs -f ctforge-server --tail=200

# systemd
sudo journalctl -u ctforge-server -f --since today

# Agent（在被管主机上）
sudo journalctl -u ctforge-agent -f
```

---

## 14. FAQ 常见问题

**Q1：改了前端代码，容器里还是旧页面？**

A：`docker-compose.yml` 把 `src/frontend/dist` 挂载进容器。改完前端后必须重新 `npm run build`，然后 `docker compose restart ctforge-server`。

**Q2：Agent 一直显示"待批准"，但批准管理里看不到？**

A：检查 agent 日志 `journalctl -u ctforge-agent -f`：
- `pending approval from administrator` → 等待批准
- `connection refused` → 服务端 `:9000` 端口不通，检查防火墙
- `bad token` → 服务端 `CTF_TOKEN` 与 agent `token` 不一致
- `certificate verify failed` → agent 开了 `server_fingerprint` 但指纹不匹配

**Q3：HTTPS 9443 端口访问报证书错误？**

A：服务端用自签证书，浏览器会警告。生产环境可：
1. 把 `/etc/ctforge/server.crt` 替换为正式证书
2. 或前置一个 nginx/caddy 做反向代理

**Q4：忘记了 admin 密码？**

A：停服后删除 `accounts.json` 中的 admin 记录（或整个文件），重启会自动重建 `admin/admin123`：

```bash
docker compose stop ctforge-server
docker run --rm -v ctforge-web_ctforge-data:/data alpine sh -c 'rm -f /data/accounts.json'
docker compose start ctforge-server
```

**Q5：远程终端连不上 / 闪断？**

A：检查 agent 上是否有 `/bin/sh` 或 `/bin/bash`；确认服务端 8080 端口可访问（WebSocket 走 8080）。容器内部 NAT 环境下，agent 需能直连服务端 IP。

**Q6：SSH 跳板连不上？**

A：确认服务端 `CTF_SSH` 配置且防火墙放行 2222；登录账号必须是控制台已存在的账户。

**Q7：一台主机能同时被多个服务端管吗？**

A：不能。批准后 agent 与 `server_id` 绑定，要切换到新服务端需先卸载 agent 再重装（或删除 `/etc/ctforge/server_id` 重启 agent）。

**Q8：Agent 装到 Windows / macOS 可以吗？**

A：当前 agent 是 C 写的 Linux-only，依赖 `/proc`、`systemd`、`glibc`。Windows/macOS 暂不支持。

**Q9：eBPF 功能加载失败？**

A：确认内核 ≥ 4.19 且开启 `CONFIG_BPF`/`CONFIG_BPF_SYSCALL`；检查 `ctforged` 日志 `journalctl -u ctforged`；SM2 密钥对必须先用 `ctforgectl genkey` 生成。

**Q10：生产环境如何加固？**

A：最少做这几件事：
1. 设置强 `CTF_TOKEN`
2. admin 密码改为强密码
3. 用 HTTPS（9443）访问 Web
4. agent 配置 `server_fingerprint` 做证书 pinning
5. 防火墙只放行必需端口
6. 定期备份 `ctforge-data` 卷

---

**最后更新：** 2026-11-17
**对应代码版本：** master @ 615c537
