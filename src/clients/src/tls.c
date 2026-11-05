/* TLS 封装层：用 OpenSSL 把普通 TCP fd 升级为 TLS。
 * 证书校验策略：
 *  - skip_verify=1：不校验证书（仅加密，防窃听不防 MITM）
 *  - fingerprint_hex 非空：校验服务端证书 SHA-256 指纹（pinning，推荐）
 *  - 两者都未设置：加密但不校验（等同 skip_verify）
 */
#include "ctforge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/sha.h>

static SSL_CTX *g_ctx = NULL;
/* fd → SSL* 映射（客户端连接数少，线性查够用） */
#define MAX_TLS_MAP 16
static struct { int fd; SSL *ssl; } g_map[MAX_TLS_MAP];
static pthread_mutex_t g_map_mu = PTHREAD_MUTEX_INITIALIZER;

static SSL *ssl_of(int fd) {
    SSL *r = NULL;
    pthread_mutex_lock(&g_map_mu);
    for (int i = 0; i < MAX_TLS_MAP; i++)
        if (g_map[i].ssl && g_map[i].fd == fd) { r = g_map[i].ssl; break; }
    pthread_mutex_unlock(&g_map_mu);
    return r;
}

static void map_set(int fd, SSL *ssl) {
    pthread_mutex_lock(&g_map_mu);
    for (int i = 0; i < MAX_TLS_MAP; i++)
        if (!g_map[i].ssl) { g_map[i].fd = fd; g_map[i].ssl = ssl; break; }
    pthread_mutex_unlock(&g_map_mu);
}

static void map_del(int fd) {
    pthread_mutex_lock(&g_map_mu);
    for (int i = 0; i < MAX_TLS_MAP; i++)
        if (g_map[i].ssl && g_map[i].fd == fd) { g_map[i].ssl = NULL; break; }
    pthread_mutex_unlock(&g_map_mu);
}

/* 供 net.c 调用：若 fd 已 TLS 化，走 SSL_read/SSL_write，否则走 read/write */
int ctc_tls_read(int fd, void *buf, int n) {
    SSL *s = ssl_of(fd);
    if (!s) return (int)read(fd, buf, (size_t)n);
    return SSL_read(s, buf, n);
}
int ctc_tls_write(int fd, const void *buf, int n) {
    SSL *s = ssl_of(fd);
    if (!s) return (int)write(fd, buf, (size_t)n);
    return SSL_write(s, buf, n);
}
int ctc_tls_active(int fd) { return ssl_of(fd) != NULL; }

static int hex2bin(const char *hex, unsigned char *out, size_t outlen) {
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > outlen) return -1;
    for (size_t i = 0; i < n; i += 2) {
        unsigned int v;
        if (sscanf(hex + i, "%2x", &v) != 1) return -1;
        out[i / 2] = (unsigned char)v;
    }
    return (int)(n / 2);
}

static int check_fingerprint(SSL *ssl, const char *expected_hex) {
    X509 *cert = SSL_get_peer_certificate(ssl);
    if (!cert) return -1;
    unsigned char der[8192];
    unsigned char *p = der;
    int derlen = i2d_X509(cert, &p);
    X509_free(cert);
    if (derlen <= 0 || derlen > (int)sizeof(der)) return -1;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(der, (size_t)derlen, digest);
    unsigned char want[SHA256_DIGEST_LENGTH];
    int wantlen = hex2bin(expected_hex, want, sizeof(want));
    if (wantlen != SHA256_DIGEST_LENGTH) return -1;
    return memcmp(digest, want, SHA256_DIGEST_LENGTH) == 0 ? 0 : -1;
}

int ctc_tls_wrap(int fd, const char *server_hostname, const char *fingerprint_hex, int skip_verify) {
    if (!g_ctx) {
        SSL_library_init();
        SSL_load_error_strings();
        g_ctx = SSL_CTX_new(TLS_client_method());
        if (!g_ctx) return -1;
        SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION);
    }
    SSL *ssl = SSL_new(g_ctx);
    if (!ssl) return -1;
    SSL_set_fd(ssl, fd);
    if (server_hostname && *server_hostname)
        SSL_set_tlsext_host_name(ssl, server_hostname); /* SNI */

    if (skip_verify) {
        SSL_CTX_set_verify(g_ctx, SSL_VERIFY_NONE, NULL);
    } else {
        /* 自签证书不走 CA 链校验；身份由 fingerprint pinning 保证 */
        SSL_CTX_set_verify(g_ctx, SSL_VERIFY_NONE, NULL);
    }

    if (SSL_connect(ssl) != 1) {
        unsigned long e = ERR_get_error();
        fprintf(stderr, "[ctc-agent] TLS handshake failed: %s\n", ERR_error_string(e, NULL));
        SSL_free(ssl);
        return -1;
    }
    fprintf(stderr, "[ctc-agent] TLS established: %s / %s\n",
            SSL_get_version(ssl), SSL_get_cipher(ssl));

    if (fingerprint_hex && *fingerprint_hex) {
        if (check_fingerprint(ssl, fingerprint_hex) != 0) {
            fprintf(stderr, "[ctc-agent] TLS certificate fingerprint mismatch! refusing connection.\n");
            SSL_shutdown(ssl);
            SSL_free(ssl);
            return -1;
        }
        fprintf(stderr, "[ctc-agent] TLS fingerprint pinned OK\n");
    } else if (!skip_verify) {
        /* 无指纹时退化为不校验证书内容，仅保证传输加密 */
        fprintf(stderr, "[ctc-agent] warning: TLS without fingerprint pinning (encryption only)\n");
    }

    map_set(fd, ssl);
    return 0;
}

void ctc_tls_cleanup(int fd) {
    SSL *s = ssl_of(fd);
    if (s) {
        SSL_shutdown(s);
        SSL_free(s);
        map_del(fd);
    }
}
