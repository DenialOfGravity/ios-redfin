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
/* identical to part 1 of ws-mesa-a phone/fpc/sweetmesa_bridge.h (4e0a7c1, authoritative); either header may come first */
#ifndef SMB_IFACE_DEFINED
#define SMB_IFACE_DEFINED	1
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
#endif /* SMB_IFACE_DEFINED */
#ifndef SMB_UUID_MAGIC
#define SMB_UUID_MAGIC		0x50464d53u	/* "SMFP": Apple identity uuid = { le32 SMB_UUID_MAGIC, le32 fid, 0, 0 } */
#endif
/* smb_event.subtype: bits 0-7 = Apple message index (0xe3ff80xx), bits 8-31 = Apple ordinal/value (0 for fpcd's events) */

/* ---- part 2: fpcd v1 payloads (proposed) ---- */
#define FPCD_ABI_VERSION	2	/* 2: struct fpcd_event grew op / percent / hat_status (80 bytes; xnu 1192d) */

/* smb_reply.status: 0 ok, else one of these (negative errno values); a TA status, when there is one, is in data[0..3] */
#define FPCD_OK			0
#define FPCD_E_IO		(-5)	/* TZ/transport error, or the TA returned a non-zero status (data: i32 TA status) */
#define FPCD_E_BUSY		(-16)
#define FPCD_E_NOENT		(-2)	/* finger id not in the TA's list */
#define FPCD_E_INVAL		(-22)
#define FPCD_E_NOTREADY		(-35)	/* TA not loaded / sensor not initialised */
#define FPCD_E_NOTAUTH		(-13)	/* enrol not authorised: end_enrol -1 (no hw-auth chain / HAT yet) */
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
/*
 * Catacomb persistence (xnu 1206, kern.sweet_mesa_catacomb=1; logs/research/touchid-persistence-2026-11-18.md):
 *   61 PrepareSaveCatacomb  Apple component (24 B {u32 user, u32 kind, 0..}): store_template_db into the in-memory
 *                           fpdata directory, stage the "SWCT" blob; reply u32 blob size
 *   FPCD_CMD_CAT_GET        {u32 offset, u32 max} -> blob bytes (the kernel pulls 62's answer in chunks)
 *   63 ConfirmSaveCatacomb  biometrickitd wrote the file: free the staged blob
 *   FPCD_CMD_CAT_PUT        {u32 total, u32 offset, bytes}: stage the blob of a coming 64
 *   64 LoadCatacomb         {u32 FPCD_CAT_LOAD_MAGIC, u32 total, u32 user}: load the staged blob into the TA
 *   49 NoCatacomb           u32 user (kernel catacomb mode): ~0 = master component, acknowledged without a TA change
 */
#define FPCD_CMD_CAT_PUT	0x110
#define FPCD_CMD_CAT_GET	0x111
#define FPCD_CAT_LOAD_MAGIC	0x4c435753u	/* "SWCL" */
#define FPCD_CAT_MAX		(1024 * 1024)

/*
 * The blob (Apple's CatacombSecureData; biometrickitd archives it with the identities in its catacomb file).
 * Little-endian. The Apple identity of finger id F is SMB_UUID(F) = {SMFP, F, 0, 0}, so fids[] is the
 * identity <-> finger map; after a load sweetfpcd checks that the TA lists exactly these ids.
 */
#define SWCT_MAGIC		0x54435753u	/* "SWCT" */
#define SWCT_VERSION		1
struct swct_hdr {
	uint32_t magic;
	uint16_t version, hdr_size;	/* sizeof(struct swct_hdr) */
	uint32_t total;			/* header + files */
	uint32_t crc;			/* crc32 (zlib) of bytes [16, total) */
	uint32_t user;			/* Apple user id of the component */
	uint32_t gid;			/* FPC TA group id (set_gid) */
	uint64_t dbid;			/* template db id (authenticator id) after the store */
	uint32_t nfids, fids[5];
	uint32_t nfiles;		/* then nfiles x {u32 name_len, u32 data_len, name (no NUL), data} */
	uint32_t flags;
	char ta[16];			/* TA name, e.g. "fingerprint" */
	uint32_t reserved[4];
};
_Static_assert(sizeof(struct swct_hdr) == 96, "swct_hdr");

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
	FPCD_EV_ENROL_FAILED = 6,	/* status = enrol code, FPC error, or FPCD_E_NOTAUTH (flag FPCD_EF_END_UNAUTH) */
	FPCD_EV_NOMATCH = 7,		/* status = identify TA status (!= 0), score, attempts in samples */
	FPCD_EV_MATCH = 8,		/* only from match_deliver(): identify status 0, allow-match present; flags FPCD_MF_* */
	FPCD_EV_MATCH_WITHHELD = 9,	/* identify said 0 but a gate refused it (flags = reason FPCD_MW_*) */
	FPCD_EV_TEMPLATES = 10,		/* the TA's id list changed: n, ids */
	FPCD_EV_OP_TIMEOUT = 11,	/* the operation ran FPCD_OP_TIMEOUT_MS without finishing */
	FPCD_EV_ERROR = 12,		/* TZ/TA error during an operation: status */
	FPCD_EV_TA_FRESH = 13,		/* ws-fpcd-b: the TA was loaded empty at start (not resident): the kernel's
					 * catacomb state goes back to "not loaded" so biometrickitd reloads the file */
};
#define FPCD_EF_END_UNAUTH	1u	/* end_enrol refused (no hw-auth chain / HAT): enrol failed, stray id deleted */
#define FPCD_MF_HAT_VERIFIED	1u	/* the HAT verifier accepted this identify's HAT (sweetfpcd hat_verify(); with the
					 * built-in "none" verifier it is never set, so the kernel never builds a match) */
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
	/* version 2 (sweetmesa_bridge.h part 2b; the kernel reads these when len >= 80) */
	uint32_t op;		/* +64 1 enrol, 2 match, 3 detect, 0 none */
	uint32_t percent;	/* +68 enrol progress 0..100 = samples * 100 / (samples + remaining), 100 when done */
	int32_t hat_status;	/* +72 MATCH / WITHHELD: HAT verifier result (0 verified; FPCD_HAT_*) */
	uint32_t reserved;	/* +76 */
};
_Static_assert(sizeof(struct fpcd_event) == 80, "fpcd_event v2");
#define FPCD_EV_MAGIC		0x44435046u	/* "FPCD" */
/* hat_status */
#define FPCD_HAT_VERIFIED	0
#define FPCD_HAT_NONE		(-1)	/* no HAT (get_auth_result failed) */
#define FPCD_HAT_BAD		(-2)	/* HAT fields wrong (version, challenge, type, authenticator id) */
#define FPCD_HAT_NO_VERIFIER	(-3)	/* no keymaster verifier available: never "verified" */
#define FPCD_HAT_REFUSED	(-4)	/* the verifier refused it */

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
