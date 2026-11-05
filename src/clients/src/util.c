#include "ctforge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---------- base64 ---------- */
static const char b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *ctc_base64_encode(const unsigned char *data, size_t len) {
    size_t olen = 4 * ((len + 2) / 3);
    char *out = malloc(olen + 1);
    if (!out) return NULL;
    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t a = i < len ? data[i++] : 0;
        uint32_t b = i < len ? data[i++] : 0;
        uint32_t c = i < len ? data[i++] : 0;
        uint32_t v = (a << 16) | (b << 8) | c;
        out[j++] = b64tab[(v >> 18) & 0x3F];
        out[j++] = b64tab[(v >> 12) & 0x3F];
        out[j++] = (i - 1 > len) ? '=' : b64tab[(v >> 6) & 0x3F];
        out[j++] = (i > len + 1) ? '=' : b64tab[v & 0x3F];
    }
    /* 修正 padding */
    size_t rem = len % 3;
    if (rem == 1) { out[olen - 1] = '='; out[olen - 2] = '='; }
    else if (rem == 2) { out[olen - 1] = '='; }
    out[olen] = 0;
    return out;
}

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

unsigned char *ctc_base64_decode(const char *b64, size_t *outlen) {
    size_t len = strlen(b64);
    if (len % 4) return NULL;
    size_t olen = len / 4 * 3;
    if (len && b64[len - 1] == '=') olen--;
    if (len > 1 && b64[len - 2] == '=') olen--;
    unsigned char *out = malloc(olen + 1);
    if (!out) return NULL;
    size_t i = 0, j = 0;
    while (i < len) {
        int a = b64val(b64[i++]);
        int b = b64val(b64[i++]);
        int c = b64[i] == '=' ? 0 : b64val(b64[i]); i++;
        int d = b64[i] == '=' ? 0 : b64val(b64[i]); i++;
        if (a < 0 || b < 0 || c < 0 || d < 0) { free(out); return NULL; }
        uint32_t v = (a << 18) | (b << 12) | (c << 6) | d;
        if (j < olen) out[j++] = (v >> 16) & 0xFF;
        if (j < olen) out[j++] = (v >> 8) & 0xFF;
        if (j < olen) out[j++] = v & 0xFF;
    }
    out[olen] = 0;
    *outlen = olen;
    return out;
}

/* ---------- 极简 json ---------- */
/* 仅支持扁平对象的 string/number 取值，满足协议应答解析需要 */
static const char *find_key(const char *json, const char *key) {
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strchr(p, '"')) != NULL) {
        p++; /* p 指向字符串内容起点 */
        /* 完整匹配 key + 闭引号 + 冒号 */
        if (strncmp(p, key, klen) == 0 && p[klen] == '"') {
            const char *q = p + klen + 1;
            while (*q && isspace((unsigned char)*q)) q++;
            if (*q == ':') return q + 1;
        }
        /* 跳过整个字符串（处理转义），从字符串之后继续找 */
        while (*p) {
            if (*p == '\\' && p[1]) { p += 2; continue; }
            if (*p == '"') { p++; break; }
            p++;
        }
    }
    return NULL;
}

const char *ctc_json_get_string(const char *json, const char *key, char *buf, size_t buflen) {
    const char *v = find_key(json, key);
    if (!v) return NULL;
    while (*v && isspace((unsigned char)*v)) v++;
    if (*v != '"') return NULL;
    v++;
    size_t n = 0;
    while (*v && *v != '"' && n + 1 < buflen) {
        if (*v == '\\' && v[1]) {
            v++;
            char c = *v;
            switch (c) {
                case 'n': buf[n++] = '\n'; break;
                case 't': buf[n++] = '\t'; break;
                case 'r': buf[n++] = '\r'; break;
                case '"': buf[n++] = '"'; break;
                case '\\': buf[n++] = '\\'; break;
                case '/': buf[n++] = '/'; break;
                case 'u': {
                    /* 简化处理：\u00XX 取低字节 */
                    unsigned int code = 0;
                    if (sscanf(v + 1, "%4x", &code) == 1) { buf[n++] = (char)(code & 0xFF); v += 4; }
                    else buf[n++] = '?';
                    break;
                }
                default: buf[n++] = c;
            }
            v++;
        } else {
            buf[n++] = *v++;
        }
    }
    buf[n] = 0;
    return buf;
}

long ctc_json_get_int(const char *json, const char *key, long defval) {
    const char *v = find_key(json, key);
    if (!v) return defval;
    while (*v && isspace((unsigned char)*v)) v++;
    return strtol(v, NULL, 10);
}
/* 取布尔值：支持 true/false 与 1/0 */
int ctc_json_get_bool(const char *json, const char *key, int defval) {
    const char *v = find_key(json, key);
    if (!v) return defval;
    while (*v && isspace((unsigned char)*v)) v++;
    if (!strncmp(v, "true", 4)) return 1;
    if (!strncmp(v, "false", 5)) return 0;
    if (*v == '1') return 1;
    if (*v == '0') return 0;
    return defval;
}
