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
 * Sysctls: kern.sweet_mesa (bit0 publish, bit1 also deliver "FPCD"-format EVENTs raw to biometrickitd,
 * bit2 hex-dump the first 32 bytes of every command), kern.sweet_mesa_match (default 0: every match-shaped
 * EVENT is dropped and logged), kern.sweet_mesa_fwd_ms (default 2000), kern.sweet_mesa_state (read-only text).
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
#define SMB_SF_PUBLISHED	(1u << 0)	/* kern.sweet_mesa bit 0: persona + bridge exist */
#define SMB_SF_MATCH_GATE	(1u << 1)	/* kern.sweet_mesa_match = 1 */
#define SMB_SF_FPCD_RAW		(1u << 2)	/* kern.sweet_mesa bit 1 */
#define SMB_SF_BRIDGE_OPEN	(1u << 3)	/* at least one bridge client (sweetfpcd) is open */

/* kernel-side limits (sizes in the persona) */
#define SMB_EVENT_INLINE_MAX	0x17	/* EVENT len <= 0x17: data inline in the interest message; larger: queued */
#define SMB_MSGQ_DEPTH		16	/* queued payloads waiting for cmd 43; the oldest is dropped when full */
#define SMB_MATCH_RESULT_MIN	0xc88	/* libBKDM1 matchResult:timestamp: v2 payload size; EVENTs this big are
					 * match-shaped and gated by kern.sweet_mesa_match */

/*
 * The interest message the persona sends (IOGeneralInterest on AppleBiometricServices, messageType 0xe3ff8000),
 * as IOBiometricFamily MCDataMessaging::messageClients builds it (iPhone 8 kernelcache):
 */
struct smb_apple_msg_hdr {
	uint64_t ticket;	/* 0 = data inline; else the cmd 43 RequestMessageData key (= timestamp) */
	uint32_t type;		/* 0xe3ff8000 + index (assumed from libBKDM1's switch; U4 in the p3 doc) */
	uint32_t value;		/* the ordinal/value (smb_event.subtype bits 8-31) */
	uint64_t timestamp;	/* mach_absolute_time */
	uint64_t aux;		/* 0 */
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
 *   3 EnrollMode            u32 gid = 0 (Apple's 68-byte v2 cmd_enroll is not decoded yet)
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
 * EVENTs (sel 2). Delivered to biometrickitd as described above, except:
 *   - data starting with le32 0x44435046 "FPCD" (struct fpcd_event) is sweetfpcd's own format, not Apple's:
 *     logged and counted, delivered raw only with kern.sweet_mesa bit 1 (dev);
 *   - match-shaped events are dropped and logged unless kern.sweet_mesa_match = 1: index 0xb (match event),
 *     an FPCD event with code 8 (FPCD_EV_MATCH), or any payload >= SMB_MATCH_RESULT_MIN bytes.
 * Nothing canned and no sysctl alone ever produces a match: only a bridge EVENT, and only through that gate.
 */

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
 * 12  Cancel                      F/ok  0    0
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
 * 38  FingerDetectMode            F/nr  0    0
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
 * 69  EnableBackgroundFdet        F/ok  0    0
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

#endif /* SWEETMESA_BRIDGE_H */
