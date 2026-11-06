/*
 * ctfd.c - ctforged 守护进程自动拉起与状态上报
 *
 * agent 注册成功后调用：检查 ctforged 是否运行，未运行则尝试拉起，
 * 结果通过 ctf_daemon_report 消息上报服务端展示。
 */

#include "ctforge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <sys/wait.h>

/* 执行命令并捕获退出码（带 10s 超时保护） */
static int run_cmd(const char *cmd)
{
    int rc = system(cmd);
    if (rc == -1)
        return -1;
    if (WIFEXITED(rc))
        return WEXITSTATUS(rc);
    return -1;
}

/* 检测 ctforged 是否在监听（unix socket 或 tcp，读配置） */
static int ctfd_probe_running(void)
{
    /* 1. 先看进程是否存在 */
    if (run_cmd("pgrep -x ctforged >/dev/null 2>&1") == 0)
        return 1;

    /* 2. 进程名不匹配时，探测配置中的 listen 地址 */
    char listen_addr[256] = {0};
    FILE *fp = fopen("/etc/ctforge/ctforge.cfg", "r");
    if (!fp)
        return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *p = strstr(line, "\"listen\"");
        if (!p)
            continue;
        char *q1 = strchr(strchr(p, ':') ? strchr(p, ':') : p, '"');
        if (!q1)
            continue;
        char *q2 = strchr(q1 + 1, '"');
        if (!q2)
            continue;
        size_t n = (size_t)(q2 - q1 - 1);
        if (n >= sizeof(listen_addr))
            n = sizeof(listen_addr) - 1;
        memcpy(listen_addr, q1 + 1, n);
        listen_addr[n] = '\0';
        found = 1;
        break;
    }
    fclose(fp);
    if (!found)
        return 0;

    /* unix socket：直接 connect 探测 */
    if (strncmp(listen_addr, "unix://", 7) == 0) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return 0;
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", listen_addr + 7);
        int ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        close(fd);
        return ok == 0 ? 1 : 0;
    }

    /* tcp：getaddrinfo 探测 */
    char *colon = strrchr(listen_addr, ':');
    if (!colon)
        return 0;
    char host[200] = {0};
    size_t hlen = (size_t)(colon - listen_addr);
    if (hlen >= sizeof(host))
        return 0;
    memcpy(host, listen_addr, hlen);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, colon + 1, &hints, &res) != 0)
        return 0;
    int ok = 0;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;
        struct timeval tv = {2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            ok = 1;
            close(fd);
            break;
        }
        close(fd);
    }
    freeaddrinfo(res);
    return ok;
}

/* 检测 ctforged 是否已安装（二进制存在） */
static int ctfd_binary_path(char *buf, size_t buflen)
{
    static const char *paths[] = {
        "/usr/sbin/ctforged",
        "/usr/local/sbin/ctforged",
        "/usr/bin/ctforged",
        "/usr/local/bin/ctforged",
        NULL,
    };
    for (int i = 0; paths[i]; i++) {
        if (access(paths[i], X_OK) == 0) {
            snprintf(buf, buflen, "%s", paths[i]);
            return 1;
        }
    }
    return 0;
}

/*
 * 确保 ctforged 运行。返回上报用的 JSON 字符串（malloc，调用方 free）。
 * 状态：running / started / failed / not_installed
 */
char *ctfd_ensure_running(void)
{
    /* 已在运行 */
    if (ctfd_probe_running()) {
        return strdup("{\"state\":\"running\",\"message\":\"ctforged is running\"}");
    }

    /* 未安装 */
    char bin[256] = {0};
    if (!ctfd_binary_path(bin, sizeof(bin))) {
        return strdup("{\"state\":\"not_installed\","
                      "\"message\":\"ctforged binary not found, "
                      "please install ctforge eBPF component\"}");
    }

    /* 尝试拉起：systemd → init.d → 直接后台启动 */
    int started = 0;
    const char *method = NULL;

    if (run_cmd("command -v systemctl >/dev/null 2>&1 && "
                "[ -f /etc/systemd/system/ctforged.service ]") == 0) {
        if (run_cmd("systemctl enable ctforged >/dev/null 2>&1; "
                    "systemctl start ctforged") == 0) {
            started = 1;
            method = "systemd";
        }
    }

    if (!started && access("/etc/init.d/ctforged", X_OK) == 0) {
        if (run_cmd("/etc/init.d/ctforged start >/dev/null 2>&1") == 0) {
            started = 1;
            method = "init.d";
        }
    }

    if (!started) {
        /* 直接后台启动（无 init 系统场景） */
        char cmd[300];
        snprintf(cmd, sizeof(cmd),
                 "nohup %s >/var/log/ctforged.log 2>&1 &", bin);
        if (run_cmd(cmd) == 0) {
            /* 等 1s 再探测 */
            sleep(1);
            if (ctfd_probe_running()) {
                started = 1;
                method = "direct";
            }
        }
    }

    /* 拉起后最终确认 */
    if (started && ctfd_probe_running()) {
        char *out = NULL;
        if (asprintf(&out,
                     "{\"state\":\"started\",\"message\":\"ctforged started via %s\"}",
                     method) < 0)
            return strdup("{\"state\":\"started\",\"message\":\"ctforged started\"}");
        return out;
    }

    /* 拉起失败 */
    char *out = NULL;
    if (asprintf(&out,
                 "{\"state\":\"failed\","
                 "\"message\":\"failed to start ctforged from %s, "
                 "check journalctl -u ctforged or /var/log/ctforged.log\"}",
                 bin) < 0)
        return strdup("{\"state\":\"failed\",\"message\":\"failed to start ctforged\"}");
    return out;
}
