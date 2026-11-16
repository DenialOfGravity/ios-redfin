#ifndef ENGLOCK_H
#define ENGLOCK_H
/*
 * englock: QMI LOC engine lock (SET_ENGINE_LOCK 0x003A / GET_ENGINE_LOCK 0x003B) encoding for sweetlocd's switchable
 * unlock (loc.c, word unlock=mi / unlock=none; Claude 2026-11-16, ws-gnss-e).
 *
 * Wire layout (Qualcomm location_service_v02.h / .c, CLO vendor/qcom-opensource/location LA.UM.9.12):
 *   SET req : 0x01 lockType u32 (mandatory, qmiLocLockEnumT_v02), 0x10 subType u32 (opt), 0x11 lockClient u64 (opt,
 *             "lockClient overwrites lockType"). SET ind: 0x01 status u32.
 *   GET req : 0x10 subType u32 (opt; default DD sub; ALL_SUB invalid). GET ind: 0x01 status, 0x10 lockType u32,
 *             0x11 subType u32, 0x12 lockClient u64 (all optional).
 *   lockType: 1 NONE, 2 MI (lock AFW/mobile-initiated), 3 MT (lock NFW/network-initiated), 4 ALL.
 *   lockClient mask: 0x1 AFW (application framework = MI), 0x2 NFW (non-framework: NI/E911/SUPL-MT), 0x4 privileged.
 * Our modem (redfin, LOC rev 0x83) answers GET with only TLV 0x12 = 3: AFW + NFW locked (= lockType ALL). A START from
 * us is an AFW (MI) session, hence QMI error + session status 7 ENGINE_LOCKED.
 *
 * Everything is expressed as a lock MASK (bit 0 MI/AFW, bit 1 NI/NFW, bit 2 privileged); lockType maps 1:1 to bits 0-1.
 */
#include <stddef.h>
#include <stdint.h>
#include "qmi.h"

#define LOC_SET_ENGINE_LOCK	0x003a
#ifndef LOC_GET_ENGINE_LOCK
#define LOC_GET_ENGINE_LOCK	0x003b
#endif

#define LK_TYPE_NONE	1
#define LK_TYPE_MI	2
#define LK_TYPE_MT	3
#define LK_TYPE_ALL	4
#define LK_SUB_DV	1
#define LK_SUB_DD	2
#define LK_SUB_ALL	3		/* what the QTI HAL (LocApiV02::setGpsLockSync) always sends with SET */
#define LK_AFW		0x1ull
#define LK_NFW		0x2ull
#define LK_PRIV		0x4ull

#ifndef LOCK_ORIG			/* the host test points it elsewhere */
#define LOCK_ORIG	"/var/local/locd.lock-orig"
#endif

struct lk_val {
	int has_status, has_type, has_sub, has_client;
	uint32_t status, type, sub;
	uint64_t client;
};

uint32_t lk_tlv_get(uint8_t *out, unsigned subtype);	/* subtype 0: no TLV (modem default = DD sub) */
uint32_t lk_tlv_set(uint8_t *out, uint32_t type, int with_client, uint64_t client);	/* + subType ALL_SUB */
void lk_parse(const struct qmi_msg *m, struct lk_val *v);	/* GET or SET indication */
int64_t lk_mask(const struct lk_val *v);	/* lockClient if present, else from lockType; -1 if neither */
uint32_t lk_type_of_mask(uint64_t mask);	/* bits 0-1 only */
uint64_t lk_mask_of_type(uint32_t type);	/* unknown type: ~0 */
const char *lk_str(uint64_t mask, char *b, size_t n);
/* the original value, written before any SET (tmp + fsync + rename); tries = repair runs so far */
int lk_save(const char *path, uint64_t mask, unsigned tries);
int lk_load(const char *path, uint64_t *mask, unsigned *tries);
#endif
