#include "ctforge.h"

struct ctforge_config ctforged_config;

char *key_path;
char *input_file;

int do_sign_ebpf_section(char *key_path, char *input_file)
{
	int i, section_number;
	char **secname;

	secname = list_sections(input_file, &section_number);
	if (!secname) {
		pr_err("Failed to list sections");
		return -1;
	}

	struct ctforge_elf_sec *sc;
	unsigned char *signature = NULL;
	size_t sign_len = 0;
	char newsec_name[PATH_MAX];

	for (i = 0; i < section_number; i++) {
		if (secname[i][0] != '.') {
			pr_info("sec %d [%s]", i + 1, secname[i]);
			if (starts_with(secname[i], "SIGN_")) {
				pr_info("find signed section %s, do nothing.",
				       secname[i]);
				goto cleanup;
			}
			snprintf(newsec_name, sizeof(newsec_name), "SIGN_%s",
				 secname[i]);
			if (section_exists(input_file, newsec_name)) {
				pr_info("find signed section %s, do nothing.",
				       newsec_name);
				goto cleanup;
			}

			sc = get_section_content(input_file, secname[i]);
			if (!sc || !sc->data) {
				pr_err("get section content failed!");
				goto cleanup;
			}

			int ret = sm2_sign((const char *)sc->data, sc->size,
					   key_path, &signature, &sign_len);
			if (ret != 0) {
				pr_err("sign data failed! error code: %d", ret);
				free(sc->data);
				free(sc);
				goto cleanup;
			}

			pr_info("sign for %s (%s), sig len: %zu", secname[i],
			       newsec_name, sign_len);

			if (create_section(input_file, newsec_name, signature,
					   sign_len) == 0) {
				pr_info("add new sec success!");
			} else {
				pr_err("add new sec failed!");
				OPENSSL_free(signature);
				free(sc->data);
				free(sc);
				goto cleanup;
			}

			OPENSSL_free(signature);
			signature = NULL;
			free(sc->data);
			free(sc);
		}
	}

cleanup:
	for (int j = 0; j < section_number; j++)
		free(secname[j]);

	free(secname);
	if (signature)
		OPENSSL_free(signature);
	return EXIT_SUCCESS;
}

int do_sign_tail(char *key_path, char *input_file)
{
	size_t input_file_size = 0;
	void *data;

	data = load_file(input_file, &input_file_size);
	if (!data) {
		pr_err("load input file failed!");
		return -1;
	}

	struct ctforged_sign_header *head =
		calloc(1, sizeof(struct ctforged_sign_header));
	if (!head) {
		free(data);
		return -1;
	}

	snprintf(head->flag, sizeof(head->flag), "%s", "CTFORGE");

	unsigned char *signature = NULL;
	size_t sign_len = 0;

	if (is_tail_signed(input_file)) {
		pr_info("file %s already tail signed!", input_file);
		free(data);
		free(head);
		return 0;
	}

	int ret = sm2_sign((const char *)data, input_file_size, key_path,
			   &signature, &sign_len);
	if (ret != 0) {
		pr_err("sign data failed! error: %d", ret);
		free(data);
		free(head);
		return -1;
	}

	if (sign_len > CTSECFS_SM3_SIG_SIZE) {
		pr_err("signature too long! (%zu > %d)", sign_len,
		       CTSECFS_SM3_SIG_SIZE);
		OPENSSL_free(signature);
		free(data);
		free(head);
		return -1;
	}

	memcpy((void *)head->sign, signature, sign_len);
	head->sign_len = sign_len;

	pr_info("data_size: %zu", input_file_size);
	pr_info("tail sign size: %zu (sign len %zu)",
		sizeof(struct ctforged_sign_header), sign_len);

	ret = write2file(input_file, data, input_file_size, 0);
	if (ret != 0)
		goto end;

	ret = write2file(input_file, head, sizeof(struct ctforged_sign_header),
			 input_file_size);
	if (ret != 0)
		goto end;

	pr_info("write file with sign to %s success!", input_file);

end:
	OPENSSL_free(signature);
	free(data);
	free(head);
	return ret;
}

int do_sign(char *key_path, char *input_file)
{
	bool check_elf_file;
	int ret = 0;

	check_elf_file = is_elf_file(input_file);
	if (!check_elf_file) {
		pr_err("File %s is not valid elf file!", input_file);
		exit(1);
	}

	bool check_ebpf_elf = is_ebpf_file(input_file);

	if (check_ebpf_elf) {
		pr_info("File %s is ebpf object, sign all ebpf section",
			input_file);
		ret = do_sign_ebpf_section(key_path, input_file);
		if (ret == 0)
			pr_info("add ebpf section sign ok!");

	} else {
		pr_info("File %s is normal ELF, just add tail sign",
			input_file);
	}

	ret = do_sign_tail(key_path, input_file);
	if (ret == 0)
		pr_info("add tail sign ok!");

	return ret;
}

