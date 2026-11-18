/*
 * SweetMesa <-> sweetfpcd bridge: the kernel side's authoritative header (ws-mesa-a, kernel patch 1192
 * "SweetMesa", iokit/Kernel/arm/SweetMesa.cpp). Part 1 is the FIXED interface (coordinator 2026-11-18) and is
 * identical to part 1 of sweetfpcd's phone/fpc/fpcd_bridge.h (same names and sizes): a file may include both
 * headers; fpcd_bridge.h can wrap its part 1 in #ifndef SMB_IFACE_DEFINED.
 *
 * Objects (all published only while kern.sweet_mesa bit 0 is set; default 0 = nothing exists):
 *   AppleMesaSEPDriver (on the platform expert) -> AppleBiometricServices -> AppleBiometricServicesUserClient
 *     the Touch ID persona that Apple's unmodified biometrickitd (-module 1, libBKDM1) opens: ONE selector (0),
 *     struct in = bm_cmd ('BM' u16 magic 0x4d42, u16 cmd, u16 version, u16 value, payload) or legacy 'CMCM',
 *     struct out = the command's raw out struct, outSize = its exact size (libBKDM1 checks it).
 *   SweetMesaBridge (on IOResources), user client type 0, root only (kIOClientPrivilegeAdministrator):
 *     sweetfpcd's end. Selectors below.
 *
 * 1192b: 12 Cancel, 38 FingerDetectMode, 69 EnableBackgroundFdet are POSTED: biometrickitd gets success at once,
 * sweetfpcd gets them via WAIT like any request; its REPLY is only logged (reply anyway, immediately). Every other
 * forwarded command waits at most kern.sweet_mesa_fwd_ms (default 300 ms), then gets the offline answer.
 * Sysctls: kern.sweet_mesa (bit0 publish, bit1 translate sweetfpcd's "FPCD" EVENTs into Apple messages (1192d;
 * clear = logged only), bit2 hex-dump the first 32 bytes of every command, bit3 log struct sizes inline/descriptor),
 * kern.sweet_mesa_match (default 0: no match record is ever built), kern.sweet_mesa_fwd_ms (default 300),
 * kern.sweet_mesa_state (read-only text).
 *
 * VERSION 2 (1192d, 2026-11-18): part 2b (fpcd_event v2) and part 3 (the Apple message layouts reversed from the
 * iPhone 8 libBKDM1 + BiometricSupport) are new; part 1 is unchanged.
 */
#ifndef SWEETMESA_BRIDGE_H
#define SWEETMESA_BRIDGE_H
#include <stdint.h>

/* ======== part 1: the fixed kernel interface ======== */
#ifndef SMB_IFACE_DEFINED
#define SMB_IFACE_DEFINED	1
#define SMB_SERVICE		"SweetMesaBridge"	/* IOServiceMatching(SMB_SERVICE) */
#define SMB_UC_TYPE		0
#define SMB_SEL_WAIT		0	/* scalar in: timeout_ms; struct out: smb_req; kIOReturnTimeout (0xe00002d6) */
#define SMB_SEL_REPLY		1	/* struct in: smb_reply (seq of the request being answered) */
#define SMB_SEL_EVENT		2	/* struct in: smb_event */
#define SMB_SEL_STATE		3	/* struct out: smb_state */
#define SMB_DATA_MAX		16376
#define SMB_MSG_BASE		0xe3ff8000u

struct smb_req {
	uint32_t seq;		/* != 0; echo it in smb_reply.seq */
	uint32_t cmd;		/* Apple bm_cmd id 1..87 (table below); private ids >= 0x100 are sweetfpcd's own */
	uint32_t len;		/* bytes used in data */
	uint8_t data[SMB_DATA_MAX];
};
struct smb_reply {
	uint32_t seq;
	int32_t status;		/* 0 ok; negative errno (FPCD_E_*), or an IOReturn (< -4096) passed through as is */
	uint32_t len;
	uint8_t data[SMB_DATA_MAX];
};
struct smb_event {
	uint32_t subtype;	/* bits 0-7: Apple message index 1..0xd (-> 0xe3ff8000 + index);
				 * bits 8-31: the header's "value"/ordinal field (0 for sweetfpcd's own events) */
	uint32_t len;
	uint8_t data[SMB_DATA_MAX];
};
struct smb_state {
	uint32_t version;	/* SMB_STATE_VERSION */
	uint32_t flags;		/* SMB_SF_* */
	uint32_t pending;	/* requests queued or sent and not yet answered (0 or 1: commands are serialised) */
	uint32_t clients;	/* AppleBiometricServicesUserClient connections (biometrickitd, MobileGestalt) */
};
#endif /* SMB_IFACE_DEFINED */

