#include "ctforge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

/* 分块读取：单次最多 4MB，整体不限（受 offset 驱动） */
#define CHUNK_MAX (4u * 1024u * 1024u)
#define FILE_READ_MAX (512u * 1024u * 1024u) /* 单次会话上限 512MB，超出需分块 */

/* 在 root 内做路径净化，防目录穿越 */
static int safe_path(const char *path, char *out, size_t outlen) {
    if (!path || !*path) return -1;
    char buf[1024];
    if (path[0] != '/') snprintf(buf, sizeof(buf), "/%s", path);
    else strncpy(buf, path, sizeof(buf) - 1);
    /* 不允许 ".." 段 */
    char tmp[1024];
    strncpy(tmp, buf, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0;
    for (char *seg = strtok(tmp, "/"); seg; seg = strtok(NULL, "/")) {
        if (!strcmp(seg, "..")) return -1;
    }
    strncpy(out, buf, outlen - 1); out[outlen - 1] = 0;
    return 0;
}

static const char *mode_str(mode_t m, char *buf) {
    snprintf(buf, 11, "%c%c%c%c%c%c%c%c%c%c",
        S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : '-',
        (m & S_IRUSR) ? 'r' : '-', (m & S_IWUSR) ? 'w' : '-', (m & S_IXUSR) ? 'x' : '-',
        (m & S_IRGRP) ? 'r' : '-', (m & S_IWGRP) ? 'w' : '-', (m & S_IXGRP) ? 'x' : '-',
        (m & S_IROTH) ? 'r' : '-', (m & S_IWOTH) ? 'w' : '-', (m & S_IXOTH) ? 'x' : '-');
    return buf;
}

/* file_list -> {"path":"...","files":[{...}]} */
char *ctc_file_list_json(const char *path) {
    char sp[1024];
    if (safe_path(path, sp, sizeof(sp)) < 0) return strdup("{\"error\":\"invalid path\"}");
    DIR *d = opendir(sp);
    if (!d) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", strerror(errno));
        return strdup(buf);
    }
    size_t cap = 4096, n = 0;
    char *out = malloc(cap);
    n += snprintf(out + n, cap - n, "{\"path\":\"%s\",\"files\":[", sp);
    struct dirent *e;
    int first = 1;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[1200];
        snprintf(full, sizeof(full), "%s%s%s", sp, sp[strlen(sp)-1] == '/' ? "" : "/", e->d_name);
        struct stat st;
        if (lstat(full, &st) < 0) continue;
        char mode[11]; mode_str(st.st_mode, mode);
        /* 转义文件名中的引号 */
        char esc[512]; size_t j = 0;
        for (const char *p = e->d_name; *p && j + 2 < sizeof(esc); p++) {
            if (*p == '"' || *p == '\\') esc[j++] = '\\';
            esc[j++] = *p;
        }
        esc[j] = 0;
        char item[1024];
        int ilen = snprintf(item, sizeof(item),
            "%s{\"name\":\"%s\",\"path\":\"%s\",\"is_dir\":%s,\"size\":%lld,\"mode\":\"%s\",\"mod_time\":%lld}",
            first ? "" : ",", esc, full,
            S_ISDIR(st.st_mode) ? "true" : "false",
            (long long)st.st_size, mode, (long long)st.st_mtime);
        if (n + (size_t)ilen + 8 > cap) { cap *= 2; out = realloc(out, cap); }
        memcpy(out + n, item, ilen); n += (size_t)ilen;
        first = 0;
    }
    closedir(d);
    snprintf(out + n, cap - n, "]}");
    return out;
}

/* file_read -> {"path":"...","size":N,"content_base64":"...","truncated":bool} */
char *ctc_file_read_json(const char *path) {
    char sp[1024];
    if (safe_path(path, sp, sizeof(sp)) < 0) return strdup("{\"error\":\"invalid path\"}");
    FILE *f = fopen(sp, "rb");
    if (!f) {
        char buf[160];
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", strerror(errno));
        return strdup(buf);
    }
    struct stat st;
    fstat(fileno(f), &st);
    if (S_ISDIR(st.st_mode)) { fclose(f); return strdup("{\"error\":\"is a directory\"}"); }

    /* 读取 chunk 参数（由上层传 offset/limit） */
    long long off = 0;
    long long lim = 0;
    extern long ctc_json_get_int(const char *json, const char *key, long defval);
    /* payload 里可能带 offset/limit */
    extern char *ctc_file_read_chunk(const char *path);  /* 内部复用 */

    size_t want = (size_t)st.st_size;
    if (want > CHUNK_MAX) want = CHUNK_MAX;
    unsigned char *data = malloc(want + 1);
    if (!data) { fclose(f); return strdup("{\"error\":\"oom\"}"); }
    size_t got = fread(data, 1, want, f);
    fclose(f);
    char *b64 = ctc_base64_encode(data, got);
    free(data);
    int truncated = (size_t)st.st_size > got;
    size_t cap = strlen(b64) + 320;
    char *out = malloc(cap);
    snprintf(out, cap,
        "{\"path\":\"%s\",\"size\":%lld,\"content_base64\":\"%s\",\"truncated\":%s,\"offset\":0,\"next_offset\":%zu}",
        sp, (long long)st.st_size, b64, truncated ? "true" : "false", got);
    free(b64);
    return out;
}

