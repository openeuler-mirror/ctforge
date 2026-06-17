#include "ctforge.h"

struct ctforged_ringbuf {
	struct ctforge_conn_ctx *ctx;
	json_object *result;
};

/**
 * @brief Cleanup client context on connection close.
 */
static void on_client_close(uv_handle_t *handle)
{
	struct ctforge_conn_ctx *ctx = (struct ctforge_conn_ctx *)handle->data;

	if (!ctx)
		return;

	free(ctx->recv_buffer);
	free(ctx->client_msg);
	free(ctx);

	pr_info("Client connection closed and context freed.");
}

/**
 * @brief Initiate graceful client closure.
 */
static void request_close_client(struct ctforge_conn_ctx *ctx)
{
	if (!ctx)
		return;

	uv_stream_t *stream = (uv_stream_t *)&ctx->stream_handle;

	if (uv_is_active((uv_handle_t *)stream))
		uv_close((uv_handle_t *)stream, on_client_close);
}

/**
 * @brief Simple write callback that frees the message buffer and request.
 */
static void on_write_end(uv_write_t *req, int status)
{
	free(req->data);
	free(req);

	if (status < 0)
		pr_err("Write error: %s", uv_strerror(status));
}

/**
 * @brief Send a null-terminated message to the client with a trailing newline.
 * This function must be called from the libuv loop thread.
 *
 * @param ctx Client context.
 * @param msg Message to send (must be valid and non-empty).
 * @return 0 on success, -1 on failure.
 */
static int send_message_to_client(struct ctforge_conn_ctx *ctx, const char *msg)
{
	if (!ctx || !msg)
		return -1;

	uv_stream_t *stream = (uv_stream_t *)&ctx->stream_handle;

	if (!uv_is_writable(stream)) {
		pr_dbg("Stream is not writable");
		return -1;
	}

	if (strlen(msg) == 0)
		return 0;

	char *final_msg = NULL;
	int ret = asprintf(&final_msg, "%s\n", msg);

	if (ret < 0) {
		pr_err("asprintf failed to allocate final message");
		return -1;
	}

	uv_buf_t buf = uv_buf_init(final_msg, strlen(final_msg));
	uv_write_t *write_req = (uv_write_t *)malloc(sizeof(uv_write_t));

	if (!write_req) {
		free(final_msg);
		return -1;
	}

	write_req->data = final_msg;

	int r = uv_write(write_req, stream, &buf, 1, on_write_end);

	if (r < 0) {
		free(final_msg);
		free(write_req);
		pr_err("uv_write failed: %s", uv_strerror(r));
		return -1;
	}

	pr_dbg("Sent to client: [%s]", final_msg);
	return 0;
}

/**
 * @brief Send raw string response to client.
 */
static void make_raw_res(struct ctforge_conn_ctx *ctx, const char *raw_msg)
{
	if (!ctx || !raw_msg)
		return;

	if (send_message_to_client(ctx, raw_msg) != 0)
		pr_err("Failed to send raw response");
}

/**
 * @brief Construct and send a JSON-formatted response.
 */
static void make_json_res(struct ctforge_conn_ctx *ctx, int taskid, int type,
			  const char *method, const char *msg)
{
	if (!ctx || !method || !msg)
		return;

	json_object *root = json_object_new_object();

	if (!root) {
		make_raw_res(ctx, "Failed to create JSON root object");
		return;
	}

	json_object_object_add(root, "type", json_object_new_int(type));
	json_object_object_add(root, "taskid", json_object_new_int(taskid));
	json_object_object_add(root, "message", json_object_new_string(msg));

	const char *json_str =
		json_object_to_json_string_ext(root, JSON_C_TO_STRING_SPACED);

	if (!json_str) {
		json_object_put(root);
		make_raw_res(ctx, "Failed to serialize JSON");
		return;
	}

	char *dup_msg = strdup(json_str);

	json_object_put(root);
	if (!dup_msg) {
		make_raw_res(ctx, "strdup failed: out of memory");
		return;
	}

	if (send_message_to_client(ctx, dup_msg) != 0)
		pr_err("Failed to send JSON response");

	free(dup_msg);
}

/**
 * @brief Send an error message as raw text.
 */
static void make_err_res(struct ctforge_conn_ctx *ctx, const char *err_msg)
{
	make_raw_res(ctx, err_msg);
}

/**
 * @brief Handle ring buffer events and forward messages to client.
 */