#define SMB_STATE_VERSION	1
#define SMB_BRIDGE_VERSION	2	/* this header's layouts (parts 2b, 3) */
#define SMB_SF_PUBLISHED	(1u << 0)	/* kern.sweet_mesa bit 0: persona + bridge exist */
#define SMB_SF_MATCH_GATE	(1u << 1)	/* kern.sweet_mesa_match = 1 */
#define SMB_SF_FPCD_RAW		(1u << 2)	/* kern.sweet_mesa bit 1 (1192d: FPCD events translated) */
#define SMB_SF_BRIDGE_OPEN	(1u << 3)	/* at least one bridge client (sweetfpcd) is open */

/* kernel-side limits (sizes in the persona) */
#define SMB_EVENT_INLINE_MAX	0x17	/* EVENT len <= 0x17: data inline in the interest message; larger: queued */
#define SMB_MSGQ_DEPTH		16	/* queued payloads waiting for cmd 43; the oldest is dropped when full */
#define SMB_MATCH_RESULT_MIN	0xc88	/* libBKDM1 matchResult:timestamp: v2 payload size; EVENTs this big are
					 * match-shaped and gated by kern.sweet_mesa_match */

/*
 * The interest message the persona sends (IOGeneralInterest on AppleBiometricServices, messageType 0xe3ff8000).
 * VERIFIED (v2): BiometricSupport MCDMExtractMessageData (iPhone 8 dsc, 0x1ef9f7d74) requires messageType 0xe3ff8000 and
 * reads type +8, version +0xc, timestamp +0x10, ordinal +0x18, size +0x20; ticket 0 -> data at +0x28 (when size != 0),
 * else -[BiometricKitXPCServerMesa performRequestMessageDataCommand:ticket size:size] = cmd 43 with in = u64 ticket,
 * out capacity = size, and libBKDM1 requires outSize == size. Then serviceStatus:version:ordinal:data:timestamp:.
 */
struct smb_apple_msg_hdr {
	uint64_t ticket;	/* 0 = data inline; else the cmd 43 RequestMessageData key */
	uint32_t type;		/* 0xe3ff8000 + index (0x1..0xd) */
	uint32_t version;	/* message struct version (1 or 2, per type below) */
	uint64_t timestamp;	/* mach_absolute_time */
	uint64_t ordinal;	/* 0xe3ff8001: the status code; other types: 0 */
	uint64_t data_len;
	/* uint8_t data[] when inline (data_len <= 0x17) */
};
/* cmd 43 RequestMessageData: in payload u64 ticket; out = the queued data, outSize = its length */

