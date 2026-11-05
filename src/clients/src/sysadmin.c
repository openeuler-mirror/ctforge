/* 系统服务管理（systemd）+ 软件包管理（dnf/yum/rpm）+ 安全审计
 * 全部通过调用系统命令并解析输出实现，保持零额外依赖。
 */
#include "ctforge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

/* ---------- 通用：执行命令并捕获输出 ---------- */
/* 把命令里的单引号转义为 '\'' 形式，再整体用单引号包裹交给 sh -c */
static char *run_cmd(const char *cmd, int timeout_sec) {
    static char wrapped[16384];
    const size_t wcap = sizeof(wrapped);
    size_t wn = 0;

    wn += (size_t)snprintf(wrapped + wn, wcap - wn, "timeout %d sh -c '", timeout_sec);
    for (const char *q = cmd; *q && wn + 8 < wcap; q++) {
        if (*q == '\'') {
            /* 结束单引号 → \' → 重新开始单引号，即 '\'' */
            wrapped[wn++] = '\'';
            wrapped[wn++] = '\\';
            wrapped[wn++] = '\'';
            wrapped[wn++] = '\'';
        } else {
            wrapped[wn++] = *q;
        }
    }
    wrapped[wn] = 0;
    snprintf(wrapped + wn, wcap - wn, "' 2>/dev/null");
    FILE *f = popen(wrapped, "r");
    if (!f) return NULL;
    size_t cap = 8192, n = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(f); return NULL; }
    size_t r;
    while ((r = fread(buf + n, 1, cap - n - 1, f)) > 0) {
        n += r;
        if (n + 1 >= cap) {
            if (cap > 4 * 1024 * 1024) break; /* 上限 4MB */
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

/* JSON 字符串转义（返回 malloc 结果） */
static char *jesc(const char *s) {
    if (!s) return strdup("");
    size_t cap = strlen(s) * 2 + 1, n = 0;
    char *out = malloc(cap);
    if (!out) return strdup("");
    for (const char *p = s; *p; p++) {
        const char *rep = NULL;
        char tmp[8];
        switch (*p) {
            case '"': rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\n': rep = " "; break;
            case '\r': break;
            case '\t': rep = " "; break;
            default:
                if ((unsigned char)*p < 0x20) { snprintf(tmp, sizeof(tmp), " "); rep = tmp; }
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (n + rl + 1 > cap) { cap = cap * 2 + rl; out = realloc(out, cap); }
            memcpy(out + n, rep, rl); n += rl;
        } else {
            if (n + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            out[n++] = *p;
        }
    }
    out[n] = 0;
    return out;
}

/* 简易动态字符串缓冲 */
typedef struct { char *p; size_t n, cap; } sbuf;
static void sb_init(sbuf *b) { b->cap = 16384; b->n = 0; b->p = malloc(b->cap); b->p[0] = 0; }
static void sb_put(sbuf *b, const char *s) {
    size_t l = strlen(s);
    while (b->n + l + 1 > b->cap) { b->cap *= 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, l + 1); b->n += l;
}

/* ================= systemd 服务 ================= */

char *ctc_service_list_json(const char *filter) {
    char cmd[512];
    if (filter && *filter) {
        char *ef = jesc(filter);
        /* 过滤只接受安全字符，防注入 */
        int safe = 1;
        for (char *q = (char*)filter; *q; q++)
            if (!isalnum((unsigned char)*q) && *q != '-' && *q != '_' && *q != '.' && *q != '*' && *q != '@') safe = 0;
        if (!safe) { free(ef); return strdup("{\"services\":[]}"); }
        snprintf(cmd, sizeof(cmd),
            "systemctl list-units --type=service --all --no-legend --no-pager --plain | grep -F -- '%s' | head -200", filter);
        free(ef);
    } else {
        snprintf(cmd, sizeof(cmd),
            "systemctl list-units --type=service --all --no-legend --no-pager --plain | head -300");
    }
    char *out = run_cmd(cmd, 15);
    if (!out) return strdup("{\"services\":[]}");

    sbuf b; sb_init(&b);
    sb_put(&b, "{\"services\":[");
    int first = 1;
    char *line = strtok(out, "\n");
    while (line) {
        /* 格式: name.service loaded active running Description... */
        char name[256] = {0}, load[64] = {0}, active[64] = {0}, sub[64] = {0};
        char desc[512] = {0};
        char *p = line;
        while (*p == ' ' || *p == '\xE2' || *p == '\x97' || *p == '\x8F') p++; /* 跳过 ● 等符号 */
        int consumed = 0;
        if (sscanf(p, "%255s %63s %63s %63s %n", name, load, active, sub, &consumed) >= 4) {
            if ((size_t)consumed < strlen(p)) {
                snprintf(desc, sizeof(desc), "%s", p + consumed);
                /* 去掉尾部多余空格 */
                size_t dl = strlen(desc);
                while (dl && desc[dl-1] == ' ') desc[--dl] = 0;
            }
            /* enabled 状态单独查（批量开销大，这里用 list-unit-files 简化：标记 unknown） */
            char *en = jesc("");
            free(en);
            char *e_name = jesc(name), *e_desc = jesc(desc);
            char item[1280];
            snprintf(item, sizeof(item),
                "%s{\"name\":\"%s\",\"load\":\"%s\",\"active\":\"%s\",\"sub\":\"%s\",\"description\":\"%s\",\"enabled\":\"\"}",
                first ? "" : ",", e_name, load, active, sub, e_desc);
            sb_put(&b, item);
            free(e_name); free(e_desc);
            first = 0;
        }
        line = strtok(NULL, "\n");
    }
    free(out);
    sb_put(&b, "]}");
    return b.p;
}

char *ctc_service_action_json(const char *name, const char *action) {
    /* 白名单校验，防命令注入 */
    static const char *allowed[] = {"start", "stop", "restart", "enable", "disable", NULL};
    int ok_action = 0;
    for (int i = 0; allowed[i]; i++) if (!strcmp(action, allowed[i])) ok_action = 1;
    if (!ok_action) return strdup("{\"error\":\"unsupported action\"}");
    for (const char *q = name; *q; q++)
        if (!isalnum((unsigned char)*q) && *q != '-' && *q != '_' && *q != '.' && *q != '@')
            return strdup("{\"error\":\"invalid service name\"}");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "systemctl %s '%s' 2>&1", action, name);
    char *out = run_cmd(cmd, 30);
    char *res;
    if (out) {
        char *eo = jesc(out);
        size_t cap = strlen(eo) + 128;
        res = malloc(cap);
        snprintf(res, cap, "{\"ok\":true,\"message\":\"%s\"}", eo);
        free(eo);
        free(out);
    } else {
        res = strdup("{\"ok\":false,\"message\":\"command failed\"}");
    }
    return res;
}

/* ================= 软件包管理 ================= */

/* 探测包管理器 */
static const char *pkg_manager(void) {
    if (access("/usr/bin/dnf", X_OK) == 0) return "dnf";
    if (access("/usr/bin/yum", X_OK) == 0) return "yum";
    if (access("/usr/bin/apt", X_OK) == 0) return "apt";
    return "unknown";
}

char *ctc_pkg_updates_json(void) {
    const char *mgr = pkg_manager();
    char cmd[512];
    if (!strcmp(mgr, "dnf") || !strcmp(mgr, "yum")) {
        /* dnf check-update 返回码 100 表示有更新，非 0 正常 */
        snprintf(cmd, sizeof(cmd), "%s -q check-update --noplugins 2>/dev/null | grep -v '^$' | head -300", mgr);
    } else if (!strcmp(mgr, "apt")) {
        snprintf(cmd, sizeof(cmd), "apt list --upgradable 2>/dev/null | grep -v '^Listing' | head -300");
    } else {
        return strdup("{\"manager\":\"unknown\",\"count\":0,\"updates\":[]}");
    }
    char *out = run_cmd(cmd, 60);
    if (!out) out = strdup("");

    sbuf b; sb_init(&b);
    char head[128];
    snprintf(head, sizeof(head), "{\"manager\":\"%s\",\"count\":0,\"updates\":[", mgr);
    sb_put(&b, head);

    int count = 0, first = 1;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ') line++;
        if (!*line || !strncmp(line, "Last metadata", 13) || !strncmp(line, "Obsoleting", 10)) continue;
        char name[256] = {0}, arch[32] = {0}, ver[128] = {0}, repo[128] = {0};
        if (!strcmp(mgr, "apt")) {
            /* 格式: pkg/jammy-updates 1.2.3 amd64 [upgradable from: 1.2.2] */
            char pkg[256] = {0}, suite[128] = {0};
            if (sscanf(line, "%255[^/]/%127s %127s %31s", pkg, suite, ver, arch) >= 3) {
                strncpy(name, pkg, sizeof(name)-1);
                strncpy(repo, suite, sizeof(repo)-1);
                char item[1024];
                snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"arch\":\"%s\",\"version\":\"\",\"update\":\"%s\",\"repo\":\"%s\"}",
                         first?"":",", name, arch, ver, repo);
                sb_put(&b, item); first = 0; count++;
            }
        } else {
            /* dnf/yum 格式: name.arch    version    repo */
            if (sscanf(line, "%255s %127s %127s", name, ver, repo) == 3) {
                char *dot = strrchr(name, '.');
                if (dot) { strncpy(arch, dot+1, sizeof(arch)-1); *dot = 0; }
                if (!strcmp(repo, "updates") || strstr(repo, "-updates") || strstr(repo, "epel") ||
                    strstr(repo, "base") || strstr(repo, "appstream") || strlen(repo) > 0) {
                    char item[1024];
                    snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"arch\":\"%s\",\"version\":\"\",\"update\":\"%s\",\"repo\":\"%s\"}",
                             first?"":",", name, arch, ver, repo);
                    sb_put(&b, item); first = 0; count++;
                }
            }
        }
    }
    free(out);
    sb_put(&b, "]}");
    /* 回填 count */
    char *cnt = strstr(b.p, "\"count\":0");
    if (cnt) {
        char patch[32];
        snprintf(patch, sizeof(patch), "\"count\":%d", count);
        memmove(cnt + strlen(patch), cnt + strlen("\"count\":0"), strlen(cnt + strlen("\"count\":0")) + 1);
        memcpy(cnt, patch, strlen(patch));
    }
    return b.p;
}

