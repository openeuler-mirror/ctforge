#ifndef CTFORGE_PROTOCOL_H
#define CTFORGE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define CTC_VERSION "0.2.0"
#define CTC_PROTO_VERSION 1
#define CTC_MAX_MSG (32u * 1024u * 1024u)
#define CTC_DISCOVER_PORT 9001

/* 消息类型 */
#define MSG_REGISTER        "register"
#define MSG_HEARTBEAT       "heartbeat"
#define MSG_METRICS         "metrics"
#define MSG_LOG             "log"
#define MSG_TERMINAL_OPEN   "terminal_open"
#define MSG_TERMINAL_INPUT  "terminal_input"
#define MSG_TERMINAL_OUTPUT "terminal_output"
#define MSG_TERMINAL_CLOSE  "terminal_close"
#define MSG_CONFIG_GET      "config_get"
#define MSG_CONFIG_SET      "config_set"
#define MSG_FILE_LIST       "file_list"
#define MSG_FILE_READ       "file_read"
#define MSG_FILE_WRITE      "file_write"
#define MSG_FILE_DELETE     "file_delete"
#define MSG_FILE_RENAME     "file_rename"
#define MSG_FILE_MKDIR      "file_mkdir"
#define MSG_ACK             "ack"
#define MSG_ERROR           "error"
#define MSG_SERVICE_LIST    "service_list"
#define MSG_SERVICE_ACTION  "service_action"
#define MSG_PKG_UPDATES     "pkg_updates"
#define MSG_PKG_UPGRADE     "pkg_upgrade"
#define MSG_PKG_HISTORY     "pkg_history"
#define MSG_SECURITY_AUDIT  "security_audit"
#define MSG_PING            "ping"
#define MSG_PONG            "pong"
#define MSG_SYS_USER_LIST   "sys_user_list"
#define MSG_SYS_USER_PASSWD "sys_user_passwd"
#define MSG_SYS_USER_LOCK   "sys_user_lock"
#define MSG_SYS_LOG         "sys_log"
#define MSG_NET_TRAFFIC     "net_traffic"
#define MSG_NET_CONN        "net_conn"
#define MSG_CMD_EXEC        "cmd_exec"
/* 批准制握手 */
#define MSG_HANDSHAKE_REQ   "handshake_req"
#define MSG_HANDSHAKE_ACK   "handshake_ack"
#define MSG_OFFLINE         "offline"
#define MSG_PENDING_APPROVAL "pending_approval"
/* 定时任务 */
#define MSG_TASK_RUN        "task_run"
#define MSG_TASK_RESULT     "task_result"
/* 异常上报 */
#define MSG_ANOMALY         "anomaly"
/* 卸载审批 */
#define MSG_UNINSTALL_REQ   "uninstall_req"
#define MSG_UNINSTALL_ACK   "uninstall_ack"
#define MSG_UNINSTALL       "uninstall"
/* 端口转发 */
#define MSG_PF_OPEN         "pf_open"
#define MSG_PF_CLOSE        "pf_close"
#define MSG_PF_DATA         "pf_data"
#define MSG_PF_ACCEPT       "pf_accept"
/* 远程升级 */
#define MSG_UPGRADE         "upgrade"
/* 硬件信息 / 磁盘 IO */
#define MSG_HARDWARE        "hardware"
#define MSG_DISK_IO         "disk_io"
/* Docker 管理 */
#define MSG_DOCKER_LIST     "docker_list"
#define MSG_DOCKER_ACTION   "docker_action"
#define MSG_DOCKER_LOGS     "docker_logs"
#define MSG_DOCKER_IMAGES   "docker_images"
#define MSG_DOCKER_INSPECT  "docker_inspect"
#define MSG_DOCKER_CREATE   "docker_create"
#define MSG_DOCKER_COMPOSE  "docker_compose"

/* CTForge eBPF 框架桥接消息 */
#define MSG_CTF_LSMOD       "ctf_lsmod"
#define MSG_CTF_INSMOD      "ctf_insmod"
#define MSG_CTF_RMMOD       "ctf_rmmod"
#define MSG_CTF_MOD         "ctf_mod"
#define MSG_CTF_STATUS      "ctf_status"

