#include "ctforge.h"

/* MFD_EXEC (kernel 6.3+) is missing from older glibc headers */
#ifndef MFD_EXEC
#define MFD_EXEC 0x0010
#endif

static pthread_mutex_t module_list_lock = PTHREAD_MUTEX_INITIALIZER;

/* module ops (insmod/rmmod/reconcile) are serialized: a reconcile
 * thread racing an RPC insmod could otherwise run a module's exit
 * on a freshly registered module
 */
static pthread_mutex_t module_op_lock = PTHREAD_MUTEX_INITIALIZER;

/* the module list is persisted so a daemon restart can restore
 * every module that was loaded before the restart
 */
#define MODULES_STATE_DIR "/var/lib/ctforge"
#define MODULES_LIST_PATH MODULES_STATE_DIR "/modules.list"

int payload_init(void)
{
	char name[NAME_MAX];
	FILE *f;
	int restored = 0, failed = 0;

	/* reconcile: re-load every module that was insmod'ed before
	 * the daemon (re)started, so restarts do not orphan modules
	 */
	f = fopen(MODULES_LIST_PATH, "r");
	if (!f)
		return 0;

	pr_info("restoring modules from %s", MODULES_LIST_PATH);
	while (fgets(name, sizeof(name), f)) {
		name[strcspn(name, "\n")] = '\0';
		if (name[0] == '\0')
			continue;
		if (do_one_insmod(name) == 0) {
			restored++;
		} else {
			failed++;
			pr_err("restore module %s failed", name);
		}
	}
	fclose(f);
	pr_info("module restore done: %d ok, %d failed", restored, failed);
	return 0;
}

static void modules_list_save_locked(void)
{
	struct module_info *iter;
	char tmp[PATH_MAX];
	FILE *f;

	mkdir(MODULES_STATE_DIR, 0755);
	snprintf(tmp, sizeof(tmp), "%s.tmp", MODULES_LIST_PATH);
	f = fopen(tmp, "w");
	if (!f) {
		pr_err("cannot save module list: %s", strerror(errno));
		return;
	}
	list_for_each_entry(iter, &module_list_head, list)
		fprintf(f, "%s\n", iter->module_name);
	fclose(f);
	if (rename(tmp, MODULES_LIST_PATH) != 0)
		pr_err("cannot publish module list: %s", strerror(errno));
}

