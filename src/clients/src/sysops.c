#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
/* 系统账户管理 / 系统日志 / 网络监控 / 远程命令执行
 * 部分操作需 root 权限（改密码、锁定账户）；非 root 时返回明确错误。
 */
#include "ctforge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <shadow.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ---- 复用 sysadmin.c 中同类工具（避免重复定义，这里用 static 前缀区分） ---- */
/* 把命令整体转义进单引号里，避免 bash -c 只取第一个词 */
static char *xrun_cmd(const char *cmd, int timeout_sec) {
    char wrapped[8192];
    size_t qcap = strlen(cmd) * 2 + 128;
    char *q = malloc(qcap);
    if (!q) return NULL;
    size_t j = 0;
    for (const char *p = cmd; *p; p++) {
        if (*p == '\'') { q[j++] = '\''; q[j++] = '\\'; q[j++] = '\''; }
        else q[j++] = *p;
    }
    q[j] = 0;
    snprintf(wrapped, sizeof(wrapped), "timeout %d bash -c '%s' 2>/dev/null", timeout_sec, q);
    free(q);
    if (strlen(wrapped) >= sizeof(wrapped)) return NULL;
    FILE *f = popen(wrapped, "r");
    if (!f) return NULL;
    size_t cap = 16384, n = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(f); return NULL; }
    size_t r;
    while ((r = fread(buf + n, 1, cap - n - 1, f)) > 0) {
        n += r;
        if (n + 1 >= cap) {
            if (cap > 8 * 1024 * 1024) break;
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
    }
    pclose(f);
    buf[n] = 0;
    return buf;
}

static char *xesc(const char *s) {
    if (!s) return strdup("");
    size_t cap = strlen(s) * 2 + 2, n = 0;
    char *out = malloc(cap);
    if (!out) return strdup("");
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = c; }
        else if (c == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if (c == '\t') { out[n++] = '\\'; out[n++] = 't'; }
        else if (c == '\r') { /* skip */ }
        else if ((unsigned char)c < 0x20) { out[n++] = ' '; }
        else out[n++] = c;
    }
    out[n] = 0;
    return out;
}

typedef struct { char *p; size_t n, cap; } xbuf;
static void xb_init(xbuf *b) { b->cap = 8192; b->n = 0; b->p = malloc(b->cap); b->p[0] = 0; }
static void xb_put(xbuf *b, const char *s) {
    size_t l = strlen(s);
    while (b->n + l + 1 > b->cap) { b->cap *= 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, l + 1);
    b->n += l;
}

static int is_root(void) { return geteuid() == 0; }

static int safe_name(const char *s) {
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') return 0;
    return 1;
}

/* ================= 系统账户管理 ================= */

