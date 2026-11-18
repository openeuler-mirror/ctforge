#include "ctforge.h"

#define MAX_CTFORGED_METHODS 64

struct ctforge_config ctforged_config = { 0 };

uv_loop_t *ctforged_uv_loop;
uv_tcp_t ctforged_tcp_server;
uv_pipe_t ctforged_unix_server;

static struct {
	char *name;
	jsonrpc_method_fn fn;
	void *user_data;
} method_table[MAX_CTFORGED_METHODS];

static int ctforged_method_count;

LIST_HEAD(module_list_head);

int check_ctforge_cmdline(void)
{
	char cmdline[4096];
	FILE *cmdline_file = fopen("/proc/cmdline", "r");

	if (cmdline_file == NULL) {
		pr_err("Error opening /proc/cmdline");
		return 0;
	}

	if (fgets(cmdline, sizeof(cmdline), cmdline_file) == NULL) {
		pr_err("Error reading /proc/cmdline");
		fclose(cmdline_file);
		return 0;
	}
	fclose(cmdline_file);

	if (strstr(cmdline, "ctforge=1") != NULL)
		return 1;
	else
		return 0;
}

void do_exit_clean(void)
{
	shutdown_cleanup();
	pr_info("Clean and exit! Good bye!");
	exit(0);
}

void ctforged_sig_handler(int sig)
{
	pr_debug("Received signal [%d]", sig);

	if (sig == SIGTERM || sig == SIGINT)
		do_exit_clean();
}

int ctforged_init_signal(void)
{
	signal(SIGTERM, ctforged_sig_handler);
	pr_info("Register SIGTERM handled");
	signal(SIGINT, ctforged_sig_handler);
	pr_info("Register SIGINT handled");
	return 0;
}

static void on_write_done(uv_write_t *req, int status)
{
	struct ctforge_write_req *wr = (struct ctforge_write_req *)req->data;

	if (status < 0)
		fprintf(stderr, "Write error: %s\n", uv_strerror(status));

	free(wr->buf);
	free(wr);
}

jsonrpc_method_fn find_method(const char *name, void **user_data)
{
	for (int i = 0; i < ctforged_method_count; i++) {
		if (strcmp(method_table[i].name, name) == 0) {
			*user_data = method_table[i].user_data;
			return method_table[i].fn;
		}
	}
	return NULL;
}

void ctforged_clean_sock(void)
{
	const char *listen_addr;
	const char *path;
	struct stat st;

	listen_addr = ctforged_config.listen;
	if (!listen_addr)
		return;

	if (strncmp(listen_addr, "unix://", 7) != 0)
		return;

	path = listen_addr + 7;
	if (*path == '\0')
		return;

	if (stat(path, &st) != 0)
		return;

	if (!S_ISSOCK(st.st_mode))
		return;

	(void)unlink(path);
}

int ops_insmod(json_object *reqmsg, json_object **result, void *user_data)
{
	json_object *msg_obj;
	int ret = -1;
	char jmsg[128];

	if (!json_object_object_get_ex(reqmsg, "params", &msg_obj))
		return -1;

	const char *msg = json_object_get_string(msg_obj);

	if (!msg) {
		*result = json_object_new_string("empty input");
		return 0;
	}
	char *msg_copy = strdup(msg);

	if (!msg_copy)
		return -1;

	char *saveptr;
	char *token = strtok_r(msg_copy, " \t\n\r\f\v", &saveptr);

	json_object *mod_array = json_object_new_array();

	if (!mod_array) {
		free(msg_copy);
		return -1;
	}

	while (token != NULL) {
		pr_dbg("try insmod %s", token);

		json_object *data_obj = json_object_new_object();

		json_object_object_add(data_obj, "modname",
				       json_object_new_string(token));

		ret = do_one_insmod(token);

		json_object_object_add(data_obj, "ret",
				       json_object_new_int(ret));
		if (!ret) {
			snprintf(jmsg, sizeof(jmsg), "%s load success!", token);
			json_object_object_add(data_obj, "msg",
					       json_object_new_string(jmsg));
		} else {
			snprintf(jmsg, sizeof(jmsg), "%s load failed!", token);
			json_object_object_add(data_obj, "msg",
					       json_object_new_string(jmsg));
		}

		json_object_array_add(mod_array, data_obj);
		token = strtok_r(NULL, " \t\n\r\f\v", &saveptr);
	}
	*result = mod_array;
	return 0;
}

