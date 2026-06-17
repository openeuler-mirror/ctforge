#include "ctforge.h"

void *policy_context_gen(void)
{
	void *data;

	data = malloc(1024);
	snprintf(data, 1024, "%s", "this is raw data");
	return data;
}

unsigned char *load_file(char *filename, size_t *len)
{
	struct stat f_stat;
	unsigned char *data = NULL;
	bool flag = false;
	FILE *file;

	file = fopen(filename, "rb");
	if (!file) {
		pr_err("open failed!");
		return NULL;
	}
	if (fstat(fileno(file), &f_stat) == -1) {
		pr_err("fstat failed");
		goto end;
	}
	data = (unsigned char *)malloc(f_stat.st_size + 1);
	if (!data) {
		pr_err("malloc failed");
		goto end;
	}
	if (fread(data, 1, f_stat.st_size, file) != f_stat.st_size) {
		pr_err("fread failed!");
		goto end;
	}
	flag = true;
	data[f_stat.st_size] = '\0';
	if (len)
		*len = f_stat.st_size;
end:
	if (!flag && data) {
		free(data);
		data = NULL;
	}
	fclose(file);
	return data;
}

void dump_bin2str(char *prefix, unsigned char *bin, int len)
{
	int i;

	printf("%s", prefix);
	for (i = 0; i < len; i++)
		printf("%02x", bin[i]);
	printf("");
}

void dump_siginfo(char *der, int derlen)
{
	unsigned char sig_r[32], sig_s[32];
	ECDSA_SIG *sig = NULL;
	const BIGNUM *r, *s;

	sig = d2i_ECDSA_SIG(NULL, (const unsigned char **)&der, derlen);
	if (!sig) {
		pr_err("Failed to decode DER ECDSA signature");
		return;
	}
	ECDSA_SIG_get0(sig, &r, &s);
	BN_bn2binpad(r, sig_r, sizeof(sig_r));
	BN_bn2binpad(s, sig_s, sizeof(sig_r));
	dump_bin2str("sig_r: ", sig_r, 32);
	dump_bin2str("sig_s: ", sig_s, 32);
	ECDSA_SIG_free(sig);
}

int get_sm2siginfo(unsigned char *der, int derlen, unsigned char sig_r[32],
		   unsigned char sig_s[32])
{
	ECDSA_SIG *sig = NULL;
	const BIGNUM *r, *s;

	sig = d2i_ECDSA_SIG(NULL, (const unsigned char **)&der, derlen);
	if (!sig) {
		pr_err("Failed to decode DER ECDSA signature");
		return -1;
	}
	ECDSA_SIG_get0(sig, &r, &s);
	BN_bn2binpad(r, sig_r, 32);
	BN_bn2binpad(s, sig_s, 32);
	ECDSA_SIG_free(sig);
	return 0;
}

unsigned char *get_pkey_alloc(const EC_KEY *eckey, int *len)
{
	unsigned char *buf = NULL;
	const EC_GROUP *group = EC_KEY_get0_group(eckey);
	const EC_POINT *pt = EC_KEY_get0_public_key(eckey);

	*len = EC_POINT_point2oct(group, pt, POINT_CONVERSION_UNCOMPRESSED,
				  NULL, 0, NULL);
	buf = (unsigned char *)malloc(*len);
	EC_POINT_point2oct(group, pt, POINT_CONVERSION_UNCOMPRESSED,
			   (unsigned char *)buf, *len, NULL);
	return buf;
}

void dump_pkey(EC_KEY *ec_key)
{
	unsigned char *buf;
	int len;

	buf = get_pkey_alloc(ec_key, &len);
	if (buf) {
		dump_bin2str("pkey: ", buf, len);
		free(buf);
	}
}

