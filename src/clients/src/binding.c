/* 服务端扫描与批准制绑定
 *
 * 流程：
 *   1. 启动时读配置中的 bound_server_id
 *   2. 若未绑定：向局域网广播探测包，收集应答的服务端
 *   3. 逐个握手：发 handshake_req，服务端回 handshake_ack
 *   4. 服务端批准 → 拿到 server_id，写入绑定，停止扫描其他服务端
 *   5. 已绑定：只连 bound_server；若 IP 变化，用 server_id 验证身份后更新配置
 */
#define _GNU_SOURCE
#include "ctforge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/utsname.h>
#include <ifaddrs.h>

/* ---- 系统信息采集 ---- */

/* 读 /etc/os-release 取 PRETTY_NAME */
void ctc_sysinfo(char *os_version, size_t os_sz, char *kernel, size_t k_sz) {
    os_version[0] = 0;
    kernel[0] = 0;

    FILE *f = fopen("/etc/os-release", "r");
    if (!f) f = fopen("/usr/lib/os-release", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "PRETTY_NAME=", 12)) {
                char *v = line + 12;
                char *nl = strchr(v, '\n');
                if (nl) *nl = 0;
                /* 去引号 */
                if (*v == '"') {
                    v++;
                    char *q = strchr(v, '"');
                    if (q) *q = 0;
                }
                snprintf(os_version, os_sz, "%s", v);
                break;
            }
        }
        fclose(f);
    }
    struct utsname un;
    if (uname(&un) == 0) {
        snprintf(kernel, k_sz, "%s", un.release);
    }
}

/* ---- 绑定文件读写 ---- */

int ctc_binding_load(ctc_binding *b, const char *path) {
    memset(b, 0, sizeof(*b));
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || !strchr(line, '=')) continue;
        char *eq = strchr(line, '=');
        *eq = 0;
        char *k = line, *v = eq + 1;
        char *nl = strchr(v, '\n');
        if (nl) *nl = 0;
        if (!strcmp(k, "bound_server_id")) snprintf(b->bound_server_id, sizeof(b->bound_server_id), "%s", v);
        else if (!strcmp(k, "bound_server_ip")) snprintf(b->bound_server_ip, sizeof(b->bound_server_ip), "%s", v);
        else if (!strcmp(k, "bound_server_port")) b->bound_server_port = atoi(v);
        else if (!strcmp(k, "approved")) b->approved = atoi(v);
    }
    fclose(f);
    return 0;
}

/* 写绑定信息到 agent.conf（按 key 替换，保留其他行） */
int ctc_binding_save(const ctc_binding *b, const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *in = fopen(path, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) return -1;

    int wrote[4] = {0, 0, 0, 0};
    if (in) {
        char line[512];
        while (fgets(line, sizeof(line), in)) {
            if (!strncmp(line, "bound_server_id=", 16)) {
                fprintf(out, "bound_server_id=%s\n", b->bound_server_id);
                wrote[0] = 1;
            } else if (!strncmp(line, "bound_server_ip=", 16)) {
                fprintf(out, "bound_server_ip=%s\n", b->bound_server_ip);
                wrote[1] = 1;
            } else if (!strncmp(line, "bound_server_port=", 18)) {
                fprintf(out, "bound_server_port=%d\n", b->bound_server_port);
                wrote[2] = 1;
            } else if (!strncmp(line, "approved=", 9)) {
                fprintf(out, "approved=%d\n", b->approved);
                wrote[3] = 1;
            } else {
                fputs(line, out);
            }
        }
        fclose(in);
    }
    if (!wrote[0]) fprintf(out, "bound_server_id=%s\n", b->bound_server_id);
    if (!wrote[1]) fprintf(out, "bound_server_ip=%s\n", b->bound_server_ip);
    if (!wrote[2]) fprintf(out, "bound_server_port=%d\n", b->bound_server_port);
    if (!wrote[3]) fprintf(out, "approved=%d\n", b->approved);
    fclose(out);
    return rename(tmp, path);
}

