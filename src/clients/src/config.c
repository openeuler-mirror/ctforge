#include "ctforge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* hostname 太通用（多台机器同名会撞 agent_id 被服务端互踢）时返回真 */
static int generic_hostname(const char *h) {
    if (!h || !*h) return 1;
    static const char *bad[] = {
        "localhost", "localhost.localdomain", "openwrt", "istoreos",
        "router", "ubuntu", "debian", "fedora", "linux", NULL
    };
    char low[128] = {0};
    size_t i = 0;
    for (; h[i] && i < sizeof(low) - 1; i++) low[i] = (char)tolower((unsigned char)h[i]);
    for (int j = 0; bad[j]; j++) if (!strcmp(low, bad[j])) return 1;
    return 0;
}

/* 取本机第一个非回环 IPv4 地址；失败返回 0 */
static int primary_ipv4(char *out, size_t n) {
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0) return 0;
    int ok = 0;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        struct sockaddr_in *a = (struct sockaddr_in *)p->ifa_addr;
        if (ntohl(a->sin_addr.s_addr) >> 24 == 127) continue; /* 跳过回环 */
        if (!inet_ntop(AF_INET, &a->sin_addr, out, (socklen_t)n)) continue;
        ok = 1;
        break;
    }
    freeifaddrs(ifa);
    return ok;
}

static void trim(char *s) {
    char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

void ctc_config_defaults(ctc_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->server_host, "127.0.0.1");
    cfg->server_port = 9000;
    cfg->metrics_interval = 30;
    cfg->heartbeat_interval = 10;
    cfg->log_level = 1;
    char host[128] = {0};
    if (gethostname(host, sizeof(host) - 1) == 0) {
        if (generic_hostname(host)) {
            char ip[64] = {0};
            if (primary_ipv4(ip, sizeof(ip))) {
                strncpy(cfg->agent_id, ip, sizeof(cfg->agent_id) - 1);
                strncpy(cfg->name, ip, sizeof(cfg->name) - 1);
            } else {
                strncpy(cfg->agent_id, host, sizeof(cfg->agent_id) - 1);
                strncpy(cfg->name, host, sizeof(cfg->name) - 1);
            }
        } else {
            strncpy(cfg->agent_id, host, sizeof(cfg->agent_id) - 1);
            strncpy(cfg->name, host, sizeof(cfg->name) - 1);
        }
    }
}

int ctc_config_load(ctc_config *cfg, const char *path) {
    ctc_config_defaults(cfg);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == ';') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k); trim(v);
        if (!strcmp(k, "server_host")) {
            /* 支持逗号分隔的多服务端地址：
               server_host 存第一个 IP（向后兼容），server_hosts 存完整列表 */
            strncpy(cfg->server_hosts, v, sizeof(cfg->server_hosts) - 1);
            const char *comma = strchr(v, ',');
            if (comma) {
                size_t n = (size_t)(comma - v);
                if (n >= sizeof(cfg->server_host))
                    n = sizeof(cfg->server_host) - 1;
                memcpy(cfg->server_host, v, n);
                cfg->server_host[n] = '\0';
            } else {
                strncpy(cfg->server_host, v, sizeof(cfg->server_host) - 1);
            }
        }
        else if (!strcmp(k, "server_port")) cfg->server_port = atoi(v);
        else if (!strcmp(k, "agent_id")) strncpy(cfg->agent_id, v, sizeof(cfg->agent_id) - 1);
        else if (!strcmp(k, "name")) strncpy(cfg->name, v, sizeof(cfg->name) - 1);
        else if (!strcmp(k, "token")) strncpy(cfg->token, v, sizeof(cfg->token) - 1);
        else if (!strcmp(k, "metrics_interval")) cfg->metrics_interval = atoi(v);
        else if (!strcmp(k, "heartbeat_interval")) cfg->heartbeat_interval = atoi(v);
        else if (!strcmp(k, "log_level")) cfg->log_level = atoi(v);
        else if (!strcmp(k, "use_tls")) cfg->use_tls = atoi(v);
        else if (!strcmp(k, "tls_skip_verify")) cfg->tls_skip_verify = atoi(v);
        else if (!strcmp(k, "server_fingerprint")) strncpy(cfg->server_fingerprint, v, sizeof(cfg->server_fingerprint) - 1);
        else if (!strcmp(k, "discover_enable")) cfg->discover_enable = atoi(v);
        else if (!strcmp(k, "discover_port")) cfg->discover_port = atoi(v);
        else if (!strcmp(k, "bound_server_id")) strncpy(cfg->bound_server_id, v, sizeof(cfg->bound_server_id) - 1);
        else if (!strcmp(k, "bound_server_ip")) strncpy(cfg->bound_server_ip, v, sizeof(cfg->bound_server_ip) - 1);
        else if (!strcmp(k, "bound_server_port")) cfg->bound_server_port = atoi(v);
        else if (!strcmp(k, "approved")) cfg->approved = atoi(v);
    }
    fclose(f);
    return 0;
}