/* 客户端本地配置（可被服务端修改） */
typedef struct {
    char server_host[128];
    char server_hosts[512]; /* 逗号分隔的服务端候选 IP 列表（自动切换） */
    int  server_port;
    char agent_id[128];
    char name[128];
    char token[256];
    int  metrics_interval;   /* 秒 */
    int  heartbeat_interval; /* 秒 */
    int  log_level;          /* 0 debug 1 info 2 warn 3 error */
    int  use_tls;            /* 1=TLS 连接（默认），0=明文（仅联调） */
    int  tls_skip_verify;    /* 1=不校验证书（不推荐），0=校验 */
    char server_fingerprint[128]; /* 服务端证书 SHA-256 指纹（hex）， pinning 校验 */
    int  discover_enable;    /* 1=开启 UDP 发现广播（默认） */
    int  discover_port;      /* 默认 9001 */
    /* 批准制绑定（持久化在配置文件，批准后写入） */
    char bound_server_id[128]; /* 服务端 UUID，空=未绑定 */
    char bound_server_ip[64];  /* 最近已知服务端地址（IP 变化时更新） */
    int  bound_server_port;    /* 服务端端口 */
    int  approved;             /* 1=已获批准 */
} ctc_config;

/* 绑定信息（持久化到 agent.conf，批准后写入） */
typedef struct {
    char bound_server_id[128]; /* 批准后绑定的服务端 UUID，空=未绑定 */
    char bound_server_ip[64];
    int  bound_server_port;
    int  approved;
} ctc_binding;

/* 异常检测状态 */
typedef struct {
    uint64_t last_rx_bytes, last_tx_bytes;
    time_t   last_sample;
    time_t   last_net_report;
    time_t   last_ssh_report;
    long     ssh_fail_count;
} ctc_anomaly;

void ctc_config_defaults(ctc_config *cfg);
char *ctc_config_to_json(const ctc_config *cfg);
int  ctc_config_set(ctc_config *cfg, const char *key, const char *value);

/* 终端 */
int  ctc_term_open(int sock, const char *session_id, int cols, int rows);
int  ctc_term_input(const char *session_id, const unsigned char *data, size_t len);
int  ctc_term_close(const char *session_id);
void ctc_term_close_all(void);

/* 系统服务管理 */
char *ctc_service_list_json(const char *filter);
char *ctc_service_action_json(const char *name, const char *action);

/* 软件包管理 */
char *ctc_pkg_updates_json(void);
char *ctc_pkg_upgrade_json(const char *packages_json); /* packages_json: ["pkg1","pkg2"] 或 NULL=全部 */
char *ctc_pkg_history_json(void);

/* 安全审计 */
char *ctc_security_audit_json(void);

/* 心跳延迟测量 */
char *ctc_pong_json(long long server_ts);

/* Docker 管理 */
char *ctc_docker_list_json(void);
char *ctc_docker_action_json(const char *container_id, const char *action);
char *ctc_docker_logs_json(const char *container_id, int lines);
char *ctc_docker_images_json(void);
char *ctc_docker_inspect_json(const char *container_id);
char *ctc_docker_create_json(const char *payload_json);
char *ctc_docker_compose_json(const char *payload_json);

/* 系统账户管理 */
char *ctc_sys_user_list_json(void);
char *ctc_sys_user_passwd_json(const char *username, const char *new_password);
char *ctc_sys_user_lock_json(const char *username, int lock);

/* 系统日志 */
char *ctc_sys_log_json(const char *source, const char *priority, const char *unit, const char *keyword, int lines);

/* 网络监控 */
char *ctc_net_traffic_json(void);
char *ctc_net_conn_json(void);

/* 远程命令 */
char *ctc_cmd_exec_json(const char *command, int timeout_sec, const char *shell);

/* 文件 */
char *ctc_file_list_json(const char *path);
char *ctc_file_read_json(const char *path);
char *ctc_file_read_range_json(const char *path, long long offset, long long limit);
char *ctc_file_write_json(const char *payload);
char *ctc_file_simple_json(const char *action, const char *payload);

/* 连接对象（简单同步模型 + 终端线程） */
typedef struct ctc_conn ctc_conn;