static int handle_log_event(void *ctx, void *data, size_t data_sz)
{
	struct ctforged_ringbuf *r = (struct ctforged_ringbuf *)ctx;

	if (data_sz != sizeof(struct ebpf_log)) {
		pr_err("Invalid message size: %zu", data_sz);
		return -1;
	}
	struct ebpf_log *msg = (struct ebpf_log *)data;

	// new json object
	json_object *newres_obj = json_object_new_object();

	json_object_object_add(newres_obj, "msg",
			       json_object_new_string(msg->msg));
	// replase resmsg in result
	json_object_object_add(r->result, "resmsg", newres_obj);
	// format string

	const char *resp_str = json_object_to_json_string_ext(
		r->result, JSON_C_TO_STRING_PLAIN);

	if (resp_str)
		send_message_to_client(r->ctx, resp_str);
	else
		make_err_res(r->ctx, "Failed to serialize response");
	return 0;
}

/**
 * @brief Handle lost ring buffer events.
 */
static void handle_watch_lost_events(void *ctx, int cpu, __u64 lost_cnt)
{
	char msg[128];

	int len = snprintf(msg, sizeof(msg), "Lost %llu events on CPU %d\n",
			   lost_cnt, cpu);
	if (len > 0 && (size_t)len < sizeof(msg))
		make_raw_res((struct ctforge_conn_ctx *)ctx, msg);
}

/**
 * @brief Enable ring buffer watching if 'log' field is set in result.
 */
void handle_log(struct ctforge_conn_ctx *ctx, json_object *result)
{
	if (!ctx || !result || !json_object_is_type(result, json_type_object))
		return;

	struct json_object *resmsg_obj = NULL;

	if (!json_object_object_get_ex(result, "resmsg", &resmsg_obj) ||
	    !json_object_is_type(resmsg_obj, json_type_object))
		return;

	json_object *watch_obj = NULL;

	if (!json_object_object_get_ex(resmsg_obj, "log", &watch_obj) ||
	    !json_object_is_type(watch_obj, json_type_int))
		return;

	int watch_val = json_object_get_int(watch_obj);

	if (watch_val != 1)
		return;

	json_object *logmap_obj = NULL;
	int logmap_fd = -1;

	if (json_object_object_get_ex(resmsg_obj, "logmap", &logmap_obj) &&
	    json_object_is_type(logmap_obj, json_type_int)) {
		logmap_fd = json_object_get_int(logmap_obj);
		pr_info("log enabled with logmap_fd = %d", logmap_fd);
	} else {
		pr_info("log enabled (no logmap_fd provided)");
	}

	pr_dbg("find logmap fd=%d", logmap_fd);

	struct ctforged_ringbuf r = {
		.ctx = ctx,
		.result = result,
	};
	struct ring_buffer *rb =
		ring_buffer__new(logmap_fd, handle_log_event, &r, NULL);

	if (!rb) {
		make_err_res(ctx, "Failed to create ring buffer");
		return;
	}

	json_object *newres_obj = json_object_new_object();
	int consumed = ring_buffer__consume(rb);

	if (consumed < 0) {
		char msg[64];

		snprintf(msg, sizeof(msg), "Consume failed: %d", consumed);

		json_object_object_add(newres_obj, "msg",
				       json_object_new_string(msg));
		json_object_object_add(result, "resmsg", newres_obj);

	} else {
		char msg[64];

		snprintf(msg, sizeof(msg), "Consumed %d events", consumed);

		json_object_object_add(newres_obj, "msg",
				       json_object_new_string(msg));
		json_object_object_add(result, "resmsg", newres_obj);
	}

	const char *resp_str =
		json_object_to_json_string_ext(result, JSON_C_TO_STRING_PLAIN);

	if (result)
		send_message_to_client(ctx, resp_str);
	else
		make_err_res(ctx, "Failed to serialize response");

	ring_buffer__free(rb);
}

/**
 * @brief Process a single JSON-RPC request.
 */
