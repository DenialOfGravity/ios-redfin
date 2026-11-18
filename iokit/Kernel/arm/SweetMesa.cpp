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
       FEV_NOMATCH, FEV_MATCH, FEV_MATCH_WITHHELD, FEV_TEMPLATES, FEV_OP_TIMEOUT, FEV_ERROR };
#define FEV_MF_HAT_VERIFIED     1u

/* Apple message indexes (0xe3ff8000 + idx) and 0xe3ff8001 status ordinals (libBKDM1 BLHelper status table) */
#define MIDX_STATUS             0x1
#define MIDX_MATCH_RESULT       0x2             /* message_match_node_v2_t, >= 0xc88 + 4 * lotlItems */
#define MIDX_ENROLL_RESULT      0x3             /* message_enroll_node_v2_t, 0x28 */
#define MIDX_MATCH_EVENT        0xb
#define ST_MATCHING_FAILED      0x33
#define ST_MATCHING_STOPPED     0x3a
#define ST_FINGER_ON            0x3f
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
static int gCatState = 3;               /* kern.sweet_mesa_catstate: 60 GetCatacombState value (1200) */
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
		return r;
	case FEV_ENROL_FAILED:
		gLastPct = 0;
		SAY("enrol failed: status %d flags 0x%x -> EnrollmentFailed\n", st, fl);
		return mesa_status(ST_ENROLL_FAILED);
	case FEV_OP_TIMEOUT:
		gLastPct = 0;
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
		if (identity && !gMatchExtend) {
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
mesa_forward(uint32_t cmd, const uint8_t *p, uint32_t plen, uint8_t *rbuf, uint32_t *rlen, bool *offline)
{
	*offline = false;
	*rlen = 0;
	if (plen > SMB_DATA_MAX) {
		return kIOReturnMessageTooLarge;
	}
	uint64_t dl = deadline_ms((uint32_t)gFwdMs);
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
	SAY("cmd %u %s -> bridge seq %u (%u bytes, waits <= %d ms)\n", cmd, mesa_name(cmd), slot.seq, plen, gFwdMs);
	IOLockWakeup(gM, &gEvQueued, false);
	while (slot.state != SLOT_DONE) {
		if (IOLockSleepDeadline(gM, &gEvDone, dl, THREAD_ABORTSAFE) != THREAD_AWAKENED && slot.state != SLOT_DONE) {
			SAY("cmd %u %s seq %u: no reply from the bridge in %d ms (%s)\n", cmd, mesa_name(cmd), slot.seq,
			    gFwdMs, slot.state == SLOT_QUEUED ? "never picked up" : "picked up, not answered");
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
	case 48:        /* GetEnabledForUnlock: no */
		ANSWER(1); return kIOReturnSuccess;
	case 50:        /* GetTemplatesValidity: valid */
		ANSWER(1); out[0] = 1; return kIOReturnSuccess;
	case 34: {      /* GetModuleSerialNumber */
		static const char sn[] = "SWEETFPC1020";
		ANSWER(17); bcopy(sn, out, sizeof(sn)); return kIOReturnSuccess;
	}
	case 39:        /* GetSKSLockState: unlocked (SweetKeyStore) */
		ANSWER(4); return kIOReturnSuccess;
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
		*how = "canned (zeros)";
		return kIOReturnSuccess;
	case 46: ANSWER(32); return kIOReturnSuccess;          /* GetProtectedConfig: nothing enabled */
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

		/* 1192d: the identities' user id (EnrollMode v2 payload +4, 65/66 payload +0) for enrol/match records */
		if (cmd == 3) {
			gMatchExtend = false;
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
		default: break;
		}
		if (cmd == 12 || cmd == 38 || cmd == 69) {         /* 1192b: posted, never blocks the caller */
			r = mesa_post(cmd, fp, flen);
			if (r == kIOReturnSuccess) {
				*how = "posted";
				ANSWER(0);
				return kIOReturnSuccess;
			}
			OSIncrementAtomic(&nOffline);
			*how = "offline";
			if (cmd == 38) {
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
	if (inLen < 6 || inLen > 0x10000) {
		SAY("%s sel 0 with %u input bytes refused\n", pn, inLen);
		return kIOReturnBadArgument;
	}
	in = (uint8_t *)IOMallocData(inLen);
	out = (uint8_t *)IOMallocData(ocap ? ocap : 1);
	if (in == NULL || out == NULL) {
		r = kIOReturnNoMemory;
		goto done;
	}
	if (!in_read(args, in, inLen)) {
		r = kIOReturnVMError;
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
	IOLockUnlock(gM);
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