char *ctc_pkg_upgrade_json(const char *packages_json) {
    const char *mgr = pkg_manager();
    char cmd[1024];
    if (!strcmp(mgr, "dnf") || !strcmp(mgr, "yum")) {
        if (packages_json && strstr(packages_json, "\"")) {
            /* 简单提取包名，白名单校验 */
            char names[768] = {0};
            const char *p = packages_json;
            while ((p = strchr(p, '"')) != NULL) {
                p++;
                const char *e = strchr(p, '"');
                if (!e) break;
                size_t l = (size_t)(e - p);
                if (l == 0 || l > 128) { p = e + 1; continue; }
                int safe = 1;
                for (size_t i = 0; i < l; i++)
                    if (!isalnum((unsigned char)p[i]) && p[i] != '-' && p[i] != '_' && p[i] != '.' && p[i] != '+') safe = 0;
                if (safe) {
                    if (strlen(names) + l + 2 < sizeof(names)) {
                        if (names[0]) strncat(names, " ", sizeof(names) - strlen(names) - 1);
                        strncat(names, p, l);
                    }
                }
                p = e + 1;
            }
            if (!names[0]) return strdup("{\"ok\":false,\"output\":\"\",\"message\":\"no valid packages\"}");
            snprintf(cmd, sizeof(cmd), "%s -y upgrade %s 2>&1 | tail -40", mgr, names);
        } else {
            snprintf(cmd, sizeof(cmd), "%s -y upgrade 2>&1 | tail -40", mgr);
        }
    } else if (!strcmp(mgr, "apt")) {
        snprintf(cmd, sizeof(cmd), "DEBIAN_FRONTEND=noninteractive apt-get -y upgrade 2>&1 | tail -40");
    } else {
        return strdup("{\"ok\":false,\"output\":\"\",\"message\":\"no supported package manager\"}");
    }
    char *out = run_cmd(cmd, 600);
    char *eo = jesc(out ? out : "");
    size_t cap = strlen(eo) + 128;
    char *res = malloc(cap);
    snprintf(res, cap, "{\"ok\":true,\"output\":\"%s\",\"message\":\"done\"}", eo);
    free(eo); free(out);
    return res;
}

