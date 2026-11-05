#include "ctforge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>

/* TLS 层接口（tls.c 实现；未 TLS 化时回退到原始 read/write） */
extern int ctc_tls_read(int fd, void *buf, int n);
extern int ctc_tls_write(int fd, const void *buf, int n);

/* 帧格式: 4字节大端长度 + JSON */
static int write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        int w = ctc_tls_write(fd, p, (int)n);
        if (w <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n > 0) {
        int r = ctc_tls_read(fd, p, (int)n);
        if (r <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += r; n -= (size_t)r;
    }
    return 0;
}

/* JSON 字符串转义输出到动态缓冲 */
static char *json_escape(const char *s) {
    if (!s) return strdup("");
    size_t cap = strlen(s) * 2 + 1, n = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    for (const char *p = s; *p; p++) {
        const char *rep = NULL;
        char tmp[8];
        switch (*p) {
            case '"': rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\n': rep = "\\n"; break;
            case '\r': rep = "\\r"; break;
            case '\t': rep = "\\t"; break;
            default:
                if ((unsigned char)*p < 0x20) { snprintf(tmp, sizeof(tmp), "\\u%04x", *p); rep = tmp; }
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (n + rl + 1 > cap) { cap *= 2; out = realloc(out, cap); }
            memcpy(out + n, rep, rl); n += rl;
        } else {
            if (n + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            out[n++] = *p;
        }
    }
    out[n] = 0;
    return out;
}

static int send_frame(int fd, const char *json) {
    uint32_t len = (uint32_t)strlen(json);
    if (len == 0 || len > CTC_MAX_MSG) return -1;
    uint32_t be = htonl(len);
    if (write_all(fd, &be, 4) < 0) return -1;
    return write_all(fd, json, len);
}

int ctc_send(int fd, const char *type, const char *id, const char *session_id, const char *payload_json) {
    char *etype = json_escape(type);
    size_t cap = 256 + (id ? strlen(id) : 0) + (session_id ? strlen(session_id) : 0)
               + (payload_json ? strlen(payload_json) : 0);
    char *buf = malloc(cap);
    if (!etype || !buf) { free(etype); free(buf); return -1; }
    size_t n = snprintf(buf, cap, "{\"type\":\"%s\",\"version\":%d", etype, CTC_PROTO_VERSION);
    if (id && *id) n += snprintf(buf + n, cap - n, ",\"id\":\"%s\"", id);
    if (session_id && *session_id) n += snprintf(buf + n, cap - n, ",\"session_id\":\"%s\"", session_id);
    if (payload_json) n += snprintf(buf + n, cap - n, ",\"payload\":%s", payload_json);
    snprintf(buf + n, cap - n, "}");
    int rc = send_frame(fd, buf);
    free(etype); free(buf);
    return rc;
}

int ctc_send_simple(int fd, const char *type, const char *payload_json) {
    return ctc_send(fd, type, NULL, NULL, payload_json);
}

/* 提取顶层字段原文（用于 id/type/session_id），payload 保留原始 JSON 子串 */
static char *dup_field_raw(const char *json, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        const char *start = p;
        while (*p && !(*p == '"' && p[-1] != '\\')) p++;
        return strndup(start, (size_t)(p - start));
    }
    /* 非字符串：取到 , 或 } */
    const char *start = p;
    while (*p && *p != ',' && *p != '}') p++;
    return strndup(start, (size_t)(p - start));
}

static char *dup_payload_raw(const char *json) {
    const char *p = strstr(json, "\"payload\"");
    if (!p) return NULL;
    p += strlen("\"payload\"");
    while (*p == ' ' || *p == ':') p++;
    if (*p != '{' && *p != '[') return NULL;
    int depth = 0; const char *start = p;
    int in_str = 0, esc = 0;
    for (; *p; p++) {
        if (esc) { esc = 0; continue; }
        if (*p == '\\') { esc = 1; continue; }
        if (*p == '"') { in_str = !in_str; continue; }
        if (in_str) continue;
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') {
            depth--;
            if (depth == 0) return strndup(start, (size_t)(p - start + 1));
        }
    }
    return NULL;
}

int ctc_recv(int fd, ctc_message *out) {
    uint32_t be;
    if (read_all(fd, &be, 4) < 0) return -1;
    uint32_t len = ntohl(be);
    if (len == 0 || len > CTC_MAX_MSG) return -1;
    char *buf = malloc(len + 1);
    if (!buf) return -1;
    if (read_all(fd, buf, len) < 0) { free(buf); return -1; }
    buf[len] = 0;

    out->id = dup_field_raw(buf, "id");
    out->type = dup_field_raw(buf, "type");
    out->session_id = dup_field_raw(buf, "session_id");
    out->payload = dup_payload_raw(buf);
    free(buf);
    if (!out->type) { ctc_message_free(out); return -1; }
    return 0;
}

void ctc_message_free(ctc_message *m) {
    if (!m) return;
    free(m->id); free(m->type); free(m->session_id); free(m->payload);
    m->id = m->type = m->session_id = m->payload = NULL;
}
