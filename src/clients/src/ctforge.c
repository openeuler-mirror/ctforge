/*
 * ctforge.c - CTForge eBPF 框架桥接模块
 *
 * 通过 ctforged 的 JSON-RPC 接口（unix socket 或 tcp）转发控制命令。
 * 支持方法: lsmod / insmod / rmmod / mod / status
 */

#include "ctforge.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <unistd.h>

/* socket I/O 超时：防止 ctforged 挂起时 agent 永远阻塞 */
#define CTFORGE_IO_TIMEOUT_SEC 10

static int ctforge_set_timeout(int fd)
{
    struct timeval tv;
    tv.tv_sec = CTFORGE_IO_TIMEOUT_SEC;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0)
        return -1;
    return 0;
}

#define CTFORGE_CFG_PATH "/etc/ctforge/ctforge.cfg"
#define CTFORGE_RESP_MAX (256 * 1024)

/* 从 ctforged 配置文件读取 listen 地址（"127.0.0.1:7000" 或 "unix:///path"） */
static int ctforge_read_listen(char *buf, size_t buflen)
{
    FILE *fp = fopen(CTFORGE_CFG_PATH, "r");
    if (!fp)
        return -1;

    char line[512];
    int found = -1;
    while (fgets(line, sizeof(line), fp)) {
        char *p = strstr(line, "\"listen\"");
        if (!p)
            continue;
        char *colon = strchr(p, ':');
        if (!colon)
            continue;
        char *q1 = strchr(colon, '"');
        if (!q1)
            continue;
        char *q2 = strchr(q1 + 1, '"');
        if (!q2)
            continue;
        size_t n = (size_t)(q2 - q1 - 1);
        if (n >= buflen)
            n = buflen - 1;
        memcpy(buf, q1 + 1, n);
        buf[n] = '\0';
        found = 0;
        break;
    }
    fclose(fp);
    return found;
}

static int ctforge_connect_unix(const char *path)
{
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "[ctforge] socket path too long (%zu >= %zu): %s\n",
                strlen(path),
                sizeof(((struct sockaddr_un *)0)->sun_path), path);
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (ctforge_set_timeout(fd) < 0) {
        close(fd);
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int ctforge_connect_tcp(const char *host, int port)
{
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return -1;
    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;
        if (ctforge_set_timeout(fd) < 0) {
            close(fd);
            fd = -1;
            continue;
        }
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* 建立到 ctforged 的连接；失败返回 -1 */
static int ctforge_connect(void)
{
    char listen_addr[256] = {0};
    if (ctforge_read_listen(listen_addr, sizeof(listen_addr)) != 0)
        return -1;

    if (strncmp(listen_addr, "unix://", 7) == 0)
        return ctforge_connect_unix(listen_addr + 7);

    char host[200] = {0};
    int port = 0;
    char *colon = strrchr(listen_addr, ':');
    if (!colon)
        return -1;
    size_t hlen = (size_t)(colon - listen_addr);
    if (hlen >= sizeof(host))
        return -1;
    memcpy(host, listen_addr, hlen);
    port = atoi(colon + 1);
    if (port <= 0)
        return -1;
    return ctforge_connect_tcp(host, port);
}

/*
 * 向 ctforged 发送 JSON-RPC 请求并读取响应（一行 JSON）。
 * 返回 malloc 的响应字符串，调用方负责 free；失败返回 NULL。
 */
static char *ctforge_rpc(const char *method, const char *params)
{
    int fd = ctforge_connect();
    if (fd < 0)
        return NULL;

    /* 构造与 ctforgectl 一致的请求格式 */
    const char *fmt =
        "{\"jsonrpc\":\"6.0\",\"type\":0,\"taskid\":%ld,"
        "\"method\":\"%s\",\"reqmsg\":{\"params\":\"%s\"}}\n";
    char *req = NULL;
    if (asprintf(&req, fmt, (long)time(NULL), method,
                 params ? params : "") < 0) {
        close(fd);
        return NULL;
    }

    size_t off = 0, len = strlen(req);
    while (off < len) {
        ssize_t n = write(fd, req + off, len - off);
        if (n <= 0) {
            free(req);
            close(fd);
            return NULL;
        }
        off += (size_t)n;
    }
    free(req);

    char *resp = malloc(CTFORGE_RESP_MAX);
    if (!resp) {
        close(fd);
        return NULL;
    }
    size_t total = 0;
    while (total < CTFORGE_RESP_MAX - 1) {
        ssize_t n = read(fd, resp + total, CTFORGE_RESP_MAX - 1 - total);
        if (n <= 0)
            break;
        total += (size_t)n;
        if (memchr(resp, '\n', total))
            break;
    }
    close(fd);
    resp[total] = '\0';

    if (total == 0) {
        free(resp);
        return NULL;
    }
    /* 去掉行尾换行 */
    char *nl = strchr(resp, '\n');
    if (nl)
        *nl = '\0';
    return resp;
}

/* 从 ctforged 响应 JSON 中提取 resmsg 字段（原样返回子串） */
static char *ctforge_extract_resmsg(const char *resp)
{
    if (!resp)
        return NULL;
    const char *key = "\"resmsg\":";
    const char *p = strstr(resp, key);
    if (!p)
        return strdup(resp); /* 没有 resmsg 时返回原文 */
    p += strlen(key);
    /* 返回从 resmsg 值开始到末尾的内容（保持 JSON 结构） */
    /* 找匹配的结尾：简单处理——剥离最后一个 '}' */
    size_t len = strlen(p);
    if (len > 0 && p[len - 1] == '}')
        len--;
    char *out = malloc(len + 1);
    if (!out)
        return NULL;
    memcpy(out, p, len);
    out[len] = '\0';
    return out;
}

/*
 * 对外接口：执行 ctforge 命令，返回 JSON 字符串（malloc）。
 * method: lsmod / insmod / rmmod / mod / status
 * params: 命令参数（如模块名），可为空字符串
 */
char *ctc_ctforge_exec(const char *method, const char *params)
{
    if (!method || !*method)
        return strdup("{\"error\":\"empty method\"}");

    /* 仅允许白名单方法，防止任意 RPC 调用 */
    static const char *allowed[] = {"lsmod", "insmod", "rmmod",
                                    "mod",   "status", NULL};
    int ok = 0;
    for (int i = 0; allowed[i]; i++) {
        if (strcmp(method, allowed[i]) == 0) {
            ok = 1;
            break;
        }
    }
    if (!ok)
        return strdup("{\"error\":\"method not allowed\"}");

    char *resp = ctforge_rpc(method, params ? params : "");
    if (!resp)
        return strdup("{\"error\":\"cannot connect to ctforged, "
                      "please check ctforged service\"}");

    char *resmsg = ctforge_extract_resmsg(resp);
    free(resp);
    if (!resmsg)
        return strdup("{\"error\":\"out of memory\"}");
    return resmsg;
}