char *ctc_sys_user_list_json(void) {
    char *out = xrun_cmd("getent passwd", 10);
    if (!out) return strdup("{\"error\":\"getent failed\"}");

    /* 读取最近登录信息 */
    char *lastout = xrun_cmd("last -n 200 2>/dev/null", 10);

    xbuf b; xb_init(&b);
    xb_put(&b, "{\"users\":[");
    int first = 1;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        /* name:x:uid:gid:gecos:home:shell */
        char name[128] = {0}, passwd[64] = {0}, home[512] = {0}, shell[256] = {0};
        int uid = 0, gid = 0;
        /* getent 格式: name:x:uid:gid:gecos:home:shell（gecos 可能为空） */
        char *seg[8] = {0};
        int nseg = 0;
        char *p = line;
        char *start = p;
        while (nseg < 8) {
            if (*p == ':' || *p == 0) {
                char save_c = *p;
                *p = 0;
                seg[nseg++] = start;
                if (save_c == 0) break;
                start = p + 1;
            }
            p++;
        }
        if (nseg < 7) continue;
        snprintf(name, sizeof(name), "%s", seg[0]);
        snprintf(passwd, sizeof(passwd), "%s", seg[1]);
        uid = atoi(seg[2]);
        gid = atoi(seg[3]);
        snprintf(home, sizeof(home), "%s", seg[5]);
        snprintf(shell, sizeof(shell), "%s", seg[6]);
        /* 只展示可登录账户：uid >= 1000，或 root（uid 0） */
        int is_normal = (uid >= 1000) || (uid == 0);
        int has_shell = strstr(shell, "nologin") == NULL && strstr(shell, "false") == NULL;
        if (!is_normal || !has_shell) continue;

        /* 判断是否锁定：passwd 字段以 ! 或 * 开头 */
        int locked = (passwd[0] == '!' || passwd[0] == '*');

        /* 查最近登录 */
        char lastlogin[64] = "";
        if (lastout) {
            char pat[160];
            snprintf(pat, sizeof(pat), "%s ", name);
            char *pos = strstr(lastout, pat);
            if (pos && pos != lastout) {
                /* 回退到行首，取前 24 字符作时间 */
                char *ls = pos;
                while (ls > lastout && *(ls - 1) != '\n') ls--;
                size_t l = (size_t)(pos - ls);
                if (l > 0 && l < sizeof(lastlogin)) {
                    memcpy(lastlogin, ls, l);
                    lastlogin[l] = 0;
                }
            }
        }

        char *e_name = xesc(name), *e_home = xesc(home), *e_shell = xesc(shell), *e_last = xesc(lastlogin);
        char item[1024];
        snprintf(item, sizeof(item),
            "%s{\"username\":\"%s\",\"uid\":%d,\"gid\":%d,\"home\":\"%s\",\"shell\":\"%s\",\"locked\":%s,\"last_login\":\"%s\"}",
            first ? "" : ",", e_name, uid, gid, e_home, e_shell, locked ? "true" : "false", e_last);
        xb_put(&b, item);
        free(e_name); free(e_home); free(e_shell); free(e_last);
        first = 0;
    }
    free(out);
    free(lastout);
    xb_put(&b, "]}");
    return b.p;
}

/* 修改系统用户密码：通过 chpasswd 从 stdin 喂入，避免密码出现在命令行 */
char *ctc_sys_user_passwd_json(const char *username, const char *new_password) {
    if (!safe_name(username))
        return strdup("{\"error\":\"invalid username\"}");
    if (!new_password || strlen(new_password) < 6)
        return strdup("{\"error\":\"password too short (min 6)\"}");
    if (!is_root())
        return strdup("{\"error\":\"agent must run as root to change passwords\"}");

    int pipefd[2];
    if (pipe(pipefd) < 0) return strdup("{\"error\":\"pipe failed\"}");

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return strdup("{\"error\":\"fork failed\"}");
    }
    if (pid == 0) {
        /* 子进程：chpasswd 从 stdin 读取 "user:password" */
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        int devnull = open("/dev/null", 1);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); close(devnull); }
        execlp("chpasswd", "chpasswd", (char *)NULL);
        _exit(127);
    }
    close(pipefd[0]);
    char line[512];
    int len = snprintf(line, sizeof(line), "%s:%s\n", username, new_password);
    ssize_t w = write(pipefd[1], line, (size_t)len);
    /* 立即清零内存中的密码 */
    memset(line, 0, sizeof(line));
    close(pipefd[1]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (w != len) return strdup("{\"error\":\"write failed\"}");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return strdup("{\"error\":\"chpasswd failed (user not found or permission denied)\"}");
    return strdup("{\"ok\":true,\"message\":\"password updated\"}");
}

