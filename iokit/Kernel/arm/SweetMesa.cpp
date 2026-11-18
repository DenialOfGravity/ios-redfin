/*
 * redfin 1192: SweetMesa, a Touch ID (Mesa) persona for Apple's unmodified biometrickitd + libBKDM1, whose real
 * sensor work is done by a userland daemon (sweetfpcd) through a bridge user client.
 *
 * What iOS expects (iPhone 8 kernelcache, logs/research/fingerprint-p3-ios-2026-11-18.md):
 *   AppleMesaSEPDriver -> AppleBiometricServices -> AppleBiometricServicesUserClient, ONE selector (0) with a
 *   variable struct in/out: in = bm_cmd ('BM' u16 0x4d42, u16 cmd 1..87, u16 version, u16 value, payload) or the
 *   legacy 'CMCM' header (MobileGestalt), out = the command's out struct, whose exact size libBKDM1 checks.
 *   Async results are IOGeneralInterest messages 0xe3ff8000 on AppleBiometricServices carrying a 0x28-byte header
 *   (+ data inline when it is <= 0x17 bytes, else queued and fetched with cmd 43 RequestMessageData by ticket).
 *
 * Here: config/identity commands get canned answers (sizes from libBKDM1's outSize checks); sensor work goes to
 * the bridge "SweetMesaBridge" (IOResources, user client type 0, root only; phone/fpc/sweetmesa_bridge.h):
 *   sel 0 WAIT  scalar timeout_ms -> struct smb_req {seq, cmd, len, data}     (kIOReturnTimeout)
 *   sel 1 REPLY struct smb_reply {seq, status, len, data}
 *   sel 2 EVENT struct smb_event {subtype, len, data} -> message 0xe3ff8000 + (subtype & 0xff)
 *   sel 3 STATE struct smb_state {version 1, flags, pending, clients}
 *
 * Trust rule: nothing canned and no sysctl produces a match. A match can only come from a bridge EVENT, and every
 * match-shaped EVENT (message index 0xb, an "FPCD" event with code 8, or a payload >= 0xc88 bytes, the size of
 * libBKDM1's match result) is dropped and logged unless kern.sweet_mesa_match = 1. Forwarded replies never carry
 * a match (MatchMode has no out struct; GetLastMatchEvent is answered "none" here).
 *
 * Switches (all default 0: nothing is published, the kernel behaves as before this patch):
 *   kern.sweet_mesa        bit 0 publish the persona + bridge (write 0 withdraws; EBUSY while a client is open),
 *                          bit 1 translate sweetfpcd's "FPCD" events into Apple messages (1192d; clear: logged only),
 *                          bit 2 hex-dump the first 32 payload bytes of every command,
 *                          bit 3 (1192c) log every call's struct in/out sizes and inline vs descriptor
 *   kern.sweet_mesa_match  1 = let match-shaped bridge EVENTs through (default 0: dropped + logged)
 *   kern.sweet_mesa_fwd_ms how long a forwarded command waits for sweetfpcd (default 300, 50..30000); no reply
 *                          in time = the "offline" answer (1192b)
 * 1192b: 12 Cancel, 38 FingerDetectMode and 69 EnableBackgroundFdet are posted: success at once, queued for the
 * next WAIT, the REPLY is only logged. No lock is held while a caller waits (IOLockSleepDeadline drops gM), and
 * every forward is logged before it is queued ("cmd N -> bridge seq S").
 *   kern.sweet_mesa_state  read-only summary
 * Every command is logged: "SWEET MESA: <proc> cmd N Name v.. val.. in.. cap.. -> ret out.. [how]".
 *
 * 1192d (bridge v2, phone/fpc/sweetmesa_bridge.h part 3): the interest message header is libBKDM1's
 * (BiometricSupport MCDMExtractMessageData): +0x0c is the message VERSION and +0x18 the ORDINAL (1192..c put the
 * value at +0x0c and 0 at +0x18). sweetfpcd's "FPCD" events are TRANSLATED into Apple messages when kern.sweet_mesa
 * bit 1 is set (bit 0 clear: logged only, as before): finger on/off and enrol statuses (0xe3ff8001 v1), enrol
 * progress (0xe3ff8001 v2, ordinal 0x64 + percent, 0xc98-byte enrollment info, zeros), enrol result (0xe3ff8003 v2,
 * {user, SMFP uuid}), no-match (status 0x33, or with the match gate open a 0xe3ff8002 record without identity).
 * A match record WITH an identity (0xe3ff8002) is built only when kern.sweet_mesa_match = 1 AND the FPCD match
 * event carries FPCD_MF_HAT_VERIFIED; raw (non-FPCD) match-shaped EVENTs are always dropped now. Every delivered
 * message is logged ("message 0x... v.. ord.. len.. inline|queued").
 * 1198 (owner decision 2026-11-18): kern.sweet_mesa_match=2 delivers TA-identified matches WITHOUT the keymaster HAT
 *   check ("UNVERIFIED match delivered"): extend-enrolment matches (MatchMode carrying an SMFP identity, phase 2 of
 *   Settings' enrolment) go to the enrolling client with flags 0; other matches unlock the SweetKeyStore keybag
 *   under the passcode rules (sweet_pc_bio_unlock: passcode since boot, 48 h, 5 failed matches, backoff), set
 *   flag 5 Unlocked when they did and arm one SweetACM BiometryMatched credential; refused -> no-identity record.
 *   8 AlignmentData / 17 RequestTopologyOfTemplate answer zeros of the asked size.
 * 1192f: 60 GetCatacombState answers {gUser, state 1} (an empty answer made biometrickitd drop every new identity).
 * 1192e: AppleBiometricServicesUserClient::message accepts 0xe3ff8000 (the aggregate messageClients result was
 * kIOReturnUnsupported from the user client itself, not from the interest notification).
 * 1206: kern.sweet_mesa_catacomb=1 makes enrolled fingers persist the iOS way (default 0 = 1200's behaviour).
 *   biometrickitd (BiometricSupport) owns the catacomb file (/var/root/Library/Catacomb/..., an NSKeyedArchiver with
 *   CatacombVersion / CatacombUserID / CatacombIdentityList / CatacombSecureData); the SEP's part is the opaque
 *   CatacombSecureData blob. Here that blob is sweetfpcd's "SWCT" image of the FPC TA's encrypted template db
 *   (store_template_db / load_template_db through an in-memory file listener), so no file is written by us.
 *   60 GetCatacombState answers {user, 1 | loaded << 1 | dirty << 2} for the identities' user (gUser, else 501):
 *   biometrickitd loads the catacomb when (state & 3) == 1 (checkCatacombForUser:) and at its start for every
 *   present user (loadCatacomb), and saves when bit 2 is set (getCatacombSaveListForComponents:).
 *   61 PrepareSaveCatacomb -> sweetfpcd stores the db and stages the blob, answers its size; 62 CompleteSaveCatacomb
 *   pulls it in chunks (FPCD_CMD_CAT_GET); 63 ConfirmSaveCatacomb clears dirty; 64 LoadCatacomb pushes the blob in
 *   chunks (FPCD_CMD_CAT_PUT) and loads it into the TA. Bulk copies of the user structs go through in_read /
 *   out_write (descriptor: prepare() / complete(), 1192c). A failed load never returns 0x192 / 0x8002 / 0x8003
 *   (libBKDM1 maps those to 0x10d "catacomb corrupted", which deletes the file): the file is always kept.
 *   loaded: set by a successful 64, 49 NoCatacomb for the user, 3 EnrollMode, 13 / 72; cleared by sweetfpcd's
 *   TA_FRESH event (TA loaded empty). dirty: enrol done / TEMPLATES events (id list or template changed).
 *   kern.sweet_mesa_cat_ms: wait for the TA store/load (default 5000). kern.sweet_mesa_cat_state: counters.
 * 1207: kern.sweet_mesa_offline=1 (default 0 = the old answers): while no sweetfpcd is connected, 15 / 49 / 56 / 65 /
 *   66 / 69 answer kIOReturnNotReady instead of "5 free", "success", "0 identities" (which made biometrickitd drop
 *   its identity list, touchid-audit-2026-11-18.md finding 10), and when the first bridge client connects the
 *   persona sends 0xe3ff800d (libBKDM1: initSensor + syncTemplateListForUser) 1.5 s later, so biometrickitd re-reads
 *   the sensor and the identity list from the now-live TA (after sweetfpcd's TA_FRESH event, if any).
 */
#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/sysctl.h>
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBSD.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/OSAtomic.h>

extern "C" {
#include <sys/errno.h>
#include <sys/proc.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
}

#define SAY(fmt, ...) IOLog("SWEET MESA: " fmt, ##__VA_ARGS__)

/* ---- bridge interface (phone/fpc/sweetmesa_bridge.h, part 1) ---- */
#define SMB_DATA_MAX            16376
#define SMB_HDR                 12              /* seq/cmd/len, seq/status/len */
#define SMB_EV_HDR              8               /* subtype/len */
#define SMB_STATE_VERSION       1
#define SMB_SF_PUBLISHED        (1u << 0)
#define SMB_SF_MATCH_GATE       (1u << 1)
#define SMB_SF_FPCD_RAW         (1u << 2)       /* 1192d: kern.sweet_mesa bit 1 = FPCD events translated */
#define SMB_SF_BRIDGE_OPEN      (1u << 3)
#define MESA_MSG                0xe3ff8000u
#define MESA_INLINE_MAX         0x17
#define MESA_MSG_HDR            0x28
#define MESA_MATCH_RESULT_MIN   0xc88
#define MESA_MSGQ               16
#define FPCD_MAGIC              0x44435046u     /* "FPCD" */
#define FPCD_EV_MATCH           8
#define SMFP_MAGIC              0x50464d53u     /* "SMFP": identity uuid {le32 SMFP, le32 fid, 0, 0} */

/* 1192d: struct fpcd_event (sweetmesa_bridge.h part 2b), little-endian offsets */
#define FEV_CODE                6               /* u16 */
#define FEV_OP_GEN              12              /* 1211: sweetfpcd S.gen when the event was sent */
#define FEV_STATUS              16
#define FEV_FID                 20
#define FEV_SCORE               24
#define FEV_REMAINING           28
#define FEV_SAMPLES             32
#define FEV_FLAGS               36
#define FEV_V1_SIZE             64
#define FEV_OP                  64              /* v2: 1 enrol, 2 match, 3 detect */
#define FEV_PERCENT             68              /* v2: enrol progress 0..100 */
#define FEV_V2_SIZE             80
enum { FEV_FINGER_DOWN = 1, FEV_FINGER_UP, FEV_IMAGE, FEV_ENROL_PROGRESS, FEV_ENROL_DONE, FEV_ENROL_FAILED,
       FEV_NOMATCH, FEV_MATCH, FEV_MATCH_WITHHELD, FEV_TEMPLATES, FEV_OP_TIMEOUT, FEV_ERROR,
       FEV_TA_FRESH /* 1206: the TA was loaded empty (sweetfpcd start without a resident TA) */ };
#define FEV_MF_HAT_VERIFIED     1u

/* Apple message indexes (0xe3ff8000 + idx) and 0xe3ff8001 status ordinals (libBKDM1 BLHelper status table) */
#define MIDX_STATUS             0x1
#define MIDX_MATCH_RESULT       0x2             /* message_match_node_v2_t, >= 0xc88 + 4 * lotlItems */
#define MIDX_ENROLL_RESULT      0x3             /* message_enroll_node_v2_t, 0x28 */
#define MIDX_MATCH_EVENT        0xb
#define ST_MATCHING_FAILED      0x33
#define ST_MATCHING_STOPPED     0x3a
#define ST_FINGER_ON            0x3f
#define ST_ENROLL_COMPLETE      0x41            /* 1204 */
#define ST_ENROLL_CANCELLED     0x42            /* 1203: op-ending statuses (libBKDM1 -> switchToNextBioOperation:) */
#define ST_MATCHING_CANCELLED   0x50
#define ST_FDET_CANCELLED       0x63
#define ST_FINGER_OFF           0x40
#define ST_ENROLL_FAILED        0x43
#define ST_ENROLL_TIMEOUT       0x44
#define ST_UNKNOWN_ERROR        0x45
#define ST_IMAGE_REJECTED       0x46
#define ST_ENROLL_PROGRESS_BASE 0x64            /* 0x64 + percent; libBKDM1 answers each with 14 EnrollContinue */
#define ENROLL_INFO_V2_SIZE     0xc98           /* message_status_enrollment_info v2 (v1 0xc80 + 0x18) */
#define ENROLL_RESULT_V2_SIZE   0x28            /* {u32 user, uuid[16], 0x14 reserved} */
#define MR_FLAGS                0x14            /* match record: u32 flags (bits 0-3 no-match reason, 4 PreArm,
                                                 * 5 Unlocked, 6 CredentialAdded, 7 HomeButton, 9 ResultIgnored) */
#define MR_LOTL_ITEMS           0xc84
#define MR_TOPO                 0x7c            /* match topology (0xc08): 12-byte header + 3060-byte node placement */
#define MR_TOPO_SIZE            0xc08
#define TOPO_PLACEMENT          0xc             /* node placement: u16 size 0xbf4, u16 version 3, s16 nodes, ... */
#define TOPO_PLACEMENT_SIZE     0xbf4
static_assert(MR_TOPO + MR_TOPO_SIZE == MR_LOTL_ITEMS && TOPO_PLACEMENT + TOPO_PLACEMENT_SIZE + 8 == MR_TOPO_SIZE,
    "match record topology layout");
#define MR_SIZE                 0xc88
#define MESA_ENTITLEMENT        "com.apple.private.applemesa.allow"

static const char *const mesa_names[88] = {
	"?", "GetCmdProtocolVersion", "ResetSensor", "EnrollMode", "MatchMode", "ReadCatacombSU", "U6", "U7",
	"AlignmentData", "RequestCaptureBuffer", "RequestPreImage", "RequestPostImage", "Cancel", "RemoveTemplate",
	"EnrollContinue", "RequestMaxIdentityCount", "GetProvisioningState", "RequestTopologyOfTemplate",
	"ProvisionSensor", "U19", "DisplayStatusChanged", "U21", "U22", "U23", "ThermalLevelChanged",
	"GetSignedCalibrationData", "GetCalibrationDataInfo", "GetSerialisedTemplates", "U28",
	"GetSensorCalibrationStatus", "TemplatesExistsAtBoot", "PrepareSerialisedTemplates", "SetCalibrationData",
	"RequestCalibrationData", "GetModuleSerialNumber", "U35", "CacheCustomPatch", "CalibrateSensorFDET",
	"FingerDetectMode", "GetSKSLockState", "GetBiometrickitdInfo", "Diagnostics", "GetLoggingType",
	"RequestMessageData", "BioLogEnabled", "U45", "GetProtectedConfig", "SetProtectedConfig",
	"GetEnabledForUnlock", "NoCatacomb", "GetTemplatesValidity", "GetTimestampCollection", "U52",
	"GetSensorInfo", "IsDisableTheDevGidSet", "UnprovisionSensor", "GetCatacombId", "DropUnlockToken",
	"GetCatacombHash", "GetSensorSerialNumber", "GetCatacombState", "PrepareSaveCatacomb",
	"CompleteSaveCatacomb", "ConfirmSaveCatacomb", "LoadCatacomb", "GetFreeIdentityCount", "GetIdentitiesList",
	"GetSystemProtectedConfig", "SetSystemProtectedConfig", "EnableBackgroundFdet", "TouchIDButtonPressed",
	"U71", "RemoveUserData", "ForceBioLockout", "SaveBioLockoutRecord", "LoadBioLockoutRecord",
	"IsXARTAvailable", "SeaCookieHandleMessage", "GetLastMatchEvent", "GetLastWakeFromHibernation", "U80",
	"U81", "U82", "IsSensorReady", "U84", "U85", "U86", "SystemSleepStateChanged",
};

