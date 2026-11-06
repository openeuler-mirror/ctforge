#include "ctforge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netdb.h>
#include <signal.h>
#include <sys/utsname.h>
#include <arpa/inet.h>

/* portforward.c */
extern char *ctc_hardware_json(void);
extern char *ctc_disk_io_json(void);
extern char *ctc_upgrade_json(const char *url, const char *sha256, const char *version);
extern int  ctc_pf_open(int sock, const char *fid, const char *th, int tp, const char *lh, int lp);
extern void ctc_pf_close(const char *fid);
extern void ctc_pf_close_all(void);
extern int  ctc_pf_write(const char *fid, const char *cid, const unsigned char *data, size_t len);
extern void ctc_pf_conn_close(const char *fid, const char *cid);
extern int  ctc_pf_accept_conn(const char *fid, const char *cid);

/* sysadmin.c docker */
extern char *ctc_docker_list_json(void);
extern char *ctc_docker_action_json(const char *container_id, const char *action);
extern char *ctc_docker_logs_json(const char *container_id, int lines);
extern char *ctc_docker_images_json(void);
extern char *ctc_docker_inspect_json(const char *container_id);
extern char *ctc_docker_create_json(const char *payload_json);
extern char *ctc_docker_compose_json(const char *payload_json);

/* tls.c */
extern int ctc_tls_write(int fd, const void *buf, int n);
extern void ctc_tls_cleanup(int fd);
/* uninstall.c */
extern int  ctc_uninstall_request(ctc_config *cfg);
extern int  ctc_do_uninstall(void);
/* discover.c */
extern int  ctc_discover_start(const ctc_config *cfg);
extern void ctc_discover_stop(void);

/* 客户端与 server 的会话循环：注册 -> 心跳/指标 -> 派发命令 */
static volatile int g_running = 1;
static void on_sig(int sig) { (void)sig; g_running = 0; }

static char *now_ms_str(void) {
    static _Thread_local char buf[32];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(buf, sizeof(buf), "%lld%03d", (long long)ts.tv_sec, (int)(ts.tv_nsec / 1000000));
    return buf;
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void send_log(int fd, const char *level, const char *msg) {
    char *esc = NULL;
    /* 简单转义引号/反斜杠 */
    size_t n = strlen(msg), j = 0;
    esc = malloc(n * 2 + 1);
    for (size_t i = 0; i < n; i++) {
        if (msg[i] == '"' || msg[i] == '\\') esc[j++] = '\\';
        esc[j++] = msg[i];
    }
    esc[j] = 0;
    char payload[1024];
    snprintf(payload, sizeof(payload),
             "{\"timestamp\":%lld,\"level\":\"%s\",\"message\":\"%s\"}",
             now_ms(), level, esc);
    free(esc);
    ctc_send_simple(fd, MSG_LOG, payload);
}

static void send_heartbeat(int fd, const ctc_metrics *m) {
    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"timestamp\":%lld,\"uptime_sec\":%llu,\"load1\":%.2f,\"mem_percent\":%.2f,\"disk_percent\":%.2f}",
             now_ms(), (unsigned long long)m->uptime_sec, m->load1, m->mem_percent, m->disk_percent);
    ctc_send_simple(fd, MSG_HEARTBEAT, payload);
}

static void send_metrics(int fd, const ctc_metrics *m) {
    char payload[1024];
    snprintf(payload, sizeof(payload),
             "{\"timestamp\":%lld,\"uptime_sec\":%llu,\"cpu_percent\":%.2f,"
             "\"load1\":%.2f,\"load5\":%.2f,\"load15\":%.2f,"
             "\"mem_total_kb\":%llu,\"mem_used_kb\":%llu,\"mem_percent\":%.2f,"
             "\"swap_total_kb\":%llu,\"swap_used_kb\":%llu,"
             "\"disk_total_kb\":%llu,\"disk_used_kb\":%llu,\"disk_percent\":%.2f,\"procs\":%d}",
             now_ms(), (unsigned long long)m->uptime_sec, m->cpu_percent,
             m->load1, m->load5, m->load15,
             (unsigned long long)m->mem_total_kb, (unsigned long long)m->mem_used_kb, m->mem_percent,
             (unsigned long long)m->swap_total_kb, (unsigned long long)m->swap_used_kb,
             (unsigned long long)m->disk_total_kb, (unsigned long long)m->disk_used_kb, m->disk_percent,
             m->procs);
    ctc_send_simple(fd, MSG_METRICS, payload);
}