/* ======== part 2: what sweetfpcd receives (smb_req) and should answer (smb_reply) ======== */
/*
 * The kernel parses Apple's bm_cmd and sends sweetfpcd small little-endian payloads (fpcd v1 conventions from
 * fpcd_bridge.h); it builds Apple's out structs itself from the reply. Commands marked C below never reach
 * sweetfpcd. F = forwarded; F/x = forwarded, and answered x locally when no bridge client is open ("offline").
 *
 * Forwarded payloads (smb_req.data):
 *   2 ResetSensor           none
 *   3 EnrollMode            u32 gid = 0 (Apple's v2 cmd_enroll: u32 userID at +4, ACM token after it; 1192d keeps
 *                           the userID as the identities' user for enrol/match records)
 *   4 MatchMode             none
 *   12 Cancel, 14 EnrollContinue, 15, 49, 56, 65, 72, 83   none
 *   13 RemoveTemplate       Apple's 20 raw bytes (u32 user + 16-byte identity uuid, assumed); the uuid of an id
 *                           the kernel reported is SMB_UUID(fid) below -> fid = le32 at data[4+4]
 *   38 FingerDetectMode     u32 enable = 1
 *   69 EnableBackgroundFdet u32 enable = the bm_cmd value field
 *   61,62,63,64             Apple's raw payload (catacomb save/load; not mapped by sweetfpcd v1 -> -45)
 *   66 GetIdentitiesList    none (Apple's u32 user id is kept by the kernel)
 * Replies the kernel converts (anything else: data ignored, status only):
 *   15, 65  u32 -> Apple u32 (outSize 4)        83  u32 0/1 -> Apple u8 (outSize 1)
 *   56      u64 db id -> Apple 16-byte catacomb uuid: le64 id, then 8 zero bytes (outSize 16)
 *   66      struct fpcd_ids -> n x 20-byte Apple entries {u32 user, SMB_UUID(fid)} (outSize 20 n)
 *   61      u32 -> Apple u32 (outSize 4)        62  raw bytes -> Apple out (catacomb blob), as long as it fits
 * status -> IOReturn: 0 success, -45 unsupported, -35 not ready, -16 busy, -22 bad argument, -2 not found,
 *   -5 I/O error, < -4096 passed through, other: kIOReturnError. Offline or no reply in kern.sweet_mesa_fwd_ms:
 *   the offline answer (F/x) or kIOReturnNotReady (no client) / kIOReturnTimeout.
 */
#define SMB_UUID_MAGIC		0x50464d53u	/* "SMFP": Apple identity uuid = { le32 SMB_UUID_MAGIC, le32 fid, 0, 0 } */

/*
 * EVENTs (sel 2), 1192d:
 *   - data starting with le32 0x44435046 "FPCD" (struct fpcd_event, part 2b, >= 64 bytes) is sweetfpcd's format: with
 *     kern.sweet_mesa bit 1 it is TRANSLATED into Apple messages (table in part 3), else only logged;
 *   - any other (raw Apple) payload is delivered as 0xe3ff8000 + index, version 1, ordinal = subtype bits 8-31, except
 *     match-shaped ones (index 2 match record, 0xb match event, or >= SMB_MATCH_RESULT_MIN bytes): always dropped.
 * Nothing canned and no sysctl alone ever produces a match: an identity match record needs an FPCD MATCH event with
 * FPCD_MF_HAT_VERIFIED AND kern.sweet_mesa_match = 1.
 */

/* ======== part 2b: sweetfpcd's event (smb_event.data), little-endian ======== */
#ifndef FPCD_EV_MAGIC
#define FPCD_EV_MAGIC		0x44435046u	/* "FPCD" */
#define FPCD_MF_HAT_VERIFIED	1u		/* MATCH flags: keymaster verified the identify HAT */
enum fpcd_ev_code {
	FPCD_EV_FINGER_DOWN = 1, FPCD_EV_FINGER_UP = 2, FPCD_EV_IMAGE = 3, FPCD_EV_ENROL_PROGRESS = 4,
	FPCD_EV_ENROL_DONE = 5, FPCD_EV_ENROL_FAILED = 6, FPCD_EV_NOMATCH = 7, FPCD_EV_MATCH = 8,
	FPCD_EV_MATCH_WITHHELD = 9, FPCD_EV_TEMPLATES = 10, FPCD_EV_OP_TIMEOUT = 11, FPCD_EV_ERROR = 12,
};
struct fpcd_event {			/* version 1: 64 bytes; version 2 (FPCD_ABI_VERSION 2): 80 bytes */
	uint32_t magic;			/* +0  FPCD_EV_MAGIC */
	uint16_t version;		/* +4  1 or 2 */
	uint16_t code;			/* +6  enum fpcd_ev_code */
	uint32_t evseq, op_gen;		/* +8, +12 */
	int32_t status;			/* +16 FPC status (enrol code, identify status, ...) */
	uint32_t fid;			/* +20 FPC finger id */
	int32_t score;			/* +24 */
	uint32_t remaining, samples;	/* +28, +32 enrol: samples still needed / accepted */
	uint32_t flags;			/* +36 MATCH: FPCD_MF_*; WITHHELD: reason; ENROL_FAILED: FPCD_EF_* */
	uint32_t n, ids[5];		/* +40, +44 id list (TEMPLATES, ENROL_DONE) */
	/* version 2 */
	uint32_t op;			/* +64 1 enrol, 2 match, 3 detect, 0 none */
	uint32_t percent;		/* +68 enrol progress 0..100 (sweetfpcd's samples / (samples + remaining)) */
	int32_t hat_status;		/* +72 MATCH: verifier result (0 verified, < 0 refused / unavailable) */
	uint32_t reserved;		/* +76 */
};
#endif