/* ---- 握手 ---- */

/* 在已连接的 fd 上执行 handshake_req/ack 交换。
 * 成功且批准返回 0，填充 out；未批准返回 1；失败返回 -1。 */
int ctc_handshake(int fd, const ctc_config *cfg, const ctc_binding *binding, ctc_binding *out) {
    memset(out, 0, sizeof(*out));

    char os_ver[256] = "", kernel[128] = "";
    ctc_sysinfo(os_ver, sizeof(os_ver), kernel, sizeof(kernel));

    struct utsname un;
    uname(&un);

    ctc_metrics m;
    ctc_collect_metrics(&m);

    /* 构造 handshake_req 的 payload */
    char payload[1536];
    snprintf(payload, sizeof(payload),
        "{\"agent_id\":\"%s\",\"name\":\"%s\",\"hostname\":\"%s\",\"os\":\"%s\","
        "\"os_version\":\"%s\",\"kernel\":\"%s\",\"arch\":\"%s\",\"version\":\"%s\","
        "\"disk_total_kb\":%llu,\"disk_used_kb\":%llu,\"disk_percent\":%.2f,"
        "\"bound_server_id\":\"%s\"}",
        cfg->agent_id, cfg->name, un.nodename, un.sysname,
        os_ver, kernel, un.machine, CTC_VERSION,
        (unsigned long long)m.disk_total_kb,
        (unsigned long long)m.disk_used_kb,
        m.disk_percent,
        binding && binding->bound_server_id[0] ? binding->bound_server_id : "");

    /* 直接构造完整消息（含 token） */
    char full[2048];
    snprintf(full, sizeof(full),
        "{\"id\":\"hs-%ld\",\"type\":\"%s\",\"version\":%d,\"token\":\"%s\",\"payload\":%s}",
        (long)time(NULL), MSG_HANDSHAKE_REQ, CTC_PROTO_VERSION, cfg->token, payload);

    /* 用 ctc_send 无法带 token，这里手工发帧 */
    extern int ctc_tls_write(int fd, const void *buf, int n);
    uint32_t len = (uint32_t)strlen(full);
    uint32_t be = htonl(len);
    if (ctc_tls_write(fd, &be, 4) != 4) return -1;
    if (ctc_tls_write(fd, full, (int)len) != (int)len) return -1;

    /* 等应答 */
    ctc_message resp = {0};
    if (ctc_recv(fd, &resp) < 0) return -1;

    int rc = -1;
    if (resp.type && !strcmp(resp.type, MSG_HANDSHAKE_ACK)) {
        char sid[128] = "", sip[64] = "";
        int approved = 0;
        if (resp.payload) {
            approved = ctc_json_get_bool(resp.payload, "approved", 0);
            ctc_json_get_string(resp.payload, "server_id", sid, sizeof(sid));
            ctc_json_get_string(resp.payload, "server_ip", sip, sizeof(sip));
        }
        if (approved && sid[0]) {
            snprintf(out->bound_server_id, sizeof(out->bound_server_id), "%s", sid);
            snprintf(out->bound_server_ip, sizeof(out->bound_server_ip), "%s",
                     sip[0] ? sip : cfg->server_host);
            out->bound_server_port = cfg->server_port;
            out->approved = 1;
            rc = 0;
        } else {
            rc = 1; /* 未批准 */
        }
    } else if (resp.type && !strcmp(resp.type, MSG_ERROR)) {
        rc = -1;
    }
    ctc_message_free(&resp);
    return rc;
}

/* ---- 局域网扫描 ---- */

/* 探测包格式（与 discover 服务端一致）：
 *   "CTC1" + 2B BE 长度 + JSON{type:"scan_req", agent_id:"..."} */

typedef struct {
    char ip[64];
    int port;
} server_cand;