static int connect_server(const ctc_config *cfg) {
    char port[16];
    snprintf(port, sizeof(port), "%d", cfg->server_port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(cfg->server_host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = {10, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        struct timeval tv = {0, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        /* TLS 升级：优先 TLS，失败回退明文（双协议回退，确保 https/http 都能通） */
        if (cfg->use_tls) {
            int tls_rc = ctc_tls_wrap(fd, cfg->server_host,
                             cfg->server_fingerprint[0] ? cfg->server_fingerprint : NULL,
                             cfg->tls_skip_verify);
            if (tls_rc < 0) {
                /* TLS 失败：关 fd 重开明文连接（服务端支持双协议时可用） */
                fprintf(stderr, "[ctc-agent] TLS connect failed, retry plaintext\n");
                close(fd);
                fd = -1;
                /* 重新 TCP 连接（明文） */
                struct addrinfo h2 = {0}, *r2 = NULL;
                h2.ai_family = AF_UNSPEC;
                h2.ai_socktype = SOCK_STREAM;
                if (getaddrinfo(cfg->server_host, port, &h2, &r2) == 0) {
                    for (struct addrinfo *p = r2; p; p = p->ai_next) {
                        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                        if (fd < 0) continue;
                        struct timeval tv2 = {10, 0};
                        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2));
                        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
                        close(fd); fd = -1;
                    }
                    freeaddrinfo(r2);
                }
                if (fd >= 0) {
                    struct timeval tv2 = {0, 0};
                    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2));
                }
            }
        }
    }
    return fd;
}

static int do_register(int fd, const ctc_config *cfg) {
    struct utsname un;
    uname(&un);
    char payload[1024];
    snprintf(payload, sizeof(payload),
             "{\"agent_id\":\"%s\",\"name\":\"%s\",\"hostname\":\"%s\",\"os\":\"%s\",\"arch\":\"%s\",\"version\":\"%s\"}",
             cfg->agent_id, cfg->name, un.nodename, un.sysname, un.machine, CTC_VERSION);
    /* register 需要带 token，直接手工构造顶层消息 */
    char full[1400];
    snprintf(full, sizeof(full),
             "{\"id\":\"reg-%s\",\"type\":\"register\",\"version\":%d,\"token\":\"%s\",\"payload\":%s}",
             now_ms_str(), CTC_PROTO_VERSION, cfg->token, payload);
    uint32_t len = (uint32_t)strlen(full);
    uint32_t be = htonl(len);
    if (ctc_tls_write(fd, &be, 4) != 4) return -1;
    if (ctc_tls_write(fd, full, (int)len) != (int)len) return -1;
    ctc_message resp = {0};
    if (ctc_recv(fd, &resp) < 0) return -1;
    int ok = resp.type && !strcmp(resp.type, MSG_ACK);
    ctc_message_free(&resp);
    return ok ? 0 : -1;
}