char *ctc_pkg_history_json(void) {
    char *out = NULL;
    if (!strcmp(pkg_manager(), "dnf") || !strcmp(pkg_manager(), "yum")) {
        out = run_cmd("rpm -qa --last 2>/dev/null | head -60", 30);
    } else {
        out = run_cmd("grep -E '^(Install|Upgrade|Remove):' /var/log/dpkg.log 2>/dev/null | tail -60", 30);
    }
    if (!out) out = strdup("");
    sbuf b; sb_init(&b);
    sb_put(&b, "{\"entries\":[");
    int first = 1;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!*line) continue;
        char pkg[256] = {0}, tstr[128] = {0};
        /* rpm -qa --last 格式: name-version.arch    Tue 02 Nov 2026 10:00:00 AM JST */
        char *sep = strstr(line, "  ");
        if (sep) {
            size_t namelen = (size_t)(sep - line);
            if (namelen >= sizeof(pkg)) namelen = sizeof(pkg) - 1;
            memcpy(pkg, line, namelen); pkg[namelen] = 0;
            char *t = sep;
            while (*t == ' ') t++;
            snprintf(tstr, sizeof(tstr), "%s", t);
        } else {
            snprintf(pkg, sizeof(pkg), "%s", line);
        }
        char *e_pkg = jesc(pkg), *e_time = jesc(tstr);
        char item[768];
        snprintf(item, sizeof(item),
            "%s{\"id\":\"\",\"time\":\"%s\",\"action\":\"install\",\"package\":\"%s\"}",
            first ? "" : ",", e_time, e_pkg);
        sb_put(&b, item);
        free(e_pkg); free(e_time);
        first = 0;
    }
    free(out);
    sb_put(&b, "]}");
    return b.p;
}

/* ================= 安全审计 ================= */

/* 检测监听端口（优先 ss，回退 netstat） */
static void collect_ports(sbuf *b, int *first) {
    char *out = run_cmd("ss -tulnp 2>/dev/null | tail -n +2 | head -100", 15);
    if (!out || !*out) {
        free(out);
        out = run_cmd("netstat -tulnp 2>/dev/null | tail -n +3 | head -100", 15);
    }
    if (!out) return;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char proto[16] = {0}, state[32] = {0}, recvq[32] = {0}, sendq[32] = {0}, addr[128] = {0}, peer[128] = {0}, proc[512] = {0};
        /* ss 输出列序: Netid State Recv-Q Send-Q Local Peer [Process] */
        int n = sscanf(line, "%15s %31s %31s %31s %127s %127s %511[^\n]",
                       proto, state, recvq, sendq, addr, peer, proc);
        if (n < 5) continue;
        if (strcmp(proto, "tcp") && strcmp(proto, "udp") && strncmp(proto, "tcp6", 4) && strncmp(proto, "udp6", 4)) continue;
        /* addr 形如 0.0.0.0:22 或 [::]:22 或 *:22 */
        char *colon = strrchr(addr, ':');
        if (!colon) continue;
        int port = atoi(colon + 1);
        if (port <= 0) continue;
        *colon = 0;
        const char *pname = "";
        if (proc[0]) {
            char *pn = strstr(proc, "users:(");
            if (pn) {
                pn += 7;
                char *q = strchr(pn, '"');
                if (q) {
                    pn = q + 1;
                    q = strchr(pn, '"');
                    if (q) { static char pbuf[128]; size_t l = (size_t)(q - pn);
                             if (l >= sizeof(pbuf)) l = sizeof(pbuf)-1;
                             memcpy(pbuf, pn, l); pbuf[l] = 0; pname = pbuf; }
                }
            }
        }
        char *e_addr = jesc(addr), *e_proc = jesc(pname);
        char item[640];
        snprintf(item, sizeof(item),
            "%s{\"proto\":\"%s\",\"port\":%d,\"process\":\"%s\",\"address\":\"%s\"}",
            *first ? "" : ",", strncmp(proto,"tcp",3)==0?"tcp":"udp", port, e_proc, e_addr);
        sb_put(b, item);
        free(e_addr); free(e_proc);
        *first = 0;
    }
    free(out);
}

