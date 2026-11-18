/*
 * ttl.c - time-limited rules with automatic revert
 *
 * A TTL rule is a module dispatch command ("apply") paired with a
 * revert command. The apply command is executed immediately; when
 * the deadline expires the revert command is replayed through the
 * normal module dispatch path. This is the safety net for rule
 * automation: a rule pushed by a detection engine can never stay
 * active forever, even if the engine or the operator disappears.
 */

#include "ctforge.h"

#define TTL_MAX_ENTRIES 128
#define TTL_POLL_US 500000 /* 500ms */

struct ttl_entry {
	int id;
	char apply_cmd[TTL_CMD_MAX];
	char revert_cmd[TTL_CMD_MAX];
	time_t deadline;
};

static struct ttl_entry ttl_entries[TTL_MAX_ENTRIES];
static int ttl_entry_count;
static int ttl_next_id = 1;
static pthread_mutex_t ttl_lock = PTHREAD_MUTEX_INITIALIZER;
static int ttl_running;
static pthread_t ttl_thread_id;

/* read "code" (int) and "msg" (string) from a module JSON reply */
static int ttl_json_code(json_object *res, const char **msg_out)
{
	json_object *obj = NULL;
	int code = -1;

	*msg_out = "";
	if (json_object_object_get_ex(res, "code", &obj) &&
	    json_object_is_type(obj, json_type_int))
		code = json_object_get_int(obj);
	if (json_object_object_get_ex(res, "msg", &obj) &&
	    json_object_is_type(obj, json_type_string))
		*msg_out = json_object_get_string(obj);
	return code;
}

static json_object *ttl_err(const char *msg)
{
	json_object *res = json_object_new_object();

	if (!res)
		return NULL;
	json_object_object_add(res, "code", json_object_new_int(-1));
	json_object_object_add(res, "msg", json_object_new_string(msg));
	return res;
}

/* extract the first word (module name) of a dispatch command */
static void ttl_first_word(char *dst, size_t dst_sz, const char *cmd)
{
	const char *space = strchr(cmd, ' ');
	size_t len = space ? (size_t)(space - cmd) : strlen(cmd);

	if (len >= dst_sz)
		len = dst_sz - 1;
	memcpy(dst, cmd, len);
	dst[len] = '\0';
}

static void ttl_expire_one(struct ttl_entry *e)
{
	json_object *res = NULL;
	const char *msg = "";
	int code;

	if (payload_module_dispatch(e->revert_cmd, &res) != 0 || !res) {
		pr_err("ttl: id=%d revert dispatch failed: '%s'", e->id,
		       e->revert_cmd);
		return;
	}

	code = ttl_json_code(res, &msg);
	json_object_put(res);

	if (code != 0) {
		pr_err("ttl: id=%d revert failed (code=%d msg=%s): '%s'",
		       e->id, code, msg, e->revert_cmd);
		return;
	}

	pr_info("ttl: id=%d expired, reverted rule '%s'", e->id,
		e->apply_cmd);
}

static void *ttl_thread(void *arg)
{
	struct ttl_entry expired[TTL_MAX_ENTRIES];

	(void)arg;

	pr_info("ttl: thread started");

	while (ttl_running) {
		time_t now;
		int n = 0;
		int i;

		pthread_mutex_lock(&ttl_lock);
		now = time(NULL);
		for (i = 0; i < ttl_entry_count; ) {
			if (ttl_entries[i].deadline <= now) {
				expired[n++] = ttl_entries[i];
				memmove(&ttl_entries[i],
					&ttl_entries[i + 1],
					(ttl_entry_count - i - 1) *
						sizeof(ttl_entries[0]));
				ttl_entry_count--;
			} else {
				i++;
			}
		}
		pthread_mutex_unlock(&ttl_lock);

		for (i = 0; i < n; i++)
			ttl_expire_one(&expired[i]);

		usleep(TTL_POLL_US);
	}

	pr_info("ttl: thread stopped");
	return NULL;
}

int ttl_start(void)
{
	if (ttl_running)
		return 0;

	ttl_running = 1;

	if (pthread_create(&ttl_thread_id, NULL, ttl_thread, NULL) != 0) {
		ttl_running = 0;
		pr_err("ttl: failed to start thread");
		return -1;
	}

	pr_info("ttl: started");
	return 0;
}

void ttl_stop(void)
{
	if (!ttl_running)
		return;

	ttl_running = 0;
	pthread_join(ttl_thread_id, NULL);
	ttl_entry_count = 0;

	pr_info("ttl: stopped");
}

