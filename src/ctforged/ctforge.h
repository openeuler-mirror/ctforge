#ifndef _CTFORGE_H
#define _CTFORGE_H

#define _GNU_SOURCE
#define OSSL_DEPRECATED_SUPPRESS

/* Standard C library */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lzma.h>
#include <stdatomic.h>
#include <time.h>

/* POSIX / System interfaces */
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Network */
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/resource.h>
#include <sys/epoll.h>
#include <sys/mman.h>

/* Linux-specific (userspace-accessible) */
#include <linux/input.h>
#include <linux/netlink.h>
#include <linux/types.h>

/* ELF & binary manipulation */
#include <elf.h>
#include <gelf.h>
#include <libelf.h>

/* OpenSSL */
#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>

/* libuv */
#include <uv.h>

/* json-c  */
#include <json-c/json.h>

#include <bpf/libbpf.h>

#include "../key/CTFORGED/sm2_signature.h"

#define CONFIG_PATH "/etc/ctforge/ctforge.cfg"
#define PUBLICKEY_PATH "/etc/ctforge/public.pem"
#define PRIVATKEY_path "/etc/ctforge/private.pem"

#define MATCH(s, n) (strcmp(section, s) == 0 && strcmp(name, n) == 0)
#define RESP_LEN_MAX 2048
#define CMD_LEN_MAX 192

#define USER_DEFAULT -1
#define USER_EMERG 0
#define USER_ALERT 1
#define USER_CRIT 2
#define USER_ERR 3
#define USER_WARNING 4
#define USER_NOTICE 5
#define USER_INFO 6
#define USER_DEBUG 7

#define pr_emerg(fmt, ...) printu(USER_EMERG, fmt, ##__VA_ARGS__)
#define pr_alert(fmt, ...) printu(USER_ALERT, fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...) printu(USER_CRIT, fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...) printu(USER_ERR, fmt, ##__VA_ARGS__)
#define pr_error(fmt, ...) printu(USER_ERR, fmt, ##__VA_ARGS__)
#define pr_warning(fmt, ...) printu(USER_WARNING, fmt, ##__VA_ARGS__)
#define pr_warn pr_warning
#define pr_notice(fmt, ...) printu(USER_NOTICE, fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) printu(USER_INFO, fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...) printu(USER_DEBUG, fmt, ##__VA_ARGS__)
#define pr_dbg(fmt, ...) printu(USER_DEBUG, fmt, ##__VA_ARGS__)

#define CTFORGED_KLOG_FILENAME "ctforge_kernel.log"
#define CTFORGED_SLOG_FILENAME "ctforge_service.log"

#define xstr(s) str(s)
#define str(s) #s

#define CTFORGE_VERSION_MAJOR 0
#define CTFORGE_VERSION_MINOR 1
#define CTFORGE_VERSION_PATCH 0
#define CTFORGE_RELEASE "20260101"
#define CTFORGE_VERSION_STR                       \
	"v" xstr(CTFORGE_VERSION_MAJOR) "." xstr( \
		CTFORGE_VERSION_MINOR) "." xstr(CTFORGE_VERSION_PATCH)
#define CTFORGE_VERSION CTFORGE_VERSION_STR "-" CTFORGE_RELEASE

#define NETLINK_CTFORGE 29

#define CTFORGE_NL_CONNECT 0x1001
#define CTFORGE_NL_AUDIT_LOG 0x1002
#define CTFORGE_NL_AUDIT_TEST 0x1003
#define CTFORGE_NL_PING 0x1004
#define CTFORGE_NL_SET_HMAC_TYPE 0x1005
#define CTFORGE_NL_GET_HMAC_TYPE 0x1006
#define CTFORGE_NL_GET_STATUS 0x1007
#define CTFORGE_NL_GET_KFIFO_AVAIL 0x1008
#define CTFORGE_NL_GET_KFIFO_DROP 0x1009
#define CTFORGE_NL_GET_KFIFO_TOTAL 0x100A

#define NETLINK_FLAG_ASYNC (1 << 0)

#define MAX_THREADS 10
#define QUEUE_SIZE 10
#define READ_SIZE 1024
#define PORT 8080
#define BUFFER_SIZE 1024

#define CTFORGE_HASH_SIZE 512

#define CTSECFS_SM3_SIG_SIZE 128
#define SM2_DEFAULT_USERID "1234567812345678"
#define SM2_DEFAULT_USERID_LEN 16