static inline void __list_add(struct list_head *new, struct list_head *prev,
			      struct list_head *next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

static inline void list_add(struct list_head *new, struct list_head *head)
{
	__list_add(new, head, head->next);
}

static inline void __list_del(struct list_head *prev, struct list_head *next)
{
	next->prev = prev;
	prev->next = next;
}

static inline void list_del(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
	entry->next = NULL;
	entry->prev = NULL;
}

static struct module_info *find_module_unlocked(const char *name)
{
	struct module_info *iter;

	list_for_each_entry(iter, &module_list_head, list) {
		if (strncmp(iter->module_name, name, NAME_MAX) == 0)
			return iter;
	}
	return NULL;
}

int add_module(struct module_info *mod)
{
	pthread_mutex_lock(&module_list_lock);
	if (find_module_unlocked(mod->module_name)) {
		pthread_mutex_unlock(&module_list_lock);
		return -1;
	}
	list_add(&mod->list, &module_list_head);
	pthread_mutex_unlock(&module_list_lock);
	return 0;
}

void delete_module(struct module_info *mod)
{
	pthread_mutex_lock(&module_list_lock);
	list_del(&mod->list);
	pthread_mutex_unlock(&module_list_lock);
}

struct module_info *find_module(const char *name)
{
	struct module_info *iter;

	pthread_mutex_lock(&module_list_lock);
	list_for_each_entry(iter, &module_list_head, list) {
		if (strncmp(iter->module_name, name, NAME_MAX) == 0) {
			pthread_mutex_unlock(&module_list_lock);
			return iter;
		}
	}
	pthread_mutex_unlock(&module_list_lock);
	return NULL;
}

so_func_t module_find_symbol(struct module_info *mi, char *module_name,
			     char *sym_name)
{
	char symbol_full_name[PATH_MAX];
	so_func_t sym_func;

	snprintf(symbol_full_name, PATH_MAX, "ctforge_%s_%s", module_name,
		 sym_name);
	pr_info("try find [%s]", symbol_full_name);
	sym_func = dlsym(mi->dl_handle, symbol_full_name);
	if (sym_func == NULL) {
		pr_err("dlsym module_init failed: %s\n", dlerror());

		return NULL;
	}
	return sym_func;
}

void *mod_find_sym(struct module_info *mi, char *module_name, char *sym_name)
{
	char symbol_full_name[PATH_MAX];
	void *sym_func;

	snprintf(symbol_full_name, PATH_MAX, "ctforge_%s_%s", module_name,
		 sym_name);
	sym_func = dlsym(mi->dl_handle, symbol_full_name);
	if (sym_func == NULL) {
		pr_err("dlsym module_init failed: %s\n", dlerror());
		return NULL;
	}
	return sym_func;
}

int decompress_xzpayload(struct ctforged_params *s)
{
	lzma_stream strm = LZMA_STREAM_INIT;
	uint8_t out_buf[BUFSIZ];
	FILE *infile = NULL;
	uint8_t *file_data = NULL;
	size_t file_size = 0;
	lzma_ret ret;

	s->modinfo->payload_fd = -1;
	infile = fopen(s->modinfo->payload_fullpath, "rb");
	if (!infile) {
		pr_err("Cannot open xz file: %s", s->modinfo->payload_fullpath);
		goto fail;
	}

	if (fseek(infile, 0, SEEK_END) != 0) {
		pr_err("fseek failed");
		goto fail;
	}

	long sz = ftell(infile);

	if (sz < 0) {
		pr_err("ftell failed");
		goto fail;
	}

	file_size = (size_t)sz;

	if (fseek(infile, 0, SEEK_SET) != 0) {
		pr_err("rewind failed");
		goto fail;
	}

	file_data = malloc(file_size);
	if (!file_data) {
		pr_err("malloc for file data failed");
		goto fail;
	}

	if (fread(file_data, 1, file_size, infile) != file_size) {
		pr_err("failed to read entire file");
		goto fail;
	}
	fclose(infile);
	infile = NULL;

	s->modinfo->payload_fd = memfd_create("ctforge_payload",
					      MFD_CLOEXEC | MFD_EXEC);
	if (s->modinfo->payload_fd == -1) {
		pr_err("memfd_create failed: %s", strerror(errno));
		goto fail;
	}
	pr_dbg("module %s payload fd %d", s->modinfo->module_name,
	       s->modinfo->payload_fd);

	ret = lzma_auto_decoder(&strm, UINT64_MAX, 0);
	if (ret != LZMA_OK) {
		pr_err("lzma_auto_decoder error: %d", ret);
		goto fail;
	}

	strm.next_in = file_data;
	strm.avail_in = file_size;

	while (1) {
		strm.next_out = out_buf;
		strm.avail_out = sizeof(out_buf);

		ret = lzma_code(&strm, LZMA_FINISH);

		if (ret != LZMA_OK && ret != LZMA_STREAM_END) {
			pr_err("Decompression error: %d", ret);
			goto fail;
		}

		size_t produced = sizeof(out_buf) - strm.avail_out;

		if (produced > 0) {
			if (write(s->modinfo->payload_fd, out_buf, produced) !=
			    (ssize_t)produced) {
				pr_err("write to memfd failed: %s",
				       strerror(errno));
				goto fail;
			}
		}

		if (ret == LZMA_STREAM_END)
			break;
	}

	off_t payload_size = lseek(s->modinfo->payload_fd, 0, SEEK_END);

	if (payload_size == -1) {
		pr_err("lseek failed: %s", strerror(errno));
		goto fail;
	}
	s->modinfo->payload_size = (size_t)payload_size;

	if (lseek(s->modinfo->payload_fd, 0, SEEK_SET) == -1)
		pr_err("lseek to beginning failed");

	snprintf(s->modinfo->payload_memfd_path,
		 sizeof(s->modinfo->payload_memfd_path), "/proc/self/fd/%d",
		 s->modinfo->payload_fd);
	lzma_end(&strm);
	free(file_data);
	return 0;

fail:
	if (infile)
		fclose(infile);
	if (file_data)
		free(file_data);
	if (s->modinfo->payload_fd != -1) {
		close(s->modinfo->payload_fd);
		s->modinfo->payload_fd = -1;
	}
	lzma_end(&strm);
	return -1;
}

static int do_one_insmod_locked(char *module_name)
{
	int ret;
	struct module_info *tmp = NULL;
	json_object *result = NULL;
	struct ctforged_params *s;
	struct module_info *mi;

	pr_dbg("do load module [%s]", module_name);

	s = calloc(1, sizeof(struct ctforged_params));
	if (!s) {
		pr_err("alloc ctforged_params failed");
		return 1;
	}
	s->argc = 0;
	s->argv = NULL;

	mi = calloc(1, sizeof(struct module_info));
	if (!mi) {
		pr_err("alloc module_info failed");
		free(s);
		return 1;
	}
	mi->payload_fd = -1;
	mi->module_name = malloc(NAME_MAX);
	if (!mi->module_name) {
		pr_err("alloc module name failed");
		free(mi);
		free(s);
		return 1;
	}
	snprintf(mi->module_name, NAME_MAX, "%s", module_name);

	snprintf(mi->payload_fullpath, PATH_MAX, "%s/ctforge_%s.so.xz",
		 ctforged_config.payload_dir, module_name);
	pr_info("try load module from %s", mi->payload_fullpath);

	s->modinfo = mi;

	tmp = find_module(module_name);
	if (tmp != NULL) {
		pr_err("module %s already loaded!\n", module_name);
		goto err_free;
	}
	ret = decompress_xzpayload(s);
	if (ret) {
		pr_err("unpack %s payload failed! ret=[%d]\n",
		       mi->payload_fullpath, ret);
		goto err_free;
	}

	/* honor the configured public key path, fall back to the
	 * compiled-in default
	 */
	char *pubkey = ctforged_config.public_key;

	if (!pubkey)
		pubkey = (char *)PUBLICKEY_PATH;

	ret = do_safty_verify_sign(pubkey, mi->payload_memfd_path);
	if (ret) {
		pr_err("Signature check failed!");
		goto err_free;
	}

	mi->dl_handle = dlopen(mi->payload_memfd_path, RTLD_NOW);
	if (!mi->dl_handle) {
		pr_err("Unable to load module: %s\n", dlerror());
		goto err_free;
	}
	pr_dbg("mod %s dlopen ret %p", module_name, mi->dl_handle);

	mi->module_ops.init = module_find_symbol(mi, module_name, "init");
	if (!mi->module_ops.init) {
		pr_err("dlsym ctforge_%s_%s failed: %s\n", module_name,
		       "init", dlerror());
		goto err_close;
	}

	mi->module_ops.exit = module_find_symbol(mi, module_name, "exit");
	if (!mi->module_ops.exit) {
		pr_err("dlsym ctforge_%s_%s failed: %s\n", module_name,
		       "exit", dlerror());
		goto err_close;
	}

	result = mi->module_ops.init(s->argc, s->argv);

	if (result == NULL) {
		pr_err("module %s loaded failed\n", module_name);
		goto err_close;
	}

	if (!json_object_is_type(result, json_type_object)) {
		pr_err("module %s init did not return a JSON object (type=%d)\n",
		       module_name, json_object_get_type(result));
		json_object_put(result);
		goto err_close;
	}

	int code = 0;
	const char *message = NULL;

	json_object *code_obj, *msg_obj;

	if (json_object_object_get_ex(result, "code", &code_obj)) {
		if (json_object_is_type(code_obj, json_type_int))
			code = json_object_get_int(code_obj);
		else
			code = -1;
	}
	if (json_object_object_get_ex(result, "message", &msg_obj)) {
		if (json_object_is_type(msg_obj, json_type_string))
			message = json_object_get_string(msg_obj);
	}

	json_object_put(result);

	if (code != 0) {
		pr_debug("init module %s failed! %s", module_name, message);
		goto err_close;
	}

	/* register the module's ring buffer with the event collector
	 * so its logs are continuously drained to the events file.
	 * The logmap fd is obtained by calling the module's "log"
	 * subcommand which returns {"code":0,"logmap":<fd>}; modules
	 * without a ringbuf (e.g. hardbase) are silently skipped.
	 */
	{
		so_func_t log_fn = mod_find_sym(mi, module_name, "log");
		int logmap_fd = -1;

		if (log_fn) {
			json_object *log_result = log_fn(0, NULL);

			if (log_result) {
				json_object *logmap_obj = NULL;

				if (json_object_object_get_ex(log_result,
							      "logmap",
							      &logmap_obj) &&
				    json_object_is_type(logmap_obj,
							json_type_int))
					logmap_fd = json_object_get_int(
						logmap_obj);
				json_object_put(log_result);
			}
		}
		if (logmap_fd >= 0)
			collector_register(module_name, logmap_fd);
	}

	pr_debug("init module %s success!", module_name);

	ret = add_module(mi);
	if (ret != 0) {
		pr_err("load module %s failed, ret=%d\n", module_name, ret);
		/* init succeeded, undo it before unloading the .so */
		if (mi->module_ops.exit)
			mi->module_ops.exit(0, NULL);
		goto err_close;
	}
	pr_debug("load module %s success!", module_name);

	modules_list_save_locked();

	/* Keep the memfd open for the module's whole lifetime.
	 * dlopen() caches loaded objects by path string: if this fd
	 * were closed here, the next module's memfd would likely
	 * reuse the same fd number, "/proc/self/fd/N" would hit the
	 * stale cache entry and dlsym() would search the WRONG
	 * module. do_one_rmmod() closes the fd at unload time.
	 */
	free(s);
	return 0;

err_close:
	dlclose(mi->dl_handle);
err_free:
	if (mi->payload_fd != -1)
		close(mi->payload_fd);
	free(mi->module_name);
	free(mi);
	free(s);
	return 1;
}

static int do_one_rmmod_locked(char *module_name)
{
	int ret;
	json_object *result;
	struct ctforged_params *s = malloc(sizeof(struct ctforged_params));

	if (!s) {
		pr_err("alloc ctforged_params failed");
		return -1;
	}

	s->modinfo = find_module(module_name);
	if (s->modinfo == NULL) {
		pr_dbg("module %s not found!", module_name);
		free(s);
		return -1;
	}

	if (!s->modinfo->module_ops.exit) {
		pr_dbg("module %s exit func not found!", module_name);
		free(s);
		return -1;
	}

	s->argc = 0;
	s->argv = NULL;
	result = s->modinfo->module_ops.exit(s->argc, s->argv);

	/* a failed exit means the module is still active: keep it loaded */
	if (!result || !json_object_is_type(result, json_type_object) ||
	    json_object_get_int(json_object_object_get(result, "code"))) {
		pr_err("module %s exit failed, keep it loaded", module_name);
		if (result)
			json_object_put(result);
		free(s);
		return -1;
	}
	json_object_put(result);

	dlerror();
	if (s->modinfo->dl_handle) {
		ret = dlclose(s->modinfo->dl_handle);
		s->modinfo->dl_handle = NULL;
		pr_dbg("dlclose %s so handle ret=%d", module_name, ret);
	}

	if (s->modinfo->payload_fd != -1) {
		pr_dbg("close %s memfd [%d]", module_name,
		       s->modinfo->payload_fd);
		close(s->modinfo->payload_fd);
		s->modinfo->payload_fd = -1;
	}

	delete_module(s->modinfo);
	pr_dbg("clean module memory %s", module_name);

	modules_list_save_locked();

	/* stop continuous event collection for this module */
	collector_unregister(module_name);

	free(s->modinfo->module_name);
	free(s->modinfo);
	free(s);
	return 0;
}

int do_one_insmod(char *module_name)
{
	int ret;

	pthread_mutex_lock(&module_op_lock);
	ret = do_one_insmod_locked(module_name);
	pthread_mutex_unlock(&module_op_lock);
	return ret;
}

int do_one_rmmod(char *module_name)
{
	int ret;

	pthread_mutex_lock(&module_op_lock);
	ret = do_one_rmmod_locked(module_name);
	pthread_mutex_unlock(&module_op_lock);
	return ret;
}

/* dispatch commands are capped at 10 tokens (module + subcmd + args) */
#define DISPATCH_ARGV_MAX 10

static json_object *dispatch_err_json(const char *msg)
{
	json_object *res = json_object_new_object();

	if (!res)
		return NULL;
	json_object_object_add(res, "code", json_object_new_int(-1));
	json_object_object_add(res, "msg", json_object_new_string(msg));
	return res;
}

/* Split a space-separated dispatch command into argv.
 * Returns the token count, or -1 when there are too many tokens.
 */
static int dispatch_split_argv(char *params_copy, char **argv, int argv_max)
{
	char *token, *save_ptr;
	int argc = 0;

	token = strtok_r(params_copy, " ", &save_ptr);
	while (token != NULL && argc < argv_max) {
		argv[argc++] = token;
		token = strtok_r(NULL, " ", &save_ptr);
	}
	if (token != NULL)
		return -1;
	return argc;
}

/*
 * payload_module_dispatch - run a module subcommand.
 * @params: space-separated command: "<module> [subcmd] [args...]"
 * @result: receives the module's JSON reply (always set on return)
 *
 * The whole call runs under module_op_lock so a subcommand cannot
 * race an rmmod/dlclose of the same module (the TTL expiry thread
 * reuses this path). argv[0] is the module name; a command with no
 * subcmd resolves to the module's "help" symbol.
 *
 * Returns 0 when a reply object was produced (module reply or a
 * constructed error object), or -1 if *result is NULL (out of
 * memory). Callers distinguish success by the reply's "code".
 */
int payload_module_dispatch(const char *params, json_object **result)
{
	struct module_info *mi;
	char *params_copy = NULL;
	char **argv = NULL;
	json_object *res = NULL;
	char *module_name;
	char *sym_name;
	so_func_t so_func;
	char msg[128];
	int argc;

	*result = NULL;

	if (!params || strlen(params) == 0) {
		*result = dispatch_err_json("Invalid parameter");
		return -1;
	}

	params_copy = strdup(params);
	argv = calloc(DISPATCH_ARGV_MAX, sizeof(char *));
	if (!params_copy || !argv)
		goto oom;

	argc = dispatch_split_argv(params_copy, argv, DISPATCH_ARGV_MAX);
	if (argc < 1)
		goto invalid;

	module_name = argv[0];
	sym_name = (argc == 1) ? "help" : argv[1];

	/* hold the op lock from lookup to the so call so the module
	 * cannot be unloaded (dlclose'd) mid-call
	 */
	pthread_mutex_lock(&module_op_lock);
	mi = find_module(module_name);
	if (!mi) {
		pthread_mutex_unlock(&module_op_lock);
		snprintf(msg, sizeof(msg), "module '%s' not found!",
			 module_name);
		*result = dispatch_err_json(msg);
		goto out;
	}

	so_func = mod_find_sym(mi, module_name, sym_name);
	if (!so_func) {
		pthread_mutex_unlock(&module_op_lock);
		snprintf(msg, sizeof(msg), "mod '%s' symbol '%s' not found",
			 module_name, sym_name);
		*result = dispatch_err_json(msg);
		goto out;
	}

	res = so_func(argc, argv);
	pthread_mutex_unlock(&module_op_lock);

	if (!res) {
		*result = dispatch_err_json("Internal error");
		goto out;
	}

	*result = res;
	goto out;

invalid:
	*result = dispatch_err_json("Invalid parameter");
	goto out;
oom:
	*result = dispatch_err_json("out of memory");
out:
	free(params_copy);
	free(argv);
	return (*result) ? 0 : -1;
}