int covert_sig_to_asn1(BIGNUM *bn_r, BIGNUM *bn_s, char *sig_p, size_t *siglen)
{
	unsigned char sig_r[32], sig_s[32];
	unsigned char *der = NULL;
	int der_len;
	ECDSA_SIG *sig = ECDSA_SIG_new();
	BIGNUM *r_copy = NULL, *s_copy = NULL;

	if (BN_bn2binpad(bn_r, sig_r, sizeof(sig_r)) != sizeof(sig_r))
		return -1;
	if (BN_bn2binpad(bn_s, sig_s, sizeof(sig_s)) != sizeof(sig_s))
		return -1;

	r_copy = BN_dup(bn_r);
	s_copy = BN_dup(bn_s);
	if (r_copy == NULL || s_copy == NULL) {
		BN_free(r_copy);
		BN_free(s_copy);
		ECDSA_SIG_free(sig);
		return -1;
	}

	if (!ECDSA_SIG_set0(sig, r_copy, s_copy)) {
		pr_err("set ec sig failed.");
		BN_free(r_copy);
		BN_free(s_copy);
		ECDSA_SIG_free(sig);
		return -1;
	}

	der_len = i2d_ECDSA_SIG(sig, &der);
	if (der_len <= 0) {
		OPENSSL_free(der);
		ECDSA_SIG_free(sig);
		return -1;
	}

	*siglen = der_len;
	memcpy(sig_p, der, der_len);

	OPENSSL_free(der);
	ECDSA_SIG_free(sig);
	return 0;
}

/*
 * Signs a message using SM2 private key from PEM file.
 */
int sm2_sign(const char *msg, size_t msg_len, const char *privkey_pem_path,
	     unsigned char **sig, size_t *sig_len)
{
	if (!msg || !privkey_pem_path || !sig || !sig_len)
		return -1;

	FILE *fp = fopen(privkey_pem_path, "r");

	if (!fp)
		return -2;

	EVP_PKEY *pkey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);

	fclose(fp);
	if (!pkey)
		return -3;

	EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();

	if (!md_ctx) {
		EVP_PKEY_free(pkey);
		return -4;
	}

	int ret = -5;

	if (EVP_DigestSignInit(md_ctx, NULL, EVP_sm3(), NULL, pkey) <= 0)
		goto cleanup;

	if (EVP_DigestSign(md_ctx, NULL, sig_len, (const unsigned char *)msg,
			   msg_len) <= 0)
		goto cleanup;

	*sig = OPENSSL_malloc(*sig_len);
	if (!*sig) {
		ret = -6;
		goto cleanup;
	}

	if (EVP_DigestSign(md_ctx, *sig, sig_len, (const unsigned char *)msg,
			   msg_len) <= 0) {
		OPENSSL_free(*sig);
		*sig = NULL;
		ret = -7;
		goto cleanup;
	}

	ret = 0;

cleanup:
	EVP_MD_CTX_free(md_ctx);
	EVP_PKEY_free(pkey);
	return ret;
}

int sm2_verify(const char *msg, size_t msg_len, const char *pubkey_pem_path,
	       const unsigned char *sig, size_t sig_len)
{
	if (!msg || !pubkey_pem_path || !sig)
		return -1;

	FILE *fp = fopen(pubkey_pem_path, "r");

	if (!fp)
		return -2;

	EVP_PKEY *pkey = PEM_read_PUBKEY(fp, NULL, NULL, NULL);

	fclose(fp);
	if (!pkey)
		return -3;

	EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();

	if (!md_ctx) {
		EVP_PKEY_free(pkey);
		return -4;
	}

	int ret = -5;

	if (EVP_DigestVerifyInit(md_ctx, NULL, EVP_sm3(), NULL, pkey) <= 0)
		goto cleanup;

	if (EVP_DigestVerify(md_ctx, sig, sig_len, (const unsigned char *)msg,
			     msg_len) == 1)
		ret = 0;
	else
		ret = -6;

cleanup:
	EVP_MD_CTX_free(md_ctx);
	EVP_PKEY_free(pkey);
	return ret;
}

/*
 * Verifies an SM2 signature using a built-in public key (OpenSSL 3.0+).
 * Returns 0 on success, negative value on error.
 */