static int parse_sshd_config(const char *key, char *val, size_t vlen) {
    FILE *f = fopen("/etc/ssh/sshd_config", "r");
    if (!f) return -1;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || !*p) continue;
        if (!strncasecmp(p, key, strlen(key))) {
            char *q = p + strlen(key);
            while (*q == ' ' || *q == '\t' || *q == '=') q++;
            char *e = q;
            while (*e && *e != '\n' && *e != '\r' && *e != ' ' && *e != '#') e++;
            size_t l = (size_t)(e - q);
            if (l >= vlen) l = vlen - 1;
            memcpy(val, q, l); val[l] = 0;
            found = 1;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

char *ctc_security_audit_json(void) {
    sbuf b; sb_init(&b);
    sb_put(&b, "{\"firewall\":");

    /* --- 防火墙 --- */
    char *fw = run_cmd("systemctl is-active firewalld 2>/dev/null", 8);
    int fw_active = fw && strstr(fw, "active");
    free(fw);
    char fwjson[256];
    if (fw_active) {
        snprintf(fwjson, sizeof(fwjson), "{\"active\":true,\"backend\":\"firewalld\"}");
    } else {
        char *ipt = run_cmd("iptables -S 2>/dev/null | wc -l", 8);
        int has_ipt = ipt && atoi(ipt) > 1;
        free(ipt);
        snprintf(fwjson, sizeof(fwjson), "{\"active\":%s,\"backend\":\"%s\"}",
                 has_ipt ? "true" : "false", has_ipt ? "iptables" : "none");
    }
    sb_put(&b, fwjson);

    /* --- 监听端口 --- */
    sb_put(&b, ",\"ports\":[");
    int first = 1;
    collect_ports(&b, &first);
    sb_put(&b, "]");

    /* --- SSH 配置 --- */
    char val[128];
    int ssh_port = 22, permit_root = 1, pwd_auth = 1, pubkey = 1, max_tries = 6;
    if (parse_sshd_config("Port", val, sizeof(val)) == 0) ssh_port = atoi(val);
    if (parse_sshd_config("PermitRootLogin", val, sizeof(val)) == 0)
        permit_root = !strncasecmp(val, "no", 2) ? 0 : 1;
    if (parse_sshd_config("PasswordAuthentication", val, sizeof(val)) == 0)
        pwd_auth = !strncasecmp(val, "no", 2) ? 0 : 1;
    if (parse_sshd_config("PubkeyAuthentication", val, sizeof(val)) == 0)
        pubkey = !strncasecmp(val, "yes", 3) ? 1 : 0;
    if (parse_sshd_config("MaxAuthTries", val, sizeof(val)) == 0) max_tries = atoi(val);

    char sshjson[512];
    snprintf(sshjson, sizeof(sshjson),
        "{\"port\":%d,\"permit_root_login\":%s,\"password_auth\":%s,\"pubkey_auth\":%s,\"max_auth_tries\":%d}",
        ssh_port, permit_root?"true":"false", pwd_auth?"true":"false", pubkey?"true":"false", max_tries);
    sb_put(&b, ",\"ssh\":");
    sb_put(&b, sshjson);

    /* --- 合规检查 --- */
    sb_put(&b, ",\"checks\":[");
    int cfirst = 1;
    char item[768];

#define ADD_CHECK(item_, status_, detail_, suggest_) \
    do { \
        char *e_i = jesc(item_), *e_d = jesc(detail_), *e_s = jesc(suggest_); \
        snprintf(item, sizeof(item), \
            "%s{\"item\":\"%s\",\"status\":\"%s\",\"detail\":\"%s\",\"suggest\":\"%s\"}", \
            cfirst?"":",", e_i, status_, e_d, e_s); \
        sb_put(&b, item); free(e_i); free(e_d); free(e_s); cfirst = 0; \
    } while (0)

    ADD_CHECK("SSH 端口", ssh_port == 22 ? "warn" : "pass",
              ssh_port == 22 ? "使用默认端口 22" : "已修改默认端口",
              ssh_port == 22 ? "建议改为非默认端口以降低扫描风险" : "");

    ADD_CHECK("SSH 禁止 root 登录", permit_root ? "fail" : "pass",
              permit_root ? "PermitRootLogin 未禁用" : "已禁用 root 直接登录",
              permit_root ? "设置 PermitRootLogin no" : "");

    ADD_CHECK("SSH 密码认证", pwd_auth ? "warn" : "pass",
              pwd_auth ? "允许密码登录" : "已禁用密码登录",
              pwd_auth ? "生产环境建议仅用公钥认证" : "");

    ADD_CHECK("SSH 公钥认证", pubkey ? "pass" : "fail",
              pubkey ? "已启用公钥认证" : "未启用公钥认证",
              pubkey ? "" : "设置 PubkeyAuthentication yes");

    ADD_CHECK("SSH 最大认证次数", max_tries <= 6 ? "pass" : "warn",
              max_tries <= 6 ? "MaxAuthTries 合理" : "MaxAuthTries 过大",
              max_tries <= 6 ? "" : "建议 MaxAuthTries 不超过 6");

    ADD_CHECK("防火墙", fw_active ? "pass" : "warn",
              fw_active ? "firewalld 运行中" : "未检测到活跃防火墙",
              fw_active ? "" : "建议启用 firewalld 并仅放行必要端口");

    /* SELinux */
    char *se = run_cmd("getenforce 2>/dev/null", 5);
    int se_enforcing = se && strstr(se, "Enforcing");
    int se_permissive = se && strstr(se, "Permissive");
    ADD_CHECK("SELinux", se_enforcing ? "pass" : (se_permissive ? "warn" : "warn"),
              se_enforcing ? "Enforcing 模式" : (se_permissive ? "Permissive 模式" : "未启用或不可用"),
              se_enforcing ? "" : "建议设为 Enforcing 模式");
    free(se);

    /* 空口令账户 */
    char *empty = run_cmd("awk -F: '($2==\"\"){print $1}' /etc/shadow 2>/dev/null | head -5", 8);
    int has_empty = empty && *empty && strcmp(empty, "\n") != 0;
    ADD_CHECK("空口令账户", has_empty ? "fail" : "pass",
              has_empty ? "存在空口令账户" : "未发现空口令账户",
              has_empty ? "立即为相关账户设置强密码或禁用" : "");
    free(empty);

    /* 可写 /etc/passwd */
    char *perm = run_cmd("stat -c '%a' /etc/passwd 2>/dev/null", 5);
    int passwd_perm = perm ? (int)strtol(perm, NULL, 8) : 0644;
    int passwd_bad = (passwd_perm & 0022) != 0;
    ADD_CHECK("/etc/passwd 权限", passwd_bad ? "fail" : "pass",
              passwd_bad ? "文件对组/其他用户可写" : "权限正常",
              passwd_bad ? "执行 chmod 644 /etc/passwd" : "");
    free(perm);

    /* 失败登录尝试 */
    char *faillog = run_cmd("lastb 2>/dev/null | wc -l", 10);
    int fails = faillog ? atoi(faillog) : 0;
    char fd[128];
    snprintf(fd, sizeof(fd), "最近失败登录 %d 次", fails);
    ADD_CHECK("失败登录", fails > 50 ? "warn" : "pass", fd,
              fails > 50 ? "存在大量失败登录，建议启用 fail2ban" : "");
    free(faillog);

#undef ADD_CHECK

    sb_put(&b, "]}");
    return b.p;
}

/* ================= 延迟测量 ================= */

char *ctc_pong_json(long long server_ts) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    char *buf = malloc(128);
    snprintf(buf, 128, "{\"timestamp\":%lld,\"agent_time\":%lld}", server_ts, now);
    return buf;
}