int ops_lsmod(json_object *reqmsg, json_object **result, void *user_data)
{
	json_object *msg_obj;

	if (!json_object_object_get_ex(reqmsg, "params", &msg_obj))
		return -1;

	struct module_info *iterator;
	json_object *result_array;

	result_array = json_object_new_array();
	if (!result_array) {
		*result = json_object_new_string("server alloc memory failed");
		return -1;
	}

	if (list_empty(&module_list_head)) {
		json_object_put(result_array);

		json_object *res = json_object_new_object();

		if (!res) {
			pr_err("failed to alloc memory");
			return -1;
		}
		json_object_object_add(
			res, "msg",
			json_object_new_string("no module insmod!"));

		*result = res;
		return 0;
	}

	list_for_each_entry(iterator, &module_list_head, list) {
		json_object *mod_obj = json_object_new_object();

		if (!mod_obj) {
			json_object_put(result_array);
			return -1;
		}

		json_object_object_add(
			mod_obj, "name",
			json_object_new_string(iterator->module_name));
		json_object_object_add(
			mod_obj, "path",
			json_object_new_string(iterator->payload_fullpath));

		json_object_array_add(result_array, mod_obj);
	}
	*result = result_array;
	return 0;
}

/*
 * ops_show - list every payload module available in payload_dir,
 * including whether it is currently loaded, the module name and
 * its on-disk path.
 */
int ops_show(json_object *reqmsg, json_object **result, void *user_data)
{
	DIR *dir;
	struct dirent *de;
	json_object *result_array;
	char module_name[NAME_MAX];
	const char *payload_dir = ctforged_config.payload_dir;
	const char *suffix = ".so.xz";
	size_t suffix_len = strlen(suffix);
	const char *prefix = "ctforge_";
	size_t prefix_len = strlen(prefix);

	(void)reqmsg;
	(void)user_data;

	if (!payload_dir)
		payload_dir = "/usr/share/ctforge/payload";

	dir = opendir(payload_dir);
	if (!dir) {
		json_object *err = json_object_new_object();
		json_object_object_add(err, "code", json_object_new_int(-1));
		json_object_object_add(err, "msg",
				       json_object_new_string("cannot open payload directory"));
		*result = err;
		return -1;
	}

	result_array = json_object_new_array();
	if (!result_array) {
		closedir(dir);
		*result = json_object_new_string("server alloc memory failed");
		return -1;
	}

	while ((de = readdir(dir)) != NULL) {
		const char *fname = de->d_name;
		size_t len = strlen(fname);
		char fullpath[PATH_MAX];
		json_object *mod_obj;
		int loaded;

		/* match ctforge_<name>.so.xz */
		if (len <= prefix_len + suffix_len)
			continue;
		if (strncmp(fname, prefix, prefix_len) != 0)
			continue;
		if (strcmp(fname + len - suffix_len, suffix) != 0)
			continue;

		snprintf(module_name, sizeof(module_name), "%.*s",
			 (int)(len - prefix_len - suffix_len),
			 fname + prefix_len);

		snprintf(fullpath, sizeof(fullpath), "%s/%s",
			 payload_dir, fname);

		loaded = (find_module(module_name) != NULL);

		mod_obj = json_object_new_object();
		if (!mod_obj)
			continue;

		json_object_object_add(mod_obj, "name",
				       json_object_new_string(module_name));
		json_object_object_add(mod_obj, "path",
				       json_object_new_string(fullpath));
		json_object_object_add(mod_obj, "loaded",
				       json_object_new_boolean(loaded));

		json_object_array_add(result_array, mod_obj);
	}
	closedir(dir);

	if (json_object_array_length(result_array) == 0) {
		json_object_put(result_array);
		json_object *err = json_object_new_object();
		json_object_object_add(err, "code", json_object_new_int(-1));
		json_object_object_add(err, "msg",
				       json_object_new_string("no module available"));
		*result = err;
		return 0;
	}

	*result = result_array;
	return 0;
}

int ops_rmmod(json_object *reqmsg, json_object **result, void *user_data)
{
	json_object *msg_obj;
	int ret = -1;
	json_object *data_obj;
	json_object *mod_array;
	char *saveptr, *token;
	char *msg_copy;
	const char *msg;
	char jmsg[128];

	if (!json_object_object_get_ex(reqmsg, "params", &msg_obj))
		return -1;

	msg = json_object_get_string(msg_obj);
	if (!msg) {
		*result = json_object_new_string("empty input");
		return 0;
	}

	msg_copy = strdup(msg);
	if (!msg_copy)
		return -1;

	token = strtok_r(msg_copy, " \t\n\r\f\v", &saveptr);

	mod_array = json_object_new_array();
	if (!mod_array) {
		free(msg_copy);
		return -1;
	}

	while (token != NULL) {
		pr_dbg("try rmmod %s", token);
		data_obj = json_object_new_object();
		json_object_object_add(data_obj, "modname",
				       json_object_new_string(token));

		ret = do_one_rmmod(token);

		json_object_object_add(data_obj, "ret",
				       json_object_new_int(ret));

		if (!ret) {
			snprintf(jmsg, sizeof(jmsg), "%s unload success!",
				 token);
			json_object_object_add(data_obj, "msg",
					       json_object_new_string(jmsg));
		} else {
			snprintf(jmsg, sizeof(jmsg), "%s unload failed!",
				 token);
			json_object_object_add(data_obj, "msg",
					       json_object_new_string(jmsg));
		}

		json_object_array_add(mod_array, data_obj);
		token = strtok_r(NULL, " \t\n\r\f\v", &saveptr);
	}
	*result = mod_array;
	return 0;
}