static const char *
mesa_name(uint32_t cmd)
{
	return cmd < 88 ? mesa_names[cmd] : "?";
}

/* ---- state ---- */
static int gMesa;                       /* kern.sweet_mesa */
static int gMatch;                      /* kern.sweet_mesa_match */
static int gOpEnd = 1;                  /* kern.sweet_mesa_opend: Cancel ends the op with its *Cancelled status (1203) */
static volatile UInt32 gOpKind;         /* 1203: running op: 0 none, 1 enrol (3), 2 match (4), 3 finger detect (38) */
static thread_call_t gEndTC;
static int gUnlockDelayMs;              /* kern.sweet_mesa_unlock_delay_ms (1205): 0 now, >0 after the record, -1 never */
static thread_call_t gUnlockTC;
static uint32_t nOpEnd;
/*
 * 1210-1213: kern.sweet_mesa_fix, one bit per fix (clear a bit = the 1203-1205 behaviour for that fix):
 *   bit 0 (0x1) 1210 a 3 EnrollMode / 4 MatchMode / 38 FingerDetectMode that pre-empts a running op ends the old op
 *               with its *Cancelled status (0x42 / 0x50 / 0x63)
 *   bit 1 (0x2) 1211 sweetfpcd events of an op that has ended (Cancel / pre-empted / wrong kind) are dropped
 *   bit 2 (0x4) 1212 8 AlignmentData answers a valid node placement (one node per accepted enrol touch)
 *   bit 3 (0x8) 1213 0x41 EnrollmentComplete after a successful enrol result (1204; off: the result ends the op)
 */
#define MESA_FIX_IMPLICIT_END   0x1u
#define MESA_FIX_STALE_DROP     0x2u
#define MESA_FIX_ALIGNMENT      0x4u
#define MESA_FIX_ENROLL_0X41    0x8u
static volatile uint32_t gAlignN;       /* 1212: accepted enrol touches since the last 3 EnrollMode (AlignmentData nodes) */
static int gFix = 0x7;                  /* kern.sweet_mesa_fix */
static uint32_t nImplicitEnd, nStaleDropped, nAlignAnswered;    /* kern.sweet_mesa_fix_stats */
/* 1210: op-ending statuses waiting for delivery, in order (1203 had one bit per kind, so two ends of one kind gave one) */
#define MESA_ENDQ               8
static struct {
	uint8_t kind;                   /* 1 enrol, 2 match, 3 finger detect */
	uint8_t cause;                  /* 12 Cancel, or the 3/4/38 command that pre-empted the op */
} gEndQ[MESA_ENDQ];
static volatile int gEndHead, gEndCount;
static IOLock *gEndL;                   /* end queue; held while delivering, so the statuses go out in order */
static volatile UInt32 gEndKick;        /* 1210: an implicit end is queued; the thread call runs when the command is done */
/* 1211: op generations */
static int gStaleMs = 750;              /* kern.sweet_mesa_stale_ms: longest a stale sweetfpcd generation is dropped */
static volatile uint32_t gKGen;         /* kernel op generation: +1 at every op start and every op end */
static volatile uint32_t gEvGen;        /* sweetfpcd op_gen of the last event received */
static volatile bool gEvGenSeen;
static volatile bool gStale;            /* an op ended; events still carrying its sweetfpcd generation are dropped */
static volatile uint32_t gStaleGen, gStaleKGen;
static volatile uint64_t gStaleDl;      /* mach_absolute_time deadline of the stale window */
static int gSks = 1;                    /* kern.sweet_mesa_sks: 39 GetSKSLockState follows the passcode rules (1202) */
static int gPcfg = 1;                   /* kern.sweet_mesa_pcfg: 46 GetProtectedConfig bits (1202) */
static int gTopo = 1;                   /* kern.sweet_mesa_topo: synthetic match topology (1201) */
static int gCatState = 3;               /* kern.sweet_mesa_catstate: 60 GetCatacombState value (1200) */
/* 1206: catacomb persistence (kern.sweet_mesa_catacomb) */
static int gCat;                        /* kern.sweet_mesa_catacomb: 0 = 1200 behaviour */
static int gCatMs = 5000;               /* kern.sweet_mesa_cat_ms: wait for sweetfpcd's TA store / load */
static int gOffHonest;                  /* kern.sweet_mesa_offline (1207): 1 = NotReady while offline + 800d on connect */
static thread_call_t gReinitTC;
static SInt32 nReinit;
#define MIDX_REINIT             0xd     /* 0xe3ff800d: libBKDM1 initSensor + syncTemplateList */
#define REINIT_DELAY_MS         1500
static IOLock *gCatLk;                  /* one catacomb transfer at a time */
static volatile bool gCatLoaded, gCatDirty;
static volatile UInt32 gCatGen;         /* +1 per change of the TA db (dirtying event) */
static UInt32 gCatSavedGen;             /* gCatGen when 61 staged the blob */
static uint32_t gCatSize;               /* size 61 answered (0: nothing staged) */
static uint32_t gCatLastLoad, gCatLastSave;
static SInt32 nCatLoads, nCatLoadFail, nCatSaves, nCatSaveFail, nCatConfirms, nCatFresh, nCatDirtyEv;
#define CAT_MAX                 (1024 * 1024)   /* largest catacomb blob */
#define CAT_CHUNK               4032            /* bridge chunk: requests / replies stay <= 4096 (inline) */
#define CAT_DEFAULT_USER        501             /* iOS' mobile user, until a 3/65/66 payload names one */
#define FPCD_CMD_CAT_PUT        0x110           /* {u32 total, u32 offset, data}: stage LoadCatacomb input */
#define FPCD_CMD_CAT_GET        0x111           /* {u32 offset, u32 max} -> bytes of the blob staged by 61 */
#define FPCD_CAT_LOAD_MAGIC     0x4c435753u     /* "SWCL": 64 payload {magic, total, user} = load the staged input */
static int gFwdMs = 300;                /* kern.sweet_mesa_fwd_ms (1192b: was 2000) */
static IOLock *gM;                      /* slot, queue, client counts */
static IOLock *gPub;                    /* publish / withdraw */

enum { SLOT_FREE = 0, SLOT_QUEUED, SLOT_SENT, SLOT_DONE };
static struct {
	int state;
	uint32_t seq, cmd, len;
	int32_t status;
	uint32_t rlen;
	uint8_t *req;                   /* SMB_DATA_MAX */
	uint8_t *rep;                   /* SMB_DATA_MAX */
} slot;
static uint32_t gSeq;
static char gEvQueued, gEvDone, gEvFree;  /* sleep events */

/*
 * 1192b: "posted" commands (12 Cancel, 38 FingerDetectMode, 69 EnableBackgroundFdet) never wait for sweetfpcd:
 * biometrickitd gets success at once (it sends 69/38 from the display-off / lock path), the request is queued
 * here for the next WAIT, and sweetfpcd's REPLY to it is only logged. Depth 8, the oldest is dropped when full.
 */
#define MESA_POSTQ              8
static struct {
	uint32_t seq, cmd, len;
	uint8_t data[8];
} gP[MESA_POSTQ];
static int gPHead, gPCount;
static uint32_t gPostedSeq[16];
static int gPostedIdx;

static struct {
	uint64_t ticket;
	uint32_t len;
	uint8_t *data;
} gQ[MESA_MSGQ];
static uint64_t gLastTicket;

static int gAbsClients, gBridgeClients;
static SInt32 nCmds, nCanned, nFwd, nOffline, nTimeouts, nUnsup, nEvents, nDelivered, nQueued, nMatchDropped,
    nFpcdEv, nMsgPulled, nRefused;
static uint32_t gLastCmd;
static int gLastRet;
/* 1192d */
static uint32_t gUser;                  /* user id of the last 3 EnrollMode / 65 / 66 payload (identities' user) */
static uint32_t gLastPct;               /* last enrol progress percent sent (never goes backwards per enrol) */
static SInt32 nTranslated, nLoggedOnly, nMatchDelivered, nMatchRefused;
/* 1198 */
static bool gMatchExtend;               /* the running MatchMode is an extend-enrolment (Settings phase 2) */
static SInt32 nUnverified, nBioUnlock, nBioRefused;
extern "C" int sweet_pc_bio_unlock(int *did_unlock, const char **why);
extern "C" void sweet_pc_bio_failed(void);
extern "C" int sweet_pc_bio_allowed(const char **why);
extern "C" void sweet_acm_note_verified_match(void);

class AppleMesaSEPDriver;
class AppleBiometricServices;
class SweetMesaBridge;
static AppleMesaSEPDriver *gDrv;
static AppleBiometricServices *gAbs;
static SweetMesaBridge *gBridge;

static inline uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void
put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void
put64(uint8_t *p, uint64_t v)
{
	put32(p, (uint32_t)v);
	put32(p + 4, (uint32_t)(v >> 32));
}

/*
 * 1201: BiometricKit (client side) -[BiometricKit _matchResult:withDictionary:] runs createMatchInfo:withTopology:,
 * i.e. GenerateMatchTemplateTopology() on the match record's topology (+0x7c). An all-zero topology fails its
 * __TranslateNodePlacement check (placement u16 size must be 0xbf4, u16 version 3) with EINVAL, so the details lose
 * "BKMatchInfo" and BKUIFingerprintEnrollViewController matchResult:withDetails: ignores the match: phase 2 of the
 * enrolment never progresses. A one-node topology passes: header +4 s16 matched node 0 (< 300), +6/+8/+0xa angle/x/y
 * 0; placement {0xbf4, 3, 1 node, +0x2e s16 scale}, node 0 at placement +0x30 {s16 angle, x, y, parent = itself}.
 */
static void
mesa_fill_topology(uint8_t *t)
{
	uint8_t *pl = t + TOPO_PLACEMENT;
	pl[0] = (uint8_t)TOPO_PLACEMENT_SIZE; pl[1] = (uint8_t)(TOPO_PLACEMENT_SIZE >> 8);
	pl[2] = 3;                              /* version */
	pl[4] = 1;                              /* one node */
	pl[0x2e] = 500 & 0xff; pl[0x2f] = 500 >> 8;     /* scale (divisor of the node coordinates) */
	/* node 0 at pl + 0x30: angle 0, x 0, y 0, parent 0 (itself = root), flags 0 */
}

/*
 * 1212: 8 AlignmentData (3060 bytes) = the node placement of the enrolment so far. Settings' BiometricKit client
 * (-[BiometricKit enrollProgressMessage:]) pulls it after every 0x64+pct status; _ComponentSetUpdate rejected the
 * 1198 zeros (resampleScale100 == 0 -> EINVAL, "AssertMacros: err == 0 ... BiometricKit.m, line: 1684"), so no
 * BiometricKitEnrollProgressInfo reached BKUI and the phase-1 haptic never ran. Layout (___TranslateNodePlacement,
 * _ComponentSetUpdate): +0 u16 size 0xbf4, +2 u16 version 3, +4 s16 count, +6 s16 insNode (new node, -1 none),
 * +8 s16 delNode (-1), +0x2e u16 scale (!= 0), nodes at +0x30, 10 bytes each {s16 angle, x, y, parent, u8 flags, pad}.
 * Node 0 is the root at (0,0) and its own parent; node i > 0 sits at a small distinct offset, child of node 0.
 */
#define PL_NODES                0x30
#define PL_NODE_SIZE            10
#define PL_MAX_NODES            ((TOPO_PLACEMENT_SIZE - 8 - PL_NODES) / PL_NODE_SIZE)  /* 300 */

static inline void
put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static uint32_t
mesa_fill_alignment(uint8_t *pl, uint32_t n)
{
	if (n > PL_MAX_NODES) {
		n = PL_MAX_NODES;
	}
	bzero(pl, TOPO_PLACEMENT_SIZE);
	put16(pl, TOPO_PLACEMENT_SIZE);
	put16(pl + 2, 3);                               /* version */
	put16(pl + 4, (uint16_t)n);                     /* count */
	put16(pl + 6, n ? (uint16_t)(n - 1) : 0xffff);  /* insNode: the node of the touch just accepted */
	put16(pl + 8, 0xffff);                          /* delNode: none */
	put16(pl + 0x2e, 500);                          /* resampleScale100 */
	for (uint32_t i = 1; i < n; i++) {
		uint8_t *nd = pl + PL_NODES + PL_NODE_SIZE * i;
		put16(nd + 2, (uint16_t)(int16_t)(40 * (int)(i % 4) - 60));    /* x */
		put16(nd + 4, (uint16_t)(int16_t)(40 * (int)(i / 4) - 60));    /* y */
		/* angle 0, parent 0 (the root), flags 0 */
	}
	/* node 0: angle 0, x 0, y 0, parent 0 = itself (root) */
	return n;
}

/*
 * 1203: libBKDM1 statusMessage:withData:timestamp: calls BiometricKitXPCServer switchToNextBioOperation: (which
 * removes the head of activeBioOpsQueue and resumes the next operation) only for 0x42 EnrollmentCancelled,
 * 0x50 MatchingCancelled and 0x63 FingerDetectCancelled. cancelWithClient: marks the head op cancelling, sends
 * 12 Cancel and waits for that status; a match or enrol result never ends an operation. Without it every cancelled
 * operation stayed at the head and all later results (matchResult:timestamp: and statusMessage go to the head's
 * client) went to a dead client: SpringBoard never saw its lock-screen matches, Settings never saw phase 2.
 * Delivered from a thread call, after the Cancel command has returned to biometrickitd.
 */
static IOReturn mesa_status(uint32_t st);

static const char *const mesa_kind_names[4] = { "none", "enrol", "match", "finger-detect" };

static IOLock *
mesa_endl(void)
{
	if (gEndL == NULL) {
		IOLock *l = IOLockAlloc();
		if (l != NULL && !OSCompareAndSwapPtr(NULL, l, (void * volatile *)&gEndL)) {
			IOLockFree(l);
		}
	}
	return gEndL;
}

/* 1210: queue the op-ending status of op `kind`; false if it cannot be delivered (no lock, queue full) */
static bool
mesa_end_push(uint32_t kind, uint32_t cause)
{
	IOLock *l = mesa_endl();
	bool ok = false;
	if (l == NULL || kind == 0 || kind > 3) {
		return false;
	}
	IOLockLock(l);
	if (gEndCount < MESA_ENDQ) {
		int i = (gEndHead + gEndCount) % MESA_ENDQ;
		gEndQ[i].kind = (uint8_t)kind;
		gEndQ[i].cause = (uint8_t)cause;
		gEndCount++;
		ok = true;
	}
	IOLockUnlock(l);
	if (!ok) {
		SAY("op-ending status queue full: end of the %s op (cmd %u) LOST\n", mesa_kind_names[kind], cause);
	}
	return ok;
}

/*
 * 1210: deliver every queued op-ending status, oldest first. Runs from the thread call (after the Cancel / the
 * pre-empting command was handled) and at the start of every sweetfpcd event translation, so the old op's end always
 * reaches biometrickitd before any status of the new op.
 */
