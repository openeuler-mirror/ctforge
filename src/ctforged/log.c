#include "ctforge.h"

extern struct ctforge_config ctforged_config;

pthread_mutex_t ctforged_output_lock;

/* ctforged kernel log */
static int ctforged_klog_fd;
static pthread_mutex_t ctforged_klog_lock;
sig_atomic_t ctforged_klog_enabled;

/* ctforged service log */
static int ctforged_slog_fd;
static pthread_mutex_t ctforged_slog_lock;
sig_atomic_t ctforged_slog_enabled;

/* ctforged log level */
const char *loglevel[] = {
	[0] = "EMERG", [1] = "ALERT", [2] = "CRIT", [3] = "ERROR",
	[4] = "WARN",  [5] = "NOTI",  [6] = "INFO", [7] = "DEBUG",
};

#define MAX_LOG_LEVEL 8

void printu(int level, char *fmt, ...)
{
	time_t now;
	struct tm tm_info;
	char time_buffer[32]; /* slightly larger for safety */
	char *msg_body = NULL;
	char *full_line = NULL;
	va_list args;
	const char *lvl_str;

	/* Validate log level */
	if (level < 0 || level >= MAX_LOG_LEVEL)
		lvl_str = "INVALID";
	else
		lvl_str = loglevel[level];

	/* Validate format string */
	if (!fmt)
		fmt = "(null format string)";

	/* Get current time safely (reentrant) */
	now = time(NULL);
	if (localtime_r(&now, &tm_info) == NULL) {
		/* Fallback to epoch time if conversion fails */
		snprintf(time_buffer, sizeof(time_buffer),
			 "1970-01-01 00:00:00");
	} else {
		strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S",
			 &tm_info);
	}

	/* Step 1: Format the user-provided message */
	va_start(args, fmt);
	if (vasprintf(&msg_body, fmt, args) < 0)
		msg_body = NULL;

	va_end(args);

	if (!msg_body) {
		/* Use static fallback to avoid further allocation */
		pthread_mutex_lock(&ctforged_output_lock);
		fprintf(stderr, "%s [%s]: (failed to format log message)\n",
			time_buffer, lvl_str);
		pthread_mutex_unlock(&ctforged_output_lock);
		return;
	}

	/* Step 2: Build full log line with timestamp and level */
	if (asprintf(&full_line, "%s [%s]: %s", time_buffer, lvl_str,
		     msg_body) < 0) {
		full_line = NULL;
	}
	free(msg_body); /* safe even if msg_body was null (but it isn't here) */

	if (!full_line) {
		pthread_mutex_lock(&ctforged_output_lock);
		fprintf(stderr, "%s [%s]: (failed to build full log line)\n",
			time_buffer, lvl_str);
		pthread_mutex_unlock(&ctforged_output_lock);
		return;
	}

	/* Output to stderr under lock */
	pthread_mutex_lock(&ctforged_output_lock);
	fprintf(stderr, "%s\n", full_line);
	pthread_mutex_unlock(&ctforged_output_lock);

	/* Output to service log file (thread-safe internally) */
	if (ctforged_slog_enabled)
		ctforged_slog_to_file(full_line);

	free(full_line);
}

void ctforged_klog_to_file(char *info)
{
	int len;
	ssize_t rc;

	if (!info) {
		/* Log error to stderr as fallback */
		pthread_mutex_lock(&ctforged_output_lock);
		pr_err("klog: null message pointer\n");
		pthread_mutex_unlock(&ctforged_output_lock);
		return;
	}

	len = strlen(info);
	if (len == 0)
		return;

	pthread_mutex_lock(&ctforged_klog_lock);

	/* Guard against invalid fd */
	if (ctforged_klog_fd < 0) {
		pthread_mutex_unlock(&ctforged_klog_lock);
		pthread_mutex_lock(&ctforged_output_lock);
		pr_err("klog: log file descriptor closed\n");
		pthread_mutex_unlock(&ctforged_output_lock);
		return;
	}

	/* Write message body */
	while (len > 0) {
		rc = write(ctforged_klog_fd, info, len);
		if (rc < 0) {
			if (errno == EINTR) {
				/* Interrupted by signal, retry */
				continue;
			}
			/* Log write error to stderr */
			pthread_mutex_unlock(&ctforged_klog_lock);
			pthread_mutex_lock(&ctforged_output_lock);
			pr_err("klog: write failed: %s\n", strerror(errno));
			pthread_mutex_unlock(&ctforged_output_lock);
			return;
		}
		info += rc;
		len -= rc;
	}

	/* Write newline */
	rc = write(ctforged_klog_fd, "\n", 1);
	if (rc < 0) {
		if (errno != EINTR) {
			pthread_mutex_unlock(&ctforged_klog_lock);
			pthread_mutex_lock(&ctforged_output_lock);
			pr_err("klog: failed to write newline: %s\n",
			       strerror(errno));
			pthread_mutex_unlock(&ctforged_output_lock);
			return;
		}
		/* If EINTR on newline, we still proceed to fsync (best effort) */
	}

	/* Best-effort fsync; ignore failure to avoid crashing */
	(void)fsync(ctforged_klog_fd);

	pthread_mutex_unlock(&ctforged_klog_lock);
}

