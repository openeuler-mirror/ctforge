/*
 * SM2 Sign/Verify Utilities for Kernel Verification
 *
 * This file provides:
 *   - sm2_sign(): signs a message with SM2 private key (DER output)
 *   - sm2_verify(): verifies DER signature with SM2 public key
 *   - generate_sm2_signature_header(): exports data to C header
 *
 * Uses OpenSSL 3.0 EVP interface with full SM2 mode (including ZA).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/core_names.h>

/*
 * Signs a message using SM2 private key from PEM file.
 * Output signature is DER-encoded (suitable for Linux kernel akcipher).
 *
 * Parameters:
 *   msg               - input message to sign
 *   msg_len           - length of message in bytes
 *   privkey_pem_path  - path to PEM-encoded SM2 private key
 *   sig               - output: pointer to allocated signature buffer (caller must free)
 *   sig_len           - output: length of DER signature
 *
 * Returns 0 on success, non-zero on error.
 */
int sm2_sign(const char *msg, size_t msg_len, const char *privkey_pem_path,
	     unsigned char **sig, size_t *sig_len)
{
	EVP_PKEY *pkey;
	FILE *fp;

	if (!msg || !privkey_pem_path || !sig || !sig_len)
		return -1;

	fp = fopen(privkey_pem_path, "r");
	if (!fp)
		return -2;

	pkey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
	fclose(fp);
	if (!pkey)
		return -3;


	EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();

	if (!md_ctx) {
		EVP_PKEY_free(pkey);
		return -4;
	}

	int ret = -5;

	ret = EVP_DigestSignInit(md_ctx, NULL, EVP_sm3(), NULL, pkey);
	if (ret <= 0)
		goto cleanup;

	ret = EVP_DigestSign(md_ctx, NULL, sig_len, (const unsigned char *)msg,
			     msg_len);
	if (ret <= 0)
		goto cleanup;

	*sig = OPENSSL_malloc(*sig_len);
	if (!*sig) {
		ret = -6;
		goto cleanup;
	}

	ret = EVP_DigestSign(md_ctx, *sig, sig_len, (const unsigned char *)msg,
			     msg_len);
	if (ret <= 0) {
		OPENSSL_free(*sig);
		*sig = NULL;
		ret = -7;
		goto cleanup;
	}

	ret = 0; // Success

cleanup:
	EVP_MD_CTX_free(md_ctx);
	EVP_PKEY_free(pkey);
	return ret;
}

/*
 * Verifies an SM2 DER-encoded signature against a message using public key from PEM.
 *
 * Parameters:
 *   msg              - message to verify
 *   msg_len          - length of message
 *   pubkey_pem_path  - path to PEM-encoded SM2 public key
 *   sig              - DER-encoded signature
 *   sig_len          - length of signature
 *
 * Returns 0 on successful verification, non-zero on error or invalid signature.
 */
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
		ret = 0; // Verification succeeded
	else
		ret = -6; // Verification failed or error

cleanup:
	EVP_MD_CTX_free(md_ctx);
	EVP_PKEY_free(pkey);
	return ret;
}

/*
 * Generates a C header file containing:
 *   - the original message string
 *   - DER-encoded SM2 signature
 *   - raw 65-byte uncompressed public key
 *
 * Assumes public key is valid SM2 key in PEM format.
 *
 * Returns 0 on success, non-zero on error.
 */
int generate_sm2_signature_header(const char *msg, const unsigned char *sig,
				  size_t sig_len, const char *pubkey_pem_path,
				  const char *header_path)
{
	if (!msg || !sig || !pubkey_pem_path || !header_path)
		return -1;

	FILE *pub_fp = fopen(pubkey_pem_path, "r");

	if (!pub_fp)
		return -2;

	EVP_PKEY *pub_pkey = PEM_read_PUBKEY(pub_fp, NULL, NULL, NULL);

	fclose(pub_fp);
	if (!pub_pkey)
		return -3;

	size_t pubkey_len = 0;

	if (!EVP_PKEY_get_octet_string_param(pub_pkey, OSSL_PKEY_PARAM_PUB_KEY,
					     NULL, 0, &pubkey_len) ||
	    pubkey_len != 65) {
		EVP_PKEY_free(pub_pkey);
		return -4;
	}

	unsigned char pubkey_raw[65];

	if (!EVP_PKEY_get_octet_string_param(pub_pkey, OSSL_PKEY_PARAM_PUB_KEY,
					     pubkey_raw, sizeof(pubkey_raw),
					     &pubkey_len)) {
		EVP_PKEY_free(pub_pkey);
		return -5;
	}
	EVP_PKEY_free(pub_pkey);

	FILE *header = fopen(header_path, "w");

	if (!header)
		return -6;
}

fprintf(header, "#ifndef __SM2_SIGNATURE_H__\n#define __SM2_SIGNATURE_H__\n\n");

// Escape double quotes in message if needed (basic handling)
fprintf(header, "static const char sm2_message[] = \"%s\";\n\n", msg);

fprintf(header, "static unsigned char sm2_signature[] = {\n    ");
for (size_t i = 0; i < sig_len; i++) {
	fprintf(header, "0x%02x", sig[i]);
	if (i != sig_len - 1) {
		fprintf(header, ", ");
		if ((i + 1) % 8 == 0)
			fprintf(header, "\n    ");
	}
}
fprintf(header, "\n};\n\n");
fprintf(header, "static const size_t sm2_signature_len = %zuU;\n\n", sig_len);

fprintf(header, "static const unsigned char sm2_public_key[] = {\n    ");
for (size_t i = 0; i < 65; i++) {
	fprintf(header, "0x%02x", pubkey_raw[i]);
	if (i != 64) {
		fprintf(header, ", ");
		if ((i + 1) % 8 == 0)
			fprintf(header, "\n    ");
	}
}
fprintf(header, "\n};\n\n");
fprintf(header, "static const size_t sm2_public_key_len = 65U;\n\n");

fprintf(header, "#endif /* __SM2_SIGNATURE_H__ */\n");
fclose(header);
return 0;
}

int main(void)
{
	const char *msg = "Hello, CTyunOS 4 SM2 Algorithm";
	size_t msg_len = strlen(msg);
	unsigned char *sig = NULL;
	size_t sig_len = 0;

	int rc = sm2_sign(msg, msg_len, "sm2_private_key.pem", &sig, &sig_len);

	if (rc != 0) {
		fprintf(stderr, "Signing failed: %d\n", rc);
		return 1;
	}

	// Optional: verify it works
	rc = sm2_verify(msg, msg_len, "sm2_public_key.pem", sig, sig_len);
	if (rc != 0) {
		fprintf(stderr, "Verification failed: %d\n", rc);
		OPENSSL_free(sig);
		return 1;
	}

	// Generate header
	rc = generate_sm2_signature_header(
		msg, sig, sig_len, "sm2_public_key.pem", "sm2_signature.h");
	if (rc != 0) {
		fprintf(stderr, "Header generation failed: %d\n", rc);
		OPENSSL_free(sig);
		return 1;
	}

	printf("Success: sm2_signature.h generated.\n");
	OPENSSL_free(sig);
	return 0;
}
