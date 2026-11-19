/*
 * collector.c - persistent event collector for payload module ring
 * buffers
 *
 * A background thread continuously drains every registered module's
 * ring buffer and writes timestamped JSON events to daily files
 * under the events directory. Modules register at insmod time (the
 * daemon extracts the logmap fd from the init result) and unregister
 * at rmmod time.
 */

#include "ctforge.h"
#include "ctforge_event.h"

#include <dirent.h>

#define COLLECTOR_MAX_MODULES 64
#define COLLECTOR_POLL_US 100000 /* 100ms */
#define EVENTS_DIR_SUB "events"
#define JSONL_BUF_MAX 1024 /* fits fully-escaped ctx + fields */

struct collector_entry {
	char name[NAME_MAX];
	int fd;
	struct ring_buffer *rb;
};

static struct collector_entry entries[COLLECTOR_MAX_MODULES];
static int entry_count;
static pthread_mutex_t collector_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t events_io_lock = PTHREAD_MUTEX_INITIALIZER;
static int collector_running;
static pthread_t collector_thread_id;
static FILE *events_fp;
static char events_date[16]; /* YYYYMMDD */

static void events_file_ensure(void)
{
	char date[16];
	char path[PATH_MAX];
	time_t now = time(NULL);
	struct tm tm_buf;

	localtime_r(&now, &tm_buf);
	strftime(date, sizeof(date), "%Y%m%d", &tm_buf);

	if (events_fp && strcmp(date, events_date) == 0)
		return; /* same day, file still valid */

	if (events_fp)
		fclose(events_fp);

	snprintf(path, sizeof(path), "%s/%s", ctforged_config.log_dir,
		 EVENTS_DIR_SUB);
	mkdir(path, 0755);

	snprintf(path, sizeof(path), "%s/%s/events-%s.jsonl",
		 ctforged_config.log_dir, EVENTS_DIR_SUB, date);

	events_fp = fopen(path, "a");
	if (!events_fp) {
		pr_err("collector: cannot open events file %s: %s", path,
		       strerror(errno));
		return;
	}
	snprintf(events_date, sizeof(events_date), "%s", date);
	pr_info("collector: events -> %s", path);
}

/* append msg to out with JSON string escaping; never writes past
 * end, always NUL-terminates. Doubled for the worst case (every
 * byte escaped as \u00XX).
 */
static void json_escape(char *out, size_t cap, const char *msg)
{
	size_t o = 0;

	for (; *msg && o + 7 < cap; msg++) {
		unsigned char c = (unsigned char)*msg;

		if (c == '"' || c == '\\') {
			out[o++] = '\\';
			out[o++] = c;
		} else if (c == '\n') {
			out[o++] = '\\';
			out[o++] = 'n';
		} else if (c == '\r') {
			out[o++] = '\\';
			out[o++] = 'r';
		} else if (c == '\t') {
			out[o++] = '\\';
			out[o++] = 't';
		} else if (c < 0x20) {
			o += snprintf(out + o, cap - o, "\\u%04x", c);
		} else {
			out[o++] = c;
		}
	}
	out[o] = '\0';
}

static void events_write_line(const char *ts, const char *module,
			       const char *payload)
{
	char esc_mod[128];

	/* events_io_lock is separate from collector_lock: this runs
	 * while collector_lock is already held by the poll loop, so
	 * taking collector_lock here would self-deadlock
	 */
	pthread_mutex_lock(&events_io_lock);
	events_file_ensure();
	if (events_fp) {
		json_escape(esc_mod, sizeof(esc_mod), module);
		/* one JSON object per line, valid JSONL */
		fprintf(events_fp,
			"{\"ts\":\"%s\",\"module\":\"%s\",%s}\n",
			ts, esc_mod, payload);
		fflush(events_fp);
	}
	pthread_mutex_unlock(&events_io_lock);
}

