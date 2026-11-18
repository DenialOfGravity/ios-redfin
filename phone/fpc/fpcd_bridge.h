/*
 * sweetfpcd <-> SweetMesa kernel bridge (ws-fpcd-a). Two parts:
 *
 * 1. The kernel interface, FIXED by the coordinator (2026-11-18). The SweetMesa agent will push its own
 *    phone/fpc/sweetmesa_bridge.h on ws-mesa-a; when it exists, it is authoritative and this part must match it
 *    (sizes are checked below).
 *      IOService "SweetMesaBridge" on IOResources, user client type 0, root only.
 *      sel 0 WAIT : scalar in timeout_ms; struct out smb_req; kIOReturnTimeout on timeout
 *      sel 1 REPLY: struct in smb_reply
 *      sel 2 EVENT: struct in smb_event (kernel sends async message 0xe3ff8000 + subtype to biometrickitd)
 *      sel 3 STATE: struct out smb_state
 *    smb_req.cmd = Apple's Mesa bm_cmd id (1..87, names in fpcd_cmd_name(); logs/research/fingerprint-p3-ios-2026-11-18.md
 *    section 2.2) or one of sweetfpcd's private ids (FPCD_CMD_*, >= 0x100).
 *
 * 2. sweetfpcd's payloads ("fpcd v1"), PROPOSED, to be aligned with sweetmesa_bridge.h: the kernel persona parses
 *    Apple's cmd_*_in structs and passes sweetfpcd small fixed little-endian payloads; sweetfpcd answers with the
 *    payloads below and never builds Apple structs itself. Unknown or short payloads take the defaults given.
 */
#ifndef FPCD_BRIDGE_H
#define FPCD_BRIDGE_H
#include <stdint.h>

/* ---- part 1: kernel interface ---- */
#define SMB_SERVICE		"SweetMesaBridge"
#define SMB_UC_TYPE		0
#define SMB_SEL_WAIT		0
#define SMB_SEL_REPLY		1
#define SMB_SEL_EVENT		2
#define SMB_SEL_STATE		3
#define SMB_DATA_MAX		16376
#define SMB_MSG_BASE		0xe3ff8000u

struct smb_req {
	uint32_t seq;
	uint32_t cmd;
	uint32_t len;
	uint8_t data[SMB_DATA_MAX];
};
struct smb_reply {
	uint32_t seq;
	int32_t status;
	uint32_t len;
	uint8_t data[SMB_DATA_MAX];
};
struct smb_event {
	uint32_t subtype;
	uint32_t len;
	uint8_t data[SMB_DATA_MAX];
};
struct smb_state {
	uint32_t version;
	uint32_t flags;
	uint32_t pending;
	uint32_t clients;
};
_Static_assert(sizeof(struct smb_req) == 16388, "smb_req");
_Static_assert(sizeof(struct smb_reply) == 16388, "smb_reply");
_Static_assert(sizeof(struct smb_event) == 16384, "smb_event");

/* ---- part 2: fpcd v1 payloads (proposed) ---- */
#define FPCD_ABI_VERSION	1

/* smb_reply.status: 0 ok, else one of these (negative errno values); a TA status, when there is one, is in data[0..3] */
#define FPCD_OK			0
#define FPCD_E_IO		(-5)	/* TZ/transport error, or the TA returned a non-zero status (data: i32 TA status) */
#define FPCD_E_BUSY		(-16)
#define FPCD_E_NOENT		(-2)	/* finger id not in the TA's list */
#define FPCD_E_INVAL		(-22)
#define FPCD_E_NOTREADY		(-35)	/* TA not loaded / sensor not initialised */
#define FPCD_E_UNSUPPORTED	(-45)	/* command not mapped (yet) */

/* request payloads (smb_req.data), all optional:
 *   3 EnrollMode            u32 gid (default 0)
 *   13 RemoveTemplate       u32 FPC finger id (FPCD_FID_ALL = every id)
 *   38 FingerDetectMode,
 *   69 EnableBackgroundFdet u32 enable (default 1; 0 stops a running detect)
 *   4 MatchMode             none (identify against every id the TA holds; the kernel filters identities)
 * reply payloads (smb_reply.data):
 *   1 GetCmdProtocolVersion u32 2                 83 IsSensorReady   u32 0/1
 *   66 GetIdentitiesList    struct fpcd_ids       65 GetFreeIdentityCount u32   15 RequestMaxIdentityCount u32 5
 *   56 GetCatacombId        u64 FPC template db id (g11/10)
 *   78 GetLastMatchEvent    struct fpcd_event of the last delivered match (len 0 if none)
 *   FPCD_CMD_STATUS         struct fpcd_status
 */
