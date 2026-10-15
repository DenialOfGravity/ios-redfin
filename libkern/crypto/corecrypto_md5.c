#include <libkern/crypto/crypto_internal.h>
#include <libkern/crypto/md5.h>
#include <kern/debug.h>
#include <corecrypto/ccdigest.h>

static uint64_t
getCount(MD5_CTX *ctx)
{
	return (((uint64_t)ctx->count[0]) << 32) | (ctx->count[1]);
}

static void
setCount(MD5_CTX *ctx, uint64_t count)
{
	ctx->count[0] = (uint32_t)(count >> 32);
	ctx->count[1] = (uint32_t)count;
}

/* Copy a ccdigest ctx into a legacy MD5 context */
#if defined(ARM64_BOARD_CONFIG_SWEET)
/*
 * SWEET has no corecrypto kext (g_crypto_funcs stays NULL). MD5 uses
 * BearSSL's portable implementation (libkern/crypto/sweet_md5.c), with
 * MD5_CTX carrying exactly its state: chaining value, byte count and the
 * partial block, converted around each call like the corecrypto path does.
 */
#include "sweet_bearssl/inc/bearssl_hash.h"

static void
MD5ToBr(MD5_CTX *md5_ctx, br_md5_context *br_ctx)
{
	br_md5_init(br_ctx);
	memcpy(br_ctx->val, md5_ctx->state, sizeof(br_ctx->val));
	br_ctx->count = getCount(md5_ctx);
	memcpy(br_ctx->buf, md5_ctx->buffer, sizeof(br_ctx->buf));
}

static void
BrToMD5(const br_md5_context *br_ctx, MD5_CTX *md5_ctx)
{
	memcpy(md5_ctx->state, br_ctx->val, sizeof(md5_ctx->state));
	setCount(md5_ctx, br_ctx->count);
	memcpy(md5_ctx->buffer, br_ctx->buf, sizeof(md5_ctx->buffer));
}

void
MD5Init(MD5_CTX *ctx)
{
	br_md5_context br_ctx;

	br_md5_init(&br_ctx);
	BrToMD5(&br_ctx, ctx);
}

void
MD5Update(MD5_CTX *ctx, const void *data, unsigned int len)
{
	br_md5_context br_ctx;

	MD5ToBr(ctx, &br_ctx);
	br_md5_update(&br_ctx, data, len);
	BrToMD5(&br_ctx, ctx);
}

void
MD5Final(unsigned char digest[MD5_DIGEST_LENGTH], MD5_CTX *ctx)
{
	br_md5_context br_ctx;

	MD5ToBr(ctx, &br_ctx);
	br_md5_out(&br_ctx, digest);
}

#else /* !ARM64_BOARD_CONFIG_SWEET */

static void
DiToMD5(const struct ccdigest_info *di, struct ccdigest_ctx *di_ctx, MD5_CTX *md5_ctx)
{
	setCount(md5_ctx, ccdigest_nbits(di, di_ctx) / 8 + ccdigest_num(di, di_ctx));
	memcpy(md5_ctx->buffer, ccdigest_data(di, di_ctx), di->block_size);
	memcpy(md5_ctx->state, ccdigest_state_ccn(di, di_ctx), di->state_size);
}

/* Copy a legacy MD5 context into a ccdigest ctx  */
static void
MD5ToDi(const struct ccdigest_info *di, MD5_CTX *md5_ctx, struct ccdigest_ctx *di_ctx)
{
	uint64_t count = getCount(md5_ctx);

	ccdigest_num(di, di_ctx) = (unsigned)(count % di->block_size);
	ccdigest_nbits(di, di_ctx) = (count - ccdigest_num(di, di_ctx)) * 8;
	memcpy(ccdigest_data(di, di_ctx), md5_ctx->buffer, di->block_size);
	memcpy(ccdigest_state_ccn(di, di_ctx), md5_ctx->state, di->state_size);
}

void
MD5Init(MD5_CTX *ctx)
{
	const struct ccdigest_info *di = g_crypto_funcs->ccmd5_di;
	ccdigest_di_decl(di, di_ctx);

	g_crypto_funcs->ccdigest_init_fn(di, di_ctx);

	DiToMD5(di, di_ctx, ctx);
}

void
MD5Update(MD5_CTX *ctx, const void *data, unsigned int len)
{
	const struct ccdigest_info *di = g_crypto_funcs->ccmd5_di;
	ccdigest_di_decl(di, di_ctx);

	MD5ToDi(di, ctx, di_ctx);
	g_crypto_funcs->ccdigest_update_fn(di, di_ctx, len, data);
	DiToMD5(di, di_ctx, ctx);
}

void
MD5Final(unsigned char digest[MD5_DIGEST_LENGTH], MD5_CTX *ctx)
{
	const struct ccdigest_info *di = g_crypto_funcs->ccmd5_di;
	ccdigest_di_decl(di, di_ctx);

	MD5ToDi(di, ctx, di_ctx);
	ccdigest_final(di, di_ctx, digest);
}

#endif /* !ARM64_BOARD_CONFIG_SWEET */