/* v1 structured record: typed fields + escaped ctx string */
static int collector_on_event_v1(struct collector_entry *e,
				 struct ctforge_event *ev)
{
	time_t now = time(NULL);
	struct tm tm_buf;
	char ts[32];
	char esc_ctx[JSONL_BUF_MAX];
	char line[JSONL_BUF_MAX];
	const char *verdict = "unknown";
	int n;

	if (ev->version != CTFORGE_EVENT_VERSION)
		return -1;

	if (ev->flags & CTFORGE_EV_FLAG_DENIED)
		verdict = "deny";
	else if (ev->flags & CTFORGE_EV_FLAG_ALLOWED)
		verdict = "allow";

	localtime_r(&now, &tm_buf);
	strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_buf);

	json_escape(esc_ctx, sizeof(esc_ctx), ev->ctx);

	n = snprintf(line, sizeof(line),
		     "\"ev\":{\"ver\":%u,\"type\":%u,\"action\":\"%s\","
		     "\"pid\":%u,\"uid\":%u,\"ts_ns\":%llu,\"dev\":%llu,"
		     "\"ino\":%llu,\"value\":%llu,\"ctx\":\"%s\"}",
		     ev->version, ev->type, verdict, ev->pid, ev->uid,
		     ev->ts_ns, ev->dev, ev->ino, ev->value, esc_ctx);
	if (n < 0 || (size_t)n >= sizeof(line))
		return -1;

	events_write_line(ts, e->name, line);
	return 0;
}

/* legacy record: 256-byte string, kept until every module is
 * converted to struct ctforge_event
 */
static int collector_on_event_legacy(struct collector_entry *e,
				     struct ebpf_log *msg)
{
	time_t now = time(NULL);
	struct tm tm_buf;
	char ts[32];
	char esc_ctx[JSONL_BUF_MAX];
	char line[JSONL_BUF_MAX];
	int n;

	localtime_r(&now, &tm_buf);
	strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_buf);

	json_escape(esc_ctx, sizeof(esc_ctx), msg->msg);

	n = snprintf(line, sizeof(line), "\"msg\":\"%s\"", esc_ctx);
	if (n < 0 || (size_t)n >= sizeof(line))
		return -1;

	events_write_line(ts, e->name, line);
	return 0;
}

static int collector_on_event(void *ctx, void *data, size_t data_sz)
{
	struct collector_entry *e = ctx;

	if (data_sz == sizeof(struct ctforge_event)) {
		collector_on_event_v1(e, data);
	} else if (data_sz == sizeof(struct ebpf_log)) {
		collector_on_event_legacy(e, data);
	} else {
		pr_err("collector: bad event size %zu", data_sz);
		return 0;
	}

	return 0;
}

static void *collector_thread(void *arg)
{
	int i;

	(void)arg;

	pr_info("collector: thread started");

	while (collector_running) {
		int drained = 0;

		pthread_mutex_lock(&collector_lock);
		for (i = 0; i < entry_count; i++) {
			if (entries[i].rb)
				drained += ring_buffer__consume(
					entries[i].rb);
		}
		pthread_mutex_unlock(&collector_lock);

	/* sleep between polls; wake immediately if shutting
	 * down (collector_running checked with a shorter
	 * sleep would be ideal but 100ms is fine)
	 */
		usleep(COLLECTOR_POLL_US);
	}

	pr_info("collector: thread stopped");
	return NULL;
}

int collector_start(void)
{
	if (collector_running)
		return 0;

	collector_running = 1;
	entry_count = 0;
	memset(entries, 0, sizeof(entries));

	if (pthread_create(&collector_thread_id, NULL, collector_thread,
			   NULL) != 0) {
		collector_running = 0;
		pr_err("collector: failed to start thread");
		return -1;
	}

	pr_info("collector: started");
	return 0;
}