int sm2_verify_builtinkey(const char *msg, size_t msg_len, const char *sig,
			  size_t sig_len)
{
	EVP_PKEY *pkey = NULL;
	int ret = -3;
	EVP_MD_CTX *md_ctx = NULL;
	EVP_PKEY_CTX *ctx = NULL;
	OSSL_PARAM params[3];

	if (!msg || !sig || sm2_public_key_len <= 0)
		return -1;

	ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_SM2, NULL);
	if (!ctx)
		return -2;

	params[0] = OSSL_PARAM_construct_utf8_string("group", (char *)"SM2", 0);

	params[1] = OSSL_PARAM_construct_octet_string(
		"pub", (void *)sm2_public_key, sm2_public_key_len);

	params[2] = OSSL_PARAM_construct_end();

	// Initialize fromdata operation
	if (EVP_PKEY_fromdata_init(ctx) <= 0) {
		ret = -7;
		goto cleanup;
	}

	// Generate public key from parameters
	if (EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
		ret = -8;
		goto cleanup;
	}

	if (!pkey) {
		ret = -9;
		goto cleanup;
	}

	// Create message digest context
	md_ctx = EVP_MD_CTX_new();
	if (!md_ctx) {
		ret = -4;
		goto cleanup;
	}

	// Initialize verification with SM3 hash
	if (EVP_DigestVerifyInit(md_ctx, NULL, EVP_sm3(), NULL, pkey) != 1) {
		ret = -9;
		goto cleanup;
	}

	// Perform verification
	int verify_ret = EVP_DigestVerify(md_ctx, (const unsigned char *)sig,
					  sig_len, (const unsigned char *)msg,
					  msg_len);

	if (verify_ret == 1)
		ret = 0; // Success
	else if (verify_ret == 0)
		ret = -5; // Signature invalid
	else
		ret = -6; // Verification error

cleanup:
	if (md_ctx)
		EVP_MD_CTX_free(md_ctx);

	if (pkey)
		EVP_PKEY_free(pkey);

	if (ctx)
		EVP_PKEY_CTX_free(ctx);

	return ret;
}

bool is_tail_signed(char *input_file)
{
	FILE *file;
	char buffer[8];
	bool result = false;

	file = fopen(input_file, "rb");
	if (file == NULL) {
		pr_err("Unable to open file %s", input_file);
		exit(EXIT_FAILURE);
	}

	if (fseek(file, -sizeof(struct ctforged_sign_header), SEEK_END) != 0) {
		pr_err("Unable to seek position");
		fclose(file);
		exit(EXIT_FAILURE);
	}

	if (fread(buffer, 1, 8, file) != 8) {
		pr_err("read flag failed!");
		fclose(file);
		exit(EXIT_FAILURE);
	}

	if (memcmp(buffer, "CTFORGE", 7) == 0)
		result = true;

	fclose(file);
	return result;
}

Elf *open_elf(const char *input_filename, int *fd)
{
	*fd = open(input_filename, O_RDWR, 0);

	if (*fd < 0) {
		pr_err("Unable to open %s", input_filename);
		return NULL;
	}

	if (elf_version(EV_CURRENT) == EV_NONE) {
		pr_err("init libelf failed!");
		close(*fd);
		return NULL;
	}

	Elf *elf = elf_begin(*fd, ELF_C_RDWR, NULL);

	if (!elf) {
		pr_err("elf_begin failed!");
		close(*fd);
		return NULL;
	}

	return elf;
}

void close_elf(Elf *elf, int fd)
{
	if (elf)
		elf_end(elf);
	if (fd >= 0)
		close(fd);
}

char **list_sections(const char *input_filename, int *section_count)
{
	int fd;
	Elf *elf = open_elf(input_filename, &fd);
	Elf_Scn *scn = NULL;
	GElf_Shdr shdr;
	int count = 0;

	if (!elf)
		return NULL;

	size_t shstrndx;

	if (elf_getshdrstrndx(elf, &shstrndx) != 0) {
		pr_err("elf_getshdrstrndx failed!");
		close_elf(elf, fd);
		return NULL;
	}

	while ((scn = elf_nextscn(elf, scn)) != NULL)
		count++;

	char **section_names = malloc(count * sizeof(char *));

	if (!section_names) {
		pr_err("malloc failed!");
		close_elf(elf, fd);
		return NULL;
	}

	scn = NULL;
	int index = 0;

	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		gelf_getshdr(scn, &shdr);
		char *name = elf_strptr(elf, shstrndx, shdr.sh_name);

		section_names[index] = strdup(name);
		if (!section_names[index]) {
			pr_err("strdup failed! malloc memory failed!");
			for (int i = 0; i < index; i++)
				free(section_names[i]);

			free(section_names);
			close_elf(elf, fd);
			return NULL;
		}
		index++;
	}

	*section_count = count;
	close_elf(elf, fd);
	return section_names;
}

