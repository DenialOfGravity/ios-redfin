/*
 * qsee_gk (ws-fpcd-a): the QSEE listener / keymaster64 / gatekeeper code of qseeprobe ws-qsee-b e1bf971
 * (qsee_lsnr.inc), built as a library part for sweetfpcd (qsee_gk.c = qsee_lsnr.inc with QSEE_LSNR_LIB).
 * Same rules as qseeprobe: only keymaster 0x200 / 0x205 / 0x1002 (uid 0x7e000001) are ever built, RPMB writes are
 * relayed only while a verify runs (and only if the kernel is at kern.sweet_rpmb=2, GK blocks 1642..1689), file
 * system requests are served read-only. Not thread-safe; the caller holds the QSEE lock and its own TA mutex.
 * Logs go through say() and /var/local/tz/log/lsnr.log.
 */
#ifndef QSEE_GK_H
#define QSEE_GK_H
#include <stdint.h>

#define QSEE_GK_UID		0x7e000001u	/* the private gatekeeper uid (test passcode) */
#define QSEE_GK_HANDLE		"/var/local/tz/gk/handle-7e000001"
#define QSEE_HAT_LEN		69		/* hw_auth_token_t */

/*
 * Credential source for the enrol authorisation: called inside the listener window with the keymaster64 app id
 * and the FPC enrol challenge; must put a gatekeeper HAT for that challenge into hat and return 0.
 */
typedef int (*qsee_hat_source_fn)(void *ctx, uint32_t km_appid, uint64_t challenge, uint8_t hat[QSEE_HAT_LEN]);

/* gatekeeper VERIFY 0x1002 (uid 0x7e000001, saved handle) with challenge; RPMB GK writes relayed during it.
 * Only valid inside qsee_fpc_authorize_enrol's window (listeners up). 0 and *hat_len = 69 on success. */
int qsee_gk_verify(uint32_t km_appid, uint64_t challenge, const char *pw, uint8_t hat[QSEE_HAT_LEN], uint32_t *hat_len);

/*
 * The FPC hw-auth chain (fpc_tee_init_hw_auth / get_enrol_challenge / authorize_enrol): listeners up, keymaster 0x205
 * auth-token key blob -> FPC (3,5) init_hw_auth -> (3,2) enrol challenge -> src(challenge) -> (3,3) authorize_enrol
 * with that HAT; listeners down. Returns 0 when the TA accepted the HAT, else a step code:
 * 1 keymaster lookup/listeners, 2 KM 0x205, 3 init_hw_auth, 4 challenge, 5 credential source, 6 authorize_enrol,
 * 7 a write attempt outside the allowed range was seen (run halted). *ta_status = the failing TA/KM status.
 */
int qsee_fpc_authorize_enrol(uint32_t fpc_appid, qsee_hat_source_fn src, void *ctx, int32_t *ta_status);

/* FPC g3 auth commands for identify (fpc_tee_set_auth_challenge (3,1) / fpc_tee_get_auth_result (3,4)) */
int qsee_fpc_set_auth_challenge(uint32_t fpc_appid, uint64_t challenge, int32_t *ta_status);
int qsee_fpc_get_auth_result(uint32_t fpc_appid, uint8_t hat[QSEE_HAT_LEN], int32_t *ta_status);

/* hw_auth_token_t fields (version u8, challenge u64 LE, user_id u64 LE, authenticator_id u64 LE, type u32 BE,
 * timestamp u64 BE, hmac[32]) */
struct qsee_hat_info {
	uint8_t version;
	uint64_t challenge, user_id, authenticator_id, timestamp;
	uint32_t authenticator_type;	/* 1 password, 2 fingerprint */
};
void qsee_hat_parse(const uint8_t hat[QSEE_HAT_LEN], struct qsee_hat_info *i);

#endif