int ops_mod(json_object *reqmsg, json_object **result, void *user_data)
{
	json_object *msg_obj;
	const char *params;
	int ret;

	(void)user_data;

	if (!json_object_object_get_ex(reqmsg, "params", &msg_obj)) {
		*result = json_object_new_object();
		if (*result) {
			json_object_object_add(
				*result, "code",
				json_object_new_int(-1));
			json_object_object_add(
				*result, "msg",
				json_object_new_string(
					"Invalid parameter"));
		}
		return (*result) ? 0 : -1;
	}

	params = json_object_get_string(msg_obj);

	/* shared dispatch path (also used by TTL expiry), holding
	 * module_op_lock from module lookup through the so call
	 */
	ret = payload_module_dispatch(params, result);

	return (ret == 0 && *result) ? 0 : -1;
}

/*
 * ops_ttl - time-limited rules with automatic revert.
 *
 * params forms (space separated):
 *   "apply SECONDS <module cmd> -- <revert cmd>"
 *       runs <module cmd> now (e.g. "banopen update --global /f 1"),
 *       then replays <revert cmd> when SECONDS elapse
 *   "cancel ID"   drop the timer, the rule stays as-is
 *   "list"        show pending ttl rules
 */
static json_object *ttl_err_json(const char *msg)
{
	json_object *res = json_object_new_object();

	if (!res)
		return NULL;
	json_object_object_add(res, "code", json_object_new_int(-1));
	json_object_object_add(res, "msg", json_object_new_string(msg));
	return res;
}

int ops_ttl(json_object *reqmsg, json_object **result, void *user_data)
{
	json_object *msg_obj;
	const char *params;
	char *params_copy = NULL;
	char *token, *save_ptr;
	char *apply_cmd = NULL;
	char *revert_cmd = NULL;
	char *p;
	int seconds = 0;
	int seen_sep = 0;
	int ret = -1;

	(void)user_data;
	*result = NULL;

	if (!json_object_object_get_ex(reqmsg, "params", &msg_obj)) {
		*result = ttl_err_json("Invalid parameter");
		goto out;
	}

	params = json_object_get_string(msg_obj);
	if (!params) {
		*result = ttl_err_json("empty input");
		goto out;
	}

	params_copy = strdup(params);
	if (!params_copy) {
		*result = ttl_err_json("out of memory");
		goto out;
	}

	token = strtok_r(params_copy, " ", &save_ptr);
	if (!token) {
		*result = ttl_err_json("Invalid parameter");
		goto out;
	}

	if (strcmp(token, "list") == 0) {
		ret = ttl_list(result);
		goto out;
	}

	if (strcmp(token, "cancel") == 0) {
		char *id_str = strtok_r(NULL, " ", &save_ptr);

		if (!id_str || strtok_r(NULL, " ", &save_ptr)) {
			*result = ttl_err_json("usage: ttl cancel ID");
			goto out;
		}
		ret = ttl_cancel(atoi(id_str), result);
		goto out;
	}

	if (strcmp(token, "apply") != 0) {
		*result = ttl_err_json("unknown ttl subcommand");
		goto out;
	}

	token = strtok_r(NULL, " ", &save_ptr);
	if (!token) {
		*result = ttl_err_json("usage: ttl apply SECONDS "
				       "<module cmd> -- <revert cmd>");
		goto out;
	}
	seconds = atoi(token);
	if (seconds <= 0) {
		*result = ttl_err_json("ttl seconds must be positive");
		goto out;
	}

	/* collect apply tokens until "--", then the revert tokens */
	apply_cmd = calloc(1, TTL_CMD_MAX);
	revert_cmd = calloc(1, TTL_CMD_MAX);
	if (!apply_cmd || !revert_cmd) {
		*result = ttl_err_json("out of memory");
		goto out;
	}

	while ((token = strtok_r(NULL, " ", &save_ptr)) != NULL) {
		if (strcmp(token, "--") == 0) {
			seen_sep = 1;
			continue;
		}
		p = seen_sep ? revert_cmd : apply_cmd;
		if (strlen(p) + strlen(token) + 2 > TTL_CMD_MAX) {
			*result = ttl_err_json("ttl command too long");
			goto out;
		}
		if (p[0])
			strcat(p, " ");
		strcat(p, token);
	}

	if (apply_cmd[0] == '\0' || revert_cmd[0] == '\0') {
		*result = ttl_err_json("usage: ttl apply SECONDS "
				       "<module cmd> -- <revert cmd>");
		goto out;
	}

	pr_info("ttl: apply %ds [%s] revert [%s]", seconds, apply_cmd,
		revert_cmd);
	ret = ttl_apply(apply_cmd, revert_cmd, seconds, result);

out:
	free(params_copy);
	free(apply_cmd);
	free(revert_cmd);
	return (ret == 0 && *result) ? 0 : -1;
}