int do_verify_sign(char *key_path, char *input_file)
{
	if (!is_elf_file(input_file)) {
		pr_err("File %s is not valid elf file!", input_file);
		exit(1);
	}

	bool check_ebpf_elf = is_ebpf_file(input_file);
	int ebpf_ret = 0, tail_ret = 0;

	if (check_ebpf_elf) {
		pr_info("%s is ebpf object, check all ebpf section",
			input_file);
		ebpf_ret = do_verify_ebpf_section_sign(key_path, input_file);
		if (ebpf_ret != 0) {
			pr_err("ebpf section sign verify failed!");
			return ebpf_ret;
		}
		pr_info("ebpf section sign verify ok!");
	}

	tail_ret = do_verify_tail_sign(key_path, input_file);
	if (tail_ret == 0)
		pr_info("tail sign verify ok!");
	else
		pr_err("tail sign verify failed!");

	if (check_ebpf_elf)
		return (ebpf_ret == 0 && tail_ret == 0) ? 0 : 1;
	else
		return tail_ret;
}

int do_remove_section(const char *input_filename, const char *section_name)
{
	char command[256];
	int result;

	snprintf(command, sizeof(command), "objcopy --remove-section=%s %s",
		 section_name, input_filename);
	result = system(command);
	if (result != 0) {
		pr_err("Error: Failed to remove section '%s' from %s.",
		       section_name, input_filename);
	}
	return result;
}

int do_remove_ebpf_section_sign(char *input_file)
{
	int i, section_number;
	int flag = 0;
	char **secname;

	secname = list_sections(input_file, &section_number);
	if (!secname)
		return -1;

	for (i = 0; i < section_number; i++) {
		if (starts_with(secname[i], "SIGN_")) {
			pr_info("find signed section %s", secname[i]);
			do_remove_section(input_file, secname[i]);
			flag = 1;
		}
	}

	for (int j = 0; j < section_number; j++)
		free(secname[j]);

	free(secname);

	if (!flag)
		pr_info("file %s does not have ebpf section signed! do nothing",
			input_file);

	return EXIT_SUCCESS;
}

int do_remove_tail_sign(char *input_file)
{
	if (!is_tail_signed(input_file)) {
		pr_info("file %s does not have tail signed! do nothing.",
			input_file);
		return 0;
	}
	return file_tail_truncate(input_file,
				  sizeof(struct ctforged_sign_header));
}

int do_remove_sign(char *input_file)
{
	if (!is_elf_file(input_file)) {
		pr_err("File %s is not valid elf file!", input_file);
		exit(1);
	}

	bool check_ebpf_elf = is_ebpf_file(input_file);
	int ret = 0;

	if (check_ebpf_elf) {
		ret = do_remove_tail_sign(input_file);
		if (ret == 0)
			pr_info("remove tail sign ok!");
		ret = do_remove_ebpf_section_sign(input_file);
		if (ret == 0)
			pr_info("remove ebpf section sign ok!");
	} else {
		ret = do_remove_tail_sign(input_file);
		if (ret == 0)
			pr_info("remove tail sign ok!");
	}

	return ret;
}

void usage(char *exec_name)
{
	printf("");
	printf("Usage: %s [-s/-v/-r] -k KEY -i FILE", exec_name);
	printf("  signature");
	printf("\t %s -s -k /PATH/TO/PRIVATE.KEY -i /PATH/TO/FILENAME",
	       exec_name);
	printf("  verify signature");
	printf("\t %s -v -k /PATH/TO/PUBLIC.KEY -i /PATH/TO/FILENAME",
	       exec_name);
	printf("  remove signature");
	printf("\t %s -r -i /PATH/TO/FILENAME", exec_name);
	printf("");
	printf("Notes:");
	printf("  1. Remove signature depends on objcopy (install via 'sudo yum install binutils')");
	printf("");
	exit(-1);
}

int main(int argc, char *argv[])
{
	int opt;
	bool opt_verify = false;
	bool opt_sign = false;
	bool opt_remove_sign = false;
	char *key_path = NULL;
	char *input_file = NULL;

	while ((opt = getopt(argc, argv, "ksvriz")) != -1) {
		switch (opt) {
		case 'k':
			if (optind < argc) {
				key_path = argv[optind];
			} else {
				pr_err("Missing key path after -k");
				usage(argv[0]);
			}
			break;
		case 's':
			opt_sign = true;
			break;
		case 'v':
			opt_verify = true;
			break;
		case 'r':
			opt_remove_sign = true;
			break;
		case 'i':
			if (optind < argc) {
				input_file = argv[optind];
			} else {
				pr_err("Missing input file after -i");
				usage(argv[0]);
			}
			break;
		default:
			usage(argv[0]);
		}
	}

	if (opt_verify) {
		if (!input_file || !key_path)
			goto show_usage;
		return do_verify_sign(key_path, input_file);
	} else if (opt_sign) {
		if (!input_file || !key_path)
			goto show_usage;
		return do_sign(key_path, input_file);
	} else if (opt_remove_sign) {
		if (!input_file)
			goto show_usage;
		return do_remove_sign(input_file);
	}

show_usage:
	usage(argv[0]);
}