/* 锁定/解锁账户：锁定用 usermod -L，解锁用 -U */
char *ctc_sys_user_lock_json(const char *username, int lock) {
    if (!safe_name(username))
        return strdup("{\"error\":\"invalid username\"}");
    if (!is_root())
        return strdup("{\"error\":\"agent must run as root to lock/unlock accounts\"}");
    /* 不允许锁定 root */
    if (!strcmp(username, "root"))
        return strdup("{\"error\":\"refusing to lock root account\"}");

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "/usr/sbin/usermod -%c '%s' 2>&1", lock ? 'L' : 'U', username);
    char *out = xrun_cmd(cmd, 15);
    int ok = (out && !*out);
    char *e_out = xesc(out ? out : "");
    free(out);
    char *res = malloc(strlen(e_out) + 128);
    snprintf(res, strlen(e_out) + 128,
        "{\"ok\":%s,\"message\":\"%s\"}", ok ? "true" : "false", e_out);
    free(e_out);
    return res;
}

/* ================= 系统日志 ================= */

/* 解析 "2026-11-02T10:30:00+0900 hostname unit[pid]: message" 格式 */
char *ctc_sys_log_json(const char *source, const char *priority, const char *unit,
                       const char *keyword, int lines) {
    if (lines <= 0 || lines > 500) lines = 100;

    char cmd[2048];
    int has_journal = (access("/usr/bin/journalctl", X_OK) == 0 ||
                       access("/bin/journalctl", X_OK) == 0);

    if (has_journal && (!source || !strcmp(source, "journald") || !*source)) {
        char filter[512] = "";
        if (priority && *priority) {
            char p[64];
            snprintf(p, sizeof(p), " -p %s", priority);
            strncat(filter, p, sizeof(filter) - strlen(filter) - 1);
        }
        if (unit && *unit && safe_name(unit)) {
            strncat(filter, " -u ", sizeof(filter) - strlen(filter) - 1);
            strncat(filter, unit, sizeof(filter) - strlen(filter) - 1);
        }
        char safe_kw[256] = "";
        if (keyword && *keyword) {
            /* 只保留安全字符，防注入 */
            size_t j = 0;
            for (const char *q = keyword; *q && j < sizeof(safe_kw) - 3; q++) {
                if (isalnum((unsigned char)*q) || *q == ' ' || *q == '-' || *q == '_' ||
                    *q == '.' || *q == '/' || *q == ':' || *q == '=') {
                    safe_kw[j++] = *q;
                }
            }
            safe_kw[j] = 0;
        }
        if (safe_kw[0]) {
            snprintf(cmd, sizeof(cmd),
                "journalctl --no-pager -o short-iso %s -n %d 2>/dev/null | grep -F -- '%s' | tail -%d",
                filter, lines * 5, safe_kw, lines);
        } else {
            snprintf(cmd, sizeof(cmd),
                "journalctl --no-pager -o short-iso %s -n %d 2>/dev/null", filter, lines);
        }
    } else {
        /* 文件日志回退：/var/log/messages 或 /var/log/syslog */
        const char *file = "/var/log/messages";
        if (access(file, R_OK) != 0) file = "/var/log/syslog";
        if (access(file, R_OK) != 0) return strdup("{\"entries\":[],\"total\":0}");

        char safe_kw[256] = "";
        if (keyword && *keyword) {
            size_t j = 0;
            for (const char *q = keyword; *q && j < sizeof(safe_kw) - 3; q++)
                if (isalnum((unsigned char)*q) || *q == ' ' || *q == '-' || *q == '_' || *q == '.')
                    safe_kw[j++] = *q;
            safe_kw[j] = 0;
        }
        if (safe_kw[0])
            snprintf(cmd, sizeof(cmd), "grep -F -- '%s' %s 2>/dev/null | tail -%d", safe_kw, file, lines);
        else
            snprintf(cmd, sizeof(cmd), "tail -%d %s 2>/dev/null", lines, file);
    }

    char *out = xrun_cmd(cmd, 30);
    if (!out) out = strdup("");

    xbuf b; xb_init(&b);
    xb_put(&b, "{\"entries\":[");
    int first = 1, total = 0;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!*line) continue;
        if (line[0] == '-' && strstr(line, "Boot")) continue; /* 跳过 journald 分隔行 */
        /* short-iso: 2026-11-02T10:30:00+09:00 hostname ident[pid]: msg */
        char ts[64] = {0}, host[128] = {0}, ident[256] = {0};
        char *msg = line;
        int consumed = 0;
        /* 先取时间戳和主机名 */
        if (sscanf(line, "%63s %127s %n", ts, host, &consumed) >= 2 && consumed > 0) {
            char *rest = line + consumed;
            /* 取 ident（到第一个 ':' 为止，且长度合理）*/
            char *colon = strchr(rest, ':');
            if (colon && (size_t)(colon - rest) < 200) {
                size_t il = (size_t)(colon - rest);
                memcpy(ident, rest, il);
                ident[il] = 0;
                /* 去掉 ident 尾部的 [pid] 部分 */
                char *bracket = strchr(ident, '[');
                if (bracket) *bracket = 0;
                msg = colon + 1;
                while (*msg == ' ') msg++;
            } else {
                msg = rest;
            }
        }

        /* 解析 ISO 时间 */
        long long ms = 0;
        struct tm tm = {0};
        if (strptime(ts, "%Y-%m-%dT%H:%M:%S", &tm))
            ms = (long long)mktime(&tm) * 1000;

        /* 优先级推断：包含 error/fail 等关键词 */
        const char *prio = "info";
        if (strcasestr(line, "error") || strcasestr(line, "failed") || strcasestr(line, "fatal"))
            prio = "err";
        else if (strcasestr(line, "warn"))
            prio = "warning";
        else if (strcasestr(line, "debug"))
            prio = "debug";

        char *e_ident = xesc(ident), *e_msg = xesc(msg);
        char item[2048];
        snprintf(item, sizeof(item),
            "%s{\"timestamp\":%lld,\"priority\":\"%s\",\"unit\":\"%s\",\"message\":\"%s\"}",
            first ? "" : ",", ms, prio, e_ident, e_msg);
        xb_put(&b, item);
        free(e_ident); free(e_msg);
        first = 0; total++;
    }
    free(out);
    char tail[64];
    snprintf(tail, sizeof(tail), "],\"total\":%d}", total);
    xb_put(&b, tail);
    return b.p;
}