/*
 * The 87 bm_cmd ids (kernel names from the iPhone 8 AppleMesaSEPDriver), what the persona does, Apple's in
 * payload size from libBKDM1's call site (v = variable, - = no libBKDM1 caller) and the exact out size libBKDM1
 * checks (outSize after the call).
 *   K  kernel (no sweetfpcd involvement)       C  canned answer        F  forwarded       U  kIOReturnUnsupported
 *
 * id  name                        do    in   out   answer / note
 *  1  GetCmdProtocolVersion       C     4    4     u32 2
 *  2  ResetSensor                 F/nr  0    0
 *  3  EnrollMode                  F/nr  68   0     v2
 *  4  MatchMode                   F/nr  v    0     result comes later as EVENTs (match gated)
 *  5  ReadCatacombSU              U     v    0     legacy catacomb (v1)
 *  6  U6                          U
 *  7  U7                          U
 *  8  AlignmentData               U     0    3060
 *  9  RequestCaptureBuffer        U     0    v
 * 10  RequestPreImage             U
 * 11  RequestPostImage            U
 * 12  Cancel                      P/ok  0    0     posted
 * 13  RemoveTemplate              F/nr  20   0
 * 14  EnrollContinue              F/nr  0    0
 * 15  RequestMaxIdentityCount     F/5   0    4     u32
 * 16  GetProvisioningState        C     0    4     u32 0x20 kAppleBiometricSensorProvisioned (also MG, 'CMCM' 0x1b)
 * 17  RequestTopologyOfTemplate   U     20   3060
 * 18  ProvisionSensor             U
 * 19  U19                         U
 * 20  DisplayStatusChanged        C     0    0     value = display state; accepted
 * 21-23 (unnamed)                 U
 * 24  ThermalLevelChanged         C     0    0     accepted
 * 25  GetSignedCalibrationData    U
 * 26  GetCalibrationDataInfo      U
 * 27  GetSerialisedTemplates      U     4    v
 * 28  U28                         U
 * 29  GetSensorCalibrationStatus  C     0    1     u8 0
 * 30  TemplatesExistsAtBoot       C     0    0     accepted
 * 31  PrepareSerialisedTemplates  U     4    4
 * 32  SetCalibrationData          C     v    0     accepted, length logged, discarded
 * 33  RequestCalibrationData      U     0    v
 * 34  GetModuleSerialNumber       C     0    17    "SWEETFPC1020" NUL-padded (also MG, 'CMCM' 0x36)
 * 35  U35                         U
 * 36  CacheCustomPatch            C     v    0     accepted, discarded
 * 37  CalibrateSensorFDET         U
 * 38  FingerDetectMode            P/nr  0    0     posted
 * 39  GetSKSLockState             C     4    4     u32 0 (SweetKeyStore: no passcode, unlocked)
 * 40  GetBiometrickitdInfo        C     0    23    zeros
 * 41  Diagnostics                 U     8    16
 * 42  GetLoggingType              C     0    1     u8 0
 * 43  RequestMessageData          K     8    v     queued EVENT payload by ticket
 * 44  BioLogEnabled               C     0    0     accepted
 * 45  U45                         U
 * 46  GetProtectedConfig          C     4    32    zeros (nothing enabled)
 * 47  SetProtectedConfig          C     60   0     kIOReturnNotPermitted (needs ACM authData; SweetACM later)
 * 48  GetEnabledForUnlock         C     0    1     u8 0
 * 49  NoCatacomb                  F/ok  4    0
 * 50  GetTemplatesValidity        C     4    1     u8 1
 * 51  GetTimestampCollection      U
 * 52  U52                         U
 * 53  GetSensorInfo               U
 * 54  IsDisableTheDevGidSet       U
 * 55  UnprovisionSensor           U
 * 56  GetCatacombId               F/0   4    16    uuid; offline: zeros
 * 57  DropUnlockToken             C     0    0     accepted (no token exists)
 * 58  GetCatacombHash             C     4    33    zeros
 * 59  GetSensorSerialNumber       U
 * 60  GetCatacombState            C     0    8n    0 bytes (no entries)
 * 61  PrepareSaveCatacomb         F/nr  24   4     v2
 * 62  CompleteSaveCatacomb        F/nr  24   v     v2
 * 63  ConfirmSaveCatacomb         F/nr  24   0     v2
 * 64  LoadCatacomb                F/nr  v    0
 * 65  GetFreeIdentityCount        F/5   4    4     u32
 * 66  GetIdentitiesList           F/0   4    20n   offline: 0 entries
 * 67  GetSystemProtectedConfig    C     0    28    zeros
 * 68  SetSystemProtectedConfig    C     68   0     kIOReturnNotPermitted (needs ACM authData)
 * 69  EnableBackgroundFdet        P/ok  0    0     posted
 * 70  TouchIDButtonPressed        U                 (kernel-internal on Apple's side)
 * 71  U71                         U
 * 72  RemoveUserData              F/nr  4    0
 * 73  ForceBioLockout             C     4    0     accepted, logged
 * 74  SaveBioLockoutRecord        U     0    v
 * 75  LoadBioLockoutRecord        C     v    0     accepted, discarded
 * 76  IsXARTAvailable             U
 * 77  SeaCookieHandleMessage      U
 * 78  GetLastMatchEvent           C     0    29    zeros (no match event; Apple's 29-byte layout unknown)
 * 79  GetLastWakeFromHibernation  U
 * 80-82 (unnamed)                 U
 * 83  IsSensorReady               F/0   0    1     u8; offline: 0
 * 84-86 (unnamed)                 U
 * 87  SystemSleepStateChanged     C     0    0     value = sleep state; accepted
 * nr = kIOReturnNotReady offline.
 */