static void
mesa_end_flush(void)
{
	static const uint32_t sts[4] = { 0, ST_ENROLL_CANCELLED, ST_MATCHING_CANCELLED, ST_FDET_CANCELLED };
	static const char *const stn[4] = { "", "EnrollmentCancelled", "MatchingCancelled", "FingerDetectCancelled" };
	IOLock *l = mesa_endl();
	if (l == NULL) {
		return;
	}
	IOLockLock(l);
	while (gEndCount > 0) {
		uint32_t k = gEndQ[gEndHead].kind, cause = gEndQ[gEndHead].cause;
		gEndHead = (gEndHead + 1) % MESA_ENDQ;
		gEndCount--;
		IOReturn r = mesa_status(sts[k]);
		nOpEnd++;
		if (cause == 12) {
			SAY("op ended: status 0x%x (%s) -> 0x%x\n", sts[k], stn[k], r);
		} else {
			nImplicitEnd++;
			SAY("implicit op end delivered: old %s op, status 0x%x (%s), new cmd %u %s -> 0x%x\n",
			    mesa_kind_names[k], sts[k], stn[k], cause, mesa_name(cause), r);
		}
	}
	IOLockUnlock(l);
}

static void
mesa_end_tc(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	mesa_end_flush();
}

static void
mesa_end_kick(void)
{
	if (OSCompareAndSwap(1, 0, &gEndKick) && gEndTC != NULL) {
		thread_call_enter(gEndTC);
	}
}

/*
 * 1211: op generations. struct fpcd_event.op_gen is sweetfpcd's S.gen at SEND time (sweetfpcd.c ev_send2), not the
 * generation of the op that produced the event, and the replies to 3/4/38 carry no generation. So the kernel cannot
 * know the daemon generation of an op that sent no event yet. What it does:
 *  (a) kind rule: a result (match / no-match / withheld -> match op, enrol progress / done / failed -> enrol op, op
 *      timeout -> its op field) that does not belong to the op the kernel has running (none after a Cancel, or another
 *      kind after a pre-emption) is dropped. This closes the audit's "cancelled match unlocks the keybag up to ~250 ms
 *      after the Cancel" (sweetfpcd sees the posted Cancel only at its next WAIT + one capture slice).
 *  (b) generation window: when an op ends (12 Cancel, or pre-empted by 3/4/38) the generation of the last event seen
 *      is taken as the ended op's. Events still carrying it are dropped (FingerOn, ImageRejected and results; never
 *      FingerOff / errors) until sweetfpcd sends an event with another generation (it has seen the Cancel or the new
 *      mode command; op_set_nowait() bumps S.gen) or kern.sweet_mesa_stale_ms has passed. If the ended op sent no
 *      event at all, its generation is unknown and only rule (a) and the time window protect the next op of the same
 *      kind. Exact fix needs sweetfpcd to stamp the op's own gen and echo it in the 3/4/38 replies.
 */
static void
mesa_stale_arm(uint32_t kind, const char *why)
{
	uint32_t ms = gStaleMs > 0 ? (uint32_t)gStaleMs : 0;
	uint64_t dl;
	gKGen++;
	if (!(gFix & MESA_FIX_STALE_DROP) || ms == 0 || !gEvGenSeen) {
		return;
	}
	clock_interval_to_deadline(ms, kMillisecondScale, &dl);
	gStaleGen = gEvGen;
	gStaleKGen = gKGen;
	gStaleDl = dl;
	gStale = true;
	if (gMesa & 4) {
		SAY("op gen %u: %s op ended (%s): sweetfpcd gen %u is stale for up to %u ms\n", gKGen, mesa_kind_names[kind],
		    why, gEvGen, ms);
	}
}

/* 1211: true if sweetfpcd event `code` (op field `op`, generation `gen`) belongs to an op that has ended */
static bool
mesa_event_stale(uint32_t code, uint32_t op, uint32_t gen, bool v2, const char **why)
{
	uint32_t want = 0;
	gEvGen = gen;
	gEvGenSeen = true;
	if (!(gFix & MESA_FIX_STALE_DROP)) {
		return false;
	}
	switch (code) {
	case FEV_ENROL_PROGRESS: case FEV_ENROL_DONE: case FEV_ENROL_FAILED:
		want = 1;
		break;
	case FEV_NOMATCH: case FEV_MATCH: case FEV_MATCH_WITHHELD:
		want = 2;
		break;
	case FEV_OP_TIMEOUT:
		want = v2 && op >= 1 && op <= 3 ? op : 0;
		break;
	case FEV_FINGER_DOWN: case FEV_IMAGE:
		break;
	default:                /* FingerOff, errors, template list: never dropped */
		return false;
	}
	if (want != 0 && gOpKind != want) {
		*why = gOpKind == 0 ? "no op running" : "other op kind running";
		return true;
	}
	if (gStale) {
		if (gen != gStaleGen || mach_absolute_time() > gStaleDl) {
			gStale = false;         /* sweetfpcd moved on (new generation) or the window is over */
		} else {
			*why = "generation of an ended op";
			return true;
		}
	}
	return false;
}

/* 1210: kicks the end thread call when the forwarded/posted command block is left (any return path) */
struct MesaEndKick {
	~MesaEndKick()
	{
		mesa_end_kick();
	}
};

/*
 * 1210: BiometricSupport startBioOperation: appends the new op to activeBioOpsQueue and sends 3/4/38 without
 * cancelling the op that runs; pauseBioOperation: sends nothing. The SEP ends the pre-empted op with its *Cancelled
 * status; without it the old op stayed at activeBioOpsQueue[0] and every later status and match went to its (dead)
 * client: SpringBoard's lock-screen op never saw a FingerOn or a match (touchid-unlock-haptic-re 2026-11-18, B.1).
 */
static void
mesa_op_started(uint32_t kind, uint32_t cmd)
{
	UInt32 old;
	do {
		old = gOpKind;
	} while (!OSCompareAndSwap(old, kind, &gOpKind));
	if (old == 0 || old > 3) {
		gKGen++;                /* 1211 */
		return;
	}
	mesa_stale_arm(old, "pre-empted");      /* 1211 */
	gKGen++;
	if (!(gFix & MESA_FIX_IMPLICIT_END) || !gOpEnd || gEndTC == NULL) {
		SAY("%s op pre-empted by cmd %u %s: no op-ending status (kern.sweet_mesa_fix bit 0 or kern.sweet_mesa_opend off)\n",
		    mesa_kind_names[old], cmd, mesa_name(cmd));
		return;
	}
	if (mesa_end_push(old, cmd)) {
		gEndKick = 1;
	}
}

/*
 * 1205: deferred Touch ID keybag unlock. With the unlock done before the match record went out, SpringBoard saw the
 * keybag event first ("State changed to Authenticated"), set the Mesa matching mode to None, cancelled its match op
 * and dropped the match result, so no biometric unlock action ran (passcode pad stayed, no rest-to-open).
 */
static void
mesa_unlock_tc(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	int did = 0;
	const char *why = "";
	int e = sweet_pc_bio_unlock(&did, &why);
	if (e == 0) {
		if (did) {
			OSIncrementAtomic(&nBioUnlock);
		}
		sweet_acm_note_verified_match();
	} else {
		OSIncrementAtomic(&nBioRefused);
	}
	SAY("deferred Touch ID unlock (%d ms after the match record): %s%s\n", gUnlockDelayMs,
	    e ? "REFUSED " : did ? "keybag unlocked" : "already unlocked", e ? why : "");
}

/* 1204: the SEP itself ended the op (enrol complete / failed / timeout): no Cancel status will follow */
static void
mesa_op_finished(uint32_t kind)
{
	OSCompareAndSwap(kind, 0, &gOpKind);
}

static void
mesa_op_cancelled(void)
{
	UInt32 k;
	do {
		k = gOpKind;
	} while (!OSCompareAndSwap(k, 0, &gOpKind));
	mesa_stale_arm(k, "Cancel");    /* 1211 */
	if (!gOpEnd || k == 0 || k > 3 || gEndTC == NULL) {
		if (k == 0) {
			SAY("Cancel with no running op: no op-ending status\n");
		}
		return;
	}
	if (mesa_end_push(k, 12)) {     /* 1210: queued, not a bit per kind */
		thread_call_enter(gEndTC);
	}
}

static IOReturn mesa_deliver(uint32_t idx, uint32_t version, uint64_t ordinal, const uint8_t *data, uint32_t len);

/* 1207: sweetfpcd connected: ask biometrickitd to re-init the sensor and re-sync its identity list */
static void
mesa_reinit_tc(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	if (!gOffHonest || gBridgeClients == 0) {
		return;
	}
	IOReturn r = mesa_deliver(MIDX_REINIT, 1, 0, NULL, 0);
	OSIncrementAtomic(&nReinit);
	SAY("sweetfpcd connected: re-init message 0xe3ff800d to biometrickitd -> 0x%x (kern.sweet_mesa_offline=1)\n", r);
}

static void
mesa_locks(void)
{
	if (gM == NULL) {
		IOLock *l = IOLockAlloc();
		if (l != NULL && !OSCompareAndSwapPtr(NULL, l, (void * volatile *)&gM)) {
			IOLockFree(l);
		}
	}
	if (gPub == NULL) {
		IOLock *l = IOLockAlloc();
		if (l != NULL && !OSCompareAndSwapPtr(NULL, l, (void * volatile *)&gPub)) {
			IOLockFree(l);
		}
	}
	if (gReinitTC == NULL) {
		thread_call_t tc = thread_call_allocate(mesa_reinit_tc, NULL);
		if (tc != NULL && !OSCompareAndSwapPtr(NULL, tc, (void * volatile *)&gReinitTC)) {
			thread_call_free(tc);
		}
	}
	if (gCatLk == NULL) {
		IOLock *l = IOLockAlloc();
		if (l != NULL && !OSCompareAndSwapPtr(NULL, l, (void * volatile *)&gCatLk)) {
			IOLockFree(l);
		}
	}
	if (gUnlockTC == NULL) {
		thread_call_t tc = thread_call_allocate(mesa_unlock_tc, NULL);
		if (tc != NULL && !OSCompareAndSwapPtr(NULL, tc, (void * volatile *)&gUnlockTC)) {
			thread_call_free(tc);
		}
	}
	if (gEndTC == NULL) {
		thread_call_t tc = thread_call_allocate(mesa_end_tc, NULL);
		if (tc != NULL && !OSCompareAndSwapPtr(NULL, tc, (void * volatile *)&gEndTC)) {
			thread_call_free(tc);
		}
	}
}

static uint64_t
deadline_ms(uint32_t ms)
{
	uint64_t dl;
	clock_interval_to_deadline(ms, kMillisecondScale, &dl);
	return dl;
}

/* ---- IOKit classes ---- */
class AppleMesaSEPDriver : public IOService
{
	OSDeclareDefaultStructors(AppleMesaSEPDriver);
public:
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AppleMesaSEPDriver, IOService);

/* libBKDM1 setAppleMesaSEPLoggingLevel writes a property here: accept and log it, store nothing. */
IOReturn
AppleMesaSEPDriver::setProperties(OSObject *properties)
{
	OSDictionary *d = OSDynamicCast(OSDictionary, properties);
	char pn[32];
	proc_selfname(pn, (int)sizeof(pn));
	if (d == NULL) {
		SAY("%s setProperties (not a dictionary) ignored\n", pn);
		return kIOReturnBadArgument;
	}
	OSCollectionIterator *it = OSCollectionIterator::withCollection(d);
	OSSymbol *k;
	while (it && (k = OSDynamicCast(OSSymbol, it->getNextObject())) != NULL) {
		SAY("%s sets AppleMesaSEPDriver property %s (accepted, ignored)\n", pn, k->getCStringNoCopy());
	}
	OSSafeReleaseNULL(it);
	return kIOReturnSuccess;
}

class AppleBiometricServices : public IOService
{
	OSDeclareDefaultStructors(AppleBiometricServices);
};
OSDefineMetaClassAndStructors(AppleBiometricServices, IOService);

class AppleBiometricServicesUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AppleBiometricServicesUserClient);
	bool fCounted;
public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
	    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
	IOReturn message(UInt32 type, IOService *provider, void *argument) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AppleBiometricServicesUserClient, IOUserClient);

/*
 * 1192e: messageClients(0xe3ff8000) reaches biometrickitd through its IOGeneralInterest notification
 * (IOServiceAddInterestNotification, libBKDM1 ___serviceStatus); the same call also messages the service's
 * clients in the service plane, i.e. this user client, whose inherited IOService::message() answered
 * kIOReturnUnsupported and turned every delivery's aggregate result into 0xe00002c7. Accept it here.
 */
IOReturn
AppleBiometricServicesUserClient::message(UInt32 type, IOService *provider, void *argument)
{
	if (type == MESA_MSG) {
		return kIOReturnSuccess;
	}
	return IOUserClient::message(type, provider, argument);
}

class SweetMesaBridge : public IOService
{
	OSDeclareDefaultStructors(SweetMesaBridge);
};
OSDefineMetaClassAndStructors(SweetMesaBridge, IOService);

class SweetMesaBridgeUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(SweetMesaBridgeUserClient);
	bool fCounted;
public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
	    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(SweetMesaBridgeUserClient, IOUserClient);

/* ---- struct in/out helpers (inline or descriptor) ---- */
static uint32_t
in_len(IOExternalMethodArguments *a)
{
	return a->structureInputDescriptor ? (uint32_t)a->structureInputDescriptor->getLength() : a->structureInputSize;
}

/*
 * 1192c: a struct larger than the inline limit (4096) arrives as an IOMemoryDescriptor over the caller's pages,
 * NOT prepared. Copying through it unprepared hung the whole phone the first time sweetfpcd's 16388-byte WAIT
 * got a request (fingerprint-freeze-bisect-2026-11-18.md). prepare() wires the pages first, complete() after.
 */
static bool
md_copy(IOMemoryDescriptor *md, bool out, void *buf, uint32_t len)
{
	if (len == 0) {
		return true;
	}
	IOReturn pr = md->prepare();
	if (pr != kIOReturnSuccess) {
		SAY("descriptor prepare() failed 0x%x (%s %u bytes)\n", pr, out ? "out" : "in", len);
		return false;
	}
	IOByteCount n = out ? md->writeBytes(0, buf, len) : md->readBytes(0, buf, len);
	md->complete();
	return n == len;
}

static void
io_debug(const char *who, uint32_t sel, IOExternalMethodArguments *a)
{
	if (gMesa & 8) {
		SAY("io %s sel %u: in %u %s, out %u %s\n", who, sel,
		    a->structureInputDescriptor ? (uint32_t)a->structureInputDescriptor->getLength() : a->structureInputSize,
		    a->structureInputDescriptor ? "descriptor" : "inline",
		    a->structureOutputDescriptor ? (uint32_t)a->structureOutputDescriptor->getLength() : a->structureOutputSize,
		    a->structureOutputDescriptor ? "descriptor" : "inline");
	}
}

static bool
in_read(IOExternalMethodArguments *a, void *buf, uint32_t len)
{
	if (a->structureInputDescriptor) {
		return md_copy(a->structureInputDescriptor, false, buf, len);
	}
	if (a->structureInput == NULL && len) {
		return false;
	}
	bcopy(a->structureInput, buf, len);
	return true;
}

