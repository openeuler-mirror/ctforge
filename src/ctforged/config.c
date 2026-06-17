#include "ctforge.h"

struct cleanup_task tasks[MAX_CLEANUP_TASKS];
int task_count;

char *ctforged_config_strdup(const char *s, const char *field_name)
{
	char *dup = NULL;

	if (!s) {
		errno = EINVAL;
		return NULL;
	}

	dup = strdup(s);
	if (!dup) {
		pr_err("Out of memory duplicating config field '%s'\n",
		       field_name);
		errno = ENOMEM;
	}
	return dup;
}

const char *ctforged_get_config(json_object *root, const char *field_name,
				const char *default_value)
{
	json_object *obj = NULL;
	const char *value;

	if (!root)
		return default_value;

	if (!json_object_object_get_ex(root, field_name, &obj))
		return default_value;

	if (!json_object_is_type(obj, json_type_string)) {
		pr_err("Config error: field '%s' must be a string\n",
		       field_name);
		return NULL;
	}

	value = json_object_get_string(obj);
	if (!value || strlen(value) == 0) {
		pr_err("Config error: field '%s' cannot be empty\n",
		       field_name);
		return NULL;
	}

	return value;
}

void ctforged_clean_config(void)
{
	free(ctforged_config.listen);
	free(ctforged_config.payload_dir);
	free(ctforged_config.log_dir);
	free(ctforged_config.private_key);
	free(ctforged_config.public_key);
	memset(&ctforged_config, 0, sizeof(ctforged_config));
}

/**
 * ctforged_init_config - Initialize configuration from a JSON file or defaults.
 * @config_path: Path to the configuration file (may be NULL to force defaults).
 * @cfg:        Pointer to the config structure to populate.
 *
 * This function attempts to read and parse a JSON configuration file.
 * If the file does not exist, is empty, or is not provided, default values
 * are used. All string fields in @cfg are allocated dynamically and must
 * be freed using ctforged_clean_config().
 *
 * Returns 0 on success, -1 on error (with errno set appropriately).
 */
int ctforged_init_config(const char *config_path, struct ctforge_config *cfg)
{
	static const char default_listen[] =
		"unix:///usr/share/ctforge/ctforge.socket";
	static const char default_payload_dir[] = "/usr/share/ctforge/payload/";
	static const char default_log_dir[] = "/var/log/ctforge/";
	static const char default_private_key[] = "/etc/ctforge/private.pem";
	static const char default_public_key[] = "/etc/ctforge/public.pem";

	const char *listen_val;
	const char *payload_dir_val;
	const char *log_dir_val;
	const char *private_key_val;
	const char *public_key_val;

	json_object *root = NULL;
	FILE *fp = NULL;
	char *buf = NULL;
	long len;
	int ret = -1;

	if (!config_path || !cfg) {
		errno = EINVAL;
		return -1;
	}

	memset(cfg, 0, sizeof(*cfg));

	/* Try to open the config file. */
	fp = fopen(config_path, "r");
	if (!fp) {
		if (errno == ENOENT) {
			/* Config file missing — proceed with defaults. */
			root = NULL;
			goto use_defaults;
		}
		pr_err("Failed to open config file %s: %s\n", config_path,
		       strerror(errno));
		goto cleanup;
	}

	/* Determine file size. */
	if (fseek(fp, 0, SEEK_END) != 0) {
		pr_err("fseek(SEEK_END) failed on %s: %s\n", config_path,
		       strerror(errno));
		goto cleanup;
	}

	len = ftell(fp);
	if (len < 0) {
		pr_err("ftell() failed on %s: %s\n", config_path,
		       strerror(errno));
		goto cleanup;
	}

	if (len == 0) {
		/* Empty file — treat as no config. */
		fclose(fp);
		fp = NULL;
		root = NULL;
		goto use_defaults;
	}

	if (fseek(fp, 0, SEEK_SET) != 0) {
		pr_err("fseek(SEEK_SET) failed on %s: %s\n", config_path,
		       strerror(errno));
		goto cleanup;
	}

	buf = malloc((size_t)len + 1);
	if (!buf) {
		pr_err("malloc() failed: %s\n", strerror(errno));
		goto cleanup;
	}

	if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
		pr_err("Failed to read config file %s: %s\n", config_path,
		       strerror(errno));
		goto cleanup;
	}
	buf[len] = '\0';

	fclose(fp);
	fp = NULL;

	/* Parse JSON content. */
	root = json_tokener_parse(buf);
	if (!root) {
		pr_err("Failed to parse JSON config file: %s\n", config_path);
		goto cleanup;
	}

	if (!json_object_is_type(root, json_type_object)) {
		pr_err("Root of config must be a JSON object\n");
		goto cleanup;
	}

use_defaults:

	listen_val = ctforged_get_config(root, "listen", default_listen);
	if (!listen_val)
		goto cleanup;
	cfg->listen = ctforged_config_strdup(listen_val, "listen");
	if (!cfg->listen)
		goto cleanup;

	payload_dir_val =
		ctforged_get_config(root, "payload_dir", default_payload_dir);
	if (!payload_dir_val)
		goto cleanup;
	cfg->payload_dir =
		ctforged_config_strdup(payload_dir_val, "payload_dir");
	if (!cfg->payload_dir)
		goto cleanup;

	log_dir_val = ctforged_get_config(root, "log_dir", default_log_dir);
	if (!log_dir_val)
		goto cleanup;
	cfg->log_dir = ctforged_config_strdup(log_dir_val, "log_dir");
	if (!cfg->log_dir)
		goto cleanup;

	private_key_val =
		ctforged_get_config(root, "private_key", default_private_key);
	if (!private_key_val)
		goto cleanup;
	cfg->private_key =
		ctforged_config_strdup(private_key_val, "private_key");
	if (!cfg->private_key)
		goto cleanup;

	public_key_val =
		ctforged_get_config(root, "public_key", default_public_key);
	if (!public_key_val)
		goto cleanup;
	cfg->public_key = ctforged_config_strdup(public_key_val, "public_key");
	if (!cfg->public_key)
		goto cleanup;

	register_cleanup(ctforged_clean_config, "clean config");
	ret = 0;

cleanup:
	if (fp)
		fclose(fp);
	free(buf);
	if (root)
		json_object_put(root);

	if (ret != 0)
		ctforged_clean_config();

	return ret;
}

int register_cleanup(void (*fn)(void), const char *name)
{
	if (!fn || !name)
		return -1;

	if (task_count >= MAX_CLEANUP_TASKS) {
		pr_err("cleanup: too many tasks\n");
		return -1;
	}

	tasks[task_count].fn = fn;
	tasks[task_count].name = name;
	task_count++;

	return 0;
}

void shutdown_cleanup(void)
{
	int i;

	for (i = task_count - 1; i >= 0; i--) {
		if (tasks[i].fn) {
			pr_info("cleanup: running %s", tasks[i].name);
			tasks[i].fn();
		}
	}

	task_count = 0;
}