/* ======== part 3: Apple message layouts (VERSION 2, reversed 2026-11-18) ========
 * Sources: iPhone 8 20H392 libBKDM1 v3 (-[BiometricKitXPCServerMesa serviceStatus:version:ordinal:data:timestamp:]
 * @0x20cc8a0f8, statusMessage:withData:timestamp: @0x20cc88864, matchResult:timestamp: @0x20cc878fc,
 * _convertEnrollmentInfoFromV1 @0x20cc8c1e4, +[BLHelper initialize] status names), BiometricSupport
 * (-[BiometricKitXPCServer serviceStatus:type:inValue:] / ...version:ordinal:...). Confidence: H = read from code,
 * M = inferred from code + names, L = guess.
 *
 * type        ver  data (bytes)          libBKDM1 handling
 * 0xe3ff8001  1    any / none            status = ordinal -> base class -> Mesa statusMessage:withData:timestamp: (H)
 *                                        -> clients' statusMessage:. Version 2 is refused here EXCEPT the Mesa ordinals
 *                                        0x39, 0x49, 0x55-0x58, 0x5f, 0x61, 0x62 and 0x64-0x163, which take v2 data
 *                                        as is (v1 data is converted and must have the exact v1 size). (H)
 * 0xe3ff8002  2    >= 0xc88 + 4 n        match record -> matchResult:timestamp: (needs an active MATCH bio op). (H)
 * 0xe3ff8003  2    >= 0x28 (v1: 0x14)    enrol result {u32 user, uuid[16], ...}: identity added (type 1, attribute 0),
 *                                        enrollResult:withTimestamp:, switchToNextBioOperation:1 (ends the enrol). (H)
 * 0xe3ff8004  1    > 0xb                 statistics            0xe3ff8005 1  > 0x10  sensor status (log)   (H)
 * 0xe3ff8006/7     -                     home button pressed / released                                    (H)
 * 0xe3ff8008  1    any                   kernel message (log)  0xe3ff8009 1  any     recovery reason (log) (H)
 * 0xe3ff800a  1    > 5                   SKS lock state {u32 user, u16 state}: bits 0 DeviceLocked, 1 PasscodeLockout,
 *                                        2 BioLockout, 3 UnlockTokenPresent, 4 BeforeFirstUnlock, 5 PasscodeValidated,
 *                                        6 IdentificationLockout, 7 CatacombCorrupted (-> syncTemplateListForUser),
 *                                        9 ApplePayTokenPresent, 10 RemoteUnlocked. NOT a template message. (H)
 * 0xe3ff800b  1    > 0x1c                match event (statistics: matchEventMessage:)                      (H)
 * 0xe3ff800c  -    -                     base class                                                        (H)
 * 0xe3ff800d  1    -                     re-init: initSensor + syncTemplateListForUser(-1), once           (H)
 */
