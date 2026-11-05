/* 端口转发与远程升级
 *
 * 端口转发链路：
 *   服务端监听 TargetPort ← 服务端转发进程 ← 控制通道(pf_data) ← agent ← 本地 LocalPort
 * 所有数据都经现有 TLS 控制连接，不额外开洞。
 */
#define _GNU_SOURCE
#include "ctforge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/stat.h>

/* ================= 端口转发 ================= */

#define MAX_PF 16
#define MAX_PF_CONN 32

typedef struct {
    char conn_id[48];
    int  fd;
    int  active;
    pthread_t tid;
} pf_conn;

typedef struct {
    char forward_id[64];
    char local_host[64];
    int  local_port;
    int  sock;            /* 控制连接 fd（发送用） */
    pf_conn conns[MAX_PF_CONN];
    int  used;
} pf_ctx;

static pf_ctx g_pf[MAX_PF];
static pthread_mutex_t g_pf_mu = PTHREAD_MUTEX_INITIALIZER;

/* 全局控制连接写锁：终端读线程、端口转发线程、主循环共用同一 socket，
 * 必须串行化写入，否则 JSON 帧会交错损坏。 */
static pthread_mutex_t g_send_mu = PTHREAD_MUTEX_INITIALIZER;

int ctc_pf_send_data(int sock, const char *forward_id, const char *conn_id,
                     const unsigned char *data, size_t len) {
    if (sock < 0 || len == 0) return -1;
    char *b64 = ctc_base64_encode(data, len);
    if (!b64) return -1;
    size_t cap = strlen(b64) + 256;
    char *payload = malloc(cap);
    if (!payload) { free(b64); return -1; }
    snprintf(payload, cap,
        "{\"forward_id\":\"%s\",\"conn_id\":\"%s\",\"data_base64\":\"%s\"}",
        forward_id, conn_id, b64);
    free(b64);

    pthread_mutex_lock(&g_send_mu);
    int rc = ctc_send(sock, "pf_data", NULL, NULL, payload);
    pthread_mutex_unlock(&g_send_mu);
    free(payload);
    return rc;
}

int ctc_pf_send_close(int sock, const char *forward_id, const char *conn_id) {
    if (sock < 0) return -1;
    char payload[512];
    snprintf(payload, sizeof(payload),
        "{\"forward_id\":\"%s\",\"conn_id\":\"%s\",\"data_base64\":\"\",\"closed\":true}",
        forward_id, conn_id);
    pthread_mutex_lock(&g_send_mu);
    int rc = ctc_send(sock, "pf_data", NULL, NULL, payload);
    pthread_mutex_unlock(&g_send_mu);
    return rc;
}

/* 每个连接一个读线程：本地目标 → 服务端 */
static void *pf_reader(void *arg) {
    /* arg 打包：pf_ctx* + pf_conn* */
    void **pack = arg;
    pf_ctx *ctx = pack[0];
    pf_conn *conn = pack[1];
    free(pack);

    unsigned char buf[8192];
    while (conn->active) {
        ssize_t r = read(conn->fd, buf, sizeof(buf));
        if (r <= 0) break;
        ctc_pf_send_data(ctx->sock, ctx->forward_id, conn->conn_id, buf, (size_t)r);
    }
    ctc_pf_send_close(ctx->sock, ctx->forward_id, conn->conn_id);
    if (conn->fd >= 0) { close(conn->fd); conn->fd = -1; }
    conn->active = 0;
    return NULL;
}

/* 建立一条到本地目标的连接 */
static int pf_connect(pf_ctx *ctx, const char *conn_id, char *errbuf, size_t errsz) {
    /* 找一个空槽 */
    int slot = -1;
    for (int i = 0; i < MAX_PF_CONN; i++) {
        if (!ctx->conns[i].active && ctx->conns[i].fd < 0) { slot = i; break; }
    }
    if (slot < 0) { snprintf(errbuf, errsz, "too many connections"); return -1; }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { snprintf(errbuf, errsz, "socket failed"); return -1; }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)ctx->local_port);
    const char *host = ctx->local_host[0] ? ctx->local_host : "127.0.0.1";
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        struct hostent *he = gethostbyname(host);
        if (!he) { close(fd); snprintf(errbuf, errsz, "bad host %s", host); return -1; }
        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
    }
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(errbuf, errsz, "connect %s:%d failed: %s", host, ctx->local_port, strerror(errno));
        close(fd);
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &(struct timeval){0,0}, sizeof(struct timeval));

    pf_conn *c = &ctx->conns[slot];
    memset(c, 0, sizeof(*c));
    snprintf(c->conn_id, sizeof(c->conn_id), "%s", conn_id);
    c->fd = fd;
    c->active = 1;

    void **pack = malloc(sizeof(void *) * 2);
    pack[0] = ctx;
    pack[1] = c;
    if (pthread_create(&c->tid, NULL, pf_reader, pack) != 0) {
        free(pack);
        c->active = 0;
        close(fd);
        c->fd = -1;
        snprintf(errbuf, errsz, "thread create failed");
        return -1;
    }
    pthread_detach(c->tid);
    return 0;
}