void collector_stop(void)
{
	int i;

	if (!collector_running)
		return;

	collector_running = 0;
	pthread_join(collector_thread_id, NULL);

	pthread_mutex_lock(&collector_lock);
	for (i = 0; i < entry_count; i++) {
		if (entries[i].rb)
			ring_buffer__free(entries[i].rb);
		if (entries[i].fd >= 0)
			close(entries[i].fd);
	}
	entry_count = 0;
	pthread_mutex_unlock(&collector_lock);

	/* events_fp is shared between the event-write path (events_io_lock)
	 * and this close path, so it must be closed under events_io_lock
	 * to prevent a use-after-free when a writer is mid-fprintf.
	 */
	pthread_mutex_lock(&events_io_lock);
	if (events_fp) {
		fclose(events_fp);
		events_fp = NULL;
	}
	pthread_mutex_unlock(&events_io_lock);

	pr_info("collector: stopped");
}

int collector_register(const char *module_name, int logmap_fd)
{
	int i;

	if (!module_name || logmap_fd < 0)
		return -1;

	pthread_mutex_lock(&collector_lock);

	if (entry_count >= COLLECTOR_MAX_MODULES) {
		pr_err("collector: too many modules (max %d)",
		       COLLECTOR_MAX_MODULES);
		pthread_mutex_unlock(&collector_lock);
		return -1;
	}

	/* avoid duplicates */
	for (i = 0; i < entry_count; i++) {
		if (strcmp(entries[i].name, module_name) == 0) {
			pthread_mutex_unlock(&collector_lock);
			return 0;
		}
	}

	struct collector_entry *e = &entries[entry_count];

	snprintf(e->name, NAME_MAX, "%s", module_name);
	e->fd = logmap_fd;
	e->rb = ring_buffer__new(logmap_fd, collector_on_event, e, NULL);

	if (!e->rb) {
		pr_err("collector: ring_buffer__new failed for %s fd=%d",
		       module_name, logmap_fd);
		close(logmap_fd);
		pthread_mutex_unlock(&collector_lock);
		return -1;
	}

	entry_count++;
	pr_info("collector: registered %s (fd=%d)", module_name, logmap_fd);
	pthread_mutex_unlock(&collector_lock);
	return 0;
}

void collector_unregister(const char *module_name)
{
	int i, j;
	struct ring_buffer *rb_to_free = NULL;
	int fd_to_close = -1;

	if (!module_name)
		return;

	pthread_mutex_lock(&collector_lock);
	for (i = 0; i < entry_count; i++) {
		if (strcmp(entries[i].name, module_name) == 0) {
			rb_to_free = entries[i].rb;
			fd_to_close = entries[i].fd;
			entries[i].rb = NULL;
			entries[i].fd = -1;
			/* compact the array */
			for (j = i; j < entry_count - 1; j++)
				entries[j] = entries[j + 1];
			entry_count--;
			break;
		}
	}
	pthread_mutex_unlock(&collector_lock);

	/* ring_buffer__free() does slow syscalls (epoll_ctl, close) and
	 * may invoke callbacks; do it outside the lock so other collector
	 * threads are not blocked.
	 */
	if (rb_to_free)
		ring_buffer__free(rb_to_free);
	if (fd_to_close >= 0)
		close(fd_to_close);
	if (rb_to_free || fd_to_close >= 0)
		pr_info("collector: unregistered %s", module_name);
}

int collector_status(json_object **result)
{
	json_object *arr = json_object_new_array();
	json_object *res = json_object_new_object();
	int i;

	if (!arr || !res) {
		if (arr)
			json_object_put(arr);
		if (res)
			json_object_put(res);
		*result = NULL;
		return -1;
	}

	pthread_mutex_lock(&collector_lock);
	for (i = 0; i < entry_count; i++)
		json_object_array_add(arr, json_object_new_string(
			entries[i].name));
	pthread_mutex_unlock(&collector_lock);

	json_object_object_add(res, "code", json_object_new_int(0));
	json_object_object_add(res, "active", json_object_new_int(
		collector_running));
	json_object_object_add(res, "modules", arr);

	*result = res;
	return 0;
}