struct ctforge_elf_sec *get_section_content(const char *input_filename,
					    const char *section_name)
{
	int fd;
	struct ctforge_elf_sec *section_content =
		calloc(1, sizeof(struct ctforge_elf_sec));

	if (!section_content)
		return NULL;

	Elf *elf = open_elf(input_filename, &fd);

	if (!elf) {
		free(section_content);
		return NULL;
	}

	size_t shstrndx;
	Elf_Scn *scn = NULL;
	GElf_Shdr shdr;

	elf_getshdrstrndx(elf, &shstrndx);

	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		gelf_getshdr(scn, &shdr);
		char *name = elf_strptr(elf, shstrndx, shdr.sh_name);

		if (strcmp(name, section_name) == 0) {
			Elf_Data *data = elf_getdata(scn, NULL);

			if (data && data->d_size > 0) {
				section_content->data = malloc(data->d_size);
				if (section_content->data) {
					memcpy(section_content->data,
					       data->d_buf, data->d_size);
					section_content->size = data->d_size;
				}
			}
			break;
		}
	}

	close_elf(elf, fd);
	return section_content;
}

int section_exists(const char *input_filename, const char *section_name)
{
	int fd;
	size_t shstrndx;
	Elf_Scn *scn = NULL;
	GElf_Shdr shdr;
	int exists = 0;
	Elf *elf = open_elf(input_filename, &fd);

	if (!elf)
		return 0;

	elf_getshdrstrndx(elf, &shstrndx);

	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		gelf_getshdr(scn, &shdr);
		char *name = elf_strptr(elf, shstrndx, shdr.sh_name);

		if (strcmp(name, section_name) == 0) {
			exists = 1;
			break;
		}
	}

	close_elf(elf, fd);
	return exists;
}

int create_section(const char *input_filename, const char *section_name,
		   const void *data, size_t size)
{
	int fd;
	Elf *elf = open_elf(input_filename, &fd);

	if (!elf)
		return -1;

	size_t shstrndx;

	if (elf_getshdrstrndx(elf, &shstrndx) != 0) {
		pr_err("elf_getshdrstrndx error");
		close_elf(elf, fd);
		return -1;
	}

	Elf_Scn *shstrscn = elf_getscn(elf, shstrndx);
	Elf_Data *shstrdata = elf_getdata(shstrscn, NULL);

	if (!shstrdata) {
		pr_err("get shstrtab error");
		close_elf(elf, fd);
		return -1;
	}

	char *new_strtab = malloc(shstrdata->d_size + strlen(section_name) + 1);

	if (!new_strtab) {
		pr_err("malloc failed!");
		close_elf(elf, fd);
		return -1;
	}
	memcpy(new_strtab, shstrdata->d_buf, shstrdata->d_size);

	strscpy(new_strtab + shstrdata->d_size, section_name,
		strlen(section_name) + 1);

	shstrdata->d_buf = new_strtab;
	shstrdata->d_size += strlen(section_name) + 1;

	Elf_Scn *scn = elf_newscn(elf);

	if (!scn) {
		free(new_strtab);
		close_elf(elf, fd);
		return -1;
	}
	GElf_Shdr shdr, *shdrp;

	shdrp = gelf_getshdr(scn, &shdr);
	if (!shdrp) {
		free(new_strtab);
		close_elf(elf, fd);
		return -1;
	}

	shdr.sh_name = shstrdata->d_size - strlen(section_name) - 1;
	shdr.sh_type = SHT_PROGBITS;
	shdr.sh_flags = SHF_ALLOC | SHF_WRITE;
	shdr.sh_addr = 0;
	shdr.sh_offset = 0;
	shdr.sh_size = size;
	shdr.sh_link = 0;
	shdr.sh_info = 0;
	shdr.sh_addralign = 1;
	shdr.sh_entsize = 0;

	if (!gelf_update_shdr(scn, &shdr)) {
		free(new_strtab);
		close_elf(elf, fd);
		return -1;
	}

	Elf_Data *new_data = elf_newdata(scn);

	if (!new_data) {
		free(new_strtab);
		close_elf(elf, fd);
		return -1;
	}
	new_data->d_buf = malloc(size);
	if (!new_data->d_buf) {
		pr_err("malloc failed!");
		free(new_strtab);
		close_elf(elf, fd);
		return -1;
	}
	memcpy(new_data->d_buf, data, size);
	new_data->d_type = ELF_T_BYTE;
	new_data->d_size = size;
	new_data->d_off = 0;
	new_data->d_align = 1;
	new_data->d_version = EV_CURRENT;

	if (elf_update(elf, ELF_C_WRITE) < 0) {
		pr_err("elf_update failed!");
		free(new_data->d_buf);
		free(new_strtab);
		close_elf(elf, fd);
		return -1;
	}

	close_elf(elf, fd);
	return 0;
}