/* 0xe3ff8001 status ordinals (libBKDM1 BLHelper table; H for the numbers, M for the meaning) */
enum mesa_status {
	MESA_ST_NO_ENROLLEES = 0x00, MESA_ST_MAX_TEMPLATE_COUNT = 0x32, MESA_ST_MATCHING_FAILED = 0x33,
	MESA_ST_CREDENTIAL_REMOVED = 0x34, MESA_ST_IMAGE_QUEUE_EMPTY = 0x35, MESA_ST_IMAGE_CAPTURED = 0x37,
	MESA_ST_TEMPLATE_LIST_NOT_UPDATED = 0x39, MESA_ST_MATCHING_STOPPED = 0x3a, MESA_ST_FINGER_ON = 0x3f,
	MESA_ST_FINGER_OFF = 0x40, MESA_ST_ENROLLMENT_COMPLETE = 0x41, MESA_ST_ENROLLMENT_CANCELLED = 0x42,
	MESA_ST_ENROLLMENT_FAILED = 0x43, MESA_ST_ENROLLMENT_TIMEOUT = 0x44, MESA_ST_UNKNOWN_ERROR = 0x45,
	MESA_ST_IMAGE_REJECTED = 0x46, MESA_ST_NO_CALIBRATION = 0x47, MESA_ST_IMAGE_FOR_PROCESSING = 0x48,
	MESA_ST_TEMPLATE_LIST_UPDATED = 0x49, MESA_ST_REQUEST_FINGER_OFF = 0x4a, MESA_ST_SCAN_TOO_SHORT = 0x4e,
	MESA_ST_MATCHING_CANCELLED = 0x50, MESA_ST_IMAGE_WAS_ACCEPTED = 0x5f, MESA_ST_IMAGE_REJECTED_MOVEMENT = 0x62,
	MESA_ST_FINGER_DETECT_CANCELLED = 0x63,
	MESA_ST_ENROLL_PROGRESS_BASE = 0x64,	/* "BaseOffset": 0x64 + percent (libBKDM1 accepts 0x64..0x163); each one
						 * makes libBKDM1 send 14 EnrollContinue (H) */
	MESA_ST_MATCH_EVENT = 0x1f4,
};
#define MESA_ENROLL_INFO_V1_SIZE	0xc80	/* v1 enrollment info: exactly this, else converted to nil (H) */
#define MESA_ENROLL_INFO_V2_SIZE	0xc98	/* v2 = v1[0..0xc60) + 0x18 bytes + v1[0xc60..0xc80) (H); content: L */

/* 0xe3ff8003 enrol result, version 2 (0x28 bytes; v1 = the first 0x14) (H for user/uuid, rest reserved) */
struct mesa_enroll_result_v2 {
	uint32_t user;			/* the identity's user id (iOS userID, from EnrollMode) */
	uint8_t uuid[16];		/* identity uuid; ours = SMB_UUID(fid) = {le32 'SMFP', le32 fid, 0, 0} */
	uint8_t reserved[0x14];
};