static uint32_t
out_cap(IOExternalMethodArguments *a)
{
	return a->structureOutputDescriptor ? (uint32_t)a->structureOutputDescriptor->getLength() : a->structureOutputSize;
}

static IOReturn
out_write(IOExternalMethodArguments *a, const void *buf, uint32_t len)
{
	if (a->structureOutputDescriptor) {
		if (len > a->structureOutputDescriptor->getLength()) {
			return kIOReturnNoSpace;
		}
		if (!md_copy(a->structureOutputDescriptor, true, (void *)(uintptr_t)buf, len)) {
			return kIOReturnVMError;
		}
		a->structureOutputDescriptorSize = len;
		return kIOReturnSuccess;
	}
	if (len > a->structureOutputSize || (len && a->structureOutput == NULL)) {
		return kIOReturnNoSpace;
	}
	if (len) {
		bcopy(buf, a->structureOutput, len);
	}
	a->structureOutputSize = len;
	return kIOReturnSuccess;
}

/* ---- the message queue for cmd 43 (gM held) ---- */
static uint64_t
q_put(uint8_t *data, uint32_t len)
{
	uint64_t t = mach_absolute_time();
	if (t <= gLastTicket) {
		t = gLastTicket + 1;
	}
	gLastTicket = t;
	int i, free_i = -1, old_i = 0;
	for (i = 0; i < MESA_MSGQ; i++) {
		if (gQ[i].data == NULL && free_i < 0) {
			free_i = i;
		}
		if (gQ[i].data != NULL && gQ[i].ticket < gQ[old_i].ticket) {
			old_i = i;
		}
	}
	if (free_i < 0) {
		SAY("message queue full: dropping ticket 0x%llx (%u bytes, never pulled)\n", gQ[old_i].ticket, gQ[old_i].len);
		IOFreeData(gQ[old_i].data, gQ[old_i].len);
		gQ[old_i].data = NULL;
		free_i = old_i;
	}
	gQ[free_i].ticket = t;
	gQ[free_i].len = len;
	gQ[free_i].data = data;
	return t;
}

static void
q_clear(void)
{
	for (int i = 0; i < MESA_MSGQ; i++) {
		if (gQ[i].data != NULL) {
			IOFreeData(gQ[i].data, gQ[i].len);
			gQ[i].data = NULL;
		}
	}
}

/*
 * Send message 0xe3ff8000 (header type 0xe3ff8000 + idx) to AppleBiometricServices' interest clients. Header as
 * BiometricSupport's MCDMExtractMessageData reads it: +0 u64 ticket (0 = inline), +8 u32 type, +0xc u32 version,
 * +0x10 u64 timestamp, +0x18 u64 ordinal, +0x20 u64 data length.
 */
static IOReturn
mesa_deliver(uint32_t idx, uint32_t version, uint64_t ordinal, const uint8_t *data, uint32_t len)
{
	uint8_t msg[MESA_MSG_HDR + MESA_INLINE_MAX];
	uint64_t now = mach_absolute_time(), ticket = 0;
	AppleBiometricServices *abs;

	if (len > MESA_INLINE_MAX) {
		uint8_t *copy = (uint8_t *)IOMallocData(len);
		if (copy == NULL) {
			return kIOReturnNoMemory;
		}
		bcopy(data, copy, len);
		IOLockLock(gM);
		ticket = q_put(copy, len);
		IOLockUnlock(gM);
		now = ticket;
		OSIncrementAtomic(&nQueued);
	}
	bzero(msg, sizeof(msg));
	put64(&msg[0x00], ticket);
	put32(&msg[0x08], MESA_MSG + idx);
	put32(&msg[0x0c], version);
	put64(&msg[0x10], now);
	put64(&msg[0x18], ordinal);
	put64(&msg[0x20], len);
	if (len <= MESA_INLINE_MAX && len) {
		bcopy(data, &msg[MESA_MSG_HDR], len);
	}
	IOLockLock(gM);
	abs = gAbs;
	if (abs) {
		abs->retain();
	}
	IOLockUnlock(gM);
	if (abs == NULL) {
		return kIOReturnNotReady;
	}
	IOReturn r = abs->messageClients(MESA_MSG, msg, MESA_MSG_HDR + (len <= MESA_INLINE_MAX ? len : 0));
	abs->release();
	OSIncrementAtomic(&nDelivered);
	SAY("message 0x%x v%u ord 0x%llx len %u %s ticket 0x%llx -> 0x%x\n", MESA_MSG + idx, version, ordinal, len,
	    ticket ? "queued" : "inline", ticket, r);
	return r;
}

/* 0xe3ff8001 version 1, no data: a status for BiometricKitXPCServer statusMessage:withData:timestamp: */
static IOReturn
mesa_status(uint32_t st)
{
	return mesa_deliver(MIDX_STATUS, 1, st, NULL, 0);
}

/* allocate, fill and deliver a queued payload (zeroed, then the caller's fill) */
static uint8_t *
mesa_zalloc(uint32_t len)
{
	return (uint8_t *)IOMallocZeroData(len);
}

/*
 * 1192d: one sweetfpcd "FPCD" event -> Apple message(s). Returns the IOReturn of the last delivery, or
 * kIOReturnSuccess when the event kind is logged only. Every decision is logged.
 */
static IOReturn
fpcd_translate(const uint8_t *d, uint32_t len)
{
	uint32_t code = le16(d + FEV_CODE);
	int32_t st = (int32_t)le32(d + FEV_STATUS);
	uint32_t fid = le32(d + FEV_FID), rem = le32(d + FEV_REMAINING), smp = le32(d + FEV_SAMPLES);
	uint32_t fl = le32(d + FEV_FLAGS);
	bool v2 = len >= FEV_V2_SIZE;
	uint32_t op = v2 ? le32(d + FEV_OP) : 0, pct = v2 ? le32(d + FEV_PERCENT) : 0;
	uint8_t *b;
	IOReturn r = kIOReturnSuccess;

	if (gEndCount > 0) {
		mesa_end_flush();       /* 1210: a queued op end goes out before anything of the next op */
	}
	const char *swhy = "";
	if (mesa_event_stale(code, op, le32(d + FEV_OP_GEN), v2, &swhy)) {
		nStaleDropped++;
		SAY("dropped stale event: fpcd code %u status %d fid 0x%x gen %u (%s; kernel op %s, op gen %u)\n", code, st,
		    fid, le32(d + FEV_OP_GEN), swhy, mesa_kind_names[gOpKind <= 3 ? gOpKind : 0], gKGen);
		return kIOReturnSuccess;
	}
	OSIncrementAtomic(&nTranslated);
	switch (code) {
	case FEV_FINGER_DOWN:
		return mesa_status(ST_FINGER_ON);
	case FEV_FINGER_UP:
		return mesa_status(ST_FINGER_OFF);
	case FEV_IMAGE:
		return mesa_status(ST_IMAGE_REJECTED);
	case FEV_ENROL_PROGRESS:
		if (st != 0 && st != 1) {       /* 4 partial, 5 too similar, 6 dirty: sample not taken */
			SAY("enrol progress: FPC code %d (sample not taken) -> ImageRejected\n", st);
			return mesa_status(ST_IMAGE_REJECTED);
		}
		if (!v2) {
			pct = (smp + rem) ? smp * 100 / (smp + rem) : 0;
		}
		if (st == 0 || pct > 100) {
			pct = 100;
		}
		if (pct < gLastPct && smp > 1) {
			pct = gLastPct;
		}
		gLastPct = pct;
		if (gAlignN < PL_MAX_NODES) {
			gAlignN++;              /* 1212: before the status, so the AlignmentData it triggers has the node */
		}
		b = mesa_zalloc(ENROLL_INFO_V2_SIZE);
		if (b == NULL) {
			return kIOReturnNoMemory;
		}
		SAY("enrol progress: FPC code %d samples %u remaining %u -> %u%% (status 0x%x)\n", st, smp, rem, pct,
		    ST_ENROLL_PROGRESS_BASE + pct);
		r = mesa_deliver(MIDX_STATUS, 2, ST_ENROLL_PROGRESS_BASE + pct, b, ENROLL_INFO_V2_SIZE);
		IOFreeData(b, ENROLL_INFO_V2_SIZE);
		return r;
	case FEV_ENROL_DONE:
		gLastPct = 0;
		if (fid == 0) {
			mesa_op_finished(1);    /* 1204 */
			return mesa_status(ST_ENROLL_FAILED);
		}
		b = mesa_zalloc(ENROLL_RESULT_V2_SIZE);
		if (b == NULL) {
			return kIOReturnNoMemory;
		}
		put32(b, gUser);
		put32(b + 4, SMFP_MAGIC);
		put32(b + 8, fid);
		SAY("enrol done: fid 0x%x -> enroll result user %u uuid SMFP/0x%x\n", fid, gUser, fid);
		r = mesa_deliver(MIDX_ENROLL_RESULT, 2, 0, b, ENROLL_RESULT_V2_SIZE);
		IOFreeData(b, ENROLL_RESULT_V2_SIZE);
		/*
		 * 1204: 0x41 EnrollmentComplete after the result. 1213: libBKDM1 serviceStatus: already calls
		 * switchToNextBioOperation:1 for the 0xe3ff8003 enrol result itself, so the 0x41 dequeued a second op
		 * (tid14 12:28:52, tid15 12:41:08: it could drop the extend-match op). Off unless kern.sweet_mesa_fix bit 3.
		 * The kernel's running op ends with the result either way (no implicit end for the next MatchMode).
		 */
		mesa_op_finished(1);
		if (gOpEnd && (gFix & MESA_FIX_ENROLL_0X41)) {
			IOReturn r2 = mesa_status(ST_ENROLL_COMPLETE);
			nOpEnd++;
			SAY("op ended: status 0x41 (EnrollmentComplete) -> 0x%x\n", r2);
		} else {
			SAY("enrol op ended by the enroll result (no 0x41: kern.sweet_mesa_fix bit 3 off)\n");
		}
		return r;
	case FEV_ENROL_FAILED:
		gLastPct = 0;
		SAY("enrol failed: status %d flags 0x%x -> EnrollmentFailed\n", st, fl);
		mesa_op_finished(1);    /* 1204: 0x43 ends the op */
		return mesa_status(ST_ENROLL_FAILED);
	case FEV_OP_TIMEOUT:
		gLastPct = 0;
		if (op == 1) {
			mesa_op_finished(1);    /* 1204: 0x44 ends the op */
		}
		return mesa_status(op == 1 ? ST_ENROLL_TIMEOUT : ST_MATCHING_STOPPED);
	case FEV_ERROR:
		return mesa_status(ST_UNKNOWN_ERROR);
	case FEV_NOMATCH:
	case FEV_MATCH_WITHHELD:
	case FEV_MATCH: {
		bool verified = (fl & FEV_MF_HAT_VERIFIED) != 0;
		bool identity = code == FEV_MATCH && fid != 0 && ((gMatch == 1 && verified) || gMatch == 2);
		uint32_t mflags = 0;
		if (code == FEV_MATCH && !identity) {
			OSIncrementAtomic(&nMatchRefused);
			SAY("match fid 0x%x: NOT delivered (kern.sweet_mesa_match=%d, HAT %s) -> no-match\n", fid, gMatch,
			    verified ? "verified" : "NOT verified");
		}
		bool unlock_later = false;
		if (identity && !gMatchExtend && gUnlockDelayMs != 0) {
			/* 1205: rules checked now, keybag unlocked after the record (> 0 ms) or never (-1: SpringBoard's job) */
			const char *why = "";
			if (!sweet_pc_bio_allowed(&why)) {
				identity = false;
				OSIncrementAtomic(&nBioRefused);
				SAY("match fid 0x%x: Touch ID unlock REFUSED (%s) -> no-match\n", fid, why);
			} else if (gUnlockDelayMs > 0) {
				mflags |= 1u << 5;      /* BKMatchDetailUnlocked: the keybag will be unlocked for this match */
				unlock_later = true;
			}
		} else if (identity && !gMatchExtend) {
			/* 1198: what the SEP does for an unlock match: the keybag, then one ACM credential */
			int did = 0;
			const char *why = "";
			int e = sweet_pc_bio_unlock(&did, &why);
			if (e != 0) {
				identity = false;
				OSIncrementAtomic(&nBioRefused);
				SAY("match fid 0x%x score %d: Touch ID unlock REFUSED (%s) -> no-match\n", fid,
				    (int32_t)le32(d + FEV_SCORE), why);
			} else {
				if (did) {
					mflags |= 1u << 5;      /* BKMatchDetailUnlocked */
					OSIncrementAtomic(&nBioUnlock);
				}
				sweet_acm_note_verified_match();
			}
		}
		if (code != FEV_MATCH && !gMatchExtend && gMatch == 2) {
			sweet_pc_bio_failed();
		}
		if (!gMatch) {
			/* gate closed: no 0xe3ff8002 record at all, only the MatchingFailed status */
			return mesa_status(ST_MATCHING_FAILED);
		}
		uint32_t sz = MR_SIZE;
		b = mesa_zalloc(sz);
		if (b == NULL) {
			return kIOReturnNoMemory;
		}
		if (identity) {
			put32(b, gUser);
			put32(b + 4, SMFP_MAGIC);
			put32(b + 8, fid);
			put32(b + MR_FLAGS, mflags);    /* 1198: bit 5 Unlocked only when the keybag was unlocked here */
			if (gTopo) {
				mesa_fill_topology(b + MR_TOPO);        /* 1201 */
			}
			OSIncrementAtomic(&nMatchDelivered);
			if (!verified) {
				OSIncrementAtomic(&nUnverified);
				SAY("UNVERIFIED match delivered: fid 0x%x score %d user %u%s flags 0x%x (kern.sweet_mesa_match=2)\n",
				    fid, (int32_t)le32(d + FEV_SCORE), gUser, gMatchExtend ? " extend-enrolment" : "", mflags);
			} else {
				SAY("MATCH: fid 0x%x user %u, HAT verified, gate open -> match record (flags 0x%x)\n", fid, gUser,
				    mflags);
			}
		} else {
			put32(b, 0xffffffffu);          /* no identity */
			put32(b + MR_FLAGS, 2);         /* no-match reason: matcher */
		}
		r = mesa_deliver(MIDX_MATCH_RESULT, 2, 0, b, sz);
		IOFreeData(b, sz);
		if (unlock_later && gUnlockTC != NULL) {
			uint64_t dl;
			clock_interval_to_deadline((uint32_t)gUnlockDelayMs, kMillisecondScale, &dl);
			thread_call_enter_delayed(gUnlockTC, dl);
		}
		return r;
	}
	default:        /* FPCD_EV_TEMPLATES and anything newer: no Apple message */
		OSIncrementAtomic(&nLoggedOnly);
		SAY("fpcd code %u status %d: logged only (no Apple message for it)\n", code, st);
		return kIOReturnSuccess;
	}
}

/* ---- forwarding to sweetfpcd ---- */
static IOReturn
fpcd_status(int32_t st)
{
	switch (st) {
	case 0: return kIOReturnSuccess;
	case -45: return kIOReturnUnsupported;
	case -35: return kIOReturnNotReady;
	case -57: return kIOReturnNotReady;
	case -16: return kIOReturnBusy;
	case -22: return kIOReturnBadArgument;
	case -2: return kIOReturnNotFound;
	case -5: return kIOReturnIOError;
	default: return st < -4096 ? (IOReturn)st : kIOReturnError;
	}
}