int ctc_pf_open(int sock, const char *forward_id, const char *target_host, int target_port,
                const char *local_host, int local_port) {
    (void)target_host; (void)target_port; /* 服务端侧信息，agent 不需要 */
    pthread_mutex_lock(&g_pf_mu);
    pf_ctx *ctx = NULL;
    for (int i = 0; i < MAX_PF; i++) {
        if (!g_pf[i].used) { ctx = &g_pf[i]; break; }
    }
    if (!ctx) { pthread_mutex_unlock(&g_pf_mu); return -1; }
    memset(ctx, 0, sizeof(*ctx));
    snprintf(ctx->forward_id, sizeof(ctx->forward_id), "%s", forward_id);
    snprintf(ctx->local_host, sizeof(ctx->local_host), "%s", local_host && *local_host ? local_host : "127.0.0.1");
    ctx->local_port = local_port;
    ctx->sock = sock;   /* 关键：记录控制连接，pf_data 才能发回服务端 */
    ctx->used = 1;
    for (int i = 0; i < MAX_PF_CONN; i++) ctx->conns[i].fd = -1;
    pthread_mutex_unlock(&g_pf_mu);
    return 0;
}

void ctc_pf_close(const char *forward_id) {
    pthread_mutex_lock(&g_pf_mu);
    for (int i = 0; i < MAX_PF; i++) {
        if (!g_pf[i].used) continue;
        if (strcmp(g_pf[i].forward_id, forward_id) != 0) continue;
        for (int j = 0; j < MAX_PF_CONN; j++) {
            if (g_pf[i].conns[j].active) {
                g_pf[i].conns[j].active = 0;
                if (g_pf[i].conns[j].fd >= 0) {
                    shutdown(g_pf[i].conns[j].fd, SHUT_RDWR);
                    close(g_pf[i].conns[j].fd);
                    g_pf[i].conns[j].fd = -1;
                }
            }
        }
        g_pf[i].used = 0;
        break;
    }
    pthread_mutex_unlock(&g_pf_mu);
}

void ctc_pf_close_all(void) {
    pthread_mutex_lock(&g_pf_mu);
    for (int i = 0; i < MAX_PF; i++) {
        if (!g_pf[i].used) continue;
        for (int j = 0; j < MAX_PF_CONN; j++) {
            if (g_pf[i].conns[j].active) {
                g_pf[i].conns[j].active = 0;
                if (g_pf[i].conns[j].fd >= 0) { close(g_pf[i].conns[j].fd); g_pf[i].conns[j].fd = -1; }
            }
        }
        g_pf[i].used = 0;
    }
    pthread_mutex_unlock(&g_pf_mu);
}

/* 服务端收到 pf_data 后调用：把数据写给本地目标 */
int ctc_pf_write(const char *forward_id, const char *conn_id,
                 const unsigned char *data, size_t len) {
    pthread_mutex_lock(&g_pf_mu);
    int fd = -1;
    for (int i = 0; i < MAX_PF; i++) {
        if (!g_pf[i].used || strcmp(g_pf[i].forward_id, forward_id) != 0) continue;
        for (int j = 0; j < MAX_PF_CONN; j++) {
            if (g_pf[i].conns[j].active && strcmp(g_pf[i].conns[j].conn_id, conn_id) == 0) {
                fd = g_pf[i].conns[j].fd;
                break;
            }
        }
        break;
    }
    pthread_mutex_unlock(&g_pf_mu);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

/* 服务端要求关闭某条转发连接 */
void ctc_pf_conn_close(const char *forward_id, const char *conn_id) {
    pthread_mutex_lock(&g_pf_mu);
    for (int i = 0; i < MAX_PF; i++) {
        if (!g_pf[i].used || strcmp(g_pf[i].forward_id, forward_id) != 0) continue;
        for (int j = 0; j < MAX_PF_CONN; j++) {
            if (g_pf[i].conns[j].active && strcmp(g_pf[i].conns[j].conn_id, conn_id) == 0) {
                g_pf[i].conns[j].active = 0;
                if (g_pf[i].conns[j].fd >= 0) { close(g_pf[i].conns[j].fd); g_pf[i].conns[j].fd = -1; }
                break;
            }
        }
        break;
    }
    pthread_mutex_unlock(&g_pf_mu);
}

/* 服务端新建一条转发连接时调用（conn_id 由服务端生成） */
int ctc_pf_accept_conn(const char *forward_id, const char *conn_id) {
    char err[256] = "";
    pthread_mutex_lock(&g_pf_mu);
    pf_ctx *ctx = NULL;
    for (int i = 0; i < MAX_PF; i++) {
        if (g_pf[i].used && strcmp(g_pf[i].forward_id, forward_id) == 0) { ctx = &g_pf[i]; break; }
    }
    if (!ctx) { pthread_mutex_unlock(&g_pf_mu); return -1; }
    int rc = pf_connect(ctx, conn_id, err, sizeof(err));
    pthread_mutex_unlock(&g_pf_mu);
    if (rc < 0) fprintf(stderr, "[ctc-agent] pf accept %s/%s: %s\n", forward_id, conn_id, err);
    return rc;
}

/* ================= 远程升级 ================= */

/* 通过 HTTP(S) 下载文件（用 curl，兼容性最好） */
static int download_file(const char *url, const char *out_path) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "curl -fsSL --connect-timeout 20 --max-time 300 -o '%s' '%s' 2>/dev/null || "
        "wget -q -T 300 -O '%s' '%s' 2>/dev/null",
        out_path, url, out_path, url);
    int rc = system(cmd);
    return rc == 0 ? 0 : -1;
}