static int do_one_hard_work(struct ctforge_conn_ctx *ctx, json_object *req)
{
	json_object *taskid_obj = NULL;
	json_object *type_obj = NULL;
	json_object *method_obj = NULL;
	json_object *data_obj = NULL;
	const char *method_str = NULL;
	json_object *ops_fn_result = NULL;
	json_object *resp = NULL;
	int ret = 0;

	pr_dbg("Processing JSON-RPC request");

	// Validate JSON-RPC version
	if (!json_object_object_get_ex(req, "jsonrpc", NULL)) {
		make_err_res(ctx, "Invalid Request: missing jsonrpc field");
		return -1;
	}

	// Extract method
	if (!json_object_object_get_ex(req, "method", &method_obj) ||
	    !json_object_is_type(method_obj, json_type_string)) {
		make_err_res(ctx, "Invalid Request: invalid or missing method");
		return -1;
	}
	method_str = json_object_get_string(method_obj);

	// Optional fields
	json_object_object_get_ex(req, "taskid", &taskid_obj);
	json_object_object_get_ex(req, "type", &type_obj);
	json_object_object_get_ex(req, "reqmsg", &data_obj);

	// Find handler
	void *user_data;
	jsonrpc_method_fn ops_fn = find_method(method_str, &user_data);

	if (!ops_fn) {
		make_err_res(ctx, "Invalid argument, Method not found");
		goto error;
	}

	// Build response
	resp = json_object_new_object();
	if (!resp) {
		make_err_res(ctx, "Failed to allocate response object");
		goto error;
	}

	pr_dbg("Executing method: %s", method_str);
	ret = ops_fn(data_obj, &ops_fn_result, user_data);
	if (ret != 0) {
		json_object_object_add(resp, "jsonrpc",
				       json_object_new_string("2.0"));
		if (type_obj)
			json_object_object_add(resp, "type",
					       json_object_get(type_obj));
		if (taskid_obj)
			json_object_object_add(resp, "taskid",
					       json_object_get(taskid_obj));
		json_object_object_add(resp, "method",
				       json_object_new_string(method_str));

		json_object_object_add(resp, "resmsg",
				       ops_fn_result ? ops_fn_result :
							     json_object_new_null());
		const char *resp_str = json_object_to_json_string_ext(
			resp, JSON_C_TO_STRING_PLAIN);

		if (!resp_str)
			make_err_res(ctx, "Failed to serialize response");

		send_message_to_client(ctx, resp_str);

		goto error;
	}

	json_object_object_add(resp, "jsonrpc", json_object_new_string("2.0"));
	if (type_obj)
		json_object_object_add(resp, "type", json_object_get(type_obj));
	if (taskid_obj)
		json_object_object_add(resp, "taskid",
				       json_object_get(taskid_obj));
	json_object_object_add(resp, "method",
			       json_object_new_string(method_str));

	// Transfer ownership of ops_fn_result to resp
	json_object_object_add(resp, "resmsg",
			       ops_fn_result ? ops_fn_result :
						     json_object_new_null());
	ops_fn_result = NULL; // Clear to avoid accidental double-free

	// beautify_json(resp);
	const char *resp_str =
		json_object_to_json_string_ext(resp, JSON_C_TO_STRING_PLAIN);

	if (resp_str)
		send_message_to_client(ctx, resp_str);
	else
		make_err_res(ctx, "Failed to serialize response");

	handle_log(ctx, resp);

	pr_dbg("done handle_log");

	json_object_put(resp);
	return 0;

error:
	if (ops_fn_result)
		json_object_put(ops_fn_result);

	if (resp)
		json_object_put(resp);

	return -1;
}

/**
 * @brief Synchronously process a client request (called from main loop thread).
 */
static void do_per_request(struct ctforge_conn_ctx *ctx, const char *client_msg)
{
	if (!client_msg || client_msg[0] == '\0')
		return;

	size_t len = strlen(client_msg);

	if (len == 0 || len > 1024) {
		pr_err("Invalid message length: %zu", len);
		return;
	}

	pr_dbg("Received client message: [%s]", client_msg);

	json_object *jreq = json_tokener_parse(client_msg);

	if (!jreq) {
		send_message_to_client(ctx, "Parse error: invalid JSON");
		return;
	}

	if (json_object_is_type(jreq, json_type_object)) {
		do_one_hard_work(ctx, jreq);
	} else {
		send_message_to_client(ctx,
				       "Invalid Request: expected JSON object");
	}

	json_object_put(jreq);
	request_close_client(ctx);
}

/**
 * @brief Callback invoked when data is read from client.
 */
static void on_client_read(uv_stream_t *stream, ssize_t nread,
			   const uv_buf_t *buf)
{
	struct ctforge_conn_ctx *ctx = (struct ctforge_conn_ctx *)stream->data;

	if (!ctx) {
		free(buf->base);
		return;
	}

	if (nread <= 0) {
		free(buf->base);

		if (nread == UV_EOF) {
			pr_info("Client gracefully disconnected");
		} else if (nread < 0) {
			pr_err("Client connection error: %s",
			       uv_strerror((int)nread));
		}

		request_close_client(ctx);
		return;
	}

	// Append new data to receive buffer
	char *new_buf =
		(char *)realloc(ctx->recv_buffer, ctx->recv_buffer_len + nread);
	if (!new_buf) {
		pr_err("Failed to realloc receive buffer");
		free(buf->base);
		request_close_client(ctx);
		return;
	}
	ctx->recv_buffer = new_buf;
	memcpy(ctx->recv_buffer + ctx->recv_buffer_len, buf->base, nread);
	ctx->recv_buffer_len += nread;
	free(buf->base);

	// Process complete lines (newline-delimited)
	char *start = ctx->recv_buffer;
	char *end;

	while ((end = memchr(start, '\n',
			     ctx->recv_buffer_len -
				     (start - ctx->recv_buffer)))) {
		*end = '\0';
		do_per_request(ctx, start);
		start = end + 1;
	}

	// Keep unprocessed partial line
	size_t remaining = ctx->recv_buffer_len - (start - ctx->recv_buffer);

	if (remaining > 0) {
		memmove(ctx->recv_buffer, start, remaining);
		ctx->recv_buffer_len = remaining;
	} else {
		free(ctx->recv_buffer);
		ctx->recv_buffer = NULL;
		ctx->recv_buffer_len = 0;
	}
}

