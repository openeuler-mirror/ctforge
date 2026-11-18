/*
 * ctforge_event.h - shared structured event definition (v1)
 *
 * This header is included from three sides:
 *
 *  - payload eBPF programs:  #include "../../ctforged/ctforge_event.h"
 *    before vmlinux.h (plain C99 types only, no kernel headers)
 *  - payload _user.c loaders: same relative include, gives the
 *    exact record size the daemon expects
 *  - ctforged collector.c:   parses ringbuf records of this size
 *
 * Keep it C99 and self-contained. Do not include anything here:
 * vmlinux.h defines almost every known identifier and this header
 * must be includable before it without clashes.
 */

#ifndef _CTFORGE_EVENT_H
#define _CTFORGE_EVENT_H

/* Bump on breaking layout change. Modules and daemon come from the
 * same signed payload tree, so mixed versions only happen during a
 * rolling upgrade window; the collector keeps accepting the previous
 * layout (legacy string records) as well.
 */
#define CTFORGE_EVENT_VERSION 1

/* Event types. The allow/deny decision lives in the flags field so
 * a type stays a pure "what hook fired" tag.
 */
enum ctforge_event_type {
	CTFORGE_EV_UNKNOWN = 0,
	CTFORGE_EV_FILE_OPEN,	/* banopen-family */
	CTFORGE_EV_FILE_DELETE,	/* antirm-family */
	CTFORGE_EV_FILE_WRITE,	/* banwrite-family */
	CTFORGE_EV_EXEC,	/* antiexec/traceexec */
	CTFORGE_EV_NET_CONNECT,	/* bannetconnect */
	CTFORGE_EV_NET_LISTEN,	/* bannetlisten */
	CTFORGE_EV_NET_XDP,	/* firewalld/antidnstun */
	CTFORGE_EV_PRIV,	/* setuid/capset transitions */
	CTFORGE_EV_PROCESS,	/* fork/exit lifecycle */
	CTFORGE_EV_KO_LOAD,	/* kernel module load */
	CTFORGE_EV_QUOTA,	/* forkbomb/limitmem */
	CTFORGE_EV_NET_ACCEPT,	/* accepted connections (auditnet) */
	CTFORGE_EV_MAX,
};

/* Event flags */
#define CTFORGE_EV_FLAG_ALLOWED 0x1
#define CTFORGE_EV_FLAG_DENIED 0x2

/* Length of the ctx string field (NUL-terminated by the producer) */
#define CTFORGE_EVENT_CTX_LEN 200

/*
 * struct ctforge_event - fixed-layout v1 ringbuf record
 *
 * Natural alignment gives sizeof == 248 on both x86-64 (daemon,
 * loaders) and the BPF target; the static assert below pins it.
 * Legacy string records (struct msg_t) are 256 bytes, so the
 * collector can tell the two layouts apart by record size.
 *
 * ctx carries type-specific context (path, port, limit value, ...)
 * as a NUL-terminated string produced by bpf_snprintf. It is a
 * rendering of the structured fields, not their replacement: the
 * typed fields above stay authoritative for rule matching.
 */
struct ctforge_event {
	unsigned char version;	/* CTFORGE_EVENT_VERSION */
	unsigned char type;	/* enum ctforge_event_type */
	unsigned short flags;	/* CTFORGE_EV_FLAG_* */
	unsigned short _pad;	/* keep natural alignment explicit */
	unsigned int pid;
	unsigned int uid;
	unsigned long long ts_ns;	/* bpf_ktime_get_ns() */
	unsigned long long dev;	/* file device id */
	unsigned long long ino;	/* file inode number */
	unsigned long long value;	/* type-specific: port, limit */
	char ctx[CTFORGE_EVENT_CTX_LEN];
};

#define CTFORGE_EVENT_V1_SIZE 248
#define CTFORGE_EVENT_LEGACY_SIZE 256

_Static_assert(sizeof(struct ctforge_event) == CTFORGE_EVENT_V1_SIZE,
	       "ctforge_event v1 layout must stay fixed");

#endif /* _CTFORGE_EVENT_H */