#define FPCD_FID_ALL		0xffffffffu
#define FPCD_CMD_STATUS		0x100	/* daemon status */
#define FPCD_CMD_WIPE_ALL	0x101	/* debug: delete every finger id in the TA */
#define FPCD_CMD_PING		0x102	/* reply: "sweetfpcd <version>" */

struct fpcd_ids {
	uint32_t count;
	uint32_t ids[5];
};

/* events: smb_event.subtype by code (fpcd_event_subtype()), data = struct fpcd_event */
enum fpcd_ev_code {
	FPCD_EV_FINGER_DOWN = 1,	/* gpio112 woke the armed sensor */
	FPCD_EV_FINGER_UP = 2,		/* re-armed sensor stayed quiet for FPCD_UP_MS (heuristic) */
	FPCD_EV_IMAGE = 3,		/* image not usable: status = cmd 4 status (3 insufficient, 2 error) */
	FPCD_EV_ENROL_PROGRESS = 4,	/* status = enrol code (0 done, 1 progress, 4 partial, 5, 6 dirty), remaining, samples */
	FPCD_EV_ENROL_DONE = 5,		/* status = end_enrol TA status, fid, flags (FPCD_EF_*) */
	FPCD_EV_ENROL_FAILED = 6,	/* status = enrol code or FPC error */
	FPCD_EV_NOMATCH = 7,		/* status = identify TA status (!= 0), score, attempts in samples */
	FPCD_EV_MATCH = 8,		/* only from match_deliver(): identify status 0, allow-match present; flags FPCD_MF_* */
	FPCD_EV_MATCH_WITHHELD = 9,	/* identify said 0 but a gate refused it (flags = reason FPCD_MW_*) */
	FPCD_EV_TEMPLATES = 10,		/* the TA's id list changed: n, ids */
	FPCD_EV_OP_TIMEOUT = 11,	/* the operation ran FPCD_OP_TIMEOUT_MS without finishing */
	FPCD_EV_ERROR = 12,		/* TZ/TA error during an operation: status */
};
#define FPCD_EF_END_UNAUTH	1u	/* end_enrol failed (no HAT/HMAC key); the id came from get_finger_ids */
#define FPCD_MF_HAT_VERIFIED	1u	/* keymaster verified the HAT (step 5; never set yet) */
#define FPCD_MW_NO_ALLOW_FILE	1u
#define FPCD_MW_UNKNOWN_FID	2u
#define FPCD_MW_HAT_REFUSED	3u

struct fpcd_event {
	uint32_t magic;		/* FPCD_EV_MAGIC */
	uint16_t version;	/* FPCD_ABI_VERSION */
	uint16_t code;		/* enum fpcd_ev_code */
	uint32_t evseq;		/* per-daemon counter */
	uint32_t op_gen;	/* operation generation (changes on every Enroll/Match/Detect start) */
	int32_t status;
	uint32_t fid;
	int32_t score;
	uint32_t remaining;
	uint32_t samples;
	uint32_t flags;
	uint32_t n;
	uint32_t ids[5];
};
#define FPCD_EV_MAGIC		0x44435046u	/* "FPCD" */

static inline uint32_t fpcd_event_subtype(unsigned code)
{
	switch (code) {
	case FPCD_EV_TEMPLATES: return 0xa;	/* Apple 0xe3ff800a: template list / lockout update */
	case FPCD_EV_MATCH: return 0xb;		/* Apple 0xe3ff800b: match event */
	default: return 0x1;			/* Apple 0xe3ff8001: sensor/bio status */
	}
}

struct fpcd_status {
	uint32_t abi;
	uint32_t appid;
	uint32_t op;		/* 0 none, 1 enrol, 2 match, 3 detect */
	uint32_t op_gen;
	uint32_t finger_on;
	uint32_t allow_match;	/* /var/local/fpc/allow-match exists */
	uint32_t captures, good_images, matches, nomatches, withheld;
	struct fpcd_ids ids;
};

#endif