/*
 * Queue one command for sweetfpcd and wait for its reply. Returns kIOReturnNotReady with *offline set when no
 * bridge client is open, kIOReturnTimeout when none answers in kern.sweet_mesa_fwd_ms, else the mapped status.
 */
static IOReturn
mesa_forward_ms(uint32_t cmd, const uint8_t *p, uint32_t plen, uint8_t *rbuf, uint32_t *rlen, bool *offline,
    int ms, bool quiet)
{
	*offline = false;
	*rlen = 0;
	if (plen > SMB_DATA_MAX) {
		return kIOReturnMessageTooLarge;
	}
	uint64_t dl = deadline_ms((uint32_t)ms);
	IOLockLock(gM);
	if (gBridgeClients == 0 || slot.req == NULL) {
		IOLockUnlock(gM);
		*offline = true;
		return kIOReturnNotReady;
	}
	while (slot.state != SLOT_FREE) {
		if (IOLockSleepDeadline(gM, &gEvFree, dl, THREAD_ABORTSAFE) != THREAD_AWAKENED && slot.state != SLOT_FREE) {
			IOLockUnlock(gM);
			OSIncrementAtomic(&nTimeouts);
			*offline = true;
			return kIOReturnTimeout;
		}
	}
	if (++gSeq == 0) {
		gSeq = 1;
	}
	slot.seq = gSeq;
	slot.cmd = cmd;
	slot.len = plen;
	if (plen) {
		bcopy(p, slot.req, plen);
	}
	slot.state = SLOT_QUEUED;
	if (!quiet) {
		SAY("cmd %u %s -> bridge seq %u (%u bytes, waits <= %d ms)\n", cmd, mesa_name(cmd), slot.seq, plen, ms);
	}
	IOLockWakeup(gM, &gEvQueued, false);
	while (slot.state != SLOT_DONE) {
		if (IOLockSleepDeadline(gM, &gEvDone, dl, THREAD_ABORTSAFE) != THREAD_AWAKENED && slot.state != SLOT_DONE) {
			SAY("cmd %u %s seq %u: no reply from the bridge in %d ms (%s)\n", cmd, mesa_name(cmd), slot.seq,
			    ms, slot.state == SLOT_QUEUED ? "never picked up" : "picked up, not answered");
			slot.state = SLOT_FREE;
			IOLockWakeup(gM, &gEvFree, true);
			IOLockUnlock(gM);
			OSIncrementAtomic(&nTimeouts);
			*offline = true;        /* 1192b: answered like "no sweetfpcd" */
			return kIOReturnTimeout;
		}
	}
	int32_t st = slot.status;
	*rlen = slot.rlen;
	if (slot.rlen) {
		bcopy(slot.rep, rbuf, slot.rlen);
	}
	slot.state = SLOT_FREE;
	IOLockWakeup(gM, &gEvFree, true);
	IOLockUnlock(gM);
	OSIncrementAtomic(&nFwd);
	return fpcd_status(st);
}

static IOReturn
mesa_forward(uint32_t cmd, const uint8_t *p, uint32_t plen, uint8_t *rbuf, uint32_t *rlen, bool *offline)
{
	return mesa_forward_ms(cmd, p, plen, rbuf, rlen, offline, gFwdMs, false);
}

/* 1192b: queue a command for sweetfpcd without waiting. kIOReturnNotReady (offline) when no bridge client. */
static IOReturn
mesa_post(uint32_t cmd, const uint8_t *p, uint32_t plen)
{
	if (plen > sizeof(gP[0].data)) {
		return kIOReturnMessageTooLarge;
	}
	IOLockLock(gM);
	if (gBridgeClients == 0) {
		IOLockUnlock(gM);
		return kIOReturnNotReady;
	}
	if (gPCount == MESA_POSTQ) {
		SAY("posted queue full: dropping cmd %u seq %u\n", gP[gPHead].cmd, gP[gPHead].seq);
		gPHead = (gPHead + 1) % MESA_POSTQ;
		gPCount--;
	}
	int i = (gPHead + gPCount) % MESA_POSTQ;
	if (++gSeq == 0) {
		gSeq = 1;
	}
	gP[i].seq = gSeq;
	gP[i].cmd = cmd;
	gP[i].len = plen;
	if (plen) {
		bcopy(p, gP[i].data, plen);
	}
	gPCount++;
	gPostedSeq[gPostedIdx] = gSeq;
	gPostedIdx = (gPostedIdx + 1) % 16;
	SAY("cmd %u %s -> bridge seq %u posted (no wait)\n", cmd, mesa_name(cmd), gSeq);
	IOLockWakeup(gM, &gEvQueued, false);
	IOLockUnlock(gM);
	OSIncrementAtomic(&nFwd);
	return kIOReturnSuccess;
}

/* ---- 1206: catacomb persistence ---- */
static uint32_t
cat_user(void)
{
	return gUser ? gUser : CAT_DEFAULT_USER;
}

static void
cat_dirty(const char *why)
{
	OSIncrementAtomic(&gCatGen);
	if (!gCatDirty) {
		SAY("catacomb: dirty (%s)%s\n", why, gCatLoaded ? "" : " (not reported until loaded)");
	}
	gCatDirty = true;
}

static void
cat_loaded(const char *why)
{
	if (!gCatLoaded) {
		SAY("catacomb: loaded (%s)\n", why);
	}
	gCatLoaded = true;
}

/* sweetfpcd events that change the TA db (called for every FPCD event, before translation) */
static void
cat_note_event(uint32_t code, int32_t st, uint32_t fid)
{
	if (!gCat) {
		return;
	}
	switch (code) {
	case FEV_ENROL_DONE:
		if (st == 0 && fid != 0) {
			cat_loaded("enrol done");
			OSIncrementAtomic(&nCatDirtyEv);
			cat_dirty("enrol done");
		}
		break;
	case FEV_TEMPLATES:
		if (st == 0) {
			OSIncrementAtomic(&nCatDirtyEv);
			cat_dirty("TA templates changed");
		}
		break;
	case FEV_TA_FRESH:
		OSIncrementAtomic(&nCatFresh);
		gCatLoaded = false;
		gCatDirty = false;
		SAY("catacomb: sweetfpcd loaded the TA empty: state back to present / not loaded (biometrickitd reloads)\n");
		break;
	default:
		break;
	}
}

/* the user of a 61/62/63 component (v2: 24 bytes {u32 user, u32 kind, 16 x 0}; v1: u32 user); ~0 = master */
static uint32_t
cat_component_user(const uint8_t *p, uint32_t plen)
{
	return plen >= 4 ? le32(p) : 0xffffffffu;
}

static IOReturn
mesa_catacomb(uint32_t cmd, const uint8_t *p, uint32_t plen, uint8_t *out, uint32_t cap, uint32_t *outLen,
    const char **how)
{
	uint8_t *rbuf = (uint8_t *)IOMallocData(SMB_DATA_MAX);
	uint8_t *cb = NULL;
	uint32_t rlen = 0, user = 0;
	bool offline = false;
	IOReturn r = kIOReturnSuccess;

	if (rbuf == NULL) {
		return kIOReturnNoMemory;
	}
	IOLockLock(gCatLk);
	switch (cmd) {
	case 61:        /* PrepareSaveCatacomb: component -> u32 size */
		user = cat_component_user(p, plen);
		if (user == 0xffffffffu) {
			*how = "catacomb: master component not kept";
			r = kIOReturnUnsupported;
			break;
		}
		gCatSize = 0;
		gCatSavedGen = gCatGen;
		r = mesa_forward_ms(61, p, plen, rbuf, &rlen, &offline, gCatMs, false);
		if (r == kIOReturnSuccess && (rlen < 4 || le32(rbuf) == 0 || le32(rbuf) > CAT_MAX)) {
			r = kIOReturnUnderrun;
		}
		if (r != kIOReturnSuccess) {
			OSIncrementAtomic(&nCatSaveFail);
			*how = offline ? "catacomb: store, sweetfpcd offline" : "catacomb: store FAILED";
			if (offline) {
				r = kIOReturnNotReady;
			}
			break;
		}
		gCatSize = le32(rbuf);
		if (cap < 4) {
			r = kIOReturnNoSpace;
			break;
		}
		*outLen = 4;
		put32(out, gCatSize);
		*how = "catacomb: TA db stored, blob staged";
		SAY("catacomb: 61 user %u: blob of %u bytes staged (gen %u)\n", user, gCatSize, gCatSavedGen);
		break;
	case 62: {      /* CompleteSaveCatacomb: component -> the staged blob */
		uint32_t size = gCatSize, off = 0;
		if (size == 0) {
			*how = "catacomb: nothing staged (no 61)";
			r = kIOReturnNotReady;
			break;
		}
		if (cap < size) {
			*how = "catacomb: out smaller than the staged blob";
			r = kIOReturnNoSpace;
			break;
		}
		while (off < size) {
			uint8_t q[8];
			uint32_t want = size - off > CAT_CHUNK ? CAT_CHUNK : size - off;
			put32(q, off);
			put32(q + 4, want);
			r = mesa_forward_ms(FPCD_CMD_CAT_GET, q, 8, rbuf, &rlen, &offline, gFwdMs, true);
			if (r != kIOReturnSuccess || rlen == 0 || rlen > want) {
				SAY("catacomb: 62 chunk at %u of %u: 0x%x len %u%s\n", off, size, r, rlen,
				    offline ? " (offline)" : "");
				if (r == kIOReturnSuccess) {
					r = kIOReturnUnderrun;
				}
				break;
			}
			bcopy(rbuf, out + off, rlen);
			off += rlen;
		}
		if (r != kIOReturnSuccess) {
			OSIncrementAtomic(&nCatSaveFail);
			*how = "catacomb: blob pull FAILED";
			break;
		}
		*outLen = size;
		gCatLastSave = size;
		*how = "catacomb: blob handed to biometrickitd";
		SAY("catacomb: 62 user %u: %u bytes to biometrickitd (it writes the catacomb file)\n",
		    cat_component_user(p, plen), size);
		break;
	}
	case 63:        /* ConfirmSaveCatacomb: biometrickitd wrote the file */
		r = mesa_forward_ms(63, p, plen, rbuf, &rlen, &offline, gFwdMs, false);
		if (offline) {
			r = kIOReturnSuccess;   /* the file is written; sweetfpcd only frees its staging buffer */
		}
		if (gCatSize != 0) {
			OSIncrementAtomic(&nCatSaves);
			if (gCatGen == gCatSavedGen) {
				gCatDirty = false;
			}
		}
		gCatSize = 0;
		OSIncrementAtomic(&nCatConfirms);
		*how = gCatDirty ? "catacomb: saved, changed again meanwhile (still dirty)" : "catacomb: saved, clean";
		SAY("catacomb: 63 confirmed (%s)\n", gCatDirty ? "still dirty" : "clean");
		break;
	case 64: {      /* LoadCatacomb: the CatacombSecureData blob from the file */
		uint32_t off = 0;
		UInt32 gen = gCatGen;
		user = cat_user();
		if (plen == 0 || plen > CAT_MAX) {
			*how = "catacomb: load, bad blob size";
			r = kIOReturnBadArgument;
			OSIncrementAtomic(&nCatLoadFail);
			break;
		}
		cb = (uint8_t *)IOMallocData(CAT_CHUNK + 8);
		if (cb == NULL) {
			r = kIOReturnNoMemory;
			break;
		}
		while (off < plen) {
			uint32_t n = plen - off > CAT_CHUNK ? CAT_CHUNK : plen - off;
			put32(cb, plen);
			put32(cb + 4, off);
			bcopy(p + off, cb + 8, n);
			r = mesa_forward_ms(FPCD_CMD_CAT_PUT, cb, n + 8, rbuf, &rlen, &offline, gFwdMs, true);
			if (r != kIOReturnSuccess) {
				SAY("catacomb: 64 chunk at %u of %u: 0x%x%s\n", off, plen, r, offline ? " (offline)" : "");
				break;
			}
			off += n;
		}
		if (r == kIOReturnSuccess) {
			uint8_t q[12];
			put32(q, FPCD_CAT_LOAD_MAGIC);
			put32(q + 4, plen);
			put32(q + 8, user);
			r = mesa_forward_ms(64, q, 12, rbuf, &rlen, &offline, gCatMs, false);
		}
		if (r != kIOReturnSuccess) {
			OSIncrementAtomic(&nCatLoadFail);
			if (offline) {
				r = kIOReturnNotReady;
			}
			/* never 0x192 / 0x8002 / 0x8003: libBKDM1 would map it to 0x10d and biometrickitd delete the file */
			*how = offline ? "catacomb: load, sweetfpcd offline (file kept)" : "catacomb: load FAILED (file kept)";
			SAY("catacomb: 64 user %u blob %u bytes: load failed 0x%x%s: biometrickitd keeps the file and retries\n",
			    user, plen, r, offline ? " (offline)" : "");
			break;
		}
		OSIncrementAtomic(&nCatLoads);
		gCatLastLoad = plen;
		cat_loaded("LoadCatacomb");
		if (gCatGen == gen) {
			gCatDirty = false;
		}
		*how = "catacomb: TA db loaded from the blob";
		SAY("catacomb: 64 user %u: %u-byte blob loaded into the TA\n", user, plen);
		break;
	}
	default:
		r = kIOReturnUnsupported;
		break;
	}
	IOLockUnlock(gCatLk);
	if (cb != NULL) {
		IOFreeData(cb, CAT_CHUNK + 8);
	}
	IOFreeData(rbuf, SMB_DATA_MAX);
	if (offline) {
		OSIncrementAtomic(&nOffline);
	}
	return r;
}

/* ---- the persona: one bm_cmd ---- */
static IOReturn
mesa_command(const uint8_t *in, uint32_t inLen, uint8_t *out, uint32_t cap, uint32_t *outLen, const char **how,
    uint32_t *pcmd, uint32_t *pver, uint32_t *pval)
{
	uint32_t cmd, ver = 0, val;
	const uint8_t *p;
	uint32_t plen;
	IOReturn r;
	bool offline = false;

	*outLen = 0;
	*how = "canned";
	if (inLen >= 8 && le16(in) == 0x4d42) {
		cmd = le16(in + 2);
		ver = le16(in + 4);
		val = le16(in + 6);
		p = in + 8;
		plen = inLen - 8;
	} else if (inLen >= 6 && le32(in) == 0x4d434d43) {             /* legacy 'CMCM' (MobileGestalt) */
		switch (in[4]) {
		case 0x01: cmd = 2; break;
		case 0x1b: cmd = 16; break;
		case 0x36: cmd = 34; break;
		default: cmd = 0x1000 | in[4]; break;
		}
		val = in[5];
		p = in + 6;
		plen = inLen - 6;
		*how = "canned (CMCM)";
	} else {
		*pcmd = 0;
		*how = "bad header";
		return kIOReturnBadArgument;
	}
	*pcmd = cmd;
	*pver = ver;
	*pval = val;

	if (gCat && gCatLk != NULL && cmd >= 61 && cmd <= 64) {      /* 1206 */
		return mesa_catacomb(cmd, p, plen, out, cap, outLen, how);
	}

#define ANSWER(n) do { if ((uint32_t)(n) > cap) { *how = "out too small"; return kIOReturnNoSpace; } \
	    *outLen = (n); bzero(out, (n)); } while (0)
	/* inside the forwarded switch: no return (rbuf is freed below); break leaves the inner switch */
