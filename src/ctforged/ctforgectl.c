#include "ctforge.h"

#define MAX_JSON_DEPTH 32
#define MAX_BUFFER 4096

struct ctforge_config ctforged_config = { 0 };

static uv_loop_t *loop;
static uv_tcp_t ctforged_tcp_client;
static uv_pipe_t ctforged_sock_client;

static int is_unix;

/* Global copies for use in callback */
static int argc_global;
static char **argv_global;
static char *global_method;

static const char *json_value_to_str(json_object *val, char *buf, size_t buf_sz)
{
	enum json_type t;

	if (!val)
		return "null";

	t = json_object_get_type(val);
	switch (t) {
	case json_type_string:
		return json_object_get_string(val);
	case json_type_boolean:
		return json_object_get_boolean(val) ? "true" : "false";
	case json_type_int:
		if (snprintf(buf, buf_sz, "%d", json_object_get_int(val)) < 0)
			return "[fmt_err]";
		return buf;
	case json_type_double:
		if (snprintf(buf, buf_sz, "%.6g", json_object_get_double(val)) <
		    0)
			return "[fmt_err]";
		return buf;
	case json_type_null:
		return "null";
	default:
		return "[complex]";
	}
}

static void format_object_line(json_object *obj, char **out_str)
{
	*out_str = NULL;
	char *buf;
	size_t offset = 0;

	if (!obj || !json_object_is_type(obj, json_type_object))
		return;

	struct json_object_iterator it = json_object_iter_begin(obj);
	struct json_object_iterator itEnd = json_object_iter_end(obj);
	size_t total_len = 0;
	size_t num_fields = 0;

	while (!json_object_iter_equal(&it, &itEnd)) {
		const char *key = json_object_iter_peek_name(&it);
		json_object *val = json_object_iter_peek_value(&it);

		if (key && val) {
			char tmp[64];
			const char *str_val =
				json_value_to_str(val, tmp, sizeof(tmp));

			total_len += strlen(key) + 1 + strlen(str_val) + 1;
			num_fields++;
		}
		json_object_iter_next(&it);
	}

	if (num_fields == 0)
		return;

	buf = malloc(total_len + 1);
	if (!buf)
		return;

	it = json_object_iter_begin(obj);
	while (!json_object_iter_equal(&it, &itEnd) && offset < total_len) {
		const char *key = json_object_iter_peek_name(&it);
		json_object *val = json_object_iter_peek_value(&it);

		if (key && val) {
			char tmp[64];
			const char *str_val =
				json_value_to_str(val, tmp, sizeof(tmp));
			int written = snprintf(buf + offset,
					       total_len - offset + 1, "%s:%s ",
					       key, str_val);
			if (written <= 0 ||
			    (size_t)written > total_len - offset) {
				break;
			}
			offset += (size_t)written;
		}
		json_object_iter_next(&it);
	}

	if (offset > 0)
		buf[offset - 1] = '\0'; // trim trailing space
	else
		buf[0] = '\0';

	*out_str = buf;
}

static void print_json_flat(const char *prefix, json_object *obj, int depth)
{
	char tmp[128];
	enum json_type type;
	size_t len;
	size_t i;
	char *line;
	const char *s;
	json_object *elem;
	const char *val_str;

	if (!prefix || !obj || depth > MAX_JSON_DEPTH) {
		if (depth > MAX_JSON_DEPTH) {
			pr_info("%s = [max depth exceeded]",
				prefix ? prefix : "???");
		}
		return;
	}

	type = json_object_get_type(obj);
	if (type == json_type_array) {
		pr_info("%s:", prefix);
		len = json_object_array_length(obj);
		for (i = 0; i < len; i++) {
			elem = json_object_array_get_idx(obj, i);
			if (!elem)
				continue;

			if (json_object_is_type(elem, json_type_object)) {
				line = NULL;
				format_object_line(elem, &line);
				if (line && line[0]) {
					pr_info("       %s", line);
					free(line);
				} else if (line) {
					free(line);
				}
			} else {
				s = json_object_to_json_string_ext(
					elem, JSON_C_TO_STRING_PLAIN);
				pr_info("       %s", s ? s : "[null]");
			}
		}
		return;
	}

	if (type == json_type_object) {
		struct json_object_iterator it = json_object_iter_begin(obj);
		struct json_object_iterator itEnd = json_object_iter_end(obj);

		while (!json_object_iter_equal(&it, &itEnd)) {
			const char *key = json_object_iter_peek_name(&it);
			json_object *val = json_object_iter_peek_value(&it);

			if (key) {
				char *new_prefix = NULL;
				int res = asprintf(&new_prefix, "%s.%s", prefix,
						   key);
				if (res != -1 && new_prefix) {
					print_json_flat(new_prefix, val,
							depth + 1);
					free(new_prefix);
				}
			}
			json_object_iter_next(&it);
		}
		return;
	}

	val_str = json_value_to_str(obj, tmp, sizeof(tmp));
	pr_info("%s = %s", prefix, val_str);
}