/* 按 offset 分块读取（供大文件下载） */
char *ctc_file_read_range_json(const char *path, long long offset, long long limit) {
    char sp[1024];
    if (safe_path(path, sp, sizeof(sp)) < 0) return strdup("{\"error\":\"invalid path\"}");
    FILE *f = fopen(sp, "rb");
    if (!f) {
        char buf[160];
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", strerror(errno));
        return strdup(buf);
    }
    struct stat st;
    fstat(fileno(f), &st);
    if (S_ISDIR(st.st_mode)) { fclose(f); return strdup("{\"error\":\"is a directory\"}"); }

    if (offset < 0) offset = 0;
    if (offset > (long long)st.st_size) offset = (long long)st.st_size;
    if (limit <= 0 || limit > (long long)CHUNK_MAX) limit = (long long)CHUNK_MAX;
    long long remain = (long long)st.st_size - offset;
    if (limit > remain) limit = remain;

    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        return strdup("{\"error\":\"seek failed\"}");
    }
    unsigned char *data = malloc((size_t)limit + 1);
    if (!data) { fclose(f); return strdup("{\"error\":\"oom\"}"); }
    size_t got = fread(data, 1, (size_t)limit, f);
    fclose(f);
    char *b64 = ctc_base64_encode(data, got);
    free(data);

    long long next = offset + (long long)got;
    int done = (next >= (long long)st.st_size);
    size_t cap = strlen(b64) + 384;
    char *out = malloc(cap);
    snprintf(out, cap,
        "{\"path\":\"%s\",\"size\":%lld,\"offset\":%lld,\"next_offset\":%lld,"
        "\"done\":%s,\"content_base64\":\"%s\"}",
        sp, (long long)st.st_size, offset, next, done ? "true" : "false", b64);
    free(b64);
    return out;
}

/* file_write（content_base64 必须） */
char *ctc_file_write_json(const char *payload) {
    char path[1024];
    size_t b64cap = 16u * 1024u * 1024u; /* 单请求最大 12MB 原文 */
    char *b64 = malloc(b64cap);
    char append_s[8] = {0};
    char *err = NULL;
    if (!b64) return strdup("{\"error\":\"oom\"}");
    if (!ctc_json_get_string(payload, "path", path, sizeof(path)))
        { err = strdup("{\"error\":\"missing path\"}"); goto out; }
    if (!ctc_json_get_string(payload, "content_base64", b64, b64cap - 4))
        { err = strdup("{\"error\":\"missing content_base64\"}"); goto out; }
    (void)append_s;
    /* 支持 JSON 布尔 true/false 与字符串 "true"/"false" */
    int append = ctc_json_get_bool(payload, "append", 0);

    char sp[1024];
    if (safe_path(path, sp, sizeof(sp)) < 0) { err = strdup("{\"error\":\"invalid path\"}"); goto out; }
    size_t dlen = 0;
    unsigned char *data = ctc_base64_decode(b64, &dlen);
    if (!data) { err = strdup("{\"error\":\"bad base64\"}"); goto out; }
    FILE *f = fopen(sp, append ? "ab" : "wb");
    if (!f) {
        free(data);
        char buf[160];
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", strerror(errno));
        err = strdup(buf);
        goto out;
    }
    fwrite(data, 1, dlen, f);
    fclose(f);
    free(data);
out:
    free(b64);
    return err ? err : strdup("{\"ok\":true,\"message\":\"written\"}");
}

char *ctc_file_simple_json(const char *action, const char *payload) {
    char a[1024] = {0}, b[1024] = {0};
    int rc = -1;
    if (!strcmp(action, "delete")) {
        if (!ctc_json_get_string(payload, "path", a, sizeof(a))) return strdup("{\"error\":\"missing path\"}");
        char sp[1024];
        if (safe_path(a, sp, sizeof(sp)) < 0) return strdup("{\"error\":\"invalid path\"}");
        rc = remove(sp);
        if (rc < 0 && errno == EISDIR) rc = rmdir(sp);
    } else if (!strcmp(action, "mkdir")) {
        if (!ctc_json_get_string(payload, "path", a, sizeof(a))) return strdup("{\"error\":\"missing path\"}");
        char sp[1024];
        if (safe_path(a, sp, sizeof(sp)) < 0) return strdup("{\"error\":\"invalid path\"}");
        rc = mkdir(sp, 0755);
    } else if (!strcmp(action, "rename")) {
        if (!ctc_json_get_string(payload, "from", a, sizeof(a)) ||
            !ctc_json_get_string(payload, "to", b, sizeof(b)))
            return strdup("{\"error\":\"missing from/to\"}");
        char sf[1024], st[1024];
        if (safe_path(a, sf, sizeof(sf)) < 0 || safe_path(b, st, sizeof(st)) < 0)
            return strdup("{\"error\":\"invalid path\"}");
        rc = rename(sf, st);
    }
    if (rc < 0) {
        char buf[160];
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", strerror(errno));
        return strdup(buf);
    }
    return strdup("{\"ok\":true}");
}