/* 处理一条来自服务端的命令 */
static void handle_command(int fd, const ctc_message *msg, ctc_config *cfg, const char *cfg_path) {
    const char *t = msg->type;
    if (!t) return;

    if (!strcmp(t, MSG_TERMINAL_OPEN)) {
        int cols = 80, rows = 24;
        if (msg->payload) {
            cols = (int)ctc_json_get_int(msg->payload, "cols", 80);
            rows = (int)ctc_json_get_int(msg->payload, "rows", 24);
        }
        ctc_term_open(fd, msg->session_id ? msg->session_id : "t0", cols, rows);
        return;
    }
    if (!strcmp(t, MSG_TERMINAL_INPUT)) {
        char b64[8192];
        if (msg->payload && ctc_json_get_string(msg->payload, "data_base64", b64, sizeof(b64))) {
            size_t dlen = 0;
            unsigned char *data = ctc_base64_decode(b64, &dlen);
            if (data) {
                ctc_term_input(msg->session_id, data, dlen);
                free(data);
            }
        }
        return;
    }
    if (!strcmp(t, MSG_TERMINAL_CLOSE)) {
        ctc_term_close(msg->session_id);
        return;
    }

    /* 需要应答的命令 */
    char *result = NULL;
    if (!strcmp(t, MSG_CONFIG_GET)) {
        result = ctc_config_to_json(cfg);
    } else if (!strcmp(t, MSG_CONFIG_SET)) {
        char key[64] = {0}, val[256] = {0};
        if (msg->payload && ctc_json_get_string(msg->payload, "key", key, sizeof(key)) &&
            ctc_json_get_string(msg->payload, "value", val, sizeof(val))) {
            if (ctc_config_set(cfg, key, val) == 0) {
                ctc_config_save(cfg, cfg_path);
                result = strdup("{\"ok\":true,\"message\":\"updated\"}");
                send_log(fd, "info", "config updated by server");
            } else {
                result = strdup("{\"error\":\"unsupported key\"}");
            }
        } else {
            result = strdup("{\"error\":\"bad payload\"}");
        }
    } else if (!strcmp(t, MSG_FILE_LIST)) {
        char path[1024] = "/";
        if (msg->payload) ctc_json_get_string(msg->payload, "path", path, sizeof(path));
        result = ctc_file_list_json(path);
    } else if (!strcmp(t, MSG_FILE_READ)) {
        char path[1024] = {0};
        long long off = 0, lim = 0;
        if (msg->payload) {
            ctc_json_get_string(msg->payload, "path", path, sizeof(path));
            off = (long long)ctc_json_get_int(msg->payload, "offset", 0);
            lim = (long long)ctc_json_get_int(msg->payload, "limit", 0);
        }
        if (off > 0 || lim > 0) {
            result = ctc_file_read_range_json(path, off, lim);
        } else {
            result = ctc_file_read_json(path);
        }
    } else if (!strcmp(t, MSG_FILE_WRITE)) {
        result = ctc_file_write_json(msg->payload ? msg->payload : "{}");
    } else if (!strcmp(t, MSG_FILE_DELETE)) {
        result = ctc_file_simple_json("delete", msg->payload ? msg->payload : "{}");
    } else if (!strcmp(t, MSG_FILE_RENAME)) {
        result = ctc_file_simple_json("rename", msg->payload ? msg->payload : "{}");
    } else if (!strcmp(t, MSG_FILE_MKDIR)) {
        result = ctc_file_simple_json("mkdir", msg->payload ? msg->payload : "{}");
    } else if (!strcmp(t, MSG_SERVICE_LIST)) {
        char filter[128] = "";
        if (msg->payload) ctc_json_get_string(msg->payload, "filter", filter, sizeof(filter));
        result = ctc_service_list_json(filter);
    } else if (!strcmp(t, MSG_SERVICE_ACTION)) {
        char name[256] = "", action[32] = "";
        if (msg->payload &&
            ctc_json_get_string(msg->payload, "name", name, sizeof(name)) &&
            ctc_json_get_string(msg->payload, "action", action, sizeof(action))) {
            result = ctc_service_action_json(name, action);
        } else {
            result = strdup("{\"error\":\"bad payload\"}");
        }
    } else if (!strcmp(t, MSG_PKG_UPDATES)) {
        result = ctc_pkg_updates_json();
    } else if (!strcmp(t, MSG_PKG_UPGRADE)) {
        result = ctc_pkg_upgrade_json(msg->payload && strstr(msg->payload, "packages") ? msg->payload : NULL);
    } else if (!strcmp(t, MSG_PKG_HISTORY)) {
        result = ctc_pkg_history_json();
    } else if (!strcmp(t, MSG_SECURITY_AUDIT)) {
        result = ctc_security_audit_json();
    } else if (!strcmp(t, MSG_PING)) {
        long long ts = 0;
        if (msg->payload) ts = ctc_json_get_int(msg->payload, "timestamp", 0);
        result = ctc_pong_json(ts);
    } else if (!strcmp(t, MSG_SYS_USER_LIST)) {
        result = ctc_sys_user_list_json();
    } else if (!strcmp(t, MSG_SYS_USER_PASSWD)) {
        char un[128] = "", pw[256] = "";
        if (msg->payload &&
            ctc_json_get_string(msg->payload, "username", un, sizeof(un)) &&
            ctc_json_get_string(msg->payload, "new_password", pw, sizeof(pw))) {
            result = ctc_sys_user_passwd_json(un, pw);
            memset(pw, 0, sizeof(pw)); /* 立即清零内存中的密码 */
        } else {
            result = strdup("{\"error\":\"bad payload\"}");
        }
    } else if (!strcmp(t, MSG_SYS_USER_LOCK)) {
        char un[128] = "";
        int lock = 1;
        if (msg->payload) {
            ctc_json_get_string(msg->payload, "username", un, sizeof(un));
            char ls[8] = "";
            if (ctc_json_get_string(msg->payload, "lock", ls, sizeof(ls)))
                lock = (!strcmp(ls, "true") || ctc_json_get_int(msg->payload, "lock", 1) != 0);
        }
        result = ctc_sys_user_lock_json(un, lock);
    } else if (!strcmp(t, MSG_SYS_LOG)) {
        char source[32] = "", priority[32] = "", unit[128] = "", keyword[256] = "";
        int lines = 100;
        if (msg->payload) {
            ctc_json_get_string(msg->payload, "source", source, sizeof(source));
            ctc_json_get_string(msg->payload, "priority", priority, sizeof(priority));
            ctc_json_get_string(msg->payload, "unit", unit, sizeof(unit));
            ctc_json_get_string(msg->payload, "keyword", keyword, sizeof(keyword));
            lines = (int)ctc_json_get_int(msg->payload, "lines", 100);
        }
        result = ctc_sys_log_json(source, priority, unit, keyword, lines);
    } else if (!strcmp(t, MSG_NET_TRAFFIC)) {
        result = ctc_net_traffic_json();
    } else if (!strcmp(t, MSG_NET_CONN)) {
        result = ctc_net_conn_json();
    } else if (!strcmp(t, MSG_TASK_RUN)) {
        char task_id[128] = "", task_name[256] = "", cmd[2048] = "";
        int timeout_sec = 60;
        if (msg->payload) {
            ctc_json_get_string(msg->payload, "task_id", task_id, sizeof(task_id));
            ctc_json_get_string(msg->payload, "task_name", task_name, sizeof(task_name));
            ctc_json_get_string(msg->payload, "command", cmd, sizeof(cmd));
            timeout_sec = (int)ctc_json_get_int(msg->payload, "timeout_sec", 60);
        }
        if (cmd[0]) {
            char *r = ctc_cmd_exec_json(cmd, timeout_sec, "/bin/bash");
            /* task_result 的 payload 需要把执行结果字段展平到顶层，
             * 服务端按 CmdExecResult 结构解析（ok/exit_code/stdout/...）。 */
            char payload[16384];
            if (r && r[0] == '{') {
                /* r 形如 {"ok":true,"exit_code":0,...}，去掉首尾花括号后拼接 */
                size_t rl = strlen(r);
                if (rl >= 2 && r[0] == '{' && r[rl-1] == '}') {
                    r[rl-1] = 0;
                    snprintf(payload, sizeof(payload),
                        "{\"task_id\":\"%s\",\"executed_at\":%lld,%s}",
                        task_id, (long long)time(NULL) * 1000, r + 1);
                } else {
                    snprintf(payload, sizeof(payload),
                        "{\"task_id\":\"%s\",\"ok\":false,\"exit_code\":-1,\"executed_at\":%lld}",
                        task_id, (long long)time(NULL) * 1000);
                }
            } else {
                snprintf(payload, sizeof(payload),
                    "{\"task_id\":\"%s\",\"ok\":false,\"exit_code\":-1,\"executed_at\":%lld}",
                    task_id, (long long)time(NULL) * 1000);
            }
            ctc_send(fd, MSG_TASK_RESULT, msg->id, NULL, payload);
            free(r);
        }
        return;
    } else if (!strcmp(t, MSG_HARDWARE)) {
        result = ctc_hardware_json();
    } else if (!strcmp(t, MSG_DISK_IO)) {
        result = ctc_disk_io_json();
    } else if (!strcmp(t, MSG_UPGRADE)) {
        char url[1024] = "", sha[128] = "", ver[64] = "";
        if (msg->payload) {
            ctc_json_get_string(msg->payload, "url", url, sizeof(url));
            ctc_json_get_string(msg->payload, "sha256", sha, sizeof(sha));
            ctc_json_get_string(msg->payload, "version", ver, sizeof(ver));
        }
        result = ctc_upgrade_json(url, sha, ver);
    } else if (!strcmp(t, MSG_PF_OPEN)) {
        char fid[64] = "", th[64] = "", lh[64] = "";
        int tp = 0, lp = 0;
        if (msg->payload) {
            ctc_json_get_string(msg->payload, "forward_id", fid, sizeof(fid));
            ctc_json_get_string(msg->payload, "target_host", th, sizeof(th));
            ctc_json_get_string(msg->payload, "local_host", lh, sizeof(lh));
            tp = (int)ctc_json_get_int(msg->payload, "target_port", 0);
            lp = (int)ctc_json_get_int(msg->payload, "local_port", 0);
        }
        if (fid[0] && lp > 0) {
            int rc = ctc_pf_open(fd, fid, th, tp, lh, lp);
            result = rc == 0 ? strdup("{\"ok\":true}") : strdup("{\"error\":\"pf open failed\"}");
        } else {
            result = strdup("{\"error\":\"bad payload\"}");
        }
    } else if (!strcmp(t, MSG_PF_CLOSE)) {
        char fid[64] = "";
        if (msg->payload) ctc_json_get_string(msg->payload, "forward_id", fid, sizeof(fid));
        ctc_pf_close(fid);
        result = strdup("{\"ok\":true}");
    } else if (!strcmp(t, MSG_PF_DATA)) {
        char fid[64] = "", cid[48] = "", b64[65536] = "";
        int closed = 0;
        if (msg->payload) {
            ctc_json_get_string(msg->payload, "forward_id", fid, sizeof(fid));
            ctc_json_get_string(msg->payload, "conn_id", cid, sizeof(cid));
            ctc_json_get_string(msg->payload, "data_base64", b64, sizeof(b64));
            closed = ctc_json_get_bool(msg->payload, "closed", 0);
        }
        if (closed) {
            ctc_pf_conn_close(fid, cid);
        } else {
            size_t dlen = 0;
            unsigned char *data = ctc_base64_decode(b64, &dlen);
            if (data && dlen > 0) {
                ctc_pf_write(fid, cid, data, dlen);
            }
            free(data);
        }
        return;
    } else if (!strcmp(t, MSG_PF_ACCEPT)) {
        char fid[64] = "", cid[48] = "";
        if (msg->payload) {
            ctc_json_get_string(msg->payload, "forward_id", fid, sizeof(fid));
            ctc_json_get_string(msg->payload, "conn_id", cid, sizeof(cid));
        }
        int rc = ctc_pf_accept_conn(fid, cid);
        result = rc == 0 ? strdup("{\"ok\":true}") : strdup("{\"error\":\"pf accept failed\"}");
        return;
    } else if (!strcmp(t, MSG_DOCKER_LIST)) {
        result = ctc_docker_list_json();
    } else if (!strcmp(t, MSG_DOCKER_IMAGES)) {
        result = ctc_docker_images_json();
    } else if (!strcmp(t, MSG_DOCKER_ACTION)) {
        char cid[128] = "", act[32] = "";
        if (msg->payload &&
            ctc_json_get_string(msg->payload, "container_id", cid, sizeof(cid)) &&
            ctc_json_get_string(msg->payload, "action", act, sizeof(act))) {
            result = ctc_docker_action_json(cid, act);
        } else {
            result = strdup("{\"error\":\"bad payload\"}");
        }
    } else if (!strcmp(t, MSG_DOCKER_INSPECT)) {
        char cid[128] = "";
        if (msg->payload) ctc_json_get_string(msg->payload, "container_id", cid, sizeof(cid));
        result = ctc_docker_inspect_json(cid);
    } else if (!strcmp(t, MSG_DOCKER_CREATE)) {
        result = ctc_docker_create_json(msg->payload ? msg->payload : "{}");
    } else if (!strcmp(t, MSG_DOCKER_COMPOSE)) {
        result = ctc_docker_compose_json(msg->payload ? msg->payload : "{}");
    } else if (!strcmp(t, MSG_DOCKER_LOGS)) {
        char cid[128] = "";
        int lines = 200;
        if (msg->payload) {
            ctc_json_get_string(msg->payload, "container_id", cid, sizeof(cid));
            lines = (int)ctc_json_get_int(msg->payload, "lines", 200);
        }
        result = ctc_docker_logs_json(cid, lines);
    } else if (!strcmp(t, MSG_UNINSTALL)) {
        /* 服务端下发的远程卸载：先回 ack，再清理自身 */
        send_log(fd, "warn", "remote uninstall requested by server");
        ctc_send(fd, MSG_ACK, msg->id, NULL, "{\"ok\":true,\"message\":\"uninstalling\"}");
        fprintf(stderr, "[ctc-agent] remote uninstall requested, removing in 2s...\n");
        if (fork() == 0) {
            sleep(2);
            ctc_do_uninstall();
            _exit(0);
        }
        return;
    } else if (!strcmp(t, MSG_CMD_EXEC)) {
        char cmd[2048] = "", shell[64] = "";
        int timeout_sec = 60;
        if (msg->payload &&
            ctc_json_get_string(msg->payload, "command", cmd, sizeof(cmd))) {
            ctc_json_get_string(msg->payload, "shell", shell, sizeof(shell));
            timeout_sec = (int)ctc_json_get_int(msg->payload, "timeout_sec", 60);
            send_log(fd, "info", "remote command executed by server");
            result = ctc_cmd_exec_json(cmd, timeout_sec, shell);
        } else {
            result = strdup("{\"error\":\"bad payload\"}");
        }
    } else if (!strcmp(t, MSG_CTF_LSMOD)) {
        result = ctc_ctforge_exec("lsmod", "");
    } else if (!strcmp(t, MSG_CTF_INSMOD)) {
        char modname[256] = "";
        if (msg->payload &&
            ctc_json_get_string(msg->payload, "module", modname, sizeof(modname))) {
            result = ctc_ctforge_exec("insmod", modname);
        } else {
            result = strdup("{\"error\":\"module name required\"}");
        }
    } else if (!strcmp(t, MSG_CTF_RMMOD)) {
        char modname[256] = "";
        if (msg->payload &&
            ctc_json_get_string(msg->payload, "module", modname, sizeof(modname))) {
            result = ctc_ctforge_exec("rmmod", modname);
        } else {
            result = strdup("{\"error\":\"module name required\"}");
        }
    } else if (!strcmp(t, MSG_CTF_MOD)) {
        char modname[256] = "", subcmd[512] = "";
        if (msg->payload &&
            ctc_json_get_string(msg->payload, "module", modname, sizeof(modname))) {
            ctc_json_get_string(msg->payload, "subcmd", subcmd, sizeof(subcmd));
            char combined[768];
            snprintf(combined, sizeof(combined), "%s %s", modname, subcmd);
            result = ctc_ctforge_exec("mod", combined);
        } else {
            result = strdup("{\"error\":\"module name required\"}");
        }
    } else if (!strcmp(t, MSG_CTF_STATUS)) {
        result = ctc_ctforge_exec("status", "");
    }

    if (result) {
        const char *type;
        if (!strcmp(t, MSG_PING)) type = "pong";
        /* ctforge 桥接响应走透传：payload 已含结构化 JSON，不靠子串匹配 */
        else if (!strncmp(t, "ctf_", 4)) type = MSG_ACK;
        else type = strstr(result, "\"error\"") ? MSG_ERROR : MSG_ACK;
        ctc_send(fd, type, msg->id, msg->session_id, result);
        free(result);
    }
}