void safe_print_resmsg(json_object *resmsg_obj)
{
	enum json_type t;

	if (!resmsg_obj) {
		pr_info("resmsg = [null]");
		return;
	}

	t = json_object_get_type(resmsg_obj);
	if (t == json_type_object || t == json_type_array)
		print_json_flat("resmsg", resmsg_obj, 0);
	else {
		const char *s = json_object_to_json_string_ext(
			resmsg_obj, JSON_C_TO_STRING_PLAIN);
		pr_info("resmsg = %s", s ? s : "[invalid]");
	}
}

char *build_json_request(const char *method, const char *params)
{
	json_object *root;
	const char *json_str;
	char *result;
	json_object *data_obj;

	root = json_object_new_object();
	if (!root)
		return NULL;

	// jsonrpc version
	json_object_object_add(root, "jsonrpc", json_object_new_string("6.0"));

	json_object_object_add(root, "type",
			       json_object_new_int(CTFORGED_TYPE_CMD));

	json_object_object_add(root, "taskid",
			       json_object_new_int(gen_timestamp()));

	json_object_object_add(root, "method", json_object_new_string(method));

	// reqmsg: { "params": "abc def ghi" }
	data_obj = json_object_new_object();
	json_object_object_add(data_obj, "params",
			       json_object_new_string(params ? params : ""));
	json_object_object_add(root, "reqmsg", data_obj);

	// serialize
	json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
	result = strdup(json_str);
	json_object_put(root);
	return result;
}

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size,
			 uv_buf_t *buf)
{
	buf->base = malloc(suggested_size);
	buf->len = buf->base ? suggested_size : 0;
}

static void on_write(uv_write_t *req, int status)
{
	free(req->data);
	free(req);
	if (status < 0) {
		fprintf(stderr, "Write error: %s\n", uv_err_name(status));
		uv_stop(loop);
	}
}

static void handle_server_response(json_object *root)
{
	json_object *data_obj;
	const char *data_val = NULL;
	json_object *resmsg_obj = NULL;
	enum json_type type;

	if (root == NULL) {
		pr_err("root is NULL");
		return;
	}

	if (!json_object_is_type(root, json_type_object)) {
		pr_err("root is not a JSON object");
		return;
	}

	if (!json_object_object_get_ex(root, "resmsg", &resmsg_obj)) {
		pr_info("no 'resmsg' field in server response");
		return;
	}

	if (resmsg_obj == NULL) {
		pr_info("'resmsg' field exists but is null");
		return;
	}

	type = json_object_get_type(resmsg_obj);
	switch (type) {
	case json_type_object:
		safe_print_resmsg(resmsg_obj);
		break;

	case json_type_array:
		safe_print_resmsg(resmsg_obj);
		break;

	case json_type_string:
		pr_info("resmsg (string) = %s",
			json_object_get_string(resmsg_obj));
		break;

	case json_type_int:
		pr_info("resmsg (int) = %d", json_object_get_int(resmsg_obj));
		break;

	case json_type_boolean:
		pr_info("resmsg (bool) = %s",
			json_object_get_boolean(resmsg_obj) ? "true" : "false");
		break;

	case json_type_double:
		pr_info("resmsg (double) = %g",
			json_object_get_double(resmsg_obj));
		break;

	case json_type_null:
		pr_info("resmsg is null");
		break;

	default:
		pr_err("resmsg has unsupported type: %d", (int)type);
		break;
	}
}

json_object *parse_json_string(const char *json_str)
{
	if (!json_str) {
		fprintf(stderr, "Input string is NULL\n");
		return NULL;
	}

	struct json_tokener *tok = json_tokener_new();
	enum json_tokener_error jerr = json_tokener_success;
	json_object *obj = json_tokener_parse_ex(tok, json_str, -1);

	jerr = json_tokener_get_error(tok);
	if (jerr != json_tokener_success) {
		fprintf(stderr, "JSON parse error at offset %ld: %s\n",
			(long)json_tokener_get_parse_end(tok),
			json_tokener_error_desc(jerr));
		obj = NULL;
	}

	json_tokener_free(tok);
	return obj;
}