int ctc_config_save(const ctc_config *cfg, const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f,
        "# ctforge agent config\n"
        "server_host=%s\n"
        "server_port=%d\n"
        "agent_id=%s\n"
        "name=%s\n"
        "token=%s\n"
        "metrics_interval=%d\n"
        "heartbeat_interval=%d\n"
        "log_level=%d\n"
        "use_tls=%d\n"
        "tls_skip_verify=%d\n"
        "server_fingerprint=%s\n"
        "discover_enable=%d\n"
        "discover_port=%d\n"
        "bound_server_id=%s\n"
        "bound_server_ip=%s\n"
        "bound_server_port=%d\n"
        "approved=%d\n",
        cfg->server_host, cfg->server_port, cfg->agent_id, cfg->name,
        cfg->token, cfg->metrics_interval, cfg->heartbeat_interval, cfg->log_level,
        cfg->use_tls, cfg->tls_skip_verify, cfg->server_fingerprint,
        cfg->discover_enable, cfg->discover_port,
        cfg->bound_server_id, cfg->bound_server_ip, cfg->bound_server_port, cfg->approved);
    fclose(f);
    return rename(tmp, path);
}

/* 将扁平配置序列化为 JSON（用于 config_get 应答） */
char *ctc_config_to_json(const ctc_config *cfg) {
    char *buf = malloc(2048);
    if (!buf) return NULL;
    snprintf(buf, 2048,
        "{\"config\":{"
        "\"server_host\":\"%s\","
        "\"server_port\":\"%d\","
        "\"agent_id\":\"%s\","
        "\"name\":\"%s\","
        "\"metrics_interval\":\"%d\","
        "\"heartbeat_interval\":\"%d\","
        "\"log_level\":\"%d\","
        "\"use_tls\":\"%d\","
        "\"discover_enable\":\"%d\","
        "\"discover_port\":\"%d\""
        "}}",
        cfg->server_host, cfg->server_port, cfg->agent_id, cfg->name,
        cfg->metrics_interval, cfg->heartbeat_interval, cfg->log_level,
        cfg->use_tls, cfg->discover_enable, cfg->discover_port);
    return buf;
}

/* 修改单个配置项，返回 0 成功 */
int ctc_config_set(ctc_config *cfg, const char *key, const char *value) {
    if (!strcmp(key, "server_host")) strncpy(cfg->server_host, value, sizeof(cfg->server_host) - 1);
    else if (!strcmp(key, "server_port")) cfg->server_port = atoi(value);
    else if (!strcmp(key, "name")) strncpy(cfg->name, value, sizeof(cfg->name) - 1);
    else if (!strcmp(key, "metrics_interval")) { int v = atoi(value); if (v >= 5) cfg->metrics_interval = v; }
    else if (!strcmp(key, "heartbeat_interval")) { int v = atoi(value); if (v >= 3) cfg->heartbeat_interval = v; }
    else if (!strcmp(key, "log_level")) cfg->log_level = atoi(value);
    else if (!strcmp(key, "discover_enable")) cfg->discover_enable = atoi(value) ? 1 : 0;
    else if (!strcmp(key, "discover_port")) { int v = atoi(value); if (v > 0 && v < 65536) cfg->discover_port = v; }
    else if (!strcmp(key, "server_fingerprint")) strncpy(cfg->server_fingerprint, value, sizeof(cfg->server_fingerprint) - 1);
    else return -1; /* agent_id / token / use_tls 不允许远程修改 */
    return 0;
}
