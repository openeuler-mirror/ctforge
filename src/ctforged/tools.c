#include "ctforge.h"

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

int strnappend(char *str, size_t size, const char *format, ...)
{
	va_list args;
	int written;
	int length = strlen(str);

	if (length >= size)
		return length;

	va_start(args, format);
	written = vsnprintf(str + length, size - length, format, args);
	va_end(args);

	if (written < 0)
		return length;

	length += written;
	if ((size_t)length >= size)
		length = size - 1;

	return length;
}

int sk_resappend(struct ctforged_params *s, const char *format, ...)
{
	va_list args;
	int written;

	if (s->res_size >= RESP_LEN_MAX)
		return s->res_size;

	va_start(args, format);
	written = vsnprintf(s->res + s->res_size, RESP_LEN_MAX - s->res_size,
			    format, args);
	va_end(args);

	if (written < 0)
		return s->res_size;

	s->res_size += written;
	if ((size_t)s->res_size >= RESP_LEN_MAX)
		s->res_size = RESP_LEN_MAX - 1;

	return s->res_size;
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
		exit(EXIT_FAILURE);
	}

	if (fseek(file, 18, SEEK_SET) != 0) {
		pr_err("File %s fseek failed", filename);
		fclose(file);
		exit(EXIT_FAILURE);
	}

	uint16_t magic;

	if (fread(&magic, sizeof(magic), 1, file) != 1) {
		pr_err("File %s fread failed", filename);
		fclose(file);
		exit(EXIT_FAILURE);
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
		pr_err("Unable to seek position %d", start);
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
		exit(1);
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

void print_json_pretty(struct json_object *obj, FILE *stream, int indent)
{
	if (!obj || !stream) {
		fprintf(stream, "Error: NULL object or stream\n");
		return;
	}

	if (indent <= 0) {
		fprintf(stream, "%s\n",
			json_object_to_json_string_ext(obj,
						       JSON_C_TO_STRING_PLAIN));
	} else {
		fprintf(stream, "%s\n",
			json_object_to_json_string_ext(
				obj, JSON_C_TO_STRING_SPACED |
					     JSON_C_TO_STRING_PRETTY));
	}
}

void beautify_json(struct json_object *obj)
{
	print_json_pretty(obj, stdout, 4);
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
#endif