static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
	json_object *root;

	if (nread > 0) {
		char *line_start = buf->base;
		char *end = buf->base + nread;
		char *newline;

		// pr_dbg("receive server raw respose %s", line_start);

		while ((newline = memchr(line_start, '\n', end - line_start))) {
			*newline = '\0';
			root = json_tokener_parse(line_start);
			if (!root) {
				/* if not json format, must be error, just print */
				pr_err("%s", line_start);
				line_start = newline + 1;
				continue;
			}

			handle_server_response(root);

			json_object_put(root);
			line_start = newline + 1;
		}
	} else if (nread < 0) {
		if (nread != UV_EOF)
			fprintf(stderr, "Read error: %s\n", uv_err_name(nread));
		uv_close((uv_handle_t *)stream, NULL);
		uv_stop(loop);
	}

	free(buf->base);
}
static void on_connect(uv_connect_t *req, int status)
{
	uv_stream_t *stream;
	char *method = NULL;
	char *params_str = NULL;
	char *json_req = NULL;
	char *line_req = NULL;
	size_t json_len;
	int i;
	uv_buf_t buf;
	uv_write_t *wr;
	size_t params_len;

	if (status < 0) {
		fprintf(stderr, "Connect error: %s\n", uv_strerror(status));
		uv_stop(loop);
		return;
	}

	stream = is_unix ? (uv_stream_t *)&ctforged_sock_client :
				 (uv_stream_t *)&ctforged_tcp_client;

	// Method is argv[1]
	if (argc_global < 2) {
		fprintf(stderr, "No method specified\n");
		uv_stop(loop);
		return;
	}
	global_method = argv_global[1];

	// Build params string from argv[2] onward
	if (argc_global > 2) {
		params_len = 1;
		for (i = 2; i < argc_global; i++) {
			if (i > 2)
				params_len++; // space
			params_len += strlen(argv_global[i]);
		}
		params_str = malloc(params_len);
		if (!params_str) {
			fprintf(stderr, "Failed to allocate params buffer\n");
			uv_stop(loop);
			return;
		}
		params_str[0] = '\0';
		for (i = 2; i < argc_global; i++) {
			if (i > 2)
				strcat(params_str, " ");
			strcat(params_str, argv_global[i]);
		}
	} else {
		params_str = strdup(""); // no params
	}

	// Build JSON request
	json_req = build_json_request(global_method, params_str);
	free(params_str);
	if (!json_req) {
		fprintf(stderr, "Failed to build JSON request\n");
		uv_stop(loop);
		return;
	}

	json_len = strlen(json_req);
	line_req = malloc(json_len + 2);
	if (!line_req) {
		free(json_req);
		fprintf(stderr, "Out of memory\n");
		uv_stop(loop);
		return;
	}
	memcpy(line_req, json_req, json_len);
	line_req[json_len] = '\n';
	line_req[json_len + 1] = '\0';
	free(json_req);

	buf = uv_buf_init(line_req, json_len + 1);
	wr = malloc(sizeof(*wr));
	if (!wr) {
		free(line_req);
		fprintf(stderr, "Failed to allocate write request\n");
		uv_stop(loop);
		return;
	}
	wr->data = line_req;
	uv_write(wr, stream, &buf, 1, on_write);
	uv_read_start(stream, alloc_buffer, on_read);
}

static void show_help(void)
{
	printf("Usage: ctforgectl <OPS>\n");
	printf("Example: ctforgectl CMD ARG1 ARG2\n");
	printf("  ctforgectl lsmod              # show all loaded module info\n");
	printf("  ctforgectl insmod MODNAME     # load module MODNAME\n");
	printf("  ctforgectl rmmod MODNAME      # unload module MODNAME\n");
	printf("  ctforgectl mod MODNAME SUBCMD # exe submodule cmd\n");
	printf("  ctforgectl status             # get ctforged status\n");
}

int main(int argc, char *argv[])
{
	struct sockaddr_in dest;
	uv_connect_t connect_req;
	int ret;
	char host[256];
	int port;

	if (argc < 2) {
		show_help();
		return 1;
	}

	argc_global = argc;
	argv_global = argv;

	loop = uv_default_loop();

	ret = ctforged_init_config(CONFIG_PATH, &ctforged_config);
	if (ret != 0) {
		pr_err("Failed to load config from %s ret=[%d]\n", CONFIG_PATH,
		       ret);
		return 1;
	}

	if (strncmp(ctforged_config.listen, "unix://", 7) == 0) {
		is_unix = 1;
		uv_pipe_init(loop, &ctforged_sock_client, 0);
		uv_pipe_connect(&connect_req, &ctforged_sock_client,
				ctforged_config.listen + 7, on_connect);
		uv_run(loop, UV_RUN_DEFAULT);
		return 0;
	}

	is_unix = 0;
	if (sscanf(ctforged_config.listen, "%255[^:]:%d", host, &port) != 2) {
		pr_err("Invalid address [%s]\n", ctforged_config.listen);
		return 1;
	}
	uv_tcp_init(loop, &ctforged_tcp_client);
	uv_ip4_addr(host, port, &dest);
	uv_tcp_connect(&connect_req, &ctforged_tcp_client,
		       (const struct sockaddr *)&dest, on_connect);
	uv_run(loop, UV_RUN_DEFAULT);
	return 0;
}