int ctc_conn_run(ctc_config *cfg) {
    const char *cfg_path = getenv("CTC_CONFIG");
    if (!cfg_path) cfg_path = "/etc/ctforge-agent.conf";

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    /* 启动局域网发现广播 */
    if (ctc_discover_start(cfg) == 0 && cfg->discover_enable)
        fprintf(stderr, "[ctc-agent] LAN discovery broadcast on udp:%d\n", cfg->discover_port);

    ctc_binding binding;
    ctc_binding_load(&binding, cfg_path);
    if (binding.approved && binding.bound_server_id[0]) {
        snprintf(cfg->bound_server_id, sizeof(cfg->bound_server_id), "%s", binding.bound_server_id);
        snprintf(cfg->bound_server_ip, sizeof(cfg->bound_server_ip), "%s", binding.bound_server_ip);
        cfg->bound_server_port = binding.bound_server_port;
        cfg->approved = 1;
        fprintf(stderr, "[ctc-agent] loaded binding: server_id=%s ip=%s port=%d\n",
                binding.bound_server_id, binding.bound_server_ip, binding.bound_server_port);
    } else {
        fprintf(stderr, "[ctc-agent] no binding yet, will scan for servers\n");
    }

    ctc_anomaly anom;
    ctc_anomaly_init(&anom);

    int backoff = 1;
    while (g_running) {
        /* 未绑定/未批准：先扫描服务端并握手 */
        if (!cfg->approved) {
            ctc_binding got;
            int rc = ctc_scan_server(cfg, &got);
            if (rc == 0) {
                /* 已批准：写入绑定并持久化 */
                snprintf(cfg->bound_server_id, sizeof(cfg->bound_server_id), "%s", got.bound_server_id);
                snprintf(cfg->bound_server_ip, sizeof(cfg->bound_server_ip), "%s",
                         got.bound_server_ip[0] ? got.bound_server_ip : cfg->server_host);
                cfg->bound_server_port = got.bound_server_port ? got.bound_server_port : cfg->server_port;
                cfg->approved = 1;
                ctc_config_save(cfg, cfg_path);
                fprintf(stderr, "[ctc-agent] APPROVED by server %s (%s:%d), binding saved\n",
                        cfg->bound_server_id, cfg->bound_server_ip, cfg->bound_server_port);
                /* 同步配置里的 server_host */
                snprintf(cfg->server_host, sizeof(cfg->server_host), "%s", cfg->bound_server_ip);
                cfg->server_port = cfg->bound_server_port;
            } else if (rc == 1) {
                fprintf(stderr, "[ctc-agent] server found but PENDING approval, retry in 15s\n");
                for (int i = 0; i < 15 && g_running; i++) sleep(1);
                continue;
            } else {
                fprintf(stderr, "[ctc-agent] no server found on LAN, retry in %ds\n", backoff);
                sleep(backoff);
                if (backoff < 30) backoff *= 2;
                continue;
            }
        }

        int fd = connect_server(cfg);
        if (fd < 0) {
            fprintf(stderr, "[ctc-agent] connect %s:%d failed: %s, retry in %ds\n",
                    cfg->server_host, cfg->server_port, strerror(errno), backoff);
            /* 已绑定但连不上：可能服务端换 IP，清标记触发重新扫描 */
            if (cfg->approved) {
                fprintf(stderr, "[ctc-agent] server unreachable, will re-scan (server_id kept)\n");
                /* 保留 bound_server_id，但允许重新扫描（scan 内部会用 id 认亲） */
                cfg->approved = 0;
            }
            sleep(backoff);
            if (backoff < 30) backoff *= 2;
            continue;
        }
        backoff = 1;
        if (do_register(fd, cfg) < 0) {
            fprintf(stderr, "[ctc-agent] register failed — server may have lost approval, re-scanning\n");
            ctc_tls_cleanup(fd);
            close(fd);
            /* 服务端重启后内存状态可能丢失：清本地 approved，重新走扫描+握手 */
            cfg->approved = 0;
            ctc_config_save(cfg, cfg_path);
            sleep(5);
            continue;
        }
        fprintf(stderr, "[ctc-agent] connected to %s:%d as %s\n",
                cfg->server_host, cfg->server_port, cfg->agent_id);
        send_log(fd, "info", "agent connected");

        time_t last_hb = 0, last_metrics = 0;
        while (g_running) {
            time_t now = time(NULL);
            if (now - last_hb >= cfg->heartbeat_interval) {
                ctc_metrics m;
                ctc_collect_metrics(&m);
                send_heartbeat(fd, &m);
                last_hb = now;
            }
            if (now - last_metrics >= cfg->metrics_interval) {
                ctc_metrics m;
                ctc_collect_metrics(&m);
                send_metrics(fd, &m);
                last_metrics = now;
            }
            /* 异常检测（内部有节流） */
            ctc_anomaly_check(fd, &anom);
            /* 非阻塞收命令 */
            fd_set rfds;
            FD_ZERO(&rfds); FD_SET(fd, &rfds);
            struct timeval tv = {1, 0};
            int r = select(fd + 1, &rfds, NULL, NULL, &tv);
            if (r > 0 && FD_ISSET(fd, &rfds)) {
                ctc_message msg = {0};
                if (ctc_recv(fd, &msg) < 0) {
                    fprintf(stderr, "[ctc-agent] connection lost\n");
                    break;
                }
                handle_command(fd, &msg, cfg, cfg_path);
                ctc_message_free(&msg);
            } else if (r < 0 && errno != EINTR) {
                break;
            }
        }
        ctc_term_close_all();
        ctc_pf_close_all();
        send_log(fd, "info", "agent disconnected");
        /* 优雅下线通知（服务端据此立即记离线事件） */
        ctc_send_simple(fd, MSG_OFFLINE, "{}");
        ctc_tls_cleanup(fd);
        close(fd);
        if (g_running) sleep(3);
    }
    ctc_discover_stop();
    return 0;
}