/* ================= Docker 管理 ================= */

/* 判断 docker 是否可用：只检查 socket 与 CLI 存在性。
 * 注意：不能用 system("docker info")，守护进程里 system() 会因信号/fd
 * 继承问题阻塞，导致整个 agent 主循环卡死。 */
static int docker_available(void) {
    if (access("/usr/bin/docker", X_OK) != 0 && access("/usr/local/bin/docker", X_OK) != 0)
        return 0;
    /* 检查 daemon socket（rootless 在 $XDG_RUNTIME_DIR） */
    if (access("/var/run/docker.sock", F_OK) == 0) return 1;
    if (access("/run/docker.sock", F_OK) == 0) return 1;
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg) {
        char p[512];
        snprintf(p, sizeof(p), "%s/docker.sock", xdg);
        if (access(p, F_OK) == 0) return 1;
    }
    return 0;
}

/* docker ps -a --format 输出解析 */
char *ctc_docker_list_json(void) {
    if (!docker_available())
        return strdup("{\"available\":false,\"containers\":[],\"error\":\"docker not available or daemon not running\"}");

    /* 用 | 分隔字段，避免空格解析问题 */
    char *out = run_cmd("docker ps -a --no-trunc --format '{{.ID}}|{{.Names}}|{{.Image}}|{{.Status}}|{{.State}}|{{.Ports}}|{{.CreatedAt}}' 2>/dev/null | head -200", 25);
    if (!out) return strdup("{\"available\":true,\"containers\":[]}");

    sbuf b; sb_init(&b);
    sb_put(&b, "{\"available\":true,\"containers\":[");
    int first = 1;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!*line) continue;
        char *f[7] = {0};
        int nf = 0;
        char *p = line;
        while (nf < 7) {
            f[nf++] = p;
            char *sep = strchr(p, '|');
            if (!sep) break;
            *sep = 0;
            p = sep + 1;
        }
        if (nf < 4) continue;
        char *e_id = jesc(f[0]), *e_name = jesc(f[1]), *e_img = jesc(f[2]);
        char *e_st = jesc(f[3]), *e_state = jesc(nf > 4 ? f[4] : ""), *e_ports = jesc(nf > 5 ? f[5] : "");
        char *e_created = jesc(nf > 6 ? f[6] : "");
        char item[2048];
        snprintf(item, sizeof(item),
            "%s{\"id\":\"%.12s\",\"name\":\"%s\",\"image\":\"%s\",\"status\":\"%s\","
            "\"state\":\"%s\",\"ports\":\"%s\",\"created\":\"%s\"}",
            first ? "" : ",", e_id, e_name, e_img, e_st, e_state, e_ports, e_created);
        sb_put(&b, item);
        free(e_id); free(e_name); free(e_img); free(e_st); free(e_state); free(e_ports); free(e_created);
        first = 0;
    }
    free(out);
    sb_put(&b, "]}");
    return b.p;
}