int ops_status(json_object *reqmsg, json_object **result, void *user_data)
{
	json_object *res_obj = json_object_new_object();

	json_object_object_add(res_obj, "daemon", json_object_new_string("ok"));

	if (check_ctforge_cmdline())
		json_object_object_add(res_obj, "kernel_ctforge",
				       json_object_new_boolean(1));
	else
		json_object_object_add(res_obj, "kernel_ctforge",
				       json_object_new_boolean(0));

	*result = res_obj;
	return 0;
}

int ops_events(json_object *reqmsg, json_object **result, void *user_data)
{
	return collector_status(result);
}

int jsonrpc_register(const char *method_name, jsonrpc_method_fn fn,
		     void *user_data)
{
	if (ctforged_method_count >= MAX_CTFORGED_METHODS)
		return -1;
	method_table[ctforged_method_count].name = strdup(method_name);
	method_table[ctforged_method_count].fn = fn;
	method_table[ctforged_method_count].user_data = user_data;
	ctforged_method_count++;
	return 0;
}

int main(void)
{
	int ret;

	ctforged_uv_loop = uv_default_loop();
	pr_info("CTForge start, version: [%s]", CTFORGE_VERSION);

	pr_info("Reading config from [%s]", CONFIG_PATH);
	ret = ctforged_init_config(CONFIG_PATH, &ctforged_config);
	if (ret != 0) {
		pr_err("Failed to load config from %s ret=[%d]\n", CONFIG_PATH,
		       ret);
		return 1;
	}

	pr_info("Init signal handle");
	ret = ctforged_init_signal();
	if (ret != 0) {
		pr_err("Failed to set signal handle ret=[%d]\n", ret);
		return 1;
	}

	pr_info("Setting rlimit");
	ret = update_rlimit();
	if (ret != 0) {
		pr_err("Failed to set signal handle ret=[%d]\n", ret);
		return 1;
	}

	pr_info("Enable loging");
	ret = ctforged_log_init();
	if (ret != 0) {
		pr_err("Failed to set signal handle ret=[%d]\n", ret);
		return 1;
	}

	pr_info("Enable workpool");
	ret = threadpoll_init();
	if (ret != 0) {
		pr_err("Failed to init workpool ret=[%d]\n", ret);
		return 1;
	}

	pr_info("Enable event collector");
	ret = collector_start();
	if (ret != 0)
		pr_err("collector start failed (non-fatal) ret=[%d]", ret);

	pr_info("Enable ttl thread");
	ret = ttl_start();
	if (ret != 0)
		pr_err("ttl start failed (non-fatal) ret=[%d]", ret);
	register_cleanup(ttl_stop, "ttl_stop");

	pr_info("Workpool add payload_init");
	if (threadpool_add(workpool, (void (*)(void *))payload_init, NULL))
		pr_err("Failed to queue payload_init task");

	pr_info("Workpool add netlink_loop");
	if (threadpool_add(workpool, (void (*)(void *))netlink_loop, NULL))
		pr_err("Failed to queue netlink_loop task");

	pr_info("Register ops method");
	ctforged_method_count = 0;
	jsonrpc_register("insmod", ops_insmod, NULL);
	jsonrpc_register("lsmod", ops_lsmod, NULL);
	jsonrpc_register("rmmod", ops_rmmod, NULL);
	jsonrpc_register("mod", ops_mod, NULL);
	jsonrpc_register("status", ops_status, NULL);
	jsonrpc_register("events", ops_events, NULL);
	jsonrpc_register("ttl", ops_ttl, NULL);
	jsonrpc_register("show", ops_show, NULL);

	pr_info("Starting server");
	start_uv_server(&ctforged_config);
}