int do_verify_ebpf_section_sign(char *key_path, char *input_file)
{
	int section_number;
	char **secname = list_sections(input_file, &section_number);

	if (!secname) {
		pr_err("Failed to list sections");
		return -1;
	}

	int has_sign_section = 0;
	int result = 0;

	for (int i = 0; i < section_number; i++) {
		if (secname[i][0] != '.') {
			if (starts_with(secname[i], "SIGN_")) {
				has_sign_section = 1;
				continue;
			}

			char newsec_name[256];

			snprintf(newsec_name, sizeof(newsec_name), "SIGN_%s",
				 secname[i]);

			if (!section_exists(input_file, newsec_name)) {
				pr_err("sec %s does not have sign section",
				       secname[i]);
				result = -1;
				goto cleanup;
			}

			struct ctforge_elf_sec *sc =
				get_section_content(input_file, secname[i]);

			if (!sc || !sc->data) {
				pr_err("get sec %s content failed!",
				       secname[i]);
				result = -1;
				goto cleanup;
			}

			struct ctforge_elf_sec *sn =
				get_section_content(input_file, newsec_name);

			if (!sn || !sn->data) {
				pr_err("get sign sec %s content failed!",
				       newsec_name);
				free(sc->data);
				free(sc);
				result = -1;
				goto cleanup;
			}

			int ret = sm2_verify((const char *)sc->data, sc->size,
					     key_path,
					     (const unsigned char *)sn->data,
					     sn->size);

			free(sc->data);
			free(sc);
			free(sn->data);
			free(sn);

			if (ret != 0) {
				pr_err("verify sec failed! [%s], error: %d",
				       secname[i], ret);
				result = -1;
				goto cleanup;
			} else {
				pr_info("verify sec success! [%s]", secname[i]);
			}
		}
	}

	if (!has_sign_section) {
		pr_err("file %s does not have ebpf section signed!",
		       input_file);
		result = -1;
	}

cleanup:
	for (int i = 0; i < section_number; i++)
		free(secname[i]);

	free(secname);
	return result;
}

int do_verify_tail_sign(char *key_path, char *input_file)
{
	if (!is_tail_signed(input_file)) {
		pr_err("File %s does not have tail signed!", input_file);
		return 1;
	}

	unsigned char *file_content = load_file(input_file, NULL);

	if (!file_content) {
		pr_err("read file %s failed!", input_file);
		return 1;
	}

	size_t total_size = get_file_size(input_file);
	size_t file_size = total_size - sizeof(struct ctforged_sign_header);
	struct ctforged_sign_header *head =
		(struct ctforged_sign_header *)(file_content + file_size);

	int ret = sm2_verify((const char *)file_content, file_size, key_path,
			     (const unsigned char *)head->sign, head->sign_len);

	free(file_content);

	if (ret != 0) {
		pr_err("verify %s tail sign failed! error: %d", input_file,
		       ret);
		return 1;
	}
	pr_info("verify %s tail sign success!", input_file);
	return 0;
}