void ctforged_slog_to_file(char *info)
{
	int len;
	ssize_t rc;

	if (!info) {
		pthread_mutex_lock(&ctforged_output_lock);
		pr_err("slog: null message pointer\n");
		pthread_mutex_unlock(&ctforged_output_lock);
		return;
	}

	len = strlen(info);
	if (len == 0)
		return;

	pthread_mutex_lock(&ctforged_slog_lock);

	if (ctforged_slog_fd < 0) {
		pthread_mutex_unlock(&ctforged_slog_lock);
		pthread_mutex_lock(&ctforged_output_lock);
		pr_err("slog: log file descriptor closed\n");
		pthread_mutex_unlock(&ctforged_output_lock);
		return;
	}

	while (len > 0) {
		rc = write(ctforged_slog_fd, info, len);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			pthread_mutex_unlock(&ctforged_slog_lock);
			pthread_mutex_lock(&ctforged_output_lock);
			pr_err("slog: write failed: %s\n", strerror(errno));
			pthread_mutex_unlock(&ctforged_output_lock);
			return;
		}
		info += rc;
		len -= rc;
	}

	rc = write(ctforged_slog_fd, "\n", 1);
	if (rc < 0) {
		if (errno != EINTR) {
			pthread_mutex_unlock(&ctforged_slog_lock);
			pthread_mutex_lock(&ctforged_output_lock);
			pr_err("slog: failed to write newline: %s\n",
			       strerror(errno));
			pthread_mutex_unlock(&ctforged_output_lock);
			return;
		}
	}

	(void)fsync(ctforged_slog_fd);

	pthread_mutex_unlock(&ctforged_slog_lock);
}

int ctforged_log_init(void)
{
	char klog_path[PATH_MAX];
	char slog_path[PATH_MAX];
	struct stat st;
	int saved_errno;

	ctforged_slog_enabled = 0;

	if (!ctforged_config.log_dir || strlen(ctforged_config.log_dir) == 0) {
		pr_err("log_dir is null or empty");
		return -1;
	}

	size_t dir_len = strlen(ctforged_config.log_dir);

	if (dir_len >= PATH_MAX - sizeof(CTFORGED_KLOG_FILENAME) ||
	    dir_len >= PATH_MAX - sizeof(CTFORGED_SLOG_FILENAME)) {
		pr_err("log_dir too long");
		return -1;
	}

	const char *dir = ctforged_config.log_dir;

	if (dir[dir_len - 1] == '/') {
		snprintf(klog_path, sizeof(klog_path), "%s%s", dir,
			 CTFORGED_KLOG_FILENAME);
		snprintf(slog_path, sizeof(slog_path), "%s%s", dir,
			 CTFORGED_SLOG_FILENAME);
	} else {
		snprintf(klog_path, sizeof(klog_path), "%s/%s", dir,
			 CTFORGED_KLOG_FILENAME);
		snprintf(slog_path, sizeof(slog_path), "%s/%s", dir,
			 CTFORGED_SLOG_FILENAME);
	}

	if (stat(ctforged_config.log_dir, &st) != 0) {
		if (errno == ENOENT) {
			if (mkdir(ctforged_config.log_dir, 0755) != 0) {
				saved_errno = errno;
				pr_err("Failed to create log directory %s: %s",
				       ctforged_config.log_dir,
				       strerror(saved_errno));
				return -saved_errno;
			}
			if (stat(ctforged_config.log_dir, &st) != 0) {
				saved_errno = errno;
				pr_err("stat failed after mkdir %s: %s",
				       ctforged_config.log_dir,
				       strerror(saved_errno));
				return -saved_errno;
			}
		} else {
			saved_errno = errno;
			pr_err("Cannot stat log directory %s: %s",
			       ctforged_config.log_dir, strerror(saved_errno));
			return -saved_errno;
		}
	}

	if (!S_ISDIR(st.st_mode)) {
		pr_err("log_dir %s is not a directory",
		       ctforged_config.log_dir);
		return -1; /* Not an errno case */
	}

	if (access(ctforged_config.log_dir, W_OK) != 0) {
		saved_errno = errno;
		pr_err("log_dir %s is not writable: %s",
		       ctforged_config.log_dir, strerror(saved_errno));
		return -saved_errno;
	}

	/* Initialize service log */
	ctforged_slog_fd = open(slog_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
	if (ctforged_slog_fd < 0) {
		saved_errno = errno;
		pr_err("Failed to open/create service log file %s: %s",
		       slog_path, strerror(saved_errno));
		goto fail;
	}

	/* Skip write("", 0) — it's always successful */

	if (pthread_mutex_init(&ctforged_slog_lock, NULL) != 0) {
		saved_errno = errno;
		pr_err("Service log mutex init failed: %s",
		       strerror(saved_errno));
		goto fail;
	}
	pr_info("service log [%s]", slog_path);

	/* Initialize kernel log */
	ctforged_klog_fd = open(klog_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
	if (ctforged_klog_fd < 0) {
		saved_errno = errno;
		pr_err("Failed to open/create kernel log file %s: %s",
		       klog_path, strerror(saved_errno));
		goto fail;
	}

	if (pthread_mutex_init(&ctforged_klog_lock, NULL) != 0) {
		saved_errno = errno;
		pr_err("Kernel log mutex init failed: %s",
		       strerror(saved_errno));
		goto fail;
	}
	pr_info("kernel log [%s]", klog_path);

	// enable service log write to file
	ctforged_slog_enabled = 1;

	return 0;
fail:
	saved_errno = errno;
	if (ctforged_slog_fd >= 0) {
		close(ctforged_slog_fd);
		ctforged_slog_fd = -1;
	}
	if (ctforged_klog_fd >= 0) {
		close(ctforged_klog_fd);
		ctforged_klog_fd = -1;
	}
	return -saved_errno;
}
