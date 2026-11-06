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
| **CTForge 管理** | Web 界面直接管理 eBPF payload 模块（加载/卸载/状态查询）★合并新增 |

## 快速开始

```bash
# 一键构建全部组件
make all

# 或分别构建
make ctforged    # eBPF 守护进程（需要 clang + libbpf）
make agent       # C 客户端（需要 gcc + openssl）
make server      # Go 服务端（需要 Go 1.21+）
make frontend    # Vue3 前端（需要 Node.js 18+）

# 运行测试
make test

# 安装到系统
sudo make install
```

## 部署架构

```
浏览器 ──HTTPS──► ctforge-server ──TCP/TLS──► ctforge-agent ──JSON-RPC──► ctforged(eBPF)
```

详细架构设计与研发文档见 [doc/README.md](doc/README.md)。

## 目录结构

```
src/
├── server/      # Go 服务端（REST API + WebSocket + SSH 跳板）
├── clients/     # C 客户端代理（采集 + 命令执行 + ctforge 桥接）
├── frontend/    # Vue3 Web 控制台（TDesign + xterm.js）
├── ctforged/    # eBPF 守护进程（payload 管理 + SM2 签名）
├── ctforgectl/  # 本地管理 CLI
├── payload/     # eBPF payload 模块
├── key/         # SM2 密钥生成
├── configs/     # 配置文件
└── scripts/     # systemd service
```

## 项目说明

CTForge 各组件职责：

- **服务端、前端**：基于 Go + Vue3 构建，提供 REST API、WebSocket、SSH 跳板
- **客户端**：`ctforge-agent` C 语言代理，新增 `ctforge.c` 桥接模块
- **eBPF 组件**：`ctforged`/`ctforgectl`/`payload` 完整保留
- **新增集成**：Web 控制台可直接管理 eBPF payload 模块

合并过程产生 130+ 个 commit，详细变更记录见 `git log`。

## License

MIT