/* 协议 IO（sock 可能是 TCP fd，也可能是 TLS 封装后的句柄） */
int  ctc_send(int fd, const char *type, const char *id, const char *session_id, const char *payload_json);
int  ctc_send_simple(int fd, const char *type, const char *payload_json);
/* 读取一帧，返回新分配的消息（调用方负责 free 各字段） */
typedef struct {
    char *id;
    char *type;
    char *session_id;
    char *payload; /* 原始 json，可能为 NULL */
} ctc_message;
int  ctc_recv(int fd, ctc_message *out);
void ctc_message_free(ctc_message *m);

/* json 工具（极简） */
const char *ctc_json_get_string(const char *json, const char *key, char *buf, size_t buflen);
long        ctc_json_get_int(const char *json, const char *key, long defval);
int         ctc_json_get_bool(const char *json, const char *key, int defval);

/* base64 */
char  *ctc_base64_encode(const unsigned char *data, size_t len);
unsigned char *ctc_base64_decode(const char *b64, size_t *outlen);

/* 采集 */
typedef struct {
    uint64_t uptime_sec;
    double   cpu_percent;
    double   load1, load5, load15;
    uint64_t mem_total_kb, mem_used_kb;
    double   mem_percent;
    uint64_t swap_total_kb, swap_used_kb;
    uint64_t disk_total_kb, disk_used_kb;
    double   disk_percent;
    int      procs;
} ctc_metrics;
void ctc_collect_metrics(ctc_metrics *m);

/* 配置 */
int  ctc_config_load(ctc_config *cfg, const char *path);
int  ctc_config_save(const ctc_config *cfg, const char *path);

/* 日志 */
void ctc_logf(ctc_conn *c, int level, const char *fmt, ...);

/* TLS：fd 为原始 TCP socket，成功返回 0，之后 ctc_send/ctc_recv 自动走 TLS。
   fingerprint 为 NULL 时仅做加密不做 pinning；非 NULL 时校验服务端证书 SHA-256。 */
int  ctc_tls_wrap(int fd, const char *server_hostname, const char *fingerprint_hex, int skip_verify);
void ctc_tls_cleanup(int fd);

/* UDP 发现广播线程（周期发送，直到进程退出） */
int  ctc_discover_start(const ctc_config *cfg);

/* 绑定与扫描 */
int  ctc_binding_load(ctc_binding *b, const char *path);
int  ctc_binding_save(const ctc_binding *b, const char *path);
int  ctc_scan_server(ctc_config *cfg, ctc_binding *binding);
int  ctc_handshake(int fd, const ctc_config *cfg, const ctc_binding *binding, ctc_binding *out);

/* 异常检测与上报 */
void ctc_anomaly_init(ctc_anomaly *st);
void ctc_anomaly_check(int fd, ctc_anomaly *st);

/* 系统信息采集（用于握手上报） */
void ctc_sysinfo(char *os_version, size_t os_sz, char *kernel, size_t k_sz);

/* 卸载：需服务端批准。返回 0=已批准可卸载，1=未批准，-1=连不上服务端 */
int ctc_uninstall_request(ctc_config *cfg);

/* 硬件信息 / 磁盘 IO */
char *ctc_hardware_json(void);
char *ctc_disk_io_json(void);

/* 端口转发：服务端要求把本机某端口流量转发到服务端 */
int  ctc_pf_open(int sock, const char *forward_id, const char *target_host, int target_port,
                 const char *local_host, int local_port);
void ctc_pf_close(const char *forward_id);
void ctc_pf_close_all(void);
/* 在终端读线程里调：转发数据回服务端 */
int  ctc_pf_send_data(int sock, const char *forward_id, const char *conn_id,
                      const unsigned char *data, size_t len);
int  ctc_pf_send_close(int sock, const char *forward_id, const char *conn_id);
/* 服务端 -> agent 方向 */
int  ctc_pf_write(const char *forward_id, const char *conn_id,
                  const unsigned char *data, size_t len);
void ctc_pf_conn_close(const char *forward_id, const char *conn_id);
int  ctc_pf_accept_conn(const char *forward_id, const char *conn_id);

/* 远程升级：从服务端拉取新版本并原地替换 */
char *ctc_upgrade_json(const char *url, const char *sha256, const char *version);

/* 连接/主循环 */
int ctc_conn_run(ctc_config *cfg);

/* ctforge eBPF 桥接：执行 ctforged 命令，返回 JSON 字符串（malloc） */
char *ctc_ctforge_exec(const char *method, const char *params);

#endif