#define ANSWER_F(n) if ((uint32_t)(n) > cap) { *how = "out too small"; r = kIOReturnNoSpace; break; } \
	    *outLen = (n); bzero(out, (n))

	switch (cmd) {
	/* canned */
	case 1:         /* GetCmdProtocolVersion: u32 2 */
		ANSWER(4); put32(out, 2); return kIOReturnSuccess;
	case 16:        /* GetProvisioningState: kAppleBiometricSensorProvisioned */
		ANSWER(4); put32(out, 0x20); return kIOReturnSuccess;
	case 29:        /* GetSensorCalibrationStatus */
	case 42:        /* GetLoggingType */
		ANSWER(1); return kIOReturnSuccess;
	case 48:        /* GetEnabledForUnlock (1202: follows kern.sweet_mesa_pcfg bit 0) */
		ANSWER(1); out[0] = (gPcfg & 1) ? 1 : 0; return kIOReturnSuccess;
	case 50:        /* GetTemplatesValidity: valid */
		ANSWER(1); out[0] = 1; return kIOReturnSuccess;
	case 34: {      /* GetModuleSerialNumber */
		static const char sn[] = "SWEETFPC1020";
		ANSWER(17); bcopy(sn, out, sizeof(sn)); return kIOReturnSuccess;
	}
	case 39: {      /* GetSKSLockState (u32 SEP bio lock-state bits) */
		/*
		 * 1202: BiometricKit +[BKDevice lockoutState:fromDeviceLockoutState:] maps these bits to the lockout state
		 * SpringBoard shows: bit 8 -> 7, bit 4 -> 2, bit 1 -> 3, bit 2 -> 4, bit 3 -> 1 (no lockout), none -> 5
		 * (bio-locked: "passcode required to enable Touch ID"); extendedLockoutState sets its bit 4 when bit 3 is
		 * clear. The old answer 0 was therefore a permanent passcode-required lockout. Bit 3 is now set while the
		 * 1198 rules allow a Touch ID unlock (passcode since boot, < 48 h, < 5 failed matches, no backoff).
		 */
		const char *why = "";
		uint32_t bits = (gSks && sweet_pc_bio_allowed(&why)) ? 0x8 : 0;
		ANSWER(4);
		put32(out, bits);
		*how = bits ? "bio unlock allowed (bit 3)" : (gSks ? why : "canned 0 (kern.sweet_mesa_sks=0)");
		return kIOReturnSuccess;
	}
	case 40:        /* GetBiometrickitdInfo (23 bytes) */
		/*
		 * 1199: BiometricSupport cacheCatacombInfo keeps the first 8 bytes: +0 u32 max identities (syncTemplateList
		 * sizes the 66 GetIdentitiesList buffer as this x 20) and +4 u32 catacomb user slots (60 GetCatacombState
		 * buffer = this x 8). All zeros made every 66 arrive with capacity 0 ("out too small"), so biometrickitd
		 * never re-synced its identities after an enrolment and phase 2 found none.
		 */
		ANSWER(23);
		put32(out, 5);
		put32(out + 4, 1);
		return kIOReturnSuccess;
	case 8:         /* AlignmentData (3060) */
	case 17:        /* RequestTopologyOfTemplate (3060): 1198 zeros instead of unsupported (assert noise) */
		if (cap == 0 || cap > 3060) {
			*how = "unsupported (size)";
			return kIOReturnUnsupported;
		}
		ANSWER(cap);
		if (cmd == 8 && cap == TOPO_PLACEMENT_SIZE && (gFix & MESA_FIX_ALIGNMENT)) {
			uint32_t n = mesa_fill_alignment(out, gAlignN);         /* 1212 */
			nAlignAnswered++;
			SAY("AlignmentData answered: %u node(s), insNode %d\n", n, n ? (int)n - 1 : -1);
			*how = "canned (node placement)";
			return kIOReturnSuccess;
		}
		*how = "canned (zeros)";
		return kIOReturnSuccess;
	case 46: {      /* GetProtectedConfig (32 bytes) */
		/*
		 * 1202: libBKDM1 performGetProtectedConfigCommand: reads two UserProtectedConfig records, setCfg at +0 and
		 * effectiveCfg at +0x10, each {u32 unlock, u32 identification, u32 login, u32 applePay}. All zeros reported
		 * "Touch ID for iPhone Unlock" off for the user. kern.sweet_mesa_pcfg bits 0-3 = unlock, identification,
		 * login, Apple Pay (default 1: unlock only).
		 */
		ANSWER(32);
		for (int i = 0; i < 4; i++) {
			uint32_t on = (gPcfg >> i) & 1;
			put32(out + 4 * i, on);
			put32(out + 0x10 + 4 * i, on);
		}
		*how = "canned (protected config from kern.sweet_mesa_pcfg)";
		return kIOReturnSuccess;
	}
	case 58: ANSWER(33); return kIOReturnSuccess;          /* GetCatacombHash */
	case 60: {      /* GetCatacombState: n x catacomb_state_v1_t {u32 userID, u32 state} (8 n bytes) */
		/*
		 * 1192f: BiometricSupport readCatacombState removes every identity whose user's state has bit 0 clear
		 * (CatacombStateCache stateOfUserComponent:), so "no entries" deleted each fresh enrolment right after
		 * its enrol result. Answer the identities' user with bit 0 set (templates live in the FPC TA).
		 */
		/*
		 * 1200: the state bits are bit 0 catacomb present, bit 1 loaded in the "SEP", bit 2 dirty (save wanted).
		 * BiometricKitXPCServer checkCatacombForUser: runs loadCatacombForUser: whenever (state & 3) == 1, and
		 * loadCatacombForComponent: first does clearTemplateListForUser: and then refills the list only from the
		 * catacomb file on disk. There is no file (templates live in TA memory, 61-64 unsupported), so state 1
		 * emptied biometrickitd's list right after each enrolment (status 100%), and syncTemplateListForUser:
		 * then sent 13 RemoveTemplate for the new fid ("not present in biometrickitd"). Answer 3 (present +
		 * loaded, never dirty): no load, no save. kern.sweet_mesa_catstate=1 restores the 1192f answer.
		 */
		if (gCat) {     /* 1206: present, loaded, dirty from the catacomb engine */
			uint32_t cst = 1u | (gCatLoaded ? 2u : 0u) | (gCatLoaded && gCatDirty ? 4u : 0u);
			uint32_t cn = cap >= 8 ? 1 : 0;
			ANSWER(8 * cn);
			if (cn) {
				put32(out, cat_user());
				put32(out + 4, cst);
			}
			*how = cst == 1 ? "catacomb (present, not loaded: load wanted)" :
			    cst == 3 ? "catacomb (present + loaded)" : "catacomb (present + loaded + dirty: save wanted)";
			return kIOReturnSuccess;
		}
		uint32_t n = gUser != 0 && cap >= 8 ? 1 : 0;
		ANSWER(8 * n);
		if (n) {
			put32(out, gUser);
			put32(out + 4, (uint32_t)gCatState);
		}
		*how = n ? (gCatState == 3 ? "canned (user state 3: present+loaded)" : "canned (user state from sysctl)") :
		    "canned (no user yet)";
		return kIOReturnSuccess;
	}
	case 67: ANSWER(28); return kIOReturnSuccess;          /* GetSystemProtectedConfig */
	case 78: ANSWER(29); *how = "canned (no match event)"; return kIOReturnSuccess;  /* GetLastMatchEvent */
	case 20: case 24: case 30: case 32: case 36: case 44: case 57: case 73: case 75: case 87:
		ANSWER(0); *how = "accepted"; return kIOReturnSuccess;
	case 47: case 68:
		*how = "refused (needs ACM authData)";
		OSIncrementAtomic(&nRefused);
		return kIOReturnNotPermitted;

	case 43: {      /* RequestMessageData: u64 ticket */
		if (plen < 8) {
			*how = "short ticket";
			return kIOReturnBadArgument;
		}
		uint64_t t = (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
		IOLockLock(gM);
		for (int i = 0; i < MESA_MSGQ; i++) {
			if (gQ[i].data != NULL && gQ[i].ticket == t) {
				if (gQ[i].len > cap) {
					IOLockUnlock(gM);
					*how = "queued message larger than out";
					return kIOReturnNoSpace;
				}
				bcopy(gQ[i].data, out, gQ[i].len);
				*outLen = gQ[i].len;
				IOFreeData(gQ[i].data, gQ[i].len);
				gQ[i].data = NULL;
				IOLockUnlock(gM);
				OSIncrementAtomic(&nMsgPulled);
				*how = "message queue";
				return kIOReturnSuccess;
			}
		}
		IOLockUnlock(gM);
		*how = "no such ticket";
		return kIOReturnNotFound;
	}

	/* forwarded to sweetfpcd */
	case 2: case 3: case 4: case 12: case 13: case 14: case 15: case 38: case 49: case 56: case 61: case 62:
	case 63: case 64: case 65: case 66: case 69: case 72: case 83: {
		uint8_t small[4];
		const uint8_t *fp = NULL;
		uint32_t flen = 0;
		uint32_t rlen = 0;
		uint8_t *rbuf;
		MesaEndKick kick;       /* 1210 */

		/* 1192d: the identities' user id (EnrollMode v2 payload +4, 65/66 payload +0) for enrol/match records */
		if (cmd == 3) {
			gMatchExtend = false;
			gAlignN = 0;            /* 1212 */
			mesa_op_started(1, cmd);        /* 1203, 1210 */
		} else if (cmd == 4) {
			mesa_op_started(2, cmd);
		}
		if (cmd == 3 && plen >= 8 && le32(p + 4) != 0xffffffffu) {
			gUser = le32(p + 4);
			gLastPct = 0;
		} else if ((cmd == 65 || cmd == 66) && plen >= 4 && le32(p) != 0xffffffffu) {
			gUser = le32(p);
		}
		switch (cmd) {
		case 3: put32(small, 0); fp = small; flen = 4; break;           /* u32 gid 0 */
		case 4:
			/*
			 * 1198: MatchMode v2 payload (0x44 + selected identities): +0 flags, +4 user; an extend-enrolment
			 * (BKOptionMatchForExtendEnrollment) puts the identity {u32 user, uuid} at +8 (libBKDM1
			 * performMatchCommand:), ours always an SMFP uuid. sweetfpcd gets u32 flags (bit 0 extend).
			 */
			gMatchExtend = plen >= 0x1c && le32(p + 0xc) == SMFP_MAGIC;
			put32(small, gMatchExtend ? 1 : 0);
			fp = small;
			flen = 4;
			if (gMatchExtend) {
				SAY("MatchMode: extend-enrolment of fid 0x%x user %u\n", le32(p + 0x10), le32(p + 8));
			} else {
				SAY("MatchMode: normal match (%u bytes)\n", plen);
			}
			break;
		case 38: put32(small, 1); fp = small; flen = 4; break;          /* u32 enable 1 */
		case 69: put32(small, val); fp = small; flen = 4; break;        /* u32 enable = value */
		case 13: case 61: case 62: case 63: case 64: fp = p; flen = plen; break;   /* Apple raw */
		case 49:        /* 1206: catacomb mode passes the user (u32; ~0 = master component) */
			if (gCat && plen >= 4) {
				fp = p;
				flen = 4;
			}
			break;
		default: break;
		}
		if (cmd == 12 || cmd == 38 || cmd == 69) {         /* 1192b: posted, never blocks the caller */
			if (cmd == 12) {
				mesa_op_cancelled();    /* 1203 */
			} else if (cmd == 38) {
				mesa_op_started(3, cmd);
			}
			r = mesa_post(cmd, fp, flen);
			if (r == kIOReturnSuccess) {
				*how = "posted";
				ANSWER(0);
				return kIOReturnSuccess;
			}
			OSIncrementAtomic(&nOffline);
			*how = "offline";
			if (cmd == 38 || (gOffHonest && cmd == 69)) {   /* 1207: 69 no longer "success" while offline */
				return kIOReturnNotReady;
			}
			ANSWER(0);
			return kIOReturnSuccess;
		}
		rbuf = (uint8_t *)IOMallocData(SMB_DATA_MAX);
		if (rbuf == NULL) {
			return kIOReturnNoMemory;
		}
		*how = "forwarded";
		r = mesa_forward(cmd, fp, flen, rbuf, &rlen, &offline);
		if (offline) {
			OSIncrementAtomic(&nOffline);
			*how = r == kIOReturnTimeout ? "timeout -> offline answer" : "offline";
			r = kIOReturnNotReady;
			if (gOffHonest && cmd != 12 && cmd != 83) {     /* 1207: not ready, no made-up answer */
				*how = "offline -> not ready (kern.sweet_mesa_offline=1)";
				IOFreeData(rbuf, SMB_DATA_MAX);
				return r;
			}
			switch (cmd) {
			case 12: case 49: case 69: ANSWER_F(0); r = kIOReturnSuccess; break;
			case 15: case 65: ANSWER_F(4); put32(out, 5); r = kIOReturnSuccess; break;
			case 56: ANSWER_F(16); r = kIOReturnSuccess; break;
			case 66: ANSWER_F(0); r = kIOReturnSuccess; break;
			case 83: ANSWER_F(1); r = kIOReturnSuccess; break;
			default: break;
			}
			IOFreeData(rbuf, SMB_DATA_MAX);
			return r;
		}
		if (r == kIOReturnSuccess && gCat) {   /* 1206: the TA db is now what biometrickitd works with */
			if (cmd == 3) {
				cat_loaded("EnrollMode");
			} else if (cmd == 13 || cmd == 72) {
				cat_loaded(cmd == 13 ? "RemoveTemplate" : "RemoveUserData");
			} else if (cmd == 49 && plen >= 4 && le32(p) != 0xffffffffu) {
				cat_loaded("NoCatacomb for the user (no file)");
			}
		}
		if (r == kIOReturnSuccess) {
			switch (cmd) {
			case 15: case 65: case 61:
				if (rlen < 4) {
					r = kIOReturnUnderrun;
					break;
				}
				ANSWER_F(4); bcopy(rbuf, out, 4);
				break;
			case 83:
				if (rlen < 1) {
					r = kIOReturnUnderrun;
					break;
				}
				ANSWER_F(1); out[0] = (rlen >= 4 ? le32(rbuf) : rbuf[0]) ? 1 : 0;
				break;
			case 56:
				ANSWER_F(16);
				bcopy(rbuf, out, rlen >= 16 ? 16 : (rlen >= 8 ? 8 : 0));
				break;
			case 66: {
				uint32_t n = rlen >= 4 ? le32(rbuf) : 0, user = plen >= 4 ? le32(p) : 0;
				if (n > 5 || rlen < 4 + 4 * n) {
					r = kIOReturnUnderrun;
					break;
				}
				ANSWER_F(20 * n);
				for (uint32_t i = 0; i < n; i++) {
					put32(out + 20 * i, user);
					put32(out + 20 * i + 4, SMFP_MAGIC);
					put32(out + 20 * i + 8, le32(rbuf + 4 + 4 * i));
				}
				break;
			}
			case 62:
				if (rlen > cap) {
					r = kIOReturnNoSpace;
					break;
				}
				ANSWER_F(rlen); bcopy(rbuf, out, rlen);
				break;
			default:
				ANSWER_F(0);
				break;
			}
		}
		IOFreeData(rbuf, SMB_DATA_MAX);
		return r;
	}
	default:
		*how = "unsupported";
		OSIncrementAtomic(&nUnsup);
		return kIOReturnUnsupported;
	}
#undef ANSWER
#undef ANSWER_F
}