/* ================= 网络流量 ================= */

char *ctc_net_traffic_json(void) {
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return strdup("{\"interfaces\":[]}");

    xbuf b; xb_init(&b);
    struct timespec tsp;
    clock_gettime(CLOCK_REALTIME, &tsp);
    char head[128];
    snprintf(head, sizeof(head), "{\"timestamp\":%lld,\"interfaces\":[",
             (long long)tsp.tv_sec * 1000 + tsp.tv_nsec / 1000000);
    xb_put(&b, head);

    char line[512];
    int first = 1;
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        if (++lineno <= 2) continue; /* 跳过两行表头 */
        char name[64] = {0};
        /* /proc/net/dev 每行: name: rx_bytes rx_packets rx_errs rx_drop rx_fifo rx_frame
         *                            rx_compressed rx_multicast
         *                            tx_bytes tx_packets tx_errs tx_drop tx_fifo tx_colls
         *                            tx_carrier tx_compressed */
        unsigned long long v[16] = {0};
        int n = sscanf(line,
            " %63[^:]: %llu %llu %llu %llu %llu %llu %llu %llu"
            " %llu %llu %llu %llu %llu %llu %llu %llu",
            name, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
            &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15]);
        if (n < 17) continue;
        unsigned long long rx_b = v[0], rx_p = v[1], rx_e = v[2];
        unsigned long long tx_b = v[8], tx_p = v[9], tx_e = v[10];
        /* 跳过 lo（回环）与非物理接口的零流量 */
        if (!strcmp(name, "lo")) continue;

        /* 判断接口是否 up：读 /sys/class/net/NAME/operstate */
        char sp[128], state[32] = "unknown";
        snprintf(sp, sizeof(sp), "/sys/class/net/%s/operstate", name);
        FILE *sf = fopen(sp, "r");
        if (sf) { if (fgets(state, sizeof(state), sf)) {
            char *nl = strchr(state, '\n'); if (nl) *nl = 0;
        } fclose(sf); }

        char *e_name = xesc(name);
        char item[512];
        snprintf(item, sizeof(item),
            "%s{\"name\":\"%s\",\"rx_bytes\":%llu,\"rx_packets\":%llu,\"rx_errs\":%llu,"
            "\"tx_bytes\":%llu,\"tx_packets\":%llu,\"tx_errs\":%llu,\"up\":%s}",
            first ? "" : ",", e_name, rx_b, rx_p, rx_e, tx_b, tx_p, tx_e,
            strstr(state, "up") ? "true" : "false");
        xb_put(&b, item);
        free(e_name);
        first = 0;
    }
    fclose(f);
    xb_put(&b, "]}");
    return b.p;
}

