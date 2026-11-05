/* UDP 局域网发现：周期向 255.255.255.255:discover_port 广播本机信息，
 * 服务端监听同端口即可在控制台展示"局域网内有哪些 agent"。
 * 报文: 4B 魔数 "CTC1" + 2B BE 长度 + JSON
 */
#include "ctforge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/utsname.h>

static int g_discover_run = 0;
static pthread_t g_tid;

static void *discover_loop(void *arg) {
    ctc_config cfg = *(ctc_config *)arg;
    free(arg);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return NULL;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)(cfg.discover_port > 0 ? cfg.discover_port : CTC_DISCOVER_PORT));
    inet_pton(AF_INET, "255.255.255.255", &to.sin_addr);

    struct utsname un;
    uname(&un);
    /* 采集发行版详细版本 + 内核，供服务端「批准管理」直接展示 */
    char os_ver[256] = "", kernel[128] = "";
    ctc_sysinfo(os_ver, sizeof(os_ver), kernel, sizeof(kernel));
    if (!kernel[0]) snprintf(kernel, sizeof(kernel), "%s", un.release);

    /* 磁盘占用 */
    ctc_metrics m;
    ctc_collect_metrics(&m);

    /* 探测本机第一个非回环 IPv4，供服务端展示真实 IP（而非回环） */
    char my_ip[64] = "";
    {
        struct ifaddrs *ifa = NULL;
        if (getifaddrs(&ifa) == 0) {
            for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
                if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
                struct sockaddr_in *sa = (struct sockaddr_in *)p->ifa_addr;
                unsigned char b0 = (unsigned char)(sa->sin_addr.s_addr & 0xFF);
                if (b0 == 127) continue;
                snprintf(my_ip, sizeof(my_ip), "%s", inet_ntoa(sa->sin_addr));
                break;
            }
            freeifaddrs(ifa);
        }
    }
    if (!my_ip[0]) snprintf(my_ip, sizeof(my_ip), "127.0.0.1");

    char body[1200];
    int blen = snprintf(body, sizeof(body),
        "{\"agent_id\":\"%s\",\"name\":\"%s\",\"hostname\":\"%s\",\"os\":\"%s\","
        "\"os_version\":\"%s\",\"kernel\":\"%s\",\"arch\":\"%s\",\"version\":\"%s\","
        "\"ip\":\"%s\",\"disk_percent\":%.2f,\"port\":%d}",
        cfg.agent_id, cfg.name, un.nodename, un.sysname,
        os_ver, kernel, un.machine, CTC_VERSION, my_ip, m.disk_percent, cfg.server_port);

    unsigned char pkt[1152];
    memcpy(pkt, "CTC1", 4);
    pkt[4] = (unsigned char)((blen >> 8) & 0xFF);
    pkt[5] = (unsigned char)(blen & 0xFF);
    memcpy(pkt + 6, body, (size_t)blen);

    while (g_discover_run) {
        sendto(fd, pkt, (size_t)(6 + blen), 0, (struct sockaddr *)&to, sizeof(to));
        /* 间隔 15s，分 15 次 sleep 便于快速退出 */
        for (int i = 0; i < 150 && g_discover_run; i++) usleep(100000);
    }
    close(fd);
    return NULL;
}

int ctc_discover_start(const ctc_config *cfg) {
    if (!cfg->discover_enable) return 0;
    if (g_discover_run) return 0;
    g_discover_run = 1;
    ctc_config *copy = malloc(sizeof(ctc_config));
    *copy = *cfg;
    if (pthread_create(&g_tid, NULL, discover_loop, copy) != 0) {
        free(copy);
        g_discover_run = 0;
        return -1;
    }
    pthread_detach(g_tid);
    return 0;
}

void ctc_discover_stop(void) { g_discover_run = 0; }
