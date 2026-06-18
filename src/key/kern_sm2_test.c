/*
 * Kernel module to verify SM2 signature using "sm2" akcipher
 * Computes SM3(ZA || msg) in kernel, then passes (DER_sig + hash) as required by sm2.c.
 * Compatible with Linux kernel 6.6 (openEuler 24.03 SP2).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/crypto.h>
#include <crypto/akcipher.h>
#include <crypto/hash.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "sm2_signature.h"

extern int sm2_compute_z_digest(struct shash_desc *desc, const void *key,
				unsigned int keylen, void *dgst);

static bool verify_sm2_signature(void)
{
	struct crypto_akcipher *tfm;
	struct akcipher_request *req;
	struct scatterlist sg;
	unsigned char *sig_and_hash = NULL;
	unsigned char *za = NULL;
	unsigned char *e_hash = NULL;
	unsigned char *pub_key = NULL;
	struct shash_desc *za_desc = NULL;
	struct shash_desc *e_desc = NULL;
	struct crypto_shash *sm3_tfm = NULL;
	size_t msg_len;
	int ret;

	msg_len = strlen(sm2_message);

	// Allocate public key
	pub_key = kmemdup(sm2_public_key, sm2_public_key_len, GFP_KERNEL);
	if (!pub_key) {
		ret = -ENOMEM;
		goto out_fail;
	}

	// Allocate descriptor for ZA computation
	za_desc = kzalloc(
		sizeof(*za_desc) +
			crypto_shash_descsize(crypto_alloc_shash("sm3", 0, 0)),
		GFP_KERNEL);
	if (!za_desc) {
		pr_err("Failed to allocate ZA shash descriptor\n");
		ret = -ENOMEM;
		goto out_pub;
	}

	za_desc->tfm = crypto_alloc_shash("sm3", 0, 0);
	if (IS_ERR(za_desc->tfm)) {
		pr_err("Failed to allocate SM3 for ZA: %ld\n",
		       PTR_ERR(za_desc->tfm));
		ret = PTR_ERR(za_desc->tfm);
		goto out_za_desc;
	}

	za = kmalloc(32, GFP_KERNEL);
	if (!za) {
		ret = -ENOMEM;
		goto out_za_tfm;
	}

	// Compute ZA = SM3(ENTLA || IDA || a || b || xG || yG || xA || yA)
	ret = sm2_compute_z_digest(za_desc, pub_key, sm2_public_key_len, za);
	if (ret) {
		pr_err("Failed to compute Z digest: %d\n", ret);
		goto out_za;
	}

	// Now compute e = SM3(ZA || msg)
	sm3_tfm = crypto_alloc_shash("sm3", 0, 0);
	if (IS_ERR(sm3_tfm)) {
		pr_err("Failed to allocate SM3 for e: %ld\n", PTR_ERR(sm3_tfm));
		ret = PTR_ERR(sm3_tfm);
		goto out_za;
	}

	e_desc = kmalloc(sizeof(*e_desc) + crypto_shash_descsize(sm3_tfm),
			 GFP_KERNEL);
	if (!e_desc) {
		ret = -ENOMEM;
		goto out_sm3_tfm;
	}
	e_desc->tfm = sm3_tfm;

	e_hash = kmalloc(32, GFP_KERNEL);
	if (!e_hash) {
		ret = -ENOMEM;
		goto out_e_desc;
	}

	ret = crypto_shash_init(e_desc);
	if (ret)
		goto out_e_hash;

	ret = crypto_shash_update(e_desc, za, 32);
	if (ret)
		goto out_e_hash;

	ret = crypto_shash_update(e_desc, sm2_message, msg_len);
	if (ret)
		goto out_e_hash;

	ret = crypto_shash_final(e_desc, e_hash);
	if (ret)
		goto out_e_hash;

	// Prepare buffer: [DER signature][e_hash]
	size_t total_len = sm2_signature_len + 32;

	sig_and_hash = kmalloc(total_len, GFP_KERNEL);
	if (!sig_and_hash) {
		ret = -ENOMEM;
		goto out_e_hash;
	}

	memcpy(sig_and_hash, sm2_signature, sm2_signature_len);
	memcpy(sig_and_hash + sm2_signature_len, e_hash, 32);

	tfm = crypto_alloc_akcipher("sm2", 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("Failed to allocate SM2 akcipher: %ld\n", PTR_ERR(tfm));
		ret = PTR_ERR(tfm);
		goto out_buf;
	}

	req = akcipher_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		pr_err("Failed to allocate akcipher request\n");
		crypto_free_akcipher(tfm);
		ret = -ENOMEM;
		goto out_buf;
	}

	ret = crypto_akcipher_set_pub_key(tfm, pub_key, sm2_public_key_len);
	if (ret) {
		pr_err("Failed to set public key: %d\n", ret);
		goto out_req;
	}

	sg_init_one(&sg, sig_and_hash, total_len);
	akcipher_request_set_crypt(req, &sg, NULL, sm2_signature_len, 32);

	ret = crypto_akcipher_verify(req);
	if (ret == 0)
		pr_info("[OK!] SM2 signature verified successfully!\n");
	else if (ret == -EBADMSG)
		pr_err("[ERROR!] SM2 signature verification failed: invalid signature\n");
	else
		pr_err("[ERROR!] SM2 verify error: %d\n", ret);

out_req:
	akcipher_request_free(req);
	crypto_free_akcipher(tfm);
out_buf:
	kfree(sig_and_hash);
out_e_hash:
	kfree(e_hash);
out_e_desc:
	kfree(e_desc);
out_sm3_tfm:
	if (!IS_ERR(sm3_tfm))
		crypto_free_shash(sm3_tfm);
out_za:
	kfree(za);
out_za_tfm:
	crypto_free_shash(za_desc->tfm);
out_za_desc:
	kfree(za_desc);
out_pub:
	kfree(pub_key);
out_fail:
	return (ret == 0);
}

static int __init sm2_verify_init(void)
{
	pr_info("Loading SM2 signature verifier (with ZA)...\n");
	pr_info("Message: %s\n", sm2_message);
	pr_info("Signature length: %zu\n", sm2_signature_len);
	pr_info("Public key length: %zu\n", sm2_public_key_len);

	if (!verify_sm2_signature())
		return -EPERM;

	return 0;
}

static void __exit sm2_verify_exit(void)
{
	pr_info("Unloading SM2 signature verifier.\n");
}

module_init(sm2_verify_init);
module_exit(sm2_verify_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CTyunOS Security Team");
MODULE_DESCRIPTION("SM2 Verifier using sm2 akcipher (Kernel 6.6)");
MODULE_VERSION("1.2");