/* 向局域网广播 scan 请求，收集应答 */
static int scan_lan(ctc_config *cfg, server_cand *out, int max) {
    int cnt = 0;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct timeval tv = {1, 500000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* 绑定任意端口以接收应答 */
    struct sockaddr_in me = {0};
    me.sin_family = AF_INET;
    me.sin_port = 0; /* 临时端口 */
    me.sin_addr.s_addr = INADDR_ANY;
    bind(fd, (struct sockaddr *)&me, sizeof(me));

    /* 探测包 */
    char body[256];
    int blen = snprintf(body, sizeof(body),
        "{\"type\":\"scan_req\",\"agent_id\":\"%s\",\"version\":\"%s\"}",
        cfg->agent_id, CTC_VERSION);
    unsigned char pkt[320];
    memcpy(pkt, "CTC1", 4);
    pkt[4] = (unsigned char)((blen >> 8) & 0xFF);
    pkt[5] = (unsigned char)(blen & 0xFF);
    memcpy(pkt + 6, body, (size_t)blen);

    /* 向广播地址和常见网段广播 */
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)(cfg->discover_port > 0 ? cfg->discover_port : CTC_DISCOVER_PORT));
    inet_pton(AF_INET, "255.255.255.255", &to.sin_addr);
    sendto(fd, pkt, (size_t)(6 + blen), 0, (struct sockaddr *)&to, sizeof(to));

    /* 同时向本机各网卡的定向广播地址发一份（更可靠） */
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            if (!p->ifa_broadaddr) continue;
            struct sockaddr_in *baddr = (struct sockaddr_in *)p->ifa_broadaddr;
            to.sin_addr = baddr->sin_addr;
            sendto(fd, pkt, (size_t)(6 + blen), 0, (struct sockaddr *)&to, sizeof(to));
        }
        freeifaddrs(ifa);
    }

    /* 收集应答（最多等 2 秒） */
    time_t deadline = time(NULL) + 2;
    unsigned char rbuf[1024];
    while (cnt < max && time(NULL) <= deadline) {
        struct sockaddr_in from = {0};
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(fd, rbuf, sizeof(rbuf), 0, (struct sockaddr *)&from, &flen);
        if (n < 6) continue;
        if (memcmp(rbuf, "CTC1", 4) != 0) continue;
        int bl = (rbuf[4] << 8) | rbuf[5];
        if (bl <= 0 || bl > (int)n - 6) continue;
        char resp[1024];
        memcpy(resp, rbuf + 6, (size_t)bl);
        resp[bl] = 0;

        /* 应答 JSON: {"type":"scan_resp","port":9000,"server_id":"..."} */
        char ip[64];
        snprintf(ip, sizeof(ip), "%s", inet_ntoa(from.sin_addr));
        int port = (int)ctc_json_get_int(resp, "port", 9000);

        /* 去重 */
        int dup = 0;
        for (int i = 0; i < cnt; i++) {
            if (!strcmp(out[i].ip, ip) && out[i].port == port) { dup = 1; break; }
        }
        if (!dup) {
            snprintf(out[cnt].ip, sizeof(out[cnt].ip), "%s", ip);
            out[cnt].port = port;
            cnt++;
        }
    }
    close(fd);
    return cnt;
}

/* 连接到指定服务端并握手（内部用） */
static int try_connect_handshake(const char *host, int port, ctc_config *cfg,
                                 const ctc_binding *binding, ctc_binding *out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    /* TLS 升级 */
    if (cfg->use_tls) {
        if (ctc_tls_wrap(fd, host, cfg->server_fingerprint[0] ? cfg->server_fingerprint : NULL,
                         cfg->tls_skip_verify) < 0) {
            close(fd);
            return -1;
        }
    }
    int rc = ctc_handshake(fd, cfg, binding, out);
    extern void ctc_tls_cleanup(int fd);
    ctc_tls_cleanup(fd);
    close(fd);
    return rc;
}