#define MAX_CLEANUP_TASKS 64

/* ctforged cmd type */
#define CTFORGED_TYPE_CMD 0x8001

/* Linux kernel-style list macros */
#define LIST_HEAD_INIT(name)     \
	{                        \
		&(name), &(name) \
	}
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

// clang-format off
#define container_of(ptr, type, member)                            \
	({                                                         \
		const typeof(((type *)0)->member) * __mptr = (ptr); \
		(type *)((char *)__mptr - offsetof(type, member)); \
	})
// clang-format on

#define list_entry(ptr, type, member) container_of(ptr, type, member)

#define list_first_entry(ptr, type, member) \
	list_entry((ptr)->next, type, member)

#define list_last_entry(ptr, type, member) list_entry((ptr)->prev, type, member)

#define list_first_entry_or_null(ptr, type, member) \
	(!list_empty(ptr) ? list_first_entry(ptr, type, member) : NULL)

#define list_next_entry(pos, member) \
	list_entry((pos)->member.next, typeof(*(pos)), member)

#define list_prev_entry(pos, member) \
	list_entry((pos)->member.prev, typeof(*(pos)), member)

#define list_for_each(pos, head) \
	for (pos = (head)->next; pos != (head); pos = pos->next)

#define list_for_each_entry(pos, head, member)                   \
	for (pos = list_first_entry(head, typeof(*pos), member); \
	     &pos->member != (head); pos = list_next_entry(pos, member))

#define list_empty(head) list_empty_careful(head)

enum output_type { CF_BPF_LOG, CF_BPF_TERMINAL, CF_BPF_NULL };

enum format_type { CF_BPF_W3C, CF_BPF_SPADE };

struct list_head {
	struct list_head *next, *prev;
};

struct ctforge_status_t {
	__u32 enabled;
};

struct ctforge_kfifo_t {
	__u32 kfifo_avail;
	__u32 kfifo_drop;
	__u32 kfifo_total;
};

struct hash_type_t {
	__u32 hash_type;
};

struct ctforge_kern;

struct module_ops {
	json_object *(*init)(int argc, char **argv);
	json_object *(*exit)(int argc, char **argv);
};

struct module_info {
	char *module_name;
	char payload_fullpath[PATH_MAX];
	char payload_memfd_path[PATH_MAX];
	struct module_ops module_ops;
	void *dl_handle;
	struct list_head list;
	__u32 payload_fd;
	size_t payload_size;
};

struct ctforged_params {
	__u32 fd;
	__u32 flags;
	char *cmd;
	__u32 cmd_size;
	char *res;
	__u32 res_size;
	__u32 argc;
	char **argv;
	__u32 ret;
	__u16 nlmsg_type;
	char origin_cmd[CMD_LEN_MAX];
	struct module_info *modinfo;
	union {
		struct ctforge_status_t ctforge_status;
		struct ctforge_kfifo_t ctforge_kfifo;
		struct hash_type_t hash_type;
		char audit_info[128];
	};
};

struct ebpf_log {
	char msg[256];
};

struct ctforge_reply_t {
	struct ctforged_params cp;
	union {
		struct ctforge_status_t ctforge_status;
		struct ctforge_kfifo_t ctforge_kfifo;
		struct hash_type_t hash_type;
		char audit_info[128];
	};
};

struct ctforge_nl_reply {
	__u32 portid;
	struct sk_buff *skb;
};

typedef void (*task_func)(void *);

struct ctforge_task {
	task_func function;
	void *arg;
};

struct ctforge_threadpool {
	pthread_mutex_t lock;
	pthread_cond_t notify;
	pthread_t *threads;
	struct ctforge_task *tasks;
	int thread_count;
	int queue_size;
	int head;
	int tail;
	int count;
	int shutdown;
	int started;
};

struct ctforged_sign_header {
	char flag[8];
	uint32_t sign_len;
	const unsigned char sign[CTSECFS_SM3_SIG_SIZE];
};

struct ctforge_elf_sec {
	void *data;
	size_t size;
};

struct ctforge_func;

typedef int (*CTFORGE_FUNC)(struct ctforged_params *s);

struct ctforge_func {
	char *key;
	CTFORGE_FUNC function;
	struct ctforge_func *next;
	size_t call_count;
};

struct ctforge_config {
	char *listen;
	char *payload_dir;
	char *log_dir;
	char *private_key;
	char *public_key;
};