int do_safty_verify_sign(char *public_key_path, char *input_file)
{
	if (!public_key_path || !input_file)
		return 1;

	FILE *file = fopen(input_file, "rb");

	if (!file) {
		pr_err("Unable to open file %s", input_file);
		return 1;
	}

	unsigned char elf_magic[4];

	if (fread(elf_magic, 1, 4, file) != 4) {
		pr_err("File %s fread failed", input_file);
		fclose(file);
		return 1;
	}

	if (!(elf_magic[0] == 0x7f && elf_magic[1] == 'E' &&
	      elf_magic[2] == 'L' && elf_magic[3] == 'F')) {
		pr_err("File %s is not valid elf file!", input_file);
		fclose(file);
		return 1;
	}

	uint16_t magic;

	if (fseek(file, 18, SEEK_SET) != 0 ||
	    fread(&magic, sizeof(magic), 1, file) != 1) {
		pr_err("File %s fseek/fread failed", input_file);
		fclose(file);
		return 1;
	}
	fclose(file);

	int ebpf_ret = 0, tail_ret = 0;

	if (magic == 0xf7) {
		pr_err("%s is ebpf object, check all ebpf section", input_file);
		ebpf_ret = do_verify_ebpf_section_sign(public_key_path,
						       input_file);
		if (ebpf_ret == 0)
			pr_err("ebpf section sign verify ok!");
		else
			pr_err("ebpf section sign verify failed!");
	}

	tail_ret = do_verify_tail_sign(public_key_path, input_file);
	if (tail_ret == 0) {
		pr_err("%s tail sign verify ok!", input_file);
	} else {
		pr_err("%s tail sign verify failed! using %s", input_file,
		       public_key_path);
	}

	if (magic == 0xf7)
		return (ebpf_ret == 0 && tail_ret == 0) ? 0 : 1;
	else
		return tail_ret;
}

// Verify SM2 signatures of eBPF sections from an in-memory ELF buffer.
// For every non-dot-prefixed section (e.g., "my_prog"), expect a corresponding
// "SIGN_my_prog" section containing its signature.
// Returns 0 on success, -1 on failure.
int do_verify_ebpf_section_sign_from_mem(const char *key_path, const void *buf,
					 size_t buf_size)
{
	if (!key_path || !buf || buf_size == 0) {
		pr_err("Invalid input");
		return -1;
	}

	// Initialize libelf library
	if (elf_version(EV_CURRENT) == EV_NONE) {
		pr_err("ELF library initialization failed");
		return -1;
	}

	// Create an Elf object from memory buffer (does not copy data)
	Elf *elf = elf_memory((char *)buf, buf_size);

	if (!elf) {
		pr_err("Failed to parse ELF from memory: %s", elf_errmsg(-1));
		return -1;
	}

	// Get index of section header string table
	size_t shstrndx;

	if (elf_getshdrstrndx(elf, &shstrndx) != 0) {
		pr_err("Failed to get section header string table index");
		elf_end(elf);
		return -1;
	}

	int has_sign_section = 0;
	int result = 0;

	// Iterate over all sections
	Elf_Scn *scn = NULL;

	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		GElf_Shdr shdr;

		if (gelf_getshdr(scn, &shdr) != &shdr)
			continue;

		// Get section name
		char *name = elf_strptr(elf, shstrndx, shdr.sh_name);

		if (!name || name[0] == '\0')
			continue;

		// Skip standard ELF sections (those starting with '.')
		if (name[0] == '.')
			continue;

		// If this is a signature section (SIGN_*), just mark existence
		if (starts_with(name, "SIGN_")) {
			has_sign_section = 1;
			continue;
		}

		// Construct expected signature section name: "SIGN_<original_name>"
		char sign_sec_name[256];

		snprintf(sign_sec_name, sizeof(sign_sec_name), "SIGN_%s", name);

		// Search for the corresponding SIGN_* section
		Elf_Scn *sign_scn = NULL;
		GElf_Shdr sign_shdr;
		int found_sign = 0;

		Elf_Scn *tmp_scn = NULL;

		while ((tmp_scn = elf_nextscn(elf, tmp_scn)) != NULL) {
			if (gelf_getshdr(tmp_scn, &sign_shdr) != &sign_shdr)
				continue;

			char *tmp_name =
				elf_strptr(elf, shstrndx, sign_shdr.sh_name);

			if (tmp_name && strcmp(tmp_name, sign_sec_name) == 0) {
				sign_scn = tmp_scn;
				found_sign = 1;
				break;
			}
		}

		if (!found_sign) {
			pr_err("Section '%s' missing signature section '%s'",
			       name, sign_sec_name);
			result = -1;
			goto cleanup;
		}

		// Retrieve raw data of the original section
		Elf_Data *data = elf_getdata(scn, NULL);

		if (!data || !data->d_buf || data->d_size == 0) {
			pr_err("Failed to get data for section '%s'", name);
			result = -1;
			goto cleanup;
		}

		// Retrieve raw data of the signature section
		Elf_Data *sig_data = elf_getdata(sign_scn, NULL);

		if (!sig_data || !sig_data->d_buf || sig_data->d_size == 0) {
			pr_err("Failed to get signature data for '%s'",
			       sign_sec_name);
			result = -1;
			goto cleanup;
		}

		// Perform SM2 signature verification
		int ret = sm2_verify((const char *)data->d_buf, data->d_size,
				     key_path,
				     (const unsigned char *)sig_data->d_buf,
				     sig_data->d_size);

		if (ret != 0) {
			pr_err("SM2 verify failed for section '%s', error: %d",
			       name, ret);
			result = -1;
			goto cleanup;
		} else {
			pr_err("SM2 verify succeeded for section '%s'", name);
		}
	}

	// Ensure at least one SIGN_* section exists
	if (!has_sign_section) {
		pr_err("No SIGN_* sections found in ELF");
		result = -1;
	}