/* 0xe3ff8002 match record, version 2: message_match_node_v2_t / match_result (H for every offset named) */
#define MESA_MR_NO_IDENTITY	0xffffffffu
#define MESA_MR_REASON_MASK	0xfu		/* 1 -> NoMatchReason 1 + NoMatchImageQuality; 2, 3 -> reason 2;
						 * (flags & 0xd) == 0 -> NoMatchMatcher */
#define MESA_MR_PREARM		(1u << 4)	/* BKMatchDetailPreArm: no match count, no haptic */
#define MESA_MR_UNLOCKED	(1u << 5)	/* BKMatchDetailUnlocked (SEP unlocked the keybag; NEVER set by us) */
#define MESA_MR_CREDENTIAL	(1u << 6)	/* BKMatchDetailCredentialAdded (SEP gave ACM a credential; NEVER set) */
#define MESA_MR_HOME_BUTTON	(1u << 7)	/* BKMatchDetailHomeButtonPressed */
#define MESA_MR_IGNORED		(1u << 9)	/* BKMatchDetailResultIgnored */
struct mesa_match_result_v2 {
	uint32_t user;			/* +0x000 identity user id, or MESA_MR_NO_IDENTITY (no match) */
	uint8_t uuid[16];		/* +0x004 identity uuid: must name an identity biometrickitd holds (66 / enrol result),
					 *        and its user must equal +0, else an error and no result */
	uint32_t flags;			/* +0x014 MESA_MR_* */
	uint8_t unknown_18[2];		/* +0x018 (not read by matchResult:) */
	uint8_t image_metadata[0x62];	/* +0x01a BKMatchDetailImageMetadata (opaque; zeros) */
	uint8_t topology[0xc08];	/* +0x07c BKMatchDetailTopologyData (opaque; zeros) */
	uint32_t lotl_items;		/* +0xc84 number of "LOTL" user ids that follow */
	/* uint32_t lotl_user_ids[lotl_items];  +0xc88 (0xffffffff entries skipped) */
};
/*
 * Where a HAT / credential goes: NOWHERE on the AP. Neither the match record nor any message carries a token: on
 * real hardware the SEP hands the bio unlock token to SKS/AKS and the credential to ACM internally and only reports
 * flags 5/6 above. A match record from SweetMesa therefore unlocks nothing; flags stay 0.
 *
 * 66 GetIdentitiesList: in u32 user, out n x identity_v1_t (20 bytes: u32 user, uuid[16]); libBKDM1 asserts
 * (size % 20) == 0 (H).
 *
 * 1192d translation (kern.sweet_mesa bit 1), FPCD event -> Apple:
 *   FINGER_DOWN / FINGER_UP     0xe3ff8001 v1 FingerOn 0x3f / FingerOff 0x40
 *   IMAGE                       0xe3ff8001 v1 ImageRejected 0x46
 *   ENROL_PROGRESS              FPC code 0/1: 0xe3ff8001 v2 ordinal 0x64 + percent, 0xc98 zero bytes (queued);
 *                               4/5/6 (sample not taken): ImageRejected
 *   ENROL_DONE (fid)            0xe3ff8003 v2 {user, SMB_UUID(fid)}
 *   ENROL_FAILED                0xe3ff8001 v1 EnrollmentFailed 0x43
 *   OP_TIMEOUT                  EnrollmentTimeout 0x44 (op 1) / MatchingStopped 0x3a
 *   ERROR                       UnknownError 0x45
 *   NOMATCH / MATCH_WITHHELD /  gate closed: MatchingFailed 0x33; gate open: 0xe3ff8002 v2 no-identity record (flags 2)
 *   MATCH without the flag
 *   MATCH + FPCD_MF_HAT_VERIFIED 0xe3ff8002 v2 {user, SMB_UUID(fid), flags 0} only with kern.sweet_mesa_match = 1
 *   TEMPLATES                   logged only
 */

#endif /* SWEETMESA_BRIDGE_H */
