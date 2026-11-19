#include "ctforge.h"
#include <limits.h>
#include <linux/bpf.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>

int list_empty_careful(const struct list_head *head)
{
	struct list_head *next = head->next;

	return (next == head) && (next == head->prev);
}

int update_rlimit(void)
{
	int err = 0;
	struct rlimit r = { RLIM_INFINITY, RLIM_INFINITY };

	err = setrlimit(RLIMIT_MEMLOCK, &r);
	if (err) {
		pr_err("Error while setting rlimit %s", strerror(errno));
		return err;
	}
	return err;
}

void print_arg(int argc, char *argv[])
{
	for (int i = 0; i < argc; ++i)
		pr_debug("param %d: %s", i, argv[i]);
}

bool is_ebpf_file(const char *filename)
{
	FILE *file = fopen(filename, "rb");

	if (!file) {
		pr_err("Unable to open file %s", filename);
		return false;
	}

	if (fseek(file, 18, SEEK_SET) != 0) {
		pr_err("File %s fseek failed", filename);
		fclose(file);
		return false;
	}

	uint16_t magic;

	if (fread(&magic, sizeof(magic), 1, file) != 1) {
		pr_err("File %s fread failed", filename);
		fclose(file);
		return false;
	}

	fclose(file);

	// 247 is eBPF filetype magic
	return magic == 0xf7;
}

void hexdump(const void *data, size_t size)
{
	const unsigned char *byte = data;
	int offset = 0;

	for (size_t i = 0; i < size; i += 16) {
		printf("%06x  ", offset);

		for (size_t j = 0; j < 16; j++) {
			if (i + j < size)
				printf("%02x ", byte[i + j]);
			else
				printf("   ");
		}

		printf(" ");

		for (size_t j = 0; j < 16; j++) {
			if (i + j < size) {
				printf("%c", isprint(byte[i + j]) ?
							   byte[i + j] :
							   '.');
			}
		}

		printf("\n");
		offset += 16;
	}
}

int write2file(char *filename, void *data, int datalen, size_t start)
{
	FILE *file = fopen(filename, "r+b");

	if (file == NULL) {
		file = fopen(filename, "wb");
		if (file == NULL) {
			pr_err("Unable to open file %s", filename);
			return -1;
		}
	}

	if (fseek(file, start, SEEK_SET) != 0) {
		pr_err("Unable to seek position %zu", start);
		fclose(file);
		return -1;
	}

	if (fwrite(data, datalen, 1, file) != 1) {
		pr_err("Write %s faile!", filename);
		fclose(file);
		return -1;
	}

	fclose(file);
	return 0;
}

size_t get_file_size(char *input_file)
{
	FILE *file;
	long file_size;

	file = fopen(input_file, "r+b");
	if (file == NULL) {
		pr_err("Unable to open file %s", input_file);
		return -1;
	}

	if (fseek(file, 0, SEEK_END) != 0) {
		pr_err("Unable to seed file %s", input_file);
		fclose(file);
		return -1;
	}

	file_size = ftell(file);
	if (file_size == -1) {
		pr_err("Unable to get file size %s", input_file);
		fclose(file);
		return -1;
	}
	return file_size;
}

int file_tail_truncate(char *input_file, size_t size)
{
	FILE *file;
	long file_size;

	file = fopen(input_file, "r+b");
	if (file == NULL) {
		pr_err("Unable to open file %s", input_file);
		return -1;
	}

	if (fseek(file, 0, SEEK_END) != 0) {
		pr_err("Unable to seed file %s", input_file);
		fclose(file);
		return -1;
	}

	file_size = ftell(file);
	if (file_size == -1) {
		pr_err("Unable to get file size %s", input_file);
		fclose(file);
		return -1;
	}

	long new_size = file_size - size;

	if (new_size < 0) {
		pr_err("Truncated size is lager than file size\n");
		fclose(file);
		return -1;
	}

	int fd = fileno(file);

	if (ftruncate(fd, new_size) == -1) {
		pr_err("Truncated file failed!");
		fclose(file);
		return -1;
	}

	fclose(file);
	return 0;
}

bool starts_with(const char *str, const char *substr)
{
	if (str == NULL || substr == NULL)
		return false;

	return strncmp(str, substr, strlen(substr)) == 0;
}

bool is_elf_file(const char *filename)
{
	FILE *file = fopen(filename, "rb");

	if (!file) {
		pr_err("Unable to open file %s", filename);
		return false;
	}

	// Read the first four bytes to check for ELF magic number
	unsigned char elf_magic[4];

	if (fread(elf_magic, sizeof(elf_magic), 1, file) != 1) {
		pr_err("File %s fread failed", filename);
		fclose(file);
		return false;
	}

	fclose(file);

	// Check if it's an ELF file
	return elf_magic[0] == 0x7f && elf_magic[1] == 'E' &&
	       elf_magic[2] == 'L' && elf_magic[3] == 'F';
}

int gen_timestamp(void)
{
	return (int)time(NULL);
}

#ifndef __KERNEL__
ssize_t strscpy(char *dest, const char *src, size_t count)
{
	if (count == 0)
		return -1;

	size_t len = strnlen(src, count - 1);

	memcpy(dest, src, len);
	dest[len] = '\0';

	return len;
}

/*
 * Attach a tracepoint program to its tracepoint.
 *
 * Loader-mode modules use light skeletons, which do not auto-attach
 * tracepoint programs (the generated attach helper is a no-op for
 * them). Modules call this helper instead with their program fd.
 *
 * Returns the bpf link fd on success (close it to detach) or a
 * negative errno on failure.
 */
int ctforge_attach_tracepoint(int prog_fd, const char *tp_category,
			      const char *tp_name)
{
	union bpf_attr attr = {};
	struct perf_event_attr pe_attr = {};
	char path[PATH_MAX];
	int link_fd, pfd;
	FILE *f;

	snprintf(path, sizeof(path),
		 "/sys/kernel/tracing/events/%s/%s/id",
		 tp_category, tp_name);
	f = fopen(path, "r");
	if (!f) {
		snprintf(path, sizeof(path),
			 "/sys/kernel/debug/tracing/events/%s/%s/id",
			 tp_category, tp_name);
		f = fopen(path, "r");
	}
	if (!f) {
		pr_err("tracepoint %s/%s not found", tp_category, tp_name);
		return -ENOENT;
	}
	if (fscanf(f, "%lld", &pe_attr.config) != 1) {
		fclose(f);
		return -ENOENT;
	}
	fclose(f);

	pe_attr.type = PERF_TYPE_TRACEPOINT;
	pe_attr.size = sizeof(pe_attr);

	pfd = syscall(__NR_perf_event_open, &pe_attr,
		      -1 /* pid */, 0 /* cpu */, -1 /* group_fd */,
		      PERF_FLAG_FD_CLOEXEC);
	if (pfd < 0) {
		pr_err("perf_event_open for tracepoint %s/%s failed: %s",
		       tp_category, tp_name, strerror(errno));
		return -errno;
	}

	attr.link_create.prog_fd = prog_fd;
	attr.link_create.target_fd = pfd;
	attr.link_create.attach_type = BPF_PERF_EVENT;
	link_fd = syscall(__NR_bpf, BPF_LINK_CREATE, &attr, sizeof(attr));
	close(pfd);
	if (link_fd < 0) {
		pr_err("link tracepoint %s/%s failed: %s",
		       tp_category, tp_name, strerror(errno));
		return -errno;
	}

	return link_fd;
}
#endif