/* ---- AppleBiometricServicesUserClient ---- */
bool
AppleBiometricServicesUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	char pn[32];
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	proc_selfname(pn, (int)sizeof(pn));
	bool ent = IOTaskHasEntitlement(owningTask, MESA_ENTITLEMENT);
	bool root = clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
	if (!ent && !root) {
		SAY("%s open (type %u) refused: no %s and not root\n", pn, (unsigned)type, MESA_ENTITLEMENT);
		return false;
	}
	IOLockLock(gM);
	gAbsClients++;
	IOLockUnlock(gM);
	fCounted = true;
	SAY("%s opened AppleBiometricServices (type %u, %s)\n", pn, (unsigned)type,
	    ent ? "entitled" : "root, WITHOUT the applemesa entitlement");
	return true;
}

IOReturn
AppleBiometricServicesUserClient::clientClose(void)
{
	if (fCounted) {
		fCounted = false;
		IOLockLock(gM);
		gAbsClients--;
		IOLockUnlock(gM);
		SAY("AppleBiometricServices client closed\n");
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
AppleBiometricServicesUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	char pn[32];
	uint32_t inLen = in_len(args), cap = out_cap(args), outLen = 0, cmd = 0, ver = 0, val = 0;
	const char *how = "";
	IOReturn r;
	uint8_t *in = NULL, *out = NULL;
	uint32_t ocap = cap > SMB_DATA_MAX + 8 ? SMB_DATA_MAX + 8 : cap;

	if (selector != 0) {
		return kIOReturnUnsupported;
	}
	io_debug("AppleBiometricServices", selector, args);
	proc_selfname(pn, (int)sizeof(pn));
	if (inLen < 6 || inLen > (gCat ? (uint32_t)CAT_MAX + 8 : 0x10000u)) {        /* 1206: 64 carries the blob */
		SAY("%s sel 0 with %u input bytes refused\n", pn, inLen);
		return kIOReturnBadArgument;
	}
	in = (uint8_t *)IOMallocData(inLen);
	if (in == NULL) {
		r = kIOReturnNoMemory;
		goto done;
	}
	if (!in_read(args, in, inLen)) {
		r = kIOReturnVMError;
		goto done;
	}
	/* 1206: 62 CompleteSaveCatacomb returns the whole blob (up to CAT_MAX) */
	if (gCat && inLen >= 8 && le16(in) == 0x4d42 && le16(in + 2) == 62) {
		ocap = cap > CAT_MAX ? CAT_MAX : cap;
	}
	out = (uint8_t *)IOMallocData(ocap ? ocap : 1);
	if (out == NULL) {
		r = kIOReturnNoMemory;
		goto done;
	}
	OSIncrementAtomic(&nCmds);
	r = mesa_command(in, inLen, out, ocap, &outLen, &how, &cmd, &ver, &val);
	if (r == kIOReturnSuccess) {
		r = out_write(args, out, outLen);
	} else {
		out_write(args, out, 0);
	}
	if (!strncmp(how, "canned", 6) || !strcmp(how, "accepted")) {
		OSIncrementAtomic(&nCanned);
	}
	gLastCmd = cmd;
	gLastRet = r;
	SAY("%s cmd %u %s v%u val %u in %u cap %u -> 0x%x out %u [%s]\n", pn, cmd, mesa_name(cmd), ver, val,
	    inLen, cap, r, outLen, how);
	if ((gMesa & 4) && inLen > 8) {
		uint32_t n = inLen - 8 > 32 ? 32 : inLen - 8, i;
		char hx[3 * 32 + 1];
		for (i = 0; i < n; i++) {
			snprintf(&hx[3 * i], 4, "%02x ", in[8 + i]);
		}
		hx[3 * n] = 0;
		SAY("  payload %s\n", hx);
	}
done:
	if (in) {
		IOFreeData(in, inLen);
	}
	if (out) {
		IOFreeData(out, ocap ? ocap : 1);
	}
	return r;
}

/* ---- SweetMesaBridgeUserClient (sweetfpcd) ---- */
bool
SweetMesaBridgeUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	char pn[32];
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	proc_selfname(pn, (int)sizeof(pn));
	if (type != 0 || clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) != kIOReturnSuccess) {
		SAY("%s bridge open refused (type %u; root only, type 0)\n", pn, (unsigned)type);
		return false;
	}
	IOLockLock(gM);
	gBridgeClients++;
	bool first = gBridgeClients == 1;
	IOLockUnlock(gM);
	if (first && gOffHonest && gReinitTC != NULL) {          /* 1207 */
		uint64_t dl;
		clock_interval_to_deadline(REINIT_DELAY_MS, kMillisecondScale, &dl);
		thread_call_enter_delayed(gReinitTC, dl);
	}
	fCounted = true;
	SAY("%s opened SweetMesaBridge\n", pn);
	return true;
}

IOReturn
SweetMesaBridgeUserClient::clientClose(void)
{
	if (fCounted) {
		fCounted = false;
		IOLockLock(gM);
		if (gBridgeClients == 1) {
			gPHead = gPCount = 0;   /* 1192b: posted requests die with the last bridge client */
		}
		if (--gBridgeClients == 0 && (slot.state == SLOT_QUEUED || slot.state == SLOT_SENT)) {
			slot.status = -57;      /* ENOTCONN -> kIOReturnNotReady for the waiting command */
			slot.rlen = 0;
			slot.state = SLOT_DONE;
			IOLockWakeup(gM, &gEvDone, false);
		}
		IOLockUnlock(gM);
		SAY("SweetMesaBridge client closed\n");
	}
	terminate();
	return kIOReturnSuccess;
}

static IOReturn
bridge_event(IOExternalMethodArguments *args)
{
	uint32_t n = in_len(args);
	if (n < SMB_EV_HDR || n > SMB_EV_HDR + SMB_DATA_MAX) {
		return kIOReturnBadArgument;
	}
	uint8_t *b = (uint8_t *)IOMallocData(n);
	if (b == NULL) {
		return kIOReturnNoMemory;
	}
	IOReturn r;
	if (!in_read(args, b, n)) {
		IOFreeData(b, n);
		return kIOReturnVMError;
	}
	uint32_t sub = le32(b), len = le32(b + 4), idx = sub & 0xff, value = sub >> 8;
	const uint8_t *d = b + SMB_EV_HDR;
	OSIncrementAtomic(&nEvents);
	if (len > n - SMB_EV_HDR || idx == 0 || idx > 0xd) {
		SAY("EVENT subtype 0x%x len %u refused (bad index or length)\n", sub, len);
		IOFreeData(b, n);
		return kIOReturnBadArgument;
	}
	bool fpcd = len >= FEV_V1_SIZE && le32(d) == FPCD_MAGIC;
	uint32_t code = fpcd ? le16(d + FEV_CODE) : 0;
	if (fpcd) {
		OSIncrementAtomic(&nFpcdEv);
		cat_note_event(code, (int32_t)le32(d + FEV_STATUS), le32(d + FEV_FID));       /* 1206 */
		SAY("EVENT fpcd v%u code %u status %d fid 0x%x flags 0x%x (%u bytes) -> %s\n", le16(d + 4), code,
		    (int32_t)le32(d + FEV_STATUS), le32(d + FEV_FID), le32(d + FEV_FLAGS), len,
		    (gMesa & 2) ? "translate" : "logged only (kern.sweet_mesa bit 1 clear)");
		if (!(gMesa & 2)) {
			if (code == FPCD_EV_MATCH) {
				OSIncrementAtomic(&nMatchDropped);
			}
			IOFreeData(b, n);
			return kIOReturnSuccess;
		}
		r = fpcd_translate(d, len);
		IOFreeData(b, n);
		return r;
	}
	/*
	 * Raw Apple-format events (not used by sweetfpcd): a match record (index 2), a match event (0xb) or anything
	 * >= 0xc88 bytes carries no HAT verification, so it is always dropped (1192d).
	 */
	if (idx == MIDX_MATCH_RESULT || idx == MIDX_MATCH_EVENT || len >= MESA_MATCH_RESULT_MIN) {
		OSIncrementAtomic(&nMatchDropped);
		SAY("EVENT index 0x%x len %u is match-shaped raw data: DROPPED (only verified FPCD matches are delivered)\n",
		    idx, len);
		IOFreeData(b, n);
		return kIOReturnNotPermitted;
	}
	r = mesa_deliver(idx, 1, value, d, len);        /* raw: version 1, subtype bits 8-31 = ordinal */
	IOFreeData(b, n);
	return r;
}

IOReturn
SweetMesaBridgeUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	io_debug("SweetMesaBridge", selector, args);
	switch (selector) {
	case 0: {       /* WAIT */
		uint64_t ms = args->scalarInputCount ? args->scalarInput[0] : 0;
		uint64_t dl = deadline_ms(ms > 60000 ? 60000 : (uint32_t)ms);
		uint32_t cap = out_cap(args);
		IOLockLock(gM);
		while (slot.state != SLOT_QUEUED && gPCount == 0) {
			if (ms == 0 || (IOLockSleepDeadline(gM, &gEvQueued, dl, THREAD_ABORTSAFE) != THREAD_AWAKENED &&
			    slot.state != SLOT_QUEUED && gPCount == 0)) {
				IOLockUnlock(gM);
				return kIOReturnTimeout;
			}
		}
		if (slot.state != SLOT_QUEUED) {        /* 1192b: a posted request (12/38/69) */
			uint8_t pt[SMB_HDR + sizeof(gP[0].data)];
			uint32_t plen = gP[gPHead].len, ptotal = SMB_HDR + plen;
			if (cap < ptotal) {
				IOLockUnlock(gM);
				return kIOReturnNoSpace;
			}
			put32(pt, gP[gPHead].seq);
			put32(pt + 4, gP[gPHead].cmd);
			put32(pt + 8, plen);
			if (plen) {
				bcopy(gP[gPHead].data, pt + SMB_HDR, plen);
			}
			gPHead = (gPHead + 1) % MESA_POSTQ;
			gPCount--;
			IOLockUnlock(gM);
			return out_write(args, pt, ptotal);
		}
		uint32_t len = slot.len, total = SMB_HDR + len;
		if (cap < total) {
			IOLockUnlock(gM);
			return kIOReturnNoSpace;
		}
		uint8_t *t = (uint8_t *)IOMallocData(total);
		if (t == NULL) {
			IOLockUnlock(gM);
			return kIOReturnNoMemory;
		}
		put32(t, slot.seq);
		put32(t + 4, slot.cmd);
		put32(t + 8, len);
		if (len) {
			bcopy(slot.req, t + SMB_HDR, len);
		}
		slot.state = SLOT_SENT;
		IOLockUnlock(gM);
		IOReturn r = out_write(args, t, total);
		if (r != kIOReturnSuccess) {
			IOLockLock(gM);
			if (slot.state == SLOT_SENT && slot.seq == le32(t)) {
				slot.state = SLOT_QUEUED;       /* give it to the next WAIT */
			}
			IOLockUnlock(gM);
		}
		IOFreeData(t, total);
		return r;
	}
	case 1: {       /* REPLY */
		uint32_t n = in_len(args);
		if (n < SMB_HDR || n > SMB_HDR + SMB_DATA_MAX) {
			return kIOReturnBadArgument;
		}
		uint8_t *b = (uint8_t *)IOMallocData(n);
		if (b == NULL) {
			return kIOReturnNoMemory;
		}
		if (!in_read(args, b, n)) {
			IOFreeData(b, n);
			return kIOReturnVMError;
		}
		uint32_t seq = le32(b), len = le32(b + 8);
		int32_t st = (int32_t)le32(b + 4);
		IOReturn r = kIOReturnSuccess;
		if (len > n - SMB_HDR) {
			r = kIOReturnBadArgument;
		} else {
			IOLockLock(gM);
			if (slot.state == SLOT_SENT && slot.seq == seq) {
				slot.status = st;
				slot.rlen = len;
				if (len) {
					bcopy(b + SMB_HDR, slot.rep, len);
				}
				slot.state = SLOT_DONE;
				IOLockWakeup(gM, &gEvDone, false);
			} else {
				bool posted = false;
				for (int i = 0; i < 16; i++) {
					posted |= seq != 0 && gPostedSeq[i] == seq;
				}
				/* 1192b: replies to posted requests and late replies (after the timeout) are taken and dropped */
				if (posted) {
					SAY("REPLY seq %u (posted) status %d len %u: logged only\n", seq, st, len);
				} else if (seq != 0 && seq <= gSeq) {
					SAY("REPLY seq %u status %d len %u came late (timed out already): dropped\n", seq, st, len);
				} else {
					r = kIOReturnNotFound;
				}
			}
			IOLockUnlock(gM);
		}
		if (r != kIOReturnSuccess) {
			SAY("REPLY seq %u status %d len %u not taken (0x%x: unknown seq)\n", seq, st, len, r);
		}
		IOFreeData(b, n);
		return r;
	}
	case 2:         /* EVENT */
		return bridge_event(args);
	case 3: {       /* STATE */
		uint8_t s[16];
		IOLockLock(gM);
		uint32_t fl = (gDrv ? SMB_SF_PUBLISHED : 0) | (gMatch ? SMB_SF_MATCH_GATE : 0) |
		    ((gMesa & 2) ? SMB_SF_FPCD_RAW : 0) | (gBridgeClients ? SMB_SF_BRIDGE_OPEN : 0);
		put32(s, SMB_STATE_VERSION);
		put32(s + 4, fl);
		put32(s + 8, ((slot.state == SLOT_QUEUED || slot.state == SLOT_SENT) ? 1 : 0) + (uint32_t)gPCount);
		put32(s + 12, (uint32_t)gAbsClients);
		IOLockUnlock(gM);
		return out_write(args, s, sizeof(s));
	}
	default:
		return kIOReturnUnsupported;
	}
}

/* ---- publish / withdraw ---- */
static bool
start_on(IOService *svc, IOService *provider, const char *name)
{
	if (!svc->attach(provider)) {
		svc->release();
		return false;
	}
	svc->setName(name);
	if (!svc->start(provider)) {
		svc->detach(provider);
		svc->release();
		return false;
	}
	svc->registerService();
	return true;
}

static void
gone(IOService *s)
{
	if (s) {
		s->terminate(kIOServiceSynchronous);
		s->release();
	}
}