cleanup:
	elf_end(elf);
	return result;
}

// Verify tail signature from an in-memory buffer.
// The signature header is expected to be at the very end of the buffer.
// Returns 0 on success, 1 on failure.
int do_verify_tail_sign_from_mem(const char *key_path, const void *buf,
				 size_t buf_size)
{
	if (!key_path || !buf || buf_size == 0) {
		pr_err("Invalid input to verify tail sign from mem");
		return 1;
	}

	// Check if buffer is large enough to contain at least the header
	if (buf_size < sizeof(struct ctforged_sign_header)) {
		pr_err("Buffer too small to contain signature header");
		return 1;
	}

	// Point to the signature header at the end of the buffer
	const struct ctforged_sign_header *head =
		(const struct ctforged_sign_header
			 *)((const char *)buf + buf_size -
			    sizeof(struct ctforged_sign_header));

	// Validate sign_len to prevent overreads
	if (head->sign_len == 0 || head->sign_len > sizeof(head->sign)) {
		pr_err("Invalid signature length in header: %u",
		       head->sign_len);
		return 1;
	}

	// The signed data is everything before the header
	size_t data_size = buf_size - sizeof(struct ctforged_sign_header);
	const char *data = (const char *)buf;

	// Perform SM2 verification
	int ret = sm2_verify(data, data_size, key_path,
			     (const unsigned char *)head->sign, head->sign_len);

	if (ret != 0) {
		pr_err("Tail signature verification failed! error: %d", ret);
		return 1;
	}

	pr_err("Tail signature verification succeeded!");
	return 0;
}

int do_safety_verify_sign_from_mem(const char *public_key_path, const void *buf,
				   size_t buf_size)
{
	if (!public_key_path || !buf || buf_size == 0)
		return 1;

	if (buf_size < 4) {
		pr_err("Buffer too small for ELF magic");
		return 1;
	}
	const unsigned char *elf_magic = (const unsigned char *)buf;

	if (!(elf_magic[0] == 0x7f && elf_magic[1] == 'E' &&
	      elf_magic[2] == 'L' && elf_magic[3] == 'F')) {
		pr_err("Buffer is not a valid ELF file!");
		return 1;
	}

	uint16_t magic = 0;

	if (buf_size < 20) {
		pr_err("Buffer too small to read e_machine");
		return 1;
	}
	memcpy(&magic, (const char *)buf + 18, sizeof(magic));

	int ebpf_ret = 0, tail_ret = 0;

	if (magic == 0xf7) {
		pr_err("Input is eBPF object, checking all eBPF sections");
		ebpf_ret = do_verify_ebpf_section_sign_from_mem(public_key_path,
								buf, buf_size);
		if (ebpf_ret == 0)
			pr_err("eBPF section signature verify OK!");
		else
			pr_err("eBPF section signature verify FAILED!");
	}

	tail_ret = do_verify_tail_sign_from_mem(public_key_path, buf, buf_size);
	if (tail_ret == 0)
		pr_err("Tail signature verify OK!");
	else
		pr_err("Tail signature verify FAILED! Using key: %s",
		       public_key_path);

	if (magic == 0xf7)
		return (ebpf_ret == 0 && tail_ret == 0) ? 0 : 1;
	else
		return tail_ret;
}