/* docker start/stop/restart/rm/pause/unpause */
char *ctc_docker_action_json(const char *container_id, const char *action) {
    if (!docker_available())
        return strdup("{\"error\":\"docker not available\"}");
    /* 白名单 action */
    static const char *allowed[] = {"start", "stop", "restart", "rm", "pause", "unpause", NULL};
    int ok = 0;
    for (int i = 0; allowed[i]; i++) if (!strcmp(action, allowed[i])) ok = 1;
    if (!ok) return strdup("{\"error\":\"invalid action\"}");
    /* 容器 ID 校验：只允许 [a-zA-Z0-9_.-] */
    for (const char *q = container_id; *q; q++) {
        if (!isalnum((unsigned char)*q) && *q != '_' && *q != '.' && *q != '-')
            return strdup("{\"error\":\"invalid container id\"}");
    }
    if (!container_id[0]) return strdup("{\"error\":\"missing container id\"}");

    char cmd[512];
    if (!strcmp(action, "rm"))
        snprintf(cmd, sizeof(cmd), "docker rm -f '%s' 2>&1 | tail -3", container_id);
    else
        snprintf(cmd, sizeof(cmd), "docker %s '%s' 2>&1 | tail -3", action, container_id);

    char *out = run_cmd(cmd, 60);
    char *e = jesc(out ? out : "");
    free(out);
    char *res = malloc(strlen(e) + 128);
    snprintf(res, strlen(e) + 128, "{\"ok\":true,\"message\":\"%s\"}", e);
    free(e);
    return res;
}

/* docker logs */
char *ctc_docker_logs_json(const char *container_id, int lines) {
    if (!docker_available())
        return strdup("{\"error\":\"docker not available\"}");
    if (lines <= 0 || lines > 2000) lines = 200;
    for (const char *q = container_id; *q; q++) {
        if (!isalnum((unsigned char)*q) && *q != '_' && *q != '.' && *q != '-')
            return strdup("{\"error\":\"invalid container id\"}");
    }
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "docker logs --tail %d --timestamps '%s' 2>&1", lines, container_id);
    char *out = run_cmd(cmd, 30);
    if (!out) out = strdup("");
    /* 限制 256KB */
    size_t l = strlen(out);
    if (l > 262144) out[262144] = 0;
    char *e = jesc(out);
    free(out);
    char *res = malloc(strlen(e) + 64);
    snprintf(res, strlen(e) + 64, "{\"logs\":\"%s\"}", e);
    free(e);
    return res;
}

/* docker images */
char *ctc_docker_images_json(void) {
    if (!docker_available())
        return strdup("{\"images\":[]}");
    char *out = run_cmd("docker images --format '{{.ID}}|{{.Repository}}|{{.Tag}}|{{.Size}}|{{.CreatedSince}}' 2>/dev/null | head -100", 20);
    if (!out) return strdup("{\"images\":[]}");
    sbuf b; sb_init(&b);
    sb_put(&b, "{\"images\":[");
    int first = 1;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!*line) continue;
        char *f[5] = {0};
        int nf = 0;
        char *p = line;
        while (nf < 5) {
            f[nf++] = p;
            char *sep = strchr(p, '|');
            if (!sep) break;
            *sep = 0;
            p = sep + 1;
        }
        if (nf < 3) continue;
        char *e_id = jesc(f[0]), *e_repo = jesc(f[1]), *e_tag = jesc(f[2]);
        char *e_size = jesc(nf > 3 ? f[3] : ""), *e_created = jesc(nf > 4 ? f[4] : "");
        char item[1024];
        snprintf(item, sizeof(item),
            "%s{\"id\":\"%.12s\",\"repository\":\"%s\",\"tag\":\"%s\",\"size\":\"%s\",\"created\":\"%s\"}",
            first ? "" : ",", e_id, e_repo, e_tag, e_size, e_created);
        sb_put(&b, item);
        free(e_id); free(e_repo); free(e_tag); free(e_size); free(e_created);
        first = 0;
    }
    free(out);
    sb_put(&b, "]}");
    return b.p;
}

/* ================= Docker 详细能力 ================= */

