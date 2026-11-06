# CTForge 研发文档

## 1. 项目概述

CTForge 是一个基于 eBPF 的 Linux 安全框架，集成了主机监控、远程运维、Web 控制台三大能力。本项目由两个子项目合并而成：

| 来源 | 功能 | 技术栈 |
|------|------|--------|
| ctforge（原有） | eBPF 内核防护、payload 动态加载、SM2 签名 | C + eBPF + libbpf |
| ctforge（合并） | 主机监控、远程终端、文件管理、告警通知 | Go + Vue3 + C |

合并后项目统一命名为 **CTForge**，保留原有 eBPF 能力的同时，获得完整的 Web 化管理界面。

## 2. 架构设计

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                    用户浏览器 (Web Console)                   │
│              Vue3 + TDesign + xterm.js + ECharts             │
└──────────────────────┬──────────────────────────────────────┘
                       │ HTTPS / WebSocket
┌──────────────────────▼──────────────────────────────────────┐
│                  CTForge Server (Go)                        │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────┐  │
│  │ HTTP API │ │ WebSocket│ │ SSH 跳板 │ │  告警引擎     │  │
│  └──────────┘ └──────────┘ └──────────┘ └──────────────┘  │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────┐  │
│  │ 用户认证  │ │ 权限管理  │ │ 审计日志  │ │  AI 分析     │  │
│  └──────────┘ └──────────┘ └──────────┘ └──────────────┘  │
└──────────────────────┬──────────────────────────────────────┘
                       │ JSON over TCP (TLS 1.2+)
        ┌──────────────┼──────────────┐
        │              │              │