/**
 * @brief Allocate read buffer for libuv.
 */
static void alloc_buffer(uv_handle_t *handle, size_t suggested_size,
			 uv_buf_t *buf)
{
	buf->base = (char *)malloc(suggested_size);
	buf->len = suggested_size;
}

/**
 * @brief Accept new client connection.
 */
void on_new_connection(uv_stream_t *server, int status)
{
	if (status < 0) {
		pr_err("Accept error: %s", uv_strerror(status));
		return;
	}

	struct ctforge_conn_ctx *ctx = (struct ctforge_conn_ctx *)calloc(
		1, sizeof(struct ctforge_conn_ctx));
	if (!ctx) {
		pr_err("Failed to allocate client context");
		return;
	}

	ctx->recv_buffer = NULL;
	ctx->recv_buffer_len = 0;
	ctx->client_msg = NULL;

	int ret;

	if (server->type == UV_TCP) {
		uv_tcp_init(ctforged_uv_loop, &ctx->stream_handle.tcp);
		ctx->stream_handle.tcp.data = ctx;
		ret = uv_accept(server, (uv_stream_t *)&ctx->stream_handle.tcp);
	} else if (server->type == UV_NAMED_PIPE) {
		uv_pipe_init(ctforged_uv_loop, &ctx->stream_handle.pipe, 0);
		ctx->stream_handle.pipe.data = ctx;
		ret = uv_accept(server,
				(uv_stream_t *)&ctx->stream_handle.pipe);
	} else {
		free(ctx);
		pr_err("Unsupported server stream type");
		return;
	}

	if (ret != 0) {
		free(ctx);
		pr_err("uv_accept failed: %s", uv_strerror(ret));
		return;
	}

	uv_read_start((uv_stream_t *)&ctx->stream_handle, alloc_buffer,
		      on_client_read);
	pr_dbg("New client connected. Awaiting messages.");
}

/**
 * @brief Start the server based on configuration.
 */
int start_uv_server(struct ctforge_config *ctforged_config)
{
	const char *listen_addr = ctforged_config->listen;
	int r;

	if (strncmp(listen_addr, "unix://", 7) == 0) {
		const char *path = listen_addr + 7;

		uv_pipe_init(ctforged_uv_loop, &ctforged_unix_server, 0);
		r = uv_pipe_bind(&ctforged_unix_server, path);
		if (r == 0) {
			r = uv_listen((uv_stream_t *)&ctforged_unix_server, 128,
				      on_new_connection);
		}
		if (r != 0) {
			pr_err("Unix socket listen error: %s", uv_strerror(r));
			return -1;
		}
		register_cleanup(ctforged_clean_sock, "clean_unix_socket");
		pr_info("Listening on Unix socket: %s", path);
	} else {
		char *addr_copy = strdup(listen_addr);

		if (!addr_copy)
			return -1;

		char *colon = strrchr(addr_copy, ':');

		if (!colon) {
			free(addr_copy);
			return -1;
		}
		*colon = '\0';
		int port = atoi(colon + 1);

		if (port <= 0 || port > 65535) {
			free(addr_copy);
			return -1;
		}

		struct sockaddr_in addr;

		r = uv_ip4_addr(addr_copy, port, &addr);
		if (r == 0) {
			uv_tcp_init(ctforged_uv_loop, &ctforged_tcp_server);
			r = uv_tcp_bind(&ctforged_tcp_server,
					(const struct sockaddr *)&addr, 0);
			if (r == 0) {
				r = uv_listen(
					(uv_stream_t *)&ctforged_tcp_server,
					128, on_new_connection);
			}
		}
		free(addr_copy);
		if (r != 0) {
			pr_err("TCP listen error: %s", uv_strerror(r));
			return -1;
		}
		pr_info("Listening on TCP %s:%d", listen_addr, port);
	}

	return uv_run(ctforged_uv_loop, UV_RUN_DEFAULT);
}