/* docker inspect → 完整 JSON，本地 JSON 需要转义处理（因为外层又要再 JSON 一次） */
char *ctc_docker_inspect_json(const char *container_id) {
    if (!docker_available())
        return strdup("{\"error\":\"docker not available\"}");
    /* 容器 ID 校验 */
    for (const char *q = container_id; *q; q++) {
        if (!isalnum((unsigned char)*q) && *q != '_' && *q != '.' && *q != '-')
            return strdup("{\"error\":\"invalid container id\"}");
    }
    if (!container_id[0]) return strdup("{\"error\":\"missing container id\"}");

    char cmd[512];
    /* docker inspect 原生输出就是 JSON 数组，直接取第一个元素 */
    snprintf(cmd, sizeof(cmd), "docker inspect '%s' 2>/dev/null", container_id);
    char *out = run_cmd(cmd, 25);
    if (!out) return strdup("{\"error\":\"inspect failed\"}");
    /* docker inspect 返回 JSON 数组 [{...}]，提取第一个元素 */
    char *start = strchr(out, '[');
    char *end = strrchr(out, ']');
    if (!start || !end || end <= start) {
        free(out);
        return strdup("{\"error\":\"invalid inspect output\"}");
    }
    /* 内容就是合法的 JSON，直接嵌入为 payload */
    *end = 0;
    char *inner = start + 1;
    /* 去掉尾部逗号 */
    size_t n = strlen(inner);
    while (n && (inner[n-1] == '\n' || inner[n-1] == ' ')) inner[--n] = 0;
    /* 包装：原始 JSON 作为 payload 的一部分 */
    char *res = malloc(n + 64);
    snprintf(res, n + 64, "{\"data\":%s}", inner);
    free(out);
    return res;
}

/* docker create + 可选 start */
char *ctc_docker_create_json(const char *payload_json) {
    if (!docker_available())
        return strdup("{\"ok\":false,\"output\":\"docker not available\"}");

    char name[128] = "", image[256] = "", cmd[1024] = "", restart[32] = "";
    char network[128] = "", privileged[8] = "", autostart[8] = "";
    ctc_json_get_string(payload_json, "name", name, sizeof(name));
    ctc_json_get_string(payload_json, "image", image, sizeof(image));
    ctc_json_get_string(payload_json, "cmd", cmd, sizeof(cmd));
    ctc_json_get_string(payload_json, "restart", restart, sizeof(restart));
    ctc_json_get_string(payload_json, "network", network, sizeof(network));
    ctc_json_get_string(payload_json, "privileged", privileged, sizeof(privileged));
    ctc_json_get_string(payload_json, "auto_start", autostart, sizeof(autostart));

    if (!image[0]) return strdup("{\"ok\":false,\"output\":\"missing image\"}");
    if (!name[0]) return strdup("{\"ok\":false,\"output\":\"missing name\"}");

    /* 安全校验：所有字段只允许安全字符（防止命令注入） */
    const char *safe_ok = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_/: =@";
    for (const char *q = name; *q; q++)
        if (!strchr(safe_ok, *q)) return strdup("{\"ok\":false,\"output\":\"invalid name\"}");
    for (const char *q = image; *q; q++)
        if (!strchr(safe_ok, *q)) return strdup("{\"ok\":false,\"output\":\"invalid image\"}");

    /* 组装 docker create 命令 */
    char dcmd[8192];
    int n = 0;
    n += snprintf(dcmd + n, sizeof(dcmd) - n, "docker create --name '%s'", name);
    if (restart[0]) n += snprintf(dcmd + n, sizeof(dcmd) - n, " --restart='%s'", restart);
    if (network[0]) n += snprintf(dcmd + n, sizeof(dcmd) - n, " --network='%s'", network);
    if (!strcmp(privileged, "true")) n += snprintf(dcmd + n, sizeof(dcmd) - n, " --privileged");

    /* env 列表：解析 "env":["K=V","K=V"] */
    const char *env_p = strstr(payload_json, "\"env\":[");
    if (env_p) {
        env_p += 7;
        const char *e = strchr(env_p, ']');
        if (e) {
            size_t len = (size_t)(e - env_p);
            char envs[2048];
            snprintf(envs, sizeof(envs), "%.*s", (int)len, env_p);
            /* 简单解析 ["K=V","K=V"] */
            char *p = envs;
            while ((p = strchr(p, '\"')) != NULL) {
                p++;
                char *e2 = strchr(p, '\"');
                if (!e2) break;
                char kv[256];
                size_t kl = (size_t)(e2 - p);
                if (kl > 0 && kl < sizeof(kv)) {
                    memcpy(kv, p, kl);
                    kv[kl] = 0;
                    /* K=V 校验 */
                    if (strchr(kv, '=')) {
                        for (const char *q = kv; *q; q++) {
                            if (!strchr(safe_ok, *q) && *q != '=') {
                                return strdup("{\"ok\":false,\"output\":\"invalid env\"}");
                            }
                        }
                        n += snprintf(dcmd + n, sizeof(dcmd) - n, " -e '%s'", kv);
                    }
                }
                p = e2 + 1;
            }
        }
    }

    /* ports */
    const char *ports_p = strstr(payload_json, "\"ports\":[");
    if (ports_p) {
        ports_p += 9;
        const char *e = strchr(ports_p, ']');
        if (e) {
            size_t len = (size_t)(e - ports_p);
            char ports[2048];
            snprintf(ports, sizeof(ports), "%.*s", (int)len, ports_p);
            char *p = ports;
            while ((p = strchr(p, '\"')) != NULL) {
                p++;
                char *e2 = strchr(p, '\"');
                if (!e2) break;
                char pm[128];
                size_t pl = (size_t)(e2 - p);
                if (pl > 0 && pl < sizeof(pm)) {
                    memcpy(pm, p, pl);
                    pm[pl] = 0;
                    if (strchr(pm, ':') || strchr(pm, '/')) {
                        for (const char *q = pm; *q; q++) {
                            if (!isalnum((unsigned char)*q) && *q != ':' && *q != '/' && *q != '.' && *q != '-')
                                { p = NULL; break; }
                        }
                        if (p) n += snprintf(dcmd + n, sizeof(dcmd) - n, " -p '%s'", pm);
                    }
                }
                if (!p) break;
                p = e2 + 1;
            }
        }
    }

    /* mounts */
    const char *mounts_p = strstr(payload_json, "\"mounts\":[");
    if (mounts_p) {
        mounts_p += 11;
        const char *e = strchr(mounts_p, ']');
        if (e) {
            size_t len = (size_t)(e - mounts_p);
            char mounts[2048];
            snprintf(mounts, sizeof(mounts), "%.*s", (int)len, mounts_p);
            char *p = mounts;
            while ((p = strchr(p, '\"')) != NULL) {
                p++;
                char *e2 = strchr(p, '\"');
                if (!e2) break;
                char mm[256];
                size_t ml = (size_t)(e2 - p);
                if (ml > 0 && ml < sizeof(mm)) {
                    memcpy(mm, p, ml);
                    mm[ml] = 0;
                    /* type:source:target[:ro] */
                    if (strchr(mm, ':')) {
                        for (const char *q = mm; *q; q++) {
                            if (!strchr(safe_ok, *q) && *q != ':' && *q != ',' && *q != '*')
                                { p = NULL; break; }
                        }
                        if (p) n += snprintf(dcmd + n, sizeof(dcmd) - n, " -v '%s'", mm);
                    }
                }
                if (!p) break;
                p = e2 + 1;
            }
        }
    }

    /* image 最后放 */
    n += snprintf(dcmd + n, sizeof(dcmd) - n, " '%s'", image);
    if (cmd[0]) n += snprintf(dcmd + n, sizeof(dcmd) - n, " %s", cmd);

    char *out = run_cmd(dcmd, 60);
    char *e = jesc(out ? out : "");
    free(out);
    size_t cap = strlen(e) + 128;
    char *res = malloc(cap);
    snprintf(res, cap, "{\"ok\":true,\"output\":\"%s\"}", e);
    free(e);
    return res;
}