┌───────▼──────┐ ┌─────▼──────┐ ┌────▼───────┐
│ ctforge-agent│ │ctforge-agent│ │ctforge-agent│
│  (Host A)    │ │  (Host B)   │ │  (Host C)  │
│ ┌──────────┐ │ │             │ │ ┌──────────┐│
│ │ctforge桥接│ │ │             │ │ │ctforge桥接││
│ └────┬─────┘ │ │             │ │ └────┬─────┘│
│      │unix/tcp│ │             │ │      │unix/tcp│
│ ┌────▼─────┐ │ │             │ │ ┌────▼─────┐│
│ │ ctforged │ │ │             │ │ │ ctforged ││
│ │ (eBPF)   │ │ │             │ │ │ (eBPF)   ││
│ └──────────┘ │ │             │ │ └──────────┘│
└──────────────┘ └─────────────┘ └─────────────┘
```

### 2.2 组件说明

| 组件 | 语言 | 职责 |
|------|------|------|
| `ctforge-server` | Go | 中心服务端，REST API + WebSocket + SSH 跳板 |
| `ctforge-agent` | C | 客户端代理，部署到被管主机，采集数据 + 执行命令 |
| `ctforged` | C | eBPF 守护进程，加载/卸载 payload 模块 |
| `ctforgectl` | C | 本地管理 CLI，通过 unix socket 控制 ctforged |
| `frontend` | Vue3 | Web 控制台，TDesign 组件库 |

## 3. 目录结构

```
ctforge-web/
├── Makefile                 # 顶层构建入口
├── README.md
├── .gitignore
├── doc/                     # 研发文档目录（本文件所在）
│   └── README.md            # 本文档
└── src/
    ├── server/              # Go 服务端
    │   ├── cmd/server/      # main.go 入口
    │   ├── internal/
    │   │   ├── agentconn/   # agent 连接管理
    │   │   ├── ai/          # AI 诊断
    │   │   ├── auth/        # 认证与 RBAC
    │   │   ├── deploy/      # 一键部署脚本生成
    │   │   ├── discover/    # UDP 局域网发现
    │   │   ├── history/     # 操作审计
    │   │   ├── httpapi/     # HTTP API + WebSocket
    │   │   ├── identity/    # server_id 身份绑定
    │   │   ├── monitor/     # 指标聚合
    │   │   ├── notify/      # 邮件/Webhook 通知
    │   │   ├── perms/       # 26 个权限点管理
    │   │   ├── portfwd/     # 端口转发
    │   │   ├── proto/       # 协议定义
    │   │   ├── rules/       # 告警规则引擎
    │   │   ├── sshgateway/  # SSH 跳板机
    │   │   ├── store/       # 内存存储 + JSON 持久化
    │   │   ├── tasks/       # 定时任务引擎
    │   │   └── tlscfg/      # TLS 证书管理
    │   ├── go.mod
    │   ├── go.sum
    │   └── Dockerfile
    │
    ├── clients/             # C 客户端
    │   ├── include/
    │   │   └── ctforge.h    # 协议头文件
    │   ├── src/
    │   │   ├── agent.c      # 主循环 + 消息分发
    │   │   ├── ctforge.c    # CTForge eBPF 桥接 ★新增
    │   │   ├── net.c        # TCP/TLS 连接
    │   │   ├── util.c       # 工具函数
    │   │   ├── metrics.c    # 指标采集
    │   │   ├── config.c     # 配置管理
    │   │   ├── terminal.c   # PTY 终端
    │   │   ├── files.c      # 文件管理
    │   │   ├── sysadmin.c   # 系统服务管理
    │   │   ├── sysops.c     # 软件包/安全审计
    │   │   ├── binding.c    # 身份绑定
    │   │   ├── anomaly.c    # 异常检测
    │   │   ├── hardware.c   # 硬件信息采集
    │   │   ├── portforward.c# 端口转发
    │   │   ├── tls.c        # TLS 封装
    │   │   ├── discover.c   # UDP 发现
    │   │   └── uninstall.c  # 受保护卸载
    │   ├── Makefile
    │   └── ctforge-agent.conf.example
    │
    ├── frontend/            # Vue3 前端
    │   ├── src/
    │   │   ├── pages/
    │   │   │   ├── login/           # 登录
    │   │   │   ├── monitor/         # 主机监控
    │   │   │   │   ├── agents/      #   主机列表
    │   │   │   │   ├── overview/    #   总览
    │   │   │   │   ├── detail/      #   详情
    │   │   │   │   ├── hardware/    #   硬件信息
    │   │   │   │   ├── network/     #   网络监控
    │   │   │   │   ├── logs/        #   日志分析
    │   │   │   │   ├── ctforge/     #   CTForge 管理 ★新增
    │   │   │   │   ├── syslog/      #   系统日志
    │   │   │   │   ├── terminal/    #   远程终端
    │   │   │   │   ├── files/       #   文件管理
    │   │   │   │   ├── services/    #   系统服务
    │   │   │   │   ├── packages/    #   软件包
    │   │   │   │   ├── docker/      #   Docker
    │   │   │   │   ├── portfwd/     #   端口转发
    │   │   │   │   ├── sysusers/    #   系统账户
    │   │   │   │   ├── exec/        #   命令执行
    │   │   │   │   ├── config/      #   客户端配置
    │   │   │   │   └── security/    #   安全审计
    │   │   │   ├── system/          # 系统管理
    │   │   │   │   ├── accounts/    #   账户管理
    │   │   │   │   ├── alerts/      #   告警中心
    │   │   │   │   ├── approvals/   #   批准管理
    │   │   │   │   ├── auditlog/    #   操作审计
    │   │   │   │   ├── groups/      #   分组授权
    │   │   │   │   ├── notify/      #   通知配置
    │   │   │   │   ├── settings/    #   系统设置
    │   │   │   │   ├── stats/       #   统计视图
    │   │   │   │   ├── tasks/       #   定时任务
    │   │   │   │   └── topology/    #   网络拓扑
    │   │   │   └── result/          # 结果页面
    │   │   ├── api/         # API 客户端
    │   │   │   └── ctforge.ts       #   CTForge API ★新增
    │   │   ├── router/      # 路由
    │   │   ├── store/       # Pinia 状态
    │   │   ├── layouts/     # 布局组件
    │   │   ├── locales/     # i18n
    │   │   └── ...
    │   ├── package.json
    │   └── vite.config.ts
    │
    ├── ctforged/            # eBPF 守护进程（原有）
    │   ├── ctforged.c       # 主程序
    │   ├── server.c         # JSON-RPC 服务器
    │   ├── payload.c        # payload 模块管理
    │   ├── signature.c      # SM2 签名验证
    │   ├── netlink.c        # netlink 通信
    │   ├── ctforge.h        # 公共头文件
    │   └── Makefile
    │
    ├── ctforgectl/          # 管理 CLI（原有）
    │   └── ctforgectl.c
    │
    ├── payload/             # eBPF payload 模块（原有）
    ├── key/                 # SM2 密钥（原有）
    ├── configs/             # 配置文件（原有）
    └── scripts/             # systemd service（原有）
```

## 4. CTForge eBPF 集成设计

### 4.1 集成思路

CTForge 原有的 eBPF 能力（payload 加载/卸载/状态查询）通过 `ctforged` 守护进程提供 JSON-RPC 接口。为了让 Web 控制台能够远程管理这些功能，在客户端 agent 中增加 **桥接模块**：

```
前端 ──HTTP──► Server ──MSG_CTF_*──► Agent(ctforge.c) ──JSON-RPC──► ctforged
```

### 4.2 消息协议

Agent 与 Server 之间使用扩展的 ctforge 协议，新增 5 种消息类型：

| 消息类型 | 方向 | 说明 |
|----------|------|------|
| `ctf_lsmod` | Server → Agent | 查询已加载模块列表 |
| `ctf_insmod` | Server → Agent | 加载 payload 模块 `{module: "name"}` |
| `ctf_rmmod` | Server → Agent | 卸载 payload 模块 `{module: "name"}` |
| `ctf_mod` | Server → Agent | 执行子命令 `{module: "name", subcmd: "cmd"}` |
| `ctf_status` | Server → Agent | 查询守护进程状态 |

Agent 收到消息后，通过 unix socket 或 TCP 连接到本地 `ctforged`，发送 JSON-RPC 请求并透传响应。

### 4.3 ctforge.c 桥接模块

```c
// 核心函数
char *ctc_ctforge_exec(const char *method, const char *params);