struct ctforge_conn_ctx {
	union {
		uv_tcp_t tcp;
		uv_pipe_t pipe;
	} stream_handle;

	char *recv_buffer;
	size_t recv_buffer_len;

	char *client_msg;
};

struct ctforge_write_req {
	uv_write_t req;
	char *buf;
};

typedef int (*jsonrpc_method_fn)(json_object *params, json_object **result,
				 void *user_data);

int jsonrpc_register(const char *method_name, jsonrpc_method_fn fn,
		     void *user_data);

int jsonrpc_server_run_tcp(const char *host, int port);

int jsonrpc_server_run_unix(const char *socket_path);

void jsonrpc_handle_request(uv_stream_t *stream, const char *request_json,
			    char **response_json);

struct cleanup_task {
	void (*fn)(void);
	const char *name;
};

//typedef int (*sym_func_t)(int argc, char **argv);
//typedef char *(*sym_func_char_t)(int argc, char **argv);
typedef json_object *(*so_func_t)(int argc, char **argv);

extern const char *loglevel[];
extern struct list_head module_list_head;
extern struct ctforge_threadpool *workpool;
extern struct ctforge_config ctforged_config;

extern uv_loop_t *ctforged_uv_loop;
extern uv_tcp_t ctforged_tcp_server;
extern uv_pipe_t ctforged_unix_server;

/* Configuration */
char *ctforged_config_strdup(const char *s, const char *field_name);
const char *ctforged_get_config(json_object *root, const char *field_name,
				const char *default_value);
void ctforged_clean_config(void);
int ctforged_init_config(const char *config_path, struct ctforge_config *cfg);
int register_cleanup(void (*fn)(void), const char *name);
void shutdown_cleanup(void);

/* Logging */
void printu(int level, char *fmt, ...);
void ctforged_klog_to_file(char *info);
void ctforged_slog_to_file(char *info);
int ctforged_log_init(void);

/* Module & Payload */
struct module_info *find_module(const char *name);
int add_module(struct module_info *mod);
void delete_module(struct module_info *mod);
void modify_module(struct module_info *mod, const char *name, const char *path);
int payload_init(void);

/* Netlink & Command Ops */
void netlink_loop(void);
void ctforge_nl_ctl(struct ctforged_params *s);

int ctforge_ops_insmod(struct ctforged_params *s);
int ctforge_ops_rmmod(struct ctforged_params *s);
int ctforge_ops_lsmod(struct ctforged_params *s);
int ctforge_ops_modinfo(struct ctforged_params *s);
int ctforge_ops_sign(struct ctforged_params *s);
int ctforge_ops_verify(struct ctforged_params *s);
int ctforge_ops_exit(struct ctforged_params *s);
int ctforge_ops_status(struct ctforged_params *s);
int ctforge_ops_print(struct ctforged_params *s);

/* Thread Pool */
struct ctforge_threadpool *threadpool_create(int thread_count, int queue_size);
int threadpool_add(struct ctforge_threadpool *pool, task_func function,
		   void *arg);
int threadpool_destroy(struct ctforge_threadpool *pool);
int threadpool_free(struct ctforge_threadpool *pool);
void *threadpool_thread(void *threadpool);
int threadpoll_init(void);

/* Signature (conditional) */
int sign_data_raw(unsigned char *priv_key, void *data, int datalen, char *sig,
		  size_t *siglen);
int verify_data_raw(unsigned char *pub_key, void *data, size_t datalen,
		    char *sig, size_t siglen);
unsigned char *load_file(char *filename, size_t *len);
void dump_bin2str(char *prefix, unsigned char *bin, int len);
int do_verify_ebpf_section_sign(char *key_path, char *input_file);
int do_verify_tail_sign(char *key_path, char *input_file);
int do_safty_verify_sign(char *key_path, char *input_file);
int do_safety_verify_sign_from_mem(const char *public_key_path, const void *buf,
				   size_t buf_size);
bool is_tail_signed(char *input_file);
Elf *open_elf(const char *input_filename, int *fd);
void close_elf(Elf *elf, int fd);
char **list_sections(const char *input_filename, int *section_count);
struct ctforge_elf_sec *get_section_content(const char *input_filename,
					    const char *section_name);
int section_exists(const char *input_filename, const char *section_name);
int create_section(const char *input_filename, const char *section_name,
		   const void *data, size_t size);

