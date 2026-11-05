#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE
#include "ctforge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pty.h>

/* 每个终端会话一个结构体 + 两个线程 */
typedef struct {
    int sock;               /* 与服务端的连接（只写 output） */
    char session_id[64];
    int pty_master;
    pid_t child;
    volatile int running;
    pthread_t reader_tid;
    pthread_mutex_t write_mu; /* 保护 sock 写 */
} term_session;

#define MAX_SESSIONS 8
static term_session g_terms[MAX_SESSIONS];
static pthread_mutex_t g_terms_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_terms_once = PTHREAD_ONCE_INIT;

static void terms_init(void) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        g_terms[i].pty_master = -1;
        g_terms[i].session_id[0] = 0;
    }
}

/* 从 pty 读输出，转发给服务端 */
static void *term_reader(void *arg) {
    term_session *t = arg;
    char buf[4096];
    while (t->running) {
        ssize_t r = read(t->pty_master, buf, sizeof(buf));
        if (r <= 0) break;
        char *b64 = ctc_base64_encode((unsigned char *)buf, (size_t)r);
        if (!b64) continue;
        size_t cap = strlen(b64) + 64;
        char *payload = malloc(cap);
        snprintf(payload, cap, "{\"data_base64\":\"%s\"}", b64);
        free(b64);
        pthread_mutex_lock(&t->write_mu);
        int rc = ctc_send(t->sock, MSG_TERMINAL_OUTPUT, NULL, t->session_id, payload);
        pthread_mutex_unlock(&t->write_mu);
        free(payload);
        if (rc < 0) break;
    }
    /* 通知关闭 */
    pthread_mutex_lock(&t->write_mu);
    ctc_send(t->sock, MSG_TERMINAL_OUTPUT, NULL, t->session_id,
             "{\"data_base64\":\"\",\"closed\":true}");
    pthread_mutex_unlock(&t->write_mu);
    t->running = 0;
    return NULL;
}

static void term_close(term_session *t) {
    if (!t->running && t->pty_master < 0) { t->session_id[0] = 0; return; }
    t->running = 0;
    if (t->child > 0) { kill(t->child, SIGHUP); waitpid(t->child, NULL, WNOHANG); }
    if (t->pty_master >= 0) { close(t->pty_master); t->pty_master = -1; }
    t->session_id[0] = 0;
}

int ctc_term_open(int sock, const char *session_id, int cols, int rows) {
    pthread_once(&g_terms_once, terms_init);
    pthread_mutex_lock(&g_terms_mu);
    term_session *slot = NULL;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_terms[i].session_id[0] == 0) { slot = &g_terms[i]; break; }
    }
    if (!slot) { pthread_mutex_unlock(&g_terms_mu); return -1; }
    memset(slot, 0, sizeof(*slot));
    slot->pty_master = -1;
    pthread_mutex_init(&slot->write_mu, NULL);
    strncpy(slot->session_id, session_id, sizeof(slot->session_id) - 1);
    slot->sock = sock;
    slot->running = 1;
    pthread_mutex_unlock(&g_terms_mu);

    struct winsize ws = {0};
    ws.ws_col = cols > 0 ? (unsigned short)cols : 80;
    ws.ws_row = rows > 0 ? (unsigned short)rows : 24;

    int master = -1;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) { term_close(slot); return -1; }
    if (pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        const char *shell = getenv("SHELL");
        if (!shell || !*shell) shell = "/bin/bash";
        execlp(shell, shell, "-i", NULL);
        execlp("/bin/sh", "sh", NULL);
        _exit(127);
    }
    slot->pty_master = master;
    slot->child = pid;
    /* 注意：不要设 O_NONBLOCK，reader 是独立线程，阻塞读即可。 */

    if (pthread_create(&slot->reader_tid, NULL, term_reader, slot) != 0) {
        term_close(slot);
        return -1;
    }
    pthread_detach(slot->reader_tid);
    return 0;
}

int ctc_term_input(const char *session_id, const unsigned char *data, size_t len) {
    pthread_once(&g_terms_once, terms_init);
    pthread_mutex_lock(&g_terms_mu);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_terms[i].session_id[0] && g_terms[i].running && !strcmp(g_terms[i].session_id, session_id)) {
            int fd = g_terms[i].pty_master;
            pthread_mutex_unlock(&g_terms_mu);
            size_t off = 0;
            while (off < len) {
                ssize_t w = write(fd, data + off, len - off);
                if (w < 0) {
                    if (errno == EINTR || errno == EAGAIN) { usleep(1000); continue; }
                    return -1;
                }
                off += (size_t)w;
            }
            return 0;
        }
    }
    pthread_mutex_unlock(&g_terms_mu);
    return -1;
}

int ctc_term_close(const char *session_id) {
    pthread_once(&g_terms_once, terms_init);
    pthread_mutex_lock(&g_terms_mu);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_terms[i].session_id[0] && g_terms[i].running && !strcmp(g_terms[i].session_id, session_id)) {
            pthread_mutex_unlock(&g_terms_mu);
            term_close(&g_terms[i]);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_terms_mu);
    return -1;
}

void ctc_term_close_all(void) {
    pthread_once(&g_terms_once, terms_init);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_terms[i].session_id[0])
            term_close(&g_terms[i]);
    }
}