/* ================= 网络连接状态 ================= */

char *ctc_net_conn_json(void) {
    /* 用 ss 拿连接状态，解析 ESTABLISHED/TIME_WAIT 等 */
    char *out = xrun_cmd("ss -tan 2>/dev/null | tail -n +2", 15);
    if (!out || !*out) {
        free(out);
        out = xrun_cmd("netstat -tan 2>/dev/null | tail -n +3", 15);
    }

    int states[16] = {0};
    /* ss/netstat 输出的状态名（含缩写形式，统一映射到索引） */
    typedef struct { const char *name; int idx; } state_alias;
    const state_alias aliases[] = {
        {"ESTAB", 0}, {"ESTABLISHED", 0},
        {"SYN-SENT", 1}, {"SYN_SENT", 1}, {"SYN-SENT", 1},
        {"SYN-RECV", 2}, {"SYN_RECV", 2},
        {"FIN-WAIT-1", 3}, {"FIN_WAIT1", 3},
        {"FIN-WAIT-2", 4}, {"FIN_WAIT2", 4},
        {"TIME-WAIT", 5}, {"TIME_WAIT", 5},
        {"CLOSE", 6},
        {"CLOSE-WAIT", 7}, {"CLOSE_WAIT", 7},
        {"LAST-ACK", 8}, {"LAST_ACK", 8},
        {"LISTEN", 9},
        {"CLOSING", 10},
        {NULL, 11}
    };

    /* 统计远端地址频次（简单哈希表） */
    typedef struct { char addr[64]; int count; } remote_t;
    remote_t remotes[128];
    int remote_cnt = 0;
    int total_tcp = 0;

    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char state[32] = {0}, local[128] = {0}, peer[128] = {0};
        /* ss 输出列序: State Recv-Q Send-Q Local:Port Peer:Port */
        int nf = sscanf(line, "%31s %*s %*s %127s %127s", state, local, peer);
        if (nf < 3) continue;
        if (!strcmp(state, "State") || !strcmp(state, "Netid")) continue;
        total_tcp++;
        int idx = 11; /* UNKNOWN */
        for (size_t i = 0; aliases[i].name; i++) {
            if (!strcmp(state, aliases[i].name)) { idx = aliases[i].idx; break; }
        }
        states[idx]++;

        if ((!strcmp(state, "ESTAB") || !strcmp(state, "ESTABLISHED")) && strcmp(peer, "*:*") && strcmp(peer, "0.0.0.0:*") && strcmp(peer, "[::]:*")) {
            /* 提取 IP（去掉端口） */
            char ip[64] = {0};
            snprintf(ip, sizeof(ip), "%s", peer);
            char *colon = strrchr(ip, ':');
            if (colon) *colon = 0;
            /* 找已有记录 */
            int hit = -1;
            for (int i = 0; i < remote_cnt; i++)
                if (!strcmp(remotes[i].addr, ip)) { hit = i; break; }
            if (hit >= 0) remotes[hit].count++;
            else if (remote_cnt < 128) {
                snprintf(remotes[remote_cnt].addr, sizeof(remotes[remote_cnt].addr), "%s", ip);
                remotes[remote_cnt].count = 1;
                remote_cnt++;
            }
        }
    }
    free(out);

    /* UDP 数量 */
    char *udpout = xrun_cmd("ss -uan 2>/dev/null | tail -n +2 | wc -l", 10);
    int udp_count = udpout ? atoi(udpout) : 0;
    free(udpout);

    /* 排序远端取 top 10（简单插入排序） */
    for (int i = 0; i < remote_cnt; i++)
        for (int j = i + 1; j < remote_cnt; j++)
            if (remotes[j].count > remotes[i].count) {
                remote_t t = remotes[i]; remotes[i] = remotes[j]; remotes[j] = t;
            }

    xbuf b; xb_init(&b);
    struct timespec tsp;
    clock_gettime(CLOCK_REALTIME, &tsp);
    char head[256];
    snprintf(head, sizeof(head), "{\"timestamp\":%lld,\"total_tcp\":%d,\"udp_count\":%d,\"tcp_states\":{",
             (long long)tsp.tv_sec * 1000 + tsp.tv_nsec / 1000000, total_tcp, udp_count);
    xb_put(&b, head);

    int sfirst = 1;
    const char *keys[] = {"ESTABLISHED","SYN-SENT","SYN-RECV","FIN-WAIT-1","FIN-WAIT-2",
                          "TIME-WAIT","CLOSE","CLOSE-WAIT","LAST-ACK","LISTEN","CLOSING","UNKNOWN"};
    for (int i = 0; i < 12; i++) {
        if (states[i] == 0) continue;
        char item[128];
        snprintf(item, sizeof(item), "%s\"%s\":%d", sfirst ? "" : ",", keys[i], states[i]);
        xb_put(&b, item);
        sfirst = 0;
    }
    xb_put(&b, "},\"top_remotes\":[");
    int rfirst = 1;
    for (int i = 0; i < remote_cnt && i < 10; i++) {
        char *e_addr = xesc(remotes[i].addr);
        char item[256];
        snprintf(item, sizeof(item), "%s{\"address\":\"%s\",\"count\":%d}",
                 rfirst ? "" : ",", e_addr, remotes[i].count);
        xb_put(&b, item);
        free(e_addr);
        rfirst = 0;
    }
    xb_put(&b, "],\"top_processes\":[]}");
    return b.p;
}