/* Utilities */
int update_rlimit(void);
int sk_resappend(struct ctforged_params *s, const char *format, ...);
int strnappend(char *str, size_t size, const char *format, ...);
bool is_ebpf_file(const char *filename);
bool is_elf_file(const char *filename);
void hexdump(const void *data, size_t size);
int write2file(char *filename, void *data, int datalen, size_t start);
size_t get_file_size(char *input_file);
int file_tail_truncate(char *input_file, size_t size);
bool starts_with(const char *str, const char *substr);
int list_empty_careful(const struct list_head *head);
int gen_timestamp(void);
ssize_t strscpy(char *dest, const char *src, size_t count);

/* Main Runtime */
unsigned int hash(char *str);
void hashtable_insert_func(char *key, CTFORGE_FUNC function);
struct ctforge_func *hashtable_find_func(char *key);
void do_exit_clean(void);
void sig_handler(int sig);
void beautify_json(struct json_object *obj);

/* payload */
int do_one_rmmod(char *mod_name);
int do_one_insmod(char *mod_name);
so_func_t module_find_symbol_c(struct module_info *mi, char *module_name,
			       char *sym_name);
void *mod_find_sym(struct module_info *mi, char *module_name, char *sym_name);

/* server */
int start_uv_server(struct ctforge_config *ctforged_config);
jsonrpc_method_fn find_method(const char *name, void **user_data);
void ctforged_clean_sock(void);

/* error */
const char *ct_strerror(int err);
const char *ct_strerror_r(int err, char *buf, size_t buflen);

int sm2_sign(const char *msg, size_t msg_len, const char *privkey_pem_path,
	     unsigned char **sig, size_t *sig_len);

int sm2_verify(const char *msg, size_t msg_len, const char *pubkey_pem_path,
	       const unsigned char *sig, size_t sig_len);

int do_sign_ebpf_section(char *key_path, char *input_file);
int do_sign(char *key_path, char *input_file);
int do_verify_ebpf_section_sign(char *key_path, char *input_file);
int do_verify_sign(char *key_path, char *input_file);
int do_remove_section(const char *input_filename, const char *section_name);
int do_remove_ebpf_section_sign(char *input_file);
int do_remove_sign(char *input_file);

#define CT_E2BIG -1
#define CT_EACCES -2
#define CT_EADDRINUSE -3
#define CT_EADDRNOTAVAIL -4
#define CT_EAFNOSUPPORT -5
#define CT_EAGAIN -6
#define CT_EALREADY -7
#define CT_EBADF -8
#define CT_EBUSY -9
#define CT_ECANCELED -10
#define CT_ECHARSET -11
#define CT_ECONNABORTED -12
#define CT_ECONNREFUSED -13
#define CT_ECONNRESET -14
#define CT_EDESTADDRREQ -15
#define CT_EEXIST -16
#define CT_EFAULT -17
#define CT_EFBIG -18
#define CT_EHOSTUNREACH -19
#define CT_EINTR -20
#define CT_EINVAL -21
#define CT_EIO -22
#define CT_EISCONN -23
#define CT_EISDIR -24
#define CT_ELOOP -25
#define CT_EMFILE -26
#define CT_EMSGSIZE -27
#define CT_ENAMETOOLONG -28
#define CT_ENETDOWN -29
#define CT_ENETUNREACH -30
#define CT_ENFILE -31
#define CT_ENOBUFS -32
#define CT_ENODEV -33
#define CT_ENOENT -34
#define CT_ENOMEM -35
#define CT_ENONET -36
#define CT_ENOPROTOOPT -37
#define CT_ENOSPC -38
#define CT_ENOSYS -39
#define CT_ENOTCONN -40
#define CT_ENOTDIR -41
#define CT_ENOTEMPTY -42
#define CT_ENOTSOCK -43
#define CT_ENOTSUP -44
#define CT_EPERM -45
#define CT_EPIPE -46
#define CT_EPROTO -47
#define CT_EPROTONOSUPPORT -48
#define CT_EPROTOTYPE -49
#define CT_ERANGE -50
#define CT_EROFS -51
#define CT_ESHUTDOWN -52
#define CT_ESPIPE -53
#define CT_ESRCH -54
#define CT_ETIMEDOUT -55
#define CT_ETXTBSY -56
#define CT_EXDEV -57
#define CT_UNKNOWN -58

#define CT_ERRNO_MAX 59

#endif /* _CTFORGE_H */