int ttl_apply(const char *apply_cmd, const char *revert_cmd, int seconds,
	      json_object **result)
{
	json_object *res = NULL;
	json_object *res_obj = NULL;
	const char *msg = "";
	time_t deadline;
	int id = -1;
	int i;

	*result = NULL;

	if (!apply_cmd || !revert_cmd || seconds <= 0 ||
	    strlen(apply_cmd) >= TTL_CMD_MAX ||
	    strlen(revert_cmd) >= TTL_CMD_MAX) {
		*result = ttl_err("invalid ttl rule parameters");
		return -1;
	}

	/* fail closed: reject before applying when the table is full
	 * and no identical rule exists to refresh
	 */
	pthread_mutex_lock(&ttl_lock);
	for (i = 0; i < ttl_entry_count; i++) {
		if (strcmp(ttl_entries[i].apply_cmd, apply_cmd) == 0)
			break;
	}
	if (i == ttl_entry_count && ttl_entry_count >= TTL_MAX_ENTRIES) {
		pthread_mutex_unlock(&ttl_lock);
		*result = ttl_err("too many ttl rules");
		return -1;
	}
	pthread_mutex_unlock(&ttl_lock);

	/* apply the rule through the normal dispatch path */
	if (payload_module_dispatch(apply_cmd, &res) != 0 || !res) {
		*result = ttl_err("apply command dispatch failed");
		return -1;
	}

	if (ttl_json_code(res, &msg) != 0) {
		/* surface the module's own error reply unchanged */
		*result = res;
		return -1;
	}

	pthread_mutex_lock(&ttl_lock);

	/* an identical apply command refreshes the entry in place;
	 * the pre-check above guarantees i is a valid slot
	 */
	for (i = 0; i < ttl_entry_count; i++) {
		if (strcmp(ttl_entries[i].apply_cmd, apply_cmd) == 0) {
			id = ttl_entries[i].id;
			break;
		}
	}

	if (id < 0) {
		if (ttl_entry_count >= TTL_MAX_ENTRIES) {
			pthread_mutex_unlock(&ttl_lock);
			json_object_put(res);
			*result = ttl_err("too many ttl rules");
			return -1;
		}
		id = ttl_next_id++;
	}

	deadline = time(NULL) + seconds;
	ttl_entries[i].id = id;
	ttl_entries[i].deadline = deadline;
	snprintf(ttl_entries[i].apply_cmd, TTL_CMD_MAX, "%s", apply_cmd);
	snprintf(ttl_entries[i].revert_cmd, TTL_CMD_MAX, "%s", revert_cmd);
	if (i == ttl_entry_count)
		ttl_entry_count++;

	pthread_mutex_unlock(&ttl_lock);

	res_obj = json_object_new_object();
	if (!res_obj) {
		json_object_put(res);
		*result = ttl_err("out of memory");
		return -1;
	}
	json_object_object_add(res_obj, "code", json_object_new_int(0));
	json_object_object_add(res_obj, "id", json_object_new_int(id));
	json_object_object_add(res_obj, "seconds",
			       json_object_new_int(seconds));
	json_object_object_add(res_obj, "apply_result", res);
	json_object_object_add(res_obj, "msg",
			       json_object_new_string(
				       "rule applied, will be reverted on expiry"));
	*result = res_obj;
	return 0;
}

int ttl_cancel(int id, json_object **result)
{
	char msg[128];
	int i;

	*result = NULL;

	pthread_mutex_lock(&ttl_lock);
	for (i = 0; i < ttl_entry_count; i++) {
		if (ttl_entries[i].id == id) {
			memmove(&ttl_entries[i], &ttl_entries[i + 1],
				(ttl_entry_count - i - 1) *
					sizeof(ttl_entries[0]));
			ttl_entry_count--;
			pthread_mutex_unlock(&ttl_lock);

			/* the timer is gone, the rule itself stays */
			snprintf(msg, sizeof(msg),
				 "ttl %d canceled, rule stays active", id);
			*result = json_object_new_object();
			if (*result) {
				json_object_object_add(
					*result, "code",
					json_object_new_int(0));
				json_object_object_add(
					*result, "msg",
					json_object_new_string(msg));
				return 0;
			}
			return -1;
		}
	}
	pthread_mutex_unlock(&ttl_lock);

	snprintf(msg, sizeof(msg), "ttl %d not found", id);
	*result = ttl_err(msg);
	return -1;
}

int ttl_list(json_object **result)
{
	json_object *res = json_object_new_object();
	json_object *arr = json_object_new_array();
	time_t now = time(NULL);
	int count;
	int i;

	*result = NULL;

	if (!res || !arr) {
		json_object_put(arr);
		json_object_put(res);
		*result = ttl_err("out of memory");
		return -1;
	}

	pthread_mutex_lock(&ttl_lock);
	count = ttl_entry_count;
	for (i = 0; i < ttl_entry_count; i++) {
		json_object *ent = json_object_new_object();
		char module[NAME_MAX];
		long long left;

		if (!ent)
			break;
		ttl_first_word(module, sizeof(module),
			       ttl_entries[i].apply_cmd);
		left = (long long)difftime(ttl_entries[i].deadline, now);
		if (left < 0)
			left = 0;
		json_object_object_add(ent, "id",
				       json_object_new_int(ttl_entries[i].id));
		json_object_object_add(ent, "module",
				       json_object_new_string(module));
		json_object_object_add(ent, "seconds_left",
				       json_object_new_int64(left));
		json_object_object_add(ent, "apply",
				       json_object_new_string(
					       ttl_entries[i].apply_cmd));
		json_object_object_add(ent, "revert",
				       json_object_new_string(
					       ttl_entries[i].revert_cmd));
		json_object_array_add(arr, ent);
	}
	pthread_mutex_unlock(&ttl_lock);

	json_object_object_add(res, "code", json_object_new_int(0));
	json_object_object_add(res, "rules", arr);
	json_object_object_add(res, "rules_num", json_object_new_int(count));
	*result = res;
	return 0;
}
