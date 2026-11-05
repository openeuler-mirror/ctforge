/* 卸载保护：agent 必须获得服务端批准才能卸载。
 *
 * 用法：ctforge-agent --uninstall
 *   1. 连接服务端（走正常 TLS + 身份绑定）
 *   2. 发送 uninstall_req
 *   3. 服务端应答 approved=true 才允许卸载
 *   4. 未批准/连接失败 → 拒绝卸载，进程保持在线
 */
#define _GNU_SOURCE
#include "ctforge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

extern int ctc_tls_wrap(int fd, const char *host, const char *fp, int skip_verify);
extern void ctc_tls_cleanup(int fd);
extern int ctc_tls_write(int fd, const void *buf, int n);

/* 连接服务端（复用主流程的简化版） */
static int connect_server_quick(const ctc_config *cfg) {
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
    if (fd < 0) return -1;
    if (cfg->use_tls) {
        if (ctc_tls_wrap(fd, cfg->server_host,
                         cfg->server_fingerprint[0] ? cfg->server_fingerprint : NULL,
                         cfg->tls_skip_verify) < 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

/* 请求服务端批准卸载。返回 0=批准，1=未批准，-1=连接失败 */
int ctc_uninstall_request(ctc_config *cfg) {
    int fd = connect_server_quick(cfg);
    if (fd < 0) {
        fprintf(stderr, "[ctc-agent] cannot reach server %s:%d — uninstall blocked\n",
                cfg->server_host, cfg->server_port);
        return -1;
    }

    char payload[512];
    snprintf(payload, sizeof(payload),
        "{\"agent_id\":\"%s\",\"hostname\":\"%s\",\"reason\":\"admin requested uninstall\"}",
        cfg->agent_id, cfg->name);

    /* 构造带 token 的顶层消息 */
    char full[768];
    snprintf(full, sizeof(full),
        "{\"id\":\"uninstall-%ld\",\"type\":\"%s\",\"version\":%d,\"token\":\"%s\",\"payload\":%s}",
        (long)time(NULL), MSG_UNINSTALL_REQ, CTC_PROTO_VERSION, cfg->token, payload);

    uint32_t len = (uint32_t)strlen(full);
    uint32_t be = htonl(len);
    if (ctc_tls_write(fd, &be, 4) != 4 || ctc_tls_write(fd, full, (int)len) != (int)len) {
        ctc_tls_cleanup(fd);
        close(fd);
        return -1;
    }

    ctc_message resp = {0};
    if (ctc_recv(fd, &resp) < 0) {
        ctc_tls_cleanup(fd);
        close(fd);
        return -1;
    }

    int approved = 0;
    if (resp.type && !strcmp(resp.type, MSG_UNINSTALL_ACK) && resp.payload) {
        approved = ctc_json_get_bool(resp.payload, "approved", 0);
        char msg[256] = "";
        ctc_json_get_string(resp.payload, "message", msg, sizeof(msg));
        fprintf(stderr, "[ctc-agent] uninstall response: %s (approved=%d)\n", msg, approved);
    }
    ctc_message_free(&resp);
    ctc_tls_cleanup(fd);
    close(fd);
    return approved ? 0 : 1;
}

/* 执行卸载（仅在获批后调用） */
int ctc_do_uninstall(void) {
    /* 停掉 systemd 服务 */
    system("systemctl stop ctforge-agent 2>/dev/null");
    system("systemctl disable ctforge-agent 2>/dev/null");
    system("rm -f /etc/systemd/system/ctforge-agent.service");
    system("systemctl daemon-reload 2>/dev/null");
    /* 删除二进制和配置 */
    unlink("/usr/local/bin/ctforge-agent");
    system("rm -rf /etc/ctforge");
    return 0;
}
