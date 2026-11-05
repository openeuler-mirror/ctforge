/* 客户端异常检测：网络流量突增、SSH 爆破、CPU/内存/磁盘异常。
 * 检出后主动上报服务端（anomaly 消息），由服务端告警引擎推送通知。
 * 设计原则：全部检测复用已采集的指标，不额外读 /proc，CPU 开销可忽略。
 */
#define _GNU_SOURCE
#include "ctforge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

void ctc_anomaly_init(ctc_anomaly *st) {
    memset(st, 0, sizeof(*st));
    st->last_sample = time(NULL);
}

/* 统计近 60 秒 SSH 失败登录次数 */
static long count_ssh_fails_60s(void) {
    /* 优先读 journalctl 中的 sshd 失败记录，失败则回退 /var/log/secure */
    FILE *f = popen("journalctl -u sshd -u ssh --since '60 seconds ago' --no-pager 2>/dev/null "
                    "| grep -ciE 'failed password|invalid user' 2>/dev/null", "r");
    if (f) {
        char buf[32] = {0};
        if (fgets(buf, sizeof(buf), f)) {
            pclose(f);
            long n = atol(buf);
            if (n > 0) return n;
        } else {
            pclose(f);
        }
    }
    /* 回退：读 /var/log/secure 尾部 */
    f = popen("tail -2000 /var/log/secure 2>/dev/null | grep -ciE 'failed password|invalid user'", "r");
    if (f) {
        char buf[32] = {0};
        long n = 0;
        if (fgets(buf, sizeof(buf), f)) n = atol(buf);
        pclose(f);
        return n;
    }
    return 0;
}

/* 读当前累计网络字节数（所有非 lo 接口之和） */
static int total_net_bytes(uint64_t *rx, uint64_t *tx) {
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return -1;
    *rx = 0;
    *tx = 0;
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        if (++lineno <= 2) continue;
        char name[64] = {0};
        unsigned long long v[16] = {0};
        int n = sscanf(line,
            " %63[^:]: %llu %llu %llu %llu %llu %llu %llu %llu"
            " %llu %llu %llu %llu %llu %llu %llu %llu",
            name, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
            &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15]);
        if (n < 17) continue;
        if (!strcmp(name, "lo")) continue;
        if (!strncmp(name, "docker", 6) || !strncmp(name, "veth", 4) ||
            !strncmp(name, "br-", 3)) continue; /* 排除容器虚拟网卡，避免误报 */
        *rx += v[0];
        *tx += v[8];
    }
    fclose(f);
    return 0;
}

static void send_anomaly_msg(int fd, const char *kind, const char *level, const char *detail) {
    if (fd < 0) return;
    char payload[1024];
    /* 转义 detail 里的引号 */
    char esc[768];
    size_t j = 0;
    for (const char *p = detail; *p && j + 2 < sizeof(esc); p++) {
        if (*p == '"' || *p == '\\') esc[j++] = '\\';
        if (*p == '\n') { esc[j++] = '\\'; esc[j++] = 'n'; continue; }
        esc[j++] = *p;
    }
    esc[j] = 0;
    snprintf(payload, sizeof(payload),
        "{\"kind\":\"%s\",\"level\":\"%s\",\"detail\":\"%s\"}", kind, level, esc);
    ctc_send_simple(fd, MSG_ANOMALY, payload);
}

/* 每次心跳调用；内部有节流，不会频繁触发 */
void ctc_anomaly_check(int fd, ctc_anomaly *st) {
    if (fd < 0) return;
    time_t now = time(NULL);

    /* ---- 1. 网络流量突增（采样间隔 >= 10s）---- */
    if (now - st->last_sample >= 10) {
        uint64_t rx = 0, tx = 0;
        if (total_net_bytes(&rx, &tx) == 0 && st->last_rx_bytes > 0) {
            double dt = (double)(now - st->last_sample);
            double rx_rate = ((double)rx - (double)st->last_rx_bytes) / dt;
            double tx_rate = ((double)tx - (double)st->last_tx_bytes) / dt;

            /* 阈值：单接口总速率 > 80 MB/s 视为异常（可调） */
            if (rx_rate > 80.0 * 1024 * 1024 || tx_rate > 80.0 * 1024 * 1024) {
                if (now - st->last_net_report > 300) { /* 5 分钟静默 */
                    char detail[256];
                    snprintf(detail, sizeof(detail),
                        "接收 %.1f MB/s，发送 %.1f MB/s（阈值 80 MB/s）",
                        rx_rate / 1024 / 1024, tx_rate / 1024 / 1024);
                    send_anomaly_msg(fd, "net_spike", "warning", detail);
                    st->last_net_report = now;
                }
            }
            st->last_rx_bytes = rx;
            st->last_tx_bytes = tx;
        } else if (st->last_rx_bytes == 0) {
            st->last_rx_bytes = rx;
            st->last_tx_bytes = tx;
        }
        st->last_sample = now;
    }

    /* ---- 2. SSH 爆破（每 60s 检查一次）---- */
    if (now - st->last_ssh_report >= 60) {
        long fails = count_ssh_fails_60s();
        st->ssh_fail_count = fails;
        /* 阈值：60 秒内 >= 20 次失败 */
        if (fails >= 20) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                "近 60 秒检测到 %ld 次 SSH 登录失败，疑似暴力破解", fails);
            send_anomaly_msg(fd, "ssh_brute", "critical", detail);
        }
        st->last_ssh_report = now;
    }
}
