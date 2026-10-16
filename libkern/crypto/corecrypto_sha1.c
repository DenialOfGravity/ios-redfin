#include <libkern/crypto/crypto_internal.h>
#include <libkern/crypto/sha1.h>
#include <kern/debug.h>
#include <corecrypto/ccdigest.h>


static uint64_t
getCount(SHA1_CTX *ctx)
{
	return ctx->c.b64[0];
}

static void
setCount(SHA1_CTX *ctx, uint64_t count)
{
	ctx->c.b64[0] = count;
}

#if defined(ARM64_BOARD_CONFIG_SWEET)
/*
 * SWEET has no corecrypto kext (g_crypto_funcs stays NULL). SHA-1 uses
 * BearSSL's portable implementation (libkern/crypto/sweet_sha1.c), with
 * SHA1_CTX carrying exactly its state: chaining value, byte count and the
 * partial block, converted around each call like the corecrypto path does.
 */
#include "sweet_bearssl/inc/bearssl_hash.h"

static void
SHA1ToBr(SHA1_CTX *sha1_ctx, br_sha1_context *br_ctx)
{
	br_sha1_init(br_ctx);
	memcpy(br_ctx->val, sha1_ctx->h.b32, sizeof(br_ctx->val));
	br_ctx->count = getCount(sha1_ctx);
	memcpy(br_ctx->buf, sha1_ctx->m.b8, sizeof(br_ctx->buf));
}

static void
BrToSHA1(const br_sha1_context *br_ctx, SHA1_CTX *sha1_ctx)
{
	memcpy(sha1_ctx->h.b32, br_ctx->val, sizeof(sha1_ctx->h.b32));
	setCount(sha1_ctx, br_ctx->count);
	memcpy(sha1_ctx->m.b8, br_ctx->buf, sizeof(sha1_ctx->m.b8));
}

void
SHA1Init(SHA1_CTX *ctx)
{
	br_sha1_context br_ctx;

	br_sha1_init(&br_ctx);
	BrToSHA1(&br_ctx, ctx);
}

void
SHA1Update(SHA1_CTX *ctx, const void *data, size_t len)
{
	br_sha1_context br_ctx;

	SHA1ToBr(ctx, &br_ctx);
	br_sha1_update(&br_ctx, data, len);
	BrToSHA1(&br_ctx, ctx);
}

void
SHA1Final(void *digest, SHA1_CTX *ctx)
{
	br_sha1_context br_ctx;

	SHA1ToBr(ctx, &br_ctx);
	br_sha1_out(&br_ctx, digest);
}

#else /* !ARM64_BOARD_CONFIG_SWEET */

/* Copy a ccdigest ctx into a legacy SHA1 context */
static void
DiToSHA1(const struct ccdigest_info *di, struct ccdigest_ctx *di_ctx, SHA1_CTX *sha1_ctx)
{
	setCount(sha1_ctx, ccdigest_nbits(di, di_ctx) / 8 + ccdigest_num(di, di_ctx));
	memcpy(sha1_ctx->m.b8, ccdigest_data(di, di_ctx), di->block_size);
	memcpy(sha1_ctx->h.b8, ccdigest_state_ccn(di, di_ctx), di->state_size);
}

/* Copy a legacy SHA1 context into a ccdigest ctx  */
static void
SHA1ToDi(const struct ccdigest_info *di, SHA1_CTX *sha1_ctx, struct ccdigest_ctx *di_ctx)
{
	uint64_t count = getCount(sha1_ctx);

	ccdigest_num(di, di_ctx) = (unsigned)(count % di->block_size);
	ccdigest_nbits(di, di_ctx) = (count - ccdigest_num(di, di_ctx)) * 8;
	memcpy(ccdigest_data(di, di_ctx), sha1_ctx->m.b8, di->block_size);
	memcpy(ccdigest_state_ccn(di, di_ctx), sha1_ctx->h.b8, di->state_size);
}

void
SHA1Init(SHA1_CTX *ctx)
{
	const struct ccdigest_info *di = g_crypto_funcs->ccsha1_di;
	ccdigest_di_decl(di, di_ctx);

	g_crypto_funcs->ccdigest_init_fn(di, di_ctx);

	DiToSHA1(di, di_ctx, ctx);
}

void
SHA1Update(SHA1_CTX *ctx, const void *data, size_t len)
{
	const struct ccdigest_info *di = g_crypto_funcs->ccsha1_di;
	ccdigest_di_decl(di, di_ctx);

	SHA1ToDi(di, ctx, di_ctx);
	g_crypto_funcs->ccdigest_update_fn(di, di_ctx, len, data);
	DiToSHA1(di, di_ctx, ctx);
}

void
SHA1Final(void *digest, SHA1_CTX *ctx)
{
	const struct ccdigest_info *di = g_crypto_funcs->ccsha1_di;
	ccdigest_di_decl(di, di_ctx);

	SHA1ToDi(di, ctx, di_ctx);
	ccdigest_final(di, di_ctx, digest);
}

#endif /* !ARM64_BOARD_CONFIG_SWEET */

/* This is not publicised in header, but exported in libkern.exports */
void SHA1Final_r(SHA1_CTX *context, void *digest);
void
SHA1Final_r(SHA1_CTX *context, void *digest)
{
	SHA1Final(digest, context);
}


/*
 * This function is called by the SHA1 hardware kext during its init.
 * This will register the function to call to perform SHA1 using hardware.
 */
#include <sys/types.h>
#include <libkern/OSAtomic.h>
#include <sys/systm.h>

typedef kern_return_t (*InKernelPerformSHA1Func)(void *ref, const void *data, size_t dataLen, u_int32_t *inHash, u_int32_t options, u_int32_t *outHash, Boolean usePhysicalAddress);
void sha1_hardware_hook(Boolean option, InKernelPerformSHA1Func func, void *ref);
static void *SHA1Ref;
static InKernelPerformSHA1Func performSHA1WithinKernelOnly;

void
sha1_hardware_hook(Boolean option, InKernelPerformSHA1Func func, void *ref)
{
	if (option) {
		// Establish the hook. The hardware is ready.
		OSCompareAndSwapPtr((void*)NULL, (void*)ref, (void * volatile*)&SHA1Ref);

		if (!OSCompareAndSwapPtr((void *)NULL, (void *)func, (void * volatile *)&performSHA1WithinKernelOnly)) {
			panic("sha1_hardware_hook: Called twice.. Should never happen");
		}
	} else {
		// The hardware is going away. Tear down the hook.
		performSHA1WithinKernelOnly = NULL;
		SHA1Ref = NULL;
	}
}