static char *sha256_of_file(const char *path) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "sha256sum '%s' 2>/dev/null | cut -d' ' -f1", path);
    FILE *f = popen(cmd, "r");
    if (!f) return NULL;
    char buf[128] = {0};
    if (!fgets(buf, sizeof(buf), f)) { pclose(f); return NULL; }
    pclose(f);
    size_t n = strlen(buf);
    while (n && (buf[n-1]=='\n' || buf[n-1]==' ')) buf[--n] = 0;
    return n == 64 ? strdup(buf) : NULL;
}

/* 执行升级：下载 → 校验 → 原子替换 → 重启服务
 * 注意：替换后当前进程仍在运行旧 inode，由 systemd 重启拉起新版本。
 */
char *ctc_upgrade_json(const char *url, const char *want_sha256, const char *version) {
    char out[1024];
    if (!url || !*url) {
        snprintf(out, sizeof(out), "{\"ok\":false,\"message\":\"missing url\"}");
        return strdup(out);
    }

    char tmp[] = "/tmp/ctforge-agent-upgrade.XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) {
        snprintf(out, sizeof(out), "{\"ok\":false,\"message\":\"mkstemp failed\"}");
        return strdup(out);
    }
    close(fd);

    if (download_file(url, tmp) < 0) {
        unlink(tmp);
        snprintf(out, sizeof(out), "{\"ok\":false,\"message\":\"download failed\",\"version\":\"%s\"}",
                 version ? version : "");
        return strdup(out);
    }

    /* 校验 SHA256（若提供） */
    if (want_sha256 && strlen(want_sha256) == 64) {
        char *got = sha256_of_file(tmp);
        if (!got || strcmp(got, want_sha256) != 0) {
            free(got);
            unlink(tmp);
            snprintf(out, sizeof(out),
                "{\"ok\":false,\"message\":\"sha256 mismatch\",\"version\":\"%s\"}",
                version ? version : "");
            return strdup(out);
        }
        free(got);
    }

    /* 校验是 ELF 可执行文件 */
    FILE *vf = fopen(tmp, "rb");
    if (vf) {
        unsigned char magic[4] = {0};
        size_t rd = fread(magic, 1, 4, vf);
        fclose(vf);
        if (rd < 4 || magic[0] != 0x7f || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
            unlink(tmp);
            snprintf(out, sizeof(out),
                "{\"ok\":false,\"message\":\"downloaded file is not a valid ELF binary\",\"version\":\"%s\"}",
                version ? version : "");
            return strdup(out);
        }
    }

    chmod(tmp, 0755);

    /* 原子替换：新文件写到同目录再 rename（跨分区不安全，先拷到目标目录） */
    const char *target = "/usr/local/bin/ctforge-agent";
    char stage[512];
    snprintf(stage, sizeof(stage), "%s.new.%d", target, (int)getpid());

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cp '%s' '%s' && chmod 755 '%s' && mv -f '%s' '%s'",
             tmp, stage, stage, stage, target);
    int rc = system(cmd);
    unlink(tmp);
    if (rc != 0) {
        unlink(stage);
        snprintf(out, sizeof(out), "{\"ok\":false,\"message\":\"install failed\",\"version\":\"%s\"}",
                 version ? version : "");
        return strdup(out);
    }

    snprintf(out, sizeof(out),
        "{\"ok\":true,\"message\":\"upgraded, restarting service\",\"version\":\"%s\"}",
        version ? version : "");

    /* 异步重启：先回包再重启，避免连接被立刻切断 */
    if (fork() == 0) {
        sleep(2);
        execlp("systemctl", "systemctl", "restart", "ctforge-agent", (char *)NULL);
        _exit(0);
    }

    return strdup(out);
}