// 内部流程：
// 1. 读取 /etc/ctforge/ctforge.cfg 获取 listen 地址
// 2. 连接 ctforged（unix://path 或 host:port）
// 3. 构造 JSON-RPC 请求: {"jsonrpc":"6.0","type":0,"taskid":...,"method":"...","reqmsg":{"params":"..."}}
// 4. 发送并读取响应
// 5. 提取 resmsg 字段返回
```

**安全设计**：方法白名单限制（仅允许 `lsmod/insmod/rmmod/mod/status`），防止通过 agent 执行任意 RPC。

### 4.4 Server API

| 端点 | 方法 | 权限 | 说明 |
|------|------|------|------|
| `/api/agents/{id}/ctforge/lsmod` | POST | operator | 模块列表 |
| `/api/agents/{id}/ctforge/status` | POST | operator | 守护进程状态 |
| `/api/agents/{id}/ctforge/insmod` | POST | operator | 加载模块 `{module}` |
| `/api/agents/{id}/ctforge/rmmod` | POST | operator | 卸载模块 `{module}` |
| `/api/agents/{id}/ctforge/mod` | POST | operator | 子命令 `{module, subcmd}` |

### 4.5 前端页面

**CTForge 管理** 页面 (`/monitor/ctforge`) 提供：

- **守护进程状态**卡片：显示 ctforged 运行状态
- **已加载模块**表格：列出所有模块，支持选择卸载
- **加载模块**表单：输入模块名，一键加载
- **子命令执行**表单：对指定模块执行子命令，显示 JSON 结果

## 5. 构建与部署

### 5.1 构建要求

| 组件 | 依赖 |
|------|------|
| ctforged | clang, libbpf, libelf, libuv, json-c, openssl, liblzma |
| ctforge-agent | gcc, openssl (静态链接) |
| ctforge-server | Go 1.21+ |
| frontend | Node.js 18+, npm |

### 5.2 一键构建

```bash
# 全部构建
make all

# 单独构建
make ctforged    # eBPF 守护进程
make agent       # C 客户端
make server      # Go 服务端
make frontend    # Vue3 前端

# 安装到系统
sudo make install

# 清理
make clean

# 测试
make test
```

### 5.3 部署流程

```bash
# 1. 构建前端并部署到服务端静态目录
cd src/frontend && npm run build
# 将 dist/ 目录复制到服务器

# 2. 启动服务端
./ctforge-server -http :8080 -sock :9000 -static /path/to/dist

# 3. 在目标主机部署 agent（通过 Web 控制台生成一键安装命令）
#    或直接复制编译好的 ctforge-agent 到目标主机

# 4. 在需要 eBPF 防护的主机安装 ctforged
sudo make -C src/ctforged install
sudo systemctl start ctforged
```

## 6. 测试策略

### 6.1 Go Server 测试

```bash
cd src/server && go test -v ./...
```

- 协议编解码测试
- 存储层 CRUD 测试
- HTTP API 集成测试
- 权限验证测试

### 6.2 C Agent 测试

```bash
# 语法检查
make test  # 包含 gcc -fsyntax-only 检查

# 完整编译
make agent
```

### 6.3 前端测试

```bash
cd src/frontend
npm run lint     # ESLint 检查
npm run build    # TypeScript 编译 + Vite 打包
```

### 6.4 端到端测试

1. 启动 `ctforge-server`
2. 在目标主机运行 `ctforge-agent`
3. 浏览器访问 Web 控制台
4. 在 **CTForge 管理** 页面操作 payload 模块
5. 验证模块加载/卸载结果

## 7. 合并变更记录

| 阶段 | 说明 | Commit 数 |
|------|------|-----------|
| A | 基础设施（.gitignore, 目录结构） | 1 |
| B | Server 模块导入（20 个 internal 包） | 22 |
| C | Client 模块导入（17 个 C 文件） | 18 |
| D | Frontend 导入（pages/router/store/locales 等） | 43 |
| E | 重命名 ctforge → ctforge | 14 |
| F | CTForge eBPF 集成（agent 桥接 + server API + 前端页面） | 8 |
| G | 构建系统 + 文档 + 测试 | 3 |
| **合计** | | **130+** |