/* 扫描局域网内的服务端并尝试握手。返回：
 *   0 = 已绑定成功（out 填充）
 *   1 = 找到服务端但未批准（待批准）
 *  -1 = 未找到任何服务端 */
int ctc_scan_server(ctc_config *cfg, ctc_binding *out) {
    memset(out, 0, sizeof(*out));

    /* 已绑定：只连绑定的服务端 */
    if (cfg->bound_server_id[0] && cfg->bound_server_ip[0]) {
        fprintf(stderr, "[ctc-agent] bound to server %s at %s:%d, verifying...\n",
                cfg->bound_server_id, cfg->bound_server_ip, cfg->bound_server_port);
        ctc_binding b = {0};
        snprintf(b.bound_server_id, sizeof(b.bound_server_id), "%s", cfg->bound_server_id);
        int rc = try_connect_handshake(cfg->bound_server_ip,
                                       cfg->bound_server_port > 0 ? cfg->bound_server_port : cfg->server_port,
                                       cfg, &b, out);
        if (rc == 0) {
            /* 校验 server_id 一致（防冒充） */
            if (strcmp(out->bound_server_id, cfg->bound_server_id) == 0) {
                return 0;
            }
            fprintf(stderr, "[ctc-agent] server_id mismatch! expected %s got %s — refusing\n",
                    cfg->bound_server_id, out->bound_server_id);
            memset(out, 0, sizeof(*out));
            return -1;
        }
        if (rc == 1) return 1;
        /* 连接失败：服务端可能换 IP 了，走扫描流程用 server_id 认亲 */
        fprintf(stderr, "[ctc-agent] bound server unreachable, scanning for same server_id...\n");
    }

    /* 扫描局域网 */
    server_cand cands[16];
    int n = scan_lan(cfg, cands, 16);
    if (n <= 0) {
        /* 回退：尝试配置里的地址 */
        if (cfg->server_host[0]) {
            ctc_binding b = {0};
            if (cfg->bound_server_id[0]) {
                snprintf(b.bound_server_id, sizeof(b.bound_server_id), "%s", cfg->bound_server_id);
            }
            int rc = try_connect_handshake(cfg->server_host, cfg->server_port, cfg, &b, out);
            if (rc == 0) {
                if (!cfg->bound_server_id[0] || strcmp(out->bound_server_id, cfg->bound_server_id) == 0) {
                    return 0;
                }
            }
            if (rc == 1) return 1;
        }
        return -1;
    }

    int saw_pending = 0;
    for (int i = 0; i < n; i++) {
        ctc_binding b = {0};
        if (cfg->bound_server_id[0]) {
            snprintf(b.bound_server_id, sizeof(b.bound_server_id), "%s", cfg->bound_server_id);
        }
        ctc_binding got = {0};
        int rc = try_connect_handshake(cands[i].ip, cands[i].port, cfg, &b, &got);
        if (rc == 0) {
            if (cfg->bound_server_id[0]) {
                /* 已绑定：只认 server_id 相同的 */
                if (strcmp(got.bound_server_id, cfg->bound_server_id) != 0) {
                    fprintf(stderr, "[ctc-agent] ignoring server %s (id mismatch)\n", cands[i].ip);
                    continue;
                }
                fprintf(stderr, "[ctc-agent] server_id matched at new address %s:%d\n",
                        cands[i].ip, cands[i].port);
            }
            *out = got;
            if (!out->bound_server_ip[0]) {
                snprintf(out->bound_server_ip, sizeof(out->bound_server_ip), "%s", cands[i].ip);
            }
            /* 更新配置里的 server_host 为最新地址 */
            snprintf(cfg->server_host, sizeof(cfg->server_host), "%s", cands[i].ip);
            cfg->server_port = cands[i].port;
            return 0;
        }
        if (rc == 1) saw_pending = 1;
    }
    return saw_pending ? 1 : -1;
}