int main(int argc, char **argv) {
    /* 配置文件多路径候选：新安装脚本写 /etc/ctforge/agent.conf，旧路径兼容 */
    static const char *cfg_candidates[] = {
        "/etc/ctforge/agent.conf",
        "/etc/ctforge-agent.conf",
        NULL
    };
    const char *cfg_path = "/etc/ctforge-agent.conf";
    const char *force_server = NULL;
    const char *force_port = NULL;
    int i = 1;
    while (i < argc) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            cfg_path = argv[i + 1];
            i += 2;
        } else if (!strcmp(argv[i], "--server") && i + 1 < argc) {
            force_server = argv[i + 1];
            i += 2;
        } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            force_port = argv[i + 1];
            i += 2;
        } else if (!strcmp(argv[i], "--uninstall")) {
            i++;
            /* 保留给后面处理 */
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("用法: ctforge-agent [选项]\n");
            printf("  -c PATH          指定配置文件（默认 /etc/ctforge-agent.conf）\n");
            printf("  --server HOST    强制指定服务端 IP/域名（覆盖配置，用于 OpenWrt 等无默认网关场景）\n");
            printf("  --port PORT      强制指定服务端端口（默认 9000）\n");
            printf("  --uninstall      卸载客户端（需服务端批准）\n");
            return 0;
        } else {
            i++;
        }
    }

    /* 若用户没显式 -c，自动探测存在的配置路径 */
    {
        int explicit_cfg = 0;
        for (int j = 1; j < argc - 1; j++) {
            if (!strcmp(argv[j], "-c")) explicit_cfg = 1;
        }
        if (!explicit_cfg) {
            FILE *f;
            for (int j = 0; cfg_candidates[j]; j++) {
                f = fopen(cfg_candidates[j], "r");
                if (f) { fclose(f); cfg_path = cfg_candidates[j]; break; }
            }
        }
    }
    ctc_config cfg;
    if (ctc_config_load(&cfg, cfg_path) < 0) {
        fprintf(stderr, "[ctc-agent] no config at %s, using defaults\n", cfg_path);
    } else {
        fprintf(stderr, "[ctc-agent] config loaded from %s\n", cfg_path);
    }
    /* 命令行参数覆盖配置文件 */
    if (force_server) {
        strncpy(cfg.server_host, force_server, sizeof(cfg.server_host) - 1);
        fprintf(stderr, "[ctc-agent] server overridden by --server: %s\n", cfg.server_host);
    }
    if (force_port) {
        cfg.server_port = atoi(force_port);
        fprintf(stderr, "[ctc-agent] port overridden by --port: %d\n", cfg.server_port);
    }
    setenv("CTC_CONFIG", cfg_path, 1);

    /* 卸载入口：必须获得服务端批准 */
    if (argc > 1 && !strcmp(argv[1], "--uninstall")) {
        fprintf(stderr, "[ctc-agent] requesting uninstall approval from server...\n");
        int rc = ctc_uninstall_request(&cfg);
        if (rc == 0) {
            fprintf(stderr, "[ctc-agent] uninstall APPROVED, removing...\n");
            ctc_do_uninstall();
            fprintf(stderr, "[ctc-agent] uninstalled\n");
            return 0;
        }
        if (rc == 1) {
            fprintf(stderr, "[ctc-agent] uninstall REJECTED by administrator — agent stays running\n");
            return 1;
        }
        fprintf(stderr, "[ctc-agent] cannot reach server — uninstall blocked, agent stays running\n");
        return 2;
    }

    return ctc_conn_run(&cfg);
}