/* gPub held. 0 ok, EALREADY, EBUSY (clients open), ENXIO, ENOMEM. */
static int
mesa_publish(bool on)
{
	if (!on) {
		if (gDrv == NULL) {
			return EALREADY;
		}
		IOLockLock(gM);
		if (gAbsClients || gBridgeClients) {
			IOLockUnlock(gM);
			return EBUSY;
		}
		AppleBiometricServices *a = gAbs;
		SweetMesaBridge *b = gBridge;
		AppleMesaSEPDriver *d = gDrv;
		gAbs = NULL;
		gBridge = NULL;
		gDrv = NULL;
		q_clear();
		IOLockUnlock(gM);
		gone(a);
		gone(b);
		gone(d);
		SAY("persona and bridge withdrawn\n");
		return 0;
	}
	if (gDrv != NULL) {
		return EALREADY;
	}
	IOService *platform = IOService::getPlatform(), *res = IOService::getResourceService();
	if (platform == NULL || res == NULL) {
		return ENXIO;
	}
	if (slot.req == NULL) {
		slot.req = (uint8_t *)IOMallocZeroData(SMB_DATA_MAX);
		slot.rep = (uint8_t *)IOMallocZeroData(SMB_DATA_MAX);
		if (slot.req == NULL || slot.rep == NULL) {
			return ENOMEM;
		}
	}
	AppleMesaSEPDriver *d = OSTypeAlloc(AppleMesaSEPDriver);
	if (d == NULL || !d->init()) {
		OSSafeReleaseNULL(d);
		return ENOMEM;
	}
	d->setProperty("SweetMesa", true);
	if (!start_on(d, platform, "AppleMesaSEPDriver")) {
		return ENOMEM;
	}
	SweetMesaBridge *b = OSTypeAlloc(SweetMesaBridge);
	if (b == NULL || !b->init()) {
		OSSafeReleaseNULL(b);
		gone(d);
		return ENOMEM;
	}
	b->setProperty(kIOUserClientClassKey, "SweetMesaBridgeUserClient");
	if (!start_on(b, res, "SweetMesaBridge")) {
		gone(d);
		return ENOMEM;
	}
	AppleBiometricServices *a = OSTypeAlloc(AppleBiometricServices);
	if (a == NULL || !a->init()) {
		OSSafeReleaseNULL(a);
		gone(b);
		gone(d);
		return ENOMEM;
	}
	a->setProperty(kIOUserClientClassKey, "AppleBiometricServicesUserClient");
	a->setProperty("SweetMesa", true);
	IOLockLock(gM);
	gDrv = d;
	gBridge = b;
	gAbs = a;               /* before registerService: launchd starts biometrickitd on it */
	IOLockUnlock(gM);
	if (!start_on(a, d, "AppleBiometricServices")) {
		IOLockLock(gM);
		gAbs = NULL;
		gBridge = NULL;
		gDrv = NULL;
		IOLockUnlock(gM);
		gone(b);
		gone(d);
		return ENOMEM;
	}
	SAY("published AppleMesaSEPDriver -> AppleBiometricServices (user client AppleBiometricServicesUserClient) "
	    "under %s, SweetMesaBridge on IOResources; match gate %s\n", platform->getName(),
	    gMatch ? "OPEN (kern.sweet_mesa_match=1)" : "closed");
	return 0;
}

/* ---- sysctls ---- */
static int
sysctl_sweet_mesa(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int v = gMesa, changed = 0;
	int e = sysctl_io_number(req, gMesa, sizeof(gMesa), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (v & ~15) {
		return EINVAL;
	}
	mesa_locks();
	if (gM == NULL || gPub == NULL) {
		return ENOMEM;
	}
	IOLockLock(gPub);
	bool want = (v & 1) != 0, have = gDrv != NULL;
	if (want != have) {
		e = mesa_publish(want);
		if (e == EALREADY) {
			e = 0;
		}
	}
	if (e == 0) {
		gMesa = v;
	}
	IOLockUnlock(gPub);
	SAY("kern.sweet_mesa=%d -> %d (%s)\n", v, e, e ? "unchanged" : "set");
	return e;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mesa, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_sweet_mesa, "I",
    "redfin 1192: SweetMesa Touch ID persona: bit0 publish, bit1 deliver FPCD-format events raw, bit2 hex dump, "
    "bit3 log struct sizes inline/descriptor");

static int
sysctl_sweet_mesa_match(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int v = gMatch, changed = 0;
	int e = sysctl_io_number(req, gMatch, sizeof(gMatch), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (v < 0 || v > 2) {
		return EINVAL;
	}
	char pn[32];
	proc_selfname(pn, (int)sizeof(pn));
	gMatch = v;
	SAY("match gate %s by %s (kern.sweet_mesa_match=%d)\n", v == 2 ? "OPEN, UNVERIFIED matches delivered" :
	    v ? "OPENED (verified only)" : "closed", pn, v);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mesa_match, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_mesa_match, "I", "redfin 1192/1198: 0 no matches, 1 HAT-verified only, 2 TA-identified matches unverified");

static int
sysctl_sweet_mesa_unlock_delay_ms(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int v = gUnlockDelayMs, changed = 0;
	int e = sysctl_io_number(req, gUnlockDelayMs, sizeof(gUnlockDelayMs), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (v < -1 || v > 5000) {
		return EINVAL;
	}
	gUnlockDelayMs = v;
	SAY("Touch ID keybag unlock %s (kern.sweet_mesa_unlock_delay_ms=%d)\n", v == 0 ? "at the match (before the record)" :
	    v < 0 ? "never (left to SpringBoard)" : "deferred after the match record", v);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mesa_unlock_delay_ms, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_mesa_unlock_delay_ms, "I", "redfin 1205: Touch ID keybag unlock 0 = before the match record "
    "(1198), N = N ms after it (1..5000), -1 = never (record without the Unlocked flag)");

static int
sysctl_sweet_mesa_opend(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int v = gOpEnd, changed = 0;
	int e = sysctl_io_number(req, gOpEnd, sizeof(gOpEnd), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (v < 0 || v > 1) {
		return EINVAL;
	}
	gOpEnd = v;
	SAY("op-ending statuses after Cancel %s (kern.sweet_mesa_opend=%d, %u sent so far)\n", v ? "on" : "off", v, nOpEnd);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mesa_opend, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_mesa_opend, "I", "redfin 1203: 12 Cancel ends the running op with EnrollmentCancelled / "
    "MatchingCancelled / FingerDetectCancelled so biometrickitd drops it from activeBioOpsQueue (default 1)");

static int
sysctl_sweet_mesa_fix(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int v = gFix, changed = 0;
	int e = sysctl_io_number(req, gFix, sizeof(gFix), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (v < 0 || v > 0xf) {
		return EINVAL;
	}
	gFix = v;
	SAY("kern.sweet_mesa_fix=0x%x: implicit op end %s, stale-event drop %s, AlignmentData %s, 0x41 after enrol %s\n", v,
	    (v & 1) ? "on" : "off", (v & 2) ? "on" : "off", (v & 4) ? "on" : "off", (v & 8) ? "on" : "off");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mesa_fix, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_mesa_fix, "I", "redfin 1210-1213: bit 0 implicit op end on 3/4/38, bit 1 drop stale sweetfpcd "
    "events, bit 2 AlignmentData node placement, bit 3 0x41 after the enrol result (default 0x7)");

static int
sysctl_sweet_mesa_fix_stats(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	char b[160];
	snprintf(b, sizeof(b), "fix=0x%x op_kind=%u op_ends=%u implicit_ends=%u end_queue=%d stale_dropped=%u "
	    "align_answered=%u", gFix, gOpKind, nOpEnd, nImplicitEnd, gEndCount, nStaleDropped, nAlignAnswered);
	return SYSCTL_OUT(req, b, strlen(b) + 1);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mesa_fix_stats, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_mesa_fix_stats, "A", "redfin 1210-1213: op-end / stale-event / AlignmentData counters");

static int
sysctl_sweet_mesa_stale_ms(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int v = gStaleMs, changed = 0;
	int e = sysctl_io_number(req, gStaleMs, sizeof(gStaleMs), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (v < 0 || v > 5000) {
		return EINVAL;
	}
	gStaleMs = v;
	SAY("kern.sweet_mesa_stale_ms=%d (%u stale events dropped so far)\n", v, nStaleDropped);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mesa_stale_ms, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_mesa_stale_ms, "I", "redfin 1211: after an op ends, events with its sweetfpcd generation are dropped "
    "for up to N ms (0 = only the op-kind rule; default 750)");

static int
sysctl_sweet_mesa_sks(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int v = gSks, changed = 0;
	int e = sysctl_io_number(req, gSks, sizeof(gSks), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (v < 0 || v > 1) {
		return EINVAL;
	}
	gSks = v;
	SAY("GetSKSLockState %s (kern.sweet_mesa_sks=%d)\n", v ? "follows the passcode rules" : "canned 0", v);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mesa_sks, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_mesa_sks, "I", "redfin 1202: 39 GetSKSLockState sets bit 3 (no bio lockout) while the passcode rules "
    "allow Touch ID unlock (default 1; 0 = the old 0 = permanent lockout)");

static int
sysctl_sweet_mesa_pcfg(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int v = gPcfg, changed = 0;
	int e = sysctl_io_number(req, gPcfg, sizeof(gPcfg), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (v < 0 || v > 15) {
		return EINVAL;
	}
	gPcfg = v;
	SAY("protected config set to 0x%x (unlock %d, identification %d, login %d, Apple Pay %d) (kern.sweet_mesa_pcfg)\n",
	    v, v & 1, (v >> 1) & 1, (v >> 2) & 1, (v >> 3) & 1);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mesa_pcfg, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_mesa_pcfg, "I", "redfin 1202: 46 GetProtectedConfig / 48 GetEnabledForUnlock: bit0 unlock, "
    "bit1 identification, bit2 login, bit3 Apple Pay (default 1; 0 = the old all-off answer)");

static int
sysctl_sweet_mesa_topo(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int v = gTopo, changed = 0;
	int e = sysctl_io_number(req, gTopo, sizeof(gTopo), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (v < 0 || v > 1) {
		return EINVAL;
	}
	gTopo = v;
	SAY("match topology %s (kern.sweet_mesa_topo=%d)\n", v ? "synthetic one-node" : "zeros", v);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mesa_topo, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_mesa_topo, "I", "redfin 1201: 1 = identity match records carry a valid one-node topology "
    "(BKMatchInfo for the enrol UI), 0 = zeros");

static int
sysctl_sweet_mesa_catstate(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int v = gCatState, changed = 0;
	int e = sysctl_io_number(req, gCatState, sizeof(gCatState), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (v < 0 || v > 7) {
		return EINVAL;
	}
	char pn[32];
	proc_selfname(pn, (int)sizeof(pn));
	gCatState = v;
	SAY("GetCatacombState answer set to %d by %s (kern.sweet_mesa_catstate)\n", v, pn);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mesa_catstate, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_mesa_catstate, "I", "redfin 1200: 60 GetCatacombState value for the identities' user "
    "(bit0 present, bit1 loaded, bit2 dirty; default 3, 1 = 1192f answer)");

static int
sysctl_sweet_mesa_catacomb(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int v = gCat, changed = 0;
	int e = sysctl_io_number(req, gCat, sizeof(gCat), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (v < 0 || v > 1) {
		return EINVAL;
	}
	mesa_locks();
	if (gCatLk == NULL) {
		return ENOMEM;
	}
	char pn[32];
	proc_selfname(pn, (int)sizeof(pn));
	IOLockLock(gCatLk);
	if (v && !gCat) {
		gCatLoaded = false;     /* a load is wanted; biometrickitd's NoCatacomb (no file) or an enrol sets it */
		gCatDirty = false;
		gCatSize = 0;
	}
	gCat = v;
	IOLockUnlock(gCatLk);
	SAY("catacomb persistence %s by %s (kern.sweet_mesa_catacomb=%d)\n", v ? "ON" : "off (1200 answers)", pn, v);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mesa_catacomb, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_mesa_catacomb, "I", "redfin 1206: 1 = Touch ID catacomb persistence (60 state from the TA db, "
    "61-64 store/load the FPC template db through sweetfpcd); 0 = 1200 behaviour");

static int
sysctl_sweet_mesa_cat_ms(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int v = gCatMs, changed = 0;
	int e = sysctl_io_number(req, gCatMs, sizeof(gCatMs), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (v < 300 || v > 30000) {
		return EINVAL;
	}
	gCatMs = v;
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mesa_cat_ms, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_mesa_cat_ms, "I", "redfin 1206: how long 61 / 64 wait for sweetfpcd's TA store / load (300..30000)");

static int
sysctl_sweet_mesa_cat_state(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	char b[320];
	snprintf(b, sizeof(b), "catacomb=%d user=%u loaded=%d dirty=%d gen=%u staged=%u loads=%d load_fail=%d "
	    "saves=%d save_fail=%d confirms=%d fresh=%d dirty_events=%d last_load=%u last_save=%u cat_ms=%d "
	    "offline_honest=%d reinit_sent=%d",
	    gCat, cat_user(), gCatLoaded, gCatDirty, (uint32_t)gCatGen, gCatSize, nCatLoads, nCatLoadFail, nCatSaves,
	    nCatSaveFail, nCatConfirms, nCatFresh, nCatDirtyEv, gCatLastLoad, gCatLastSave, gCatMs, gOffHonest, nReinit);
	return SYSCTL_OUT(req, b, strlen(b) + 1);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mesa_cat_state, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_mesa_cat_state, "A", "redfin 1206: catacomb persistence state and counters");

static int
sysctl_sweet_mesa_offline(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int v = gOffHonest, changed = 0;
	int e = sysctl_io_number(req, gOffHonest, sizeof(gOffHonest), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (v < 0 || v > 1) {
		return EINVAL;
	}
	mesa_locks();
	gOffHonest = v;
	SAY("offline answers %s (kern.sweet_mesa_offline=%d)\n", v ? "honest (NotReady) + 800d on connect" : "old", v);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mesa_offline, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_mesa_offline, "I", "redfin 1207: 1 = commands needing sweetfpcd answer NotReady while it is offline "
    "and 0xe3ff800d re-init is sent when it connects; 0 = old answers");

static int
sysctl_sweet_mesa_fwd_ms(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int v = gFwdMs, changed = 0;
	int e = sysctl_io_number(req, gFwdMs, sizeof(gFwdMs), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (v < 50 || v > 30000) {
		return EINVAL;
	}
	gFwdMs = v;
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mesa_fwd_ms, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_mesa_fwd_ms, "I", "redfin 1192: ms a forwarded command waits for sweetfpcd (50..30000)");

static int
sysctl_sweet_mesa_state(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	char b[800];
	int q = 0;
	if (gM) {
		IOLockLock(gM);
		for (int i = 0; i < MESA_MSGQ; i++) {
			q += gQ[i].data != NULL;
		}
		IOLockUnlock(gM);
	}
	snprintf(b, sizeof(b), "mesa=%d match=%d fwd_ms=%d published=%d abs_clients=%d bridge_clients=%d slot=%d "
	    "seq=%u cmds=%d canned=%d fwd=%d offline=%d timeouts=%d unsup=%d refused=%d events=%d fpcd_events=%d "
	    "delivered=%d queued=%d pulled=%d msgq=%d match_dropped=%d translated=%d logged_only=%d match_delivered=%d "
	    "match_refused=%d unverified=%d bio_unlock=%d bio_refused=%d extend=%d user=%u last=%u(%s)->0x%x",
	    gMesa, gMatch, gFwdMs, gDrv != NULL, gAbsClients, gBridgeClients, slot.state, gSeq, nCmds, nCanned, nFwd,
	    nOffline, nTimeouts, nUnsup, nRefused, nEvents, nFpcdEv, nDelivered, nQueued, nMsgPulled, q,
	    nMatchDropped, nTranslated, nLoggedOnly, nMatchDelivered, nMatchRefused, nUnverified, nBioUnlock, nBioRefused,
	    gMatchExtend, gUser, gLastCmd, mesa_name(gLastCmd),
	    gLastRet);
	return SYSCTL_OUT(req, b, strlen(b) + 1);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mesa_state, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_mesa_state, "A", "redfin 1192: SweetMesa counters");
#endif /* ARM64_BOARD_CONFIG_SWEET */
