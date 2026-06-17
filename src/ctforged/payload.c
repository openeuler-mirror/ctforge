#include "ctforge.h"

struct ctforge_kern *skel;
struct module_info *payload_module_list;
static pthread_mutex_t module_list_lock = PTHREAD_MUTEX_INITIALIZER;

int payload_init(void)
{
	return 0;
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

so_func_t module_find_symbol_c(struct module_info *mi, char *module_name,
			       char *sym_name)
{
	char symbol_full_name[PATH_MAX];
	so_func_t sym_func;

	snprintf(symbol_full_name, PATH_MAX, "ctforge_%s_%s", module_name,
		 sym_name);
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

	s->modinfo->payload_fd = memfd_create("xz_payload", MFD_CLOEXEC);
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

int do_one_insmod(char *module_name)
{
	int ret = -1;
	struct module_info *tmp = NULL;
	json_object *result;
	struct ctforged_params *s = malloc(sizeof(struct ctforged_params));

	pr_dbg("do load module [%s]", module_name);

	s->modinfo = malloc(sizeof(struct module_info));
	s->modinfo->module_name = malloc(NAME_MAX);
	snprintf(s->modinfo->module_name, NAME_MAX, "%s", module_name);

	snprintf(s->modinfo->payload_fullpath, PATH_MAX, "%s/ctforge_%s.so.xz",
		 ctforged_config.payload_dir, module_name);
	pr_info("try load module from %s", s->modinfo->payload_fullpath);

	tmp = find_module(module_name);
	if (tmp != NULL) {
		pr_err("module %s already loaded!\n", module_name);
		free(s->modinfo);
		return 1;
	}
	ret = decompress_xzpayload(s);
	if (ret) {
		pr_err("unpack %s payload failed! ret=[%d]\n",
		       s->modinfo->payload_fullpath, ret);
		free(s->modinfo);
		return 1;
	}

	ret = do_safty_verify_sign(PUBLICKEY_PATH,
				   s->modinfo->payload_memfd_path);
	if (ret) {
		pr_err("Signature check failed!");
		return 1;
	}

	s->modinfo->dl_handle =
		dlopen(s->modinfo->payload_memfd_path, RTLD_NOW);
	if (!s->modinfo->dl_handle) {
		pr_err("Unable to load module: %s\n", dlerror());
		return 1;
	}
	pr_dbg("mod %s dlopen ret %p", s->modinfo->dl_handle);

	snprintf(s->modinfo->module_name, NAME_MAX, "%s", module_name);

	s->modinfo->module_ops.init =
		module_find_symbol(s->modinfo, module_name, "init");
	if (!s->modinfo->module_ops.init) {
		pr_err("dlsym ctforge_%s_%s failed: %s\n", module_name, "init",
		       dlerror());
		goto close_handler;
	}

	s->modinfo->module_ops.exit =
		module_find_symbol(s->modinfo, module_name, "exit");
	if (!s->modinfo->module_ops.exit) {
		pr_err("dlsym ctforge_%s_%s failed: %s\n", module_name, "exit",
		       dlerror());
		goto close_handler;
	}

	result = s->modinfo->module_ops.init(s->argc, s->argv);

	if (result == NULL) {
		pr_err("module %s loaded failed\n", module_name);
		goto close_handler;
	}

	pr_dbg("result = %p", (void *)result);

	if (!json_object_is_type(result, json_type_object)) {
		pr_err("module %s init did not return a JSON object (type=%d)\n",
		       module_name, json_object_get_type(result));
		json_object_put(
			result); // safe to put even if invalid? better avoid.
		// But if it's not a valid json_object*, putting may crash too!
		// So we avoid put() if type check fails.
		goto close_handler;
	}

	int code = 0;
	const char *status = NULL;
	const char *message = NULL;

	json_object *code_obj, *status_obj, *msg_obj;

	if (json_object_object_get_ex(result, "code", &code_obj)) {
		if (json_object_is_type(code_obj, json_type_int)) {
			code = json_object_get_int(code_obj);
		} else {
			pr_warn("module %s: 'code' is not an integer\n",
				module_name);
			code = -1;
		}
	}
	if (json_object_object_get_ex(result, "status", &status_obj)) {
		if (json_object_is_type(status_obj, json_type_string)) {
			status = json_object_get_string(status_obj);
		} else {
			pr_warn("module %s: 'status' is not a string\n",
				module_name);
			status = "invalid";
		}
	}
	if (json_object_object_get_ex(result, "message", &msg_obj)) {
		if (json_object_is_type(msg_obj, json_type_string)) {
			message = json_object_get_string(msg_obj);
		} else {
			pr_warn("module %s: 'message' is not a string\n",
				module_name);
			message = "invalid message type";
		}
	}
	if (code != 0) {
		pr_debug("init module %s failed! %s", module_name, message);
		goto close_handler;
	}

	pr_debug("init module %s success!", module_name);

	ret = add_module(s->modinfo);
	if (ret != 0) {
		pr_err("load module %s failed, ret=%d\n", module_name, ret);
		goto close_handler;
	}
	pr_debug("load module %s success!", module_name);

	close(s->modinfo->payload_fd);
	s->modinfo->payload_fd = -1;
	return 0;

close_handler:
	dlclose(s->modinfo->dl_handle);
	return 1;
}

int do_one_rmmod(char *module_name)
{
	int ret = -1;
	json_object *result;
	struct ctforged_params *s = malloc(sizeof(struct ctforged_params));

	s->modinfo = find_module(module_name);
	if (s->modinfo == NULL) {
		pr_dbg("module %s not found!");
		return -1;
	}

	if (!s->modinfo->module_ops.exit) {
		pr_dbg("module %s exit func not found!");
		return -1;
	}

	s->argc = 0;
	s->argv = NULL;
	result = s->modinfo->module_ops.exit(s->argc, s->argv);

	dlerror();
	if (s->modinfo->dl_handle) {
		ret = dlclose(s->modinfo->dl_handle);
		s->modinfo->dl_handle = NULL;
		pr_dbg("dlclose %s so handle ret=%d", module_name, ret);
	}

	if (s->modinfo->payload_fd != -1) {
		close(s->modinfo->payload_fd);
		pr_dbg("close %s memfd [%d]", module_name,
		       s->modinfo->payload_fd);
		s->modinfo->payload_fd = -1;
	}

	delete_module(s->modinfo);
	pr_dbg("clean module memory", module_name);

	free(s->modinfo);
	free(s);
	return 0;
}
