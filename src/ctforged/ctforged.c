#include "ctforge.h"

int new_socket;

#define MAX_EVENTS 10
#define MAX_CTFORGED_METHODS 64

struct ctforge_config ctforged_config = { 0 };

uv_loop_t *ctforged_uv_loop;
uv_tcp_t ctforged_tcp_server;
uv_pipe_t ctforged_unix_server;

struct ctforge_func *ctforge_ops_hashtable[CTFORGE_HASH_SIZE];
pthread_mutex_t hashtable_lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
	char *name;
	jsonrpc_method_fn fn;
	void *user_data;
} method_table[MAX_CTFORGED_METHODS];

static int ctforged_method_count;

LIST_HEAD(module_list_head);

unsigned int hash(char *str)
{
	unsigned int hash = 0;

	while (*str)
		hash = hash * 31 + *str++;

	return hash % CTFORGE_HASH_SIZE;
}

void hashtable_insert_func(char *key, CTFORGE_FUNC function)
{
	unsigned int index = hash(key);
	struct ctforge_func *newNode =
		(struct ctforge_func *)calloc(1, sizeof(struct ctforge_func));
	newNode->key = key;
	newNode->function = function;
	newNode->next = ctforge_ops_hashtable[index];
	ctforge_ops_hashtable[index] = newNode;
}

struct ctforge_func *hashtable_find_func(char *key)
{
	unsigned int index = hash(key);
	struct ctforge_func *node = ctforge_ops_hashtable[index];

	while (node) {
		if (strcmp(node->key, key) == 0)
			return node;
		node = node->next;
	}
	return NULL;
}

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

void on_write_response(uv_write_t *req, int status)
{
	if (status < 0)
		pr_err("Write response failed: %s", uv_err_name(status));

	free(req->data);
	free(req);
}

void on_close(uv_handle_t *handle)
{
	//free(handle);
	pr_info("Close connection");
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
	char *sym_name = NULL;
	json_object *err_ret = NULL;
	struct module_info *mi;
	json_object *msg_obj;
	const char *params;
	char *params_copy = NULL;
	int argc = 0;
	char **argv = NULL;
	char *token, *save_ptr;
	char *module_name;
	so_func_t so_func;
	char msg[128];

	*result = NULL;

	if (!json_object_object_get_ex(reqmsg, "params", &msg_obj))
		goto invalid_param;

	params = json_object_get_string(msg_obj);
	if (!params)
		goto invalid_param;

	params_copy = strdup(params);
	if (!params_copy)
		goto oom;

	argv = calloc(10, sizeof(char *));
	if (!argv)
		goto oom;

	token = strtok_r(params_copy, " ", &save_ptr);
	while (token != NULL && argc < 10) {
		argv[argc] = token;
		argc++;
		token = strtok_r(NULL, " ", &save_ptr);
	}

	if (argc < 1 || argc > 10)
		goto invalid_param;

	module_name = argv[0];
	if (module_name == NULL)
		goto invalid_param;
	pr_info("try find module %s", module_name);
	mi = find_module(module_name);
	if (mi == NULL) {
		err_ret = json_object_new_object();
		json_object_object_add(err_ret, "code",
				       json_object_new_int(-1));
		snprintf(msg, sizeof(msg), "module '%s' not found!",
			 module_name);
		json_object_object_add(err_ret, "msg",
				       json_object_new_string(msg));
		*result = err_ret;
		goto cleanup;
	}

	sym_name = (argc == 1) ? "help" : argv[1];

	so_func = mod_find_sym(mi, module_name, sym_name);
	if (!so_func) {
		err_ret = json_object_new_object();
		json_object_object_add(err_ret, "code",
				       json_object_new_int(-1));
		snprintf(msg, sizeof(msg), "mod '%s' symbol '%s' not found",
			 module_name, sym_name);
		json_object_object_add(err_ret, "msg",
				       json_object_new_string(msg));
		*result = err_ret;
		goto cleanup;
	}

	*result = so_func(argc, argv);
	if (*result == NULL) {
		err_ret = json_object_new_object();
		json_object_object_add(err_ret, "code",
				       json_object_new_int(-1));
		json_object_object_add(
			err_ret, "msg",
			json_object_new_string("Internal error"));
		*result = err_ret;
	}

	goto cleanup;

invalid_param:
	err_ret = json_object_new_object();
	json_object_object_add(err_ret, "code", json_object_new_int(-1));
	json_object_object_add(err_ret, "msg",
			       json_object_new_string("Invalid parameter"));
	*result = err_ret;
	goto cleanup;

oom:
	goto cleanup;

cleanup:
	free(params_copy);
	free(argv);
	return (*result) ? 0 : -1;
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
		pr_err("Failed to set signal handle ret=[%d]\n", ret);
		return 1;
	}

	pr_info("Workpool add payload_init");
	threadpool_add(workpool, (void (*)(void *))payload_init, NULL);

	pr_info("Workpool add netlink_loop");
	threadpool_add(workpool, (void (*)(void *))netlink_loop, NULL);

	pr_info("Register ops method");
	ctforged_method_count = 0;
	jsonrpc_register("insmod", ops_insmod, NULL);
	jsonrpc_register("lsmod", ops_lsmod, NULL);
	jsonrpc_register("rmmod", ops_rmmod, NULL);
	jsonrpc_register("mod", ops_mod, NULL);
	jsonrpc_register("status", ops_status, NULL);

	pr_info("Starting server");
	start_uv_server(&ctforged_config);
}