/* ================= 远程命令执行 ================= */

/* 执行命令，同步等待，超时用 timeout 命令保证 */
char *ctc_cmd_exec_json(const char *command, int timeout_sec, const char *shell) {
    if (!command || !*command) return strdup("{\"ok\":false,\"exit_code\":-1,\"stdout\":\"\",\"stderr\":\"\",\"duration_ms\":0}");
    if (timeout_sec <= 0 || timeout_sec > 300) timeout_sec = 60;
    if (!shell || !*shell) shell = "/bin/bash";

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int outpipe[2], errpipe[2];
    if (pipe(outpipe) < 0 || pipe(errpipe) < 0) 
        return strdup("{\"ok\":false,\"exit_code\":-1,\"stdout\":\"\",\"stderr\":\"pipe failed\",\"duration_ms\":0}");

    pid_t pid = fork();
    if (pid < 0) {
        close(outpipe[0]); close(outpipe[1]); close(errpipe[0]); close(errpipe[1]);
        return strdup("{\"ok\":false,\"exit_code\":-1,\"stdout\":\"\",\"stderr\":\"fork failed\",\"duration_ms\":0}");
    }
    if (pid == 0) {
        close(outpipe[0]); close(errpipe[0]);
        dup2(outpipe[1], STDOUT_FILENO);
        dup2(errpipe[1], STDERR_FILENO);
        close(outpipe[1]); close(errpipe[1]);
        /* 独立进程组，便于超时后 kill 整组 */
        setpgid(0, 0);
        execl(shell, shell, "-c", command, (char *)NULL);
        _exit(127);
    }
    close(outpipe[1]); close(errpipe[1]);

    /* 读 stdout/stderr */
    xbuf ob, eb;
    xb_init(&ob); xb_init(&eb);
    int timed_out = 0;
    char buf[4096];
    ssize_t r;

    /* 设置非阻塞 + select 同时读两个管道 */
    fd_set rfds;
    int ofd = outpipe[0], efd = errpipe[0];
    int maxfd = (ofd > efd ? ofd : efd) + 1;
    /* deadline 用单调时钟，timeout 即硬上限（不再额外缓冲） */
    struct timespec tstart;
    clock_gettime(CLOCK_MONOTONIC, &tstart);
    long deadline_ms = timeout_sec * 1000L;

    while (ofd >= 0 || efd >= 0) {
        struct timespec tnow;
        clock_gettime(CLOCK_MONOTONIC, &tnow);
        long elapsed_ms = (tnow.tv_sec - tstart.tv_sec) * 1000L +
                          (tnow.tv_nsec - tstart.tv_nsec) / 1000000L;
        if (elapsed_ms >= deadline_ms) {
            /* 超时：杀掉整个进程组（子进程已 setpgid(0,0)，组号即其 pid） */
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            timed_out = 1;
            break;
        }
        FD_ZERO(&rfds);
        if (ofd >= 0) FD_SET(ofd, &rfds);
        if (efd >= 0) FD_SET(efd, &rfds);
        struct timeval tv = {1, 0};
        int sel = select(maxfd, &rfds, NULL, NULL, &tv);
        if (sel <= 0) continue;
        if (ofd >= 0 && FD_ISSET(ofd, &rfds)) {
            r = read(ofd, buf, sizeof(buf) - 1);
            if (r > 0) { buf[r] = 0; xb_put(&ob, buf); }
            else { close(ofd); ofd = -1; }
        }
        if (efd >= 0 && FD_ISSET(efd, &rfds)) {
            r = read(efd, buf, sizeof(buf) - 1);
            if (r > 0) { buf[r] = 0; xb_put(&eb, buf); }
            else { close(efd); efd = -1; }
        }
    }
    if (ofd >= 0) close(ofd);
    if (efd >= 0) close(efd);

    int status = 0;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (WIFSIGNALED(status)) exit_code = 128 + WTERMSIG(status);
    if (timed_out) exit_code = 124; /* 与 GNU timeout 一致：超时退出码 124 */

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long dur = (long long)(t1.tv_sec - t0.tv_sec) * 1000 +
                    (t1.tv_nsec - t0.tv_nsec) / 1000000;

    /* 限制输出大小 */
    if (ob.n > 128 * 1024) ob.p[128 * 1024] = 0, ob.n = 128 * 1024;
    if (eb.n > 32 * 1024) eb.p[32 * 1024] = 0, eb.n = 32 * 1024;

    char *e_out = xesc(ob.p), *e_err = xesc(eb.p);
    free(ob.p); free(eb.p);
    size_t cap = strlen(e_out) + strlen(e_err) + 256;
    char *res = malloc(cap);
    int ok_final = !timed_out && exit_code == 0;
    snprintf(res, cap,
        "{\"ok\":%s,\"exit_code\":%d,\"stdout\":\"%s\",\"stderr\":\"%s\",\"duration_ms\":%lld,\"timed_out\":%s}",
        ok_final ? "true" : "false", exit_code, e_out, e_err, dur, timed_out ? "true" : "false");
    free(e_out); free(e_err);
    return res;
}