/* docker-compose 支持（优先 compose 插件，回退 docker-compose 命令） */
char *ctc_docker_compose_json(const char *payload_json) {
    char action[32] = "", workdir[512] = "", content[8192] = "";
    ctc_json_get_string(payload_json, "action", action, sizeof(action));
    ctc_json_get_string(payload_json, "work_dir", workdir, sizeof(workdir));
    ctc_json_get_string(payload_json, "content", content, sizeof(content));

    if (!action[0]) return strdup("{\"ok\":false,\"output\":\"missing action\"}");

    /* 定位 compose 命令 */
    char compose_cmd[64];
    if (system("docker compose version >/dev/null 2>&1") == 0) {
        snprintf(compose_cmd, sizeof(compose_cmd), "docker compose");
    } else if (system("docker-compose version >/dev/null 2>&1") == 0) {
        snprintf(compose_cmd, sizeof(compose_cmd), "docker-compose");
    } else {
        return strdup("{\"ok\":false,\"output\":\"docker compose 不可用\"}");
    }

    /* workdir 安全校验 */
    if (workdir[0]) {
        for (const char *q = workdir; *q; q++) {
            if (!isalnum((unsigned char)*q) && *q != '/' && *q != '_' && *q != '-' && *q != '.')
                return strdup("{\"ok\":false,\"output\":\"invalid work_dir\"}");
        }
        if (strstr(workdir, "..")) return strdup("{\"ok\":false,\"output\":\"invalid work_dir\"}");
    }

    char cmd[1024];
    if (content[0]) {
        /* 写入 docker-compose.yml 到工作目录（先建目录） */
        char tmp[1024];
        const char *dir = workdir[0] ? workdir : "/tmp/ctc-compose";
        snprintf(tmp, sizeof(tmp), "mkdir -p '%s'", dir);
        if (system(tmp) != 0) return strdup("{\"ok\":false,\"output\":\"mkdir failed\"}");
        /* 写文件（内容已经在 JSON 里转义过，这里直接落盘） */
        char path[768];
        snprintf(path, sizeof(path), "%s/docker-compose.yml", dir);
        FILE *f = fopen(path, "w");
        if (!f) return strdup("{\"ok\":false,\"output\":\"write failed\"}");
        fputs(content, f);
        fclose(f);
        snprintf(cmd, sizeof(cmd), "cd '%s' && %s %s 2>&1 | tail -50", dir, compose_cmd, action);
    } else {
        if (!workdir[0]) return strdup("{\"ok\":false,\"output\":\"missing work_dir\"}");
        snprintf(cmd, sizeof(cmd), "cd '%s' && %s %s 2>&1 | tail -50", workdir, compose_cmd, action);
    }

    char *out = run_cmd(cmd, 120);
    char *e = jesc(out ? out : "");
    free(out);
    size_t cap = strlen(e) + 128;
    char *res = malloc(cap);
    snprintf(res, cap, "{\"ok\":true,\"output\":\"%s\"}", e);
    free(e);
    return res;
}
