/*
 * Built-in AppleKeyStore for SWEET: a device without a passcode whose
 * system keybag is always loaded and unlocked.
 *
 * Apple's AppleKeyStore kext (closed, SEP-backed) owns the keybags behind
 * data protection. libaks (AppleKeyStore.framework, statically in keybagd)
 * opens IOServiceMatching("AppleKeyStore") with type 0, calls selector 0,
 * then one selector per aks_* function (iOS 16.7 numbering below). keybagd
 * --init asks for the system bag (14) and only loads systembag.kb when the
 * answer is kIOReturnNotFound; answering with a handle is the path a
 * userspace reboot takes, so no bag file is needed.
 *
 * Every call is logged (selector, caller, argument shapes, result) so the
 * next selectors the system needs show up on the console; unknown ones fail
 * with kIOReturnUnsupported.
 */
#include <pexpert/arm64/board_config.h>

#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSString.h>
#include <sys/proc.h>

/*
 * 1195: passcode support (bsd/dev/sweet_passcode.c), only when kern.sweet_passcode != 0. At 0 every selector
 * takes the switch below exactly as before. The passcode is an authentication gate, never a key: wrap/unwrap
 * (10/11) ignore it and the file layer never sees a lock (design: passcode-acm-design-2026-11-18.md).
 */
extern "C" {
#include <sys/errno.h>
int sweet_pc_mode(void);
int sweet_pc_is_set(void);
void sweet_pc_aks_state(uint32_t *ss, uint32_t *sls, uint32_t *sfa, uint32_t *sb);
int sweet_pc_check_user(uint64_t uaddr, uint32_t len, int unlock);
int sweet_pc_check_kbuf(const uint8_t *pw, uint32_t len, int unlock);
int sweet_pc_change_user(uint64_t old_ua, uint32_t old_len, uint64_t new_ua, uint32_t new_len);
void sweet_pc_lock(void);
int sweet_pc_locked(void);
int sweet_pc_bio_pending(void);                 /* 1214 */
int sweet_pc_bio_confirm(int *was_pending);     /* 1214 */
void sweet_acm_publish(void);
void sweet_aks_notify(void);
}

/* libaks selectors (AppleKeyStore.framework, iOS 16.7). */
enum {
	kAKSOpen             = 0,
	kAKSCreateBag        = 2,
	kAKSSaveBag          = 3,
	kAKSUnloadBag        = 4,
	kAKSSetSystem        = 5,
	kAKSLoadBag          = 6,
	kAKSGetLockState     = 7,
	kAKSUnlockDevice     = 9,
	kAKSWrapKey          = 10,
	kAKSUnwrapKey        = 11,
	kAKSUnlockBag        = 12,
	kAKSLockBag          = 13,
	kAKSGetSystem        = 14,
	kAKSGetDeviceState   = 17,
	kAKSGetExtDeviceState = 35,     /* aks_get_extended_device_state */
	kAKSAssertHold       = 26,
	kAKSAssertDrop       = 27,
	kAKSUnloadSessionBags = 55,
	kAKSNotifyStateSet   = 102,
	kAKSNotifyStateIsSet = 103,
	kAKSAssertConsume    = 101,     /* aks_assert_consume */
};

/* The system (device) bag handle. */
#define SWEET_AKS_SYSTEM_HANDLE 0

/*
 * aks lock state bits (aks_get_lock_state): bit 2 is "unlocked since boot"
 * (MobileKeyBag MKBDeviceUnlockedSinceBoot), bit 1 "no passcode"; clear
 * lock bits = unlocked. 0910 (after sailfish 0180): with only bit 2 and lock
 * state 0, coreduetd asked thousands of times a minute; answer no passcode,
 * lock state disabled (HackiOS MiniAppleKeyStore.cpp does the same).
 */
#define SWEET_AKS_LOCK_STATE    0x6

/*
 * 1196: keybag status events. MobileKeyBag's MKBEventsRegister (SpringBoard's SBFMobileKeyBag, Preferences, ...)
 * adds an IOGeneralInterest notification on AppleKeyStore; aksNotificationCB maps message 0xe3ffd00d to
 * "keybag status changed" (SpringBoard re-reads the lock state, hasPasscodeSet) and 0xe3ffd00f to "first
 * unlock". Without it SpringBoard kept its boot-time "no passcode" state: it never called aks_lock_device and
 * the lock screen never asked. Sent only by sweet_passcode.c (passcode set/removed, lock, unlock, mode change).
 */
#define SWEET_AKS_MSG_STATUS    0xe3ffd00du
static IOService *gAKS;

class AppleKeyStore : public IOService {
	OSDeclareDefaultStructors(AppleKeyStore);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleKeyStore, IOService);

bool
AppleKeyStore::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "AppleKeyStoreUserClient");
	gAKS = this;
	registerService();
	IOLog("SWEET AKS: AppleKeyStore registered (no passcode, unlocked)\n");
	if (sweet_pc_mode() != 0) {
		sweet_acm_publish();    /* 1195: boot-arg sweetpasscode=1..3 */
	}
	return true;
}

class AppleKeyStoreUserClient : public IOUserClient {
	OSDeclareDefaultStructors(AppleKeyStoreUserClient);

public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleKeyStoreUserClient, IOUserClient);

IOReturn
AppleKeyStoreUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

/* Copy len bytes to the caller's output structure (inband or descriptor). */
static IOReturn
sweet_aks_struct_out(IOExternalMethodArguments *args, const void *buf, uint32_t len)
{
	if (args->structureOutputDescriptor != NULL) {
		if (len > args->structureOutputDescriptor->getLength()) {
			return kIOReturnNoSpace;
		}
		args->structureOutputDescriptor->writeBytes(0, buf, len);
		args->structureOutputDescriptorSize = len;
		return kIOReturnSuccess;
	}
	if (args->structureOutput == NULL || len > args->structureOutputSize) {
		return kIOReturnNoSpace;
	}
	memcpy(args->structureOutput, buf, len);
	args->structureOutputSize = len;
	return kIOReturnSuccess;
}

/* Append SEQUENCE { UTF8String key, INTEGER value } (short forms only). */
static uint32_t
sweet_aks_der_pair(uint8_t *p, const char *key, uint8_t value)
{
	uint32_t klen = (uint32_t)strlen(key);
	uint32_t n = 0;
	p[n++] = 0x30;
	p[n++] = (uint8_t)(2 + klen + 3);
	p[n++] = 0x0c;
	p[n++] = (uint8_t)klen;
	memcpy(p + n, key, klen);
	n += klen;
	p[n++] = 0x02;
	p[n++] = 0x01;
	p[n++] = value;
	return n;
}

/*
 * Device state (selector 17): a DER SET of key/INTEGER pairs that libaks
 * decode_extended_state reads: ss state, sls lock state (MKBGetDeviceLockState:
 * 0 = unlocked), sb backoff, sfa failed attempts, sgs generation, sas
 * assertions, sgpe grace period, srcd recovery countdown.
 */
static IOReturn
sweet_aks_device_state(IOExternalMethodArguments *args)
{
	static const struct {
		const char *key;
		uint8_t value;
	} fields[] = {
		{ "ss", SWEET_AKS_LOCK_STATE }, { "sls", 3 }, { "sb", 0 }, { "sfa", 0 },
		{ "sgs", 0 }, { "sas", 0 }, { "sgpe", 0 }, { "srcd", 0 },
	};
	uint8_t buf[128];
	uint32_t n = 2;
	for (unsigned int i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		n += sweet_aks_der_pair(buf + n, fields[i].key, fields[i].value);
	}
	buf[0] = 0x31;
	buf[1] = (uint8_t)(n - 2);
	return sweet_aks_struct_out(args, buf, n);
}

static uint32_t
sweet_aks_struct_out_size(IOExternalMethodArguments *args)
{
	return args->structureOutputDescriptor != NULL ?
	       (uint32_t)args->structureOutputDescriptor->getLength() : args->structureOutputSize;
}

extern "C" void
sweet_aks_notify(void)
{
	if (gAKS != NULL) {
		gAKS->messageClients(SWEET_AKS_MSG_STATUS);
	}
}

/* ---- 1195: passcode-mode selectors (kern.sweet_passcode != 0) ---- */
enum {
	kAKSLockDevice       = 8,       /* aks_lock_device (32 = lock now) */
	kAKSChangeSecret     = 15,      /* aks_change_secret(_se): 11 scalars */
	kAKSGeneration       = 40,
	kAKSVerifyPassword   = 42,      /* packed struct, item 0 = passcode */
	kAKSLockDeviceNow    = 32,
	kAKSChangeSecretEpilogue = 105,
};
#define SWEET_AKS_BAD_PASSCODE  ((IOReturn)0xe007c008)  /* MobileKeyBag -> -14; SEP status -8 */

static IOReturn
pc_ret(int e)
{
	switch (e) {
	case 0:
		return kIOReturnSuccess;
	case EAUTH:
		return SWEET_AKS_BAD_PASSCODE;
	case EBUSY:
		return kIOReturnNotPermitted;   /* backoff */
	case EFAULT:
	case EINVAL:
		return kIOReturnBadArgument;
	default:
		return kIOReturnError;
	}
}

/* SEQUENCE { UTF8String key, INTEGER value } with a minimal two's-complement INTEGER (0..2^31). */
static uint32_t
pc_der_pair(uint8_t *p, const char *key, uint32_t value)
{
	uint8_t iv[5];
	uint32_t il = 0, klen = (uint32_t)strlen(key), n = 0;
	for (int sh = 24; sh >= 0; sh -= 8) {
		uint8_t b = (uint8_t)(value >> sh);
		if (il == 0 && b == 0 && sh > 0) {
			continue;
		}
		if (il == 0 && (b & 0x80)) {
			iv[il++] = 0;
		}
		iv[il++] = b;
	}
	p[n++] = 0x30;
	p[n++] = (uint8_t)(2 + klen + 2 + il);
	p[n++] = 0x0c;
	p[n++] = (uint8_t)klen;
	memcpy(p + n, key, klen);
	n += klen;
	p[n++] = 0x02;
	p[n++] = (uint8_t)il;
	memcpy(p + n, iv, il);
	return n + il;
}

static IOReturn
pc_device_state(IOExternalMethodArguments *args, bool ext)
{
	uint32_t ss, sls, sfa, sb;
	sweet_pc_aks_state(&ss, &sls, &sfa, &sb);
	if (ext && sls == 0 && sweet_pc_bio_pending()) {
		sls = 7;        /* 1214: InBioUnlock (extended state only), see sweet_passcode.c */
		IOLog("SWEET KS: extended device state: lock state 7 (InBioUnlock)\n");
	}
	const struct {
		const char *key;
		uint32_t value;
	} fields[] = {
		{ "ss", ss }, { "sls", sls }, { "sb", sb }, { "sfa", sfa },
		{ "sgs", 0 }, { "sas", 0 }, { "sgpe", 0 }, { "srcd", 0 },
	};
	uint8_t buf[160];
	uint32_t n = 2;
	for (unsigned int i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		n += pc_der_pair(buf + n, fields[i].key, fields[i].value);
	}
	buf[0] = 0x31;
	buf[1] = (uint8_t)(n - 2);
	return sweet_aks_struct_out(args, buf, n);
}

/* aks_pack_data: u32 count, then per item u32 len + data padded to 4. Item 0 = passcode. */
static IOReturn
pc_verify_packed(IOExternalMethodArguments *args)
{
	uint8_t buf[512];
	uint32_t len;
	if (args->structureInputDescriptor != NULL) {
		len = (uint32_t)args->structureInputDescriptor->getLength();
		if (len > sizeof(buf) || args->structureInputDescriptor->prepare() != kIOReturnSuccess) {
			return kIOReturnBadArgument;
		}
		args->structureInputDescriptor->readBytes(0, buf, len);
		args->structureInputDescriptor->complete();
	} else {
		len = args->structureInputSize;
		if (args->structureInput == NULL || len > sizeof(buf)) {
			return kIOReturnBadArgument;
		}
		memcpy(buf, args->structureInput, len);
	}
	IOReturn r = kIOReturnBadArgument;
	uint32_t cnt, ilen;
	if (len >= 8) {
		memcpy(&cnt, buf, 4);
		memcpy(&ilen, buf + 4, 4);
		if (cnt >= 1 && ilen <= len - 8) {
			r = pc_ret(sweet_pc_check_kbuf(buf + 8, ilen, 0));
		}
	}
	bzero(buf, sizeof(buf));
	return r;
}

/* true = handled here (ret set); false = take the existing switch. */
static bool
sweet_aks_pc(uint32_t selector, IOExternalMethodArguments *args, IOReturn *ret, bool *logall)
{
	uint32_t ns = args->scalarInputCount, no = args->scalarOutputCount;
	const uint64_t *in = args->scalarInput;
	*logall = true;
	switch (selector) {
	case kAKSGetLockState: {
		uint32_t ss, sls, sfa, sb;
		*logall = false;
		if (no < 1) {
			*ret = kIOReturnBadArgument;
			return true;
		}
		sweet_pc_aks_state(&ss, &sls, &sfa, &sb);
		args->scalarOutput[0] = ss;
		args->scalarOutputCount = 1;
		*ret = kIOReturnSuccess;
		return true;
	}
	case kAKSGetDeviceState:
	case kAKSGetExtDeviceState:
		*logall = false;
		args->scalarOutputCount = 0;
		*ret = pc_device_state(args, selector == kAKSGetExtDeviceState);
		return true;
	case kAKSLockDevice:
	case kAKSLockDeviceNow:
		sweet_pc_lock();
		args->scalarOutputCount = 0;
		*ret = kIOReturnSuccess;
		return true;
	case kAKSUnlockDevice:
		args->scalarOutputCount = 0;
		if (ns >= 3 && in[2] == 0) {
			/* 1214: no passcode = SpringBoard confirming a Touch ID unlock (kern.sweet_passcode_biostate) */
			int wp = 0, e = sweet_pc_bio_confirm(&wp);
			if (e != ENOTSUP) {
				*ret = pc_ret(e);
				return true;
			}
		}
		*ret = ns >= 3 ? pc_ret(sweet_pc_check_user(in[1], (uint32_t)in[2], 1)) : kIOReturnBadArgument;
		return true;
	case kAKSUnlockBag:
		if (ns < 3 || in[0] != SWEET_AKS_SYSTEM_HANDLE) {
			return false;   /* other bags: as before */
		}
		args->scalarOutputCount = 0;
		*ret = pc_ret(sweet_pc_check_user(in[1], (uint32_t)in[2], 0));
		return true;
	case kAKSChangeSecret:
		if (ns < 7) {
			*ret = kIOReturnBadArgument;
			return true;
		}
		*ret = pc_ret(sweet_pc_change_user(in[3], (uint32_t)in[4], in[5], (uint32_t)in[6]));
		if (no >= 1) {
			args->scalarOutput[0] = 0;
			args->scalarOutputCount = 1;
		}
		return true;
	case kAKSChangeSecretEpilogue:
	case kAKSGeneration:
		for (uint32_t i = 0; i < no; i++) {
			args->scalarOutput[i] = 0;
		}
		*ret = kIOReturnSuccess;
		return true;
	case kAKSAssertHold:
		/*
		 * 1197: a lock assertion (aks_assert_hold) keeps an *unlocked* bag unlocked; real AKS refuses it while
		 * the bag is locked. SpringBoard's grace-period assertion (SBFAuthenticationAssertionTracker ->
		 * MKBDeviceLockAssertion, sel 26 type 2) right after aks_lock_device decides "authenticated": our
		 * old blanket success meant the lock screen never asked. Locked -> refused; unlocked -> as before.
		 */
		if (!sweet_pc_locked()) {
			return false;
		}
		args->scalarOutputCount = 0;
		*ret = kIOReturnNotPermitted;
		return true;
	case kAKSAssertDrop:
		return false;   /* as before, but logged every time in passcode mode */
	case kAKSVerifyPassword:
		args->scalarOutputCount = 0;
		*ret = pc_verify_packed(args);
		return true;
	case kAKSSaveBag:
		args->scalarOutputCount = 0;
		*ret = sweet_aks_struct_out(args, "SWEETKB1", 8);
		return true;
	default:
		*logall = false;
		return false;
	}
}

IOReturn
AppleKeyStoreUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint32_t in_scalars = args->scalarInputCount;
	uint32_t in_struct = args->structureInputDescriptor != NULL ?
	    (uint32_t)args->structureInputDescriptor->getLength() : args->structureInputSize;
	uint32_t out_scalars = args->scalarOutputCount;
	uint32_t out_struct = sweet_aks_struct_out_size(args);
	uint64_t arg0 = in_scalars > 0 ? args->scalarInput[0] : 0;

	bool logall = false;
	bool pc = sweet_pc_mode() != 0 && sweet_aks_pc(selector, args, &ret, &logall);

	if (!pc) switch (selector) {
	case kAKSOpen:
	case kAKSUnloadBag:
	case kAKSSetSystem:
	case kAKSUnlockDevice:
	case kAKSUnlockBag:
	case kAKSLockBag:
	case kAKSAssertHold:
	case kAKSAssertDrop:
	case kAKSAssertConsume:
	case kAKSUnloadSessionBags:
	case kAKSNotifyStateSet:
		args->scalarOutputCount = 0;
		break;
	case kAKSGetSystem:
		if (out_scalars < 1) {
			ret = kIOReturnBadArgument;
			break;
		}
		args->scalarOutput[0] = SWEET_AKS_SYSTEM_HANDLE;
		args->scalarOutputCount = 1;
		break;
	case kAKSGetLockState:
		if (out_scalars < 1) {
			ret = kIOReturnBadArgument;
			break;
		}
		args->scalarOutput[0] = SWEET_AKS_LOCK_STATE;
		args->scalarOutputCount = 1;
		break;
	case kAKSNotifyStateIsSet:
		/* Nothing was ever set: answer "not set". */
		if (out_scalars < 1) {
			ret = kIOReturnBadArgument;
			break;
		}
		args->scalarOutput[0] = 0;
		args->scalarOutputCount = 1;
		break;
	case kAKSGetDeviceState:
	case kAKSGetExtDeviceState:
		args->scalarOutputCount = 0;
		ret = sweet_aks_device_state(args);
		break;
	/*
	 * (from HackiOS) kAKSWrapKey / kAKSUnwrapKey: securityd wraps each keychain item's key with
	 * its protection class before storing it. No SEP, no secret: a mark, the class and the key
	 * mixed with a fixed pattern, 8 bytes longer like AES key wrap. Without them no keychain item
	 * (wifid's network passwords among them) could be stored.
	 */
	case kAKSWrapKey: {
		uint8_t wrapped[8 + 64];
		size_t len = args->structureInputSize;
		if (args->structureInput == NULL || len == 0 || len > 64 || in_scalars < 2) {
			ret = kIOReturnBadArgument;
			break;
		}
		memcpy(wrapped, "HKwr", 4);
		wrapped[4] = (uint8_t)args->scalarInput[1];
		wrapped[5] = wrapped[6] = wrapped[7] = 0;
		memcpy(wrapped + 8, args->structureInput, len);
		for (size_t i = 0; i < len; i++) {
			wrapped[8 + i] ^= (uint8_t)(0x5a + 37 * i);
		}
		if (out_scalars >= 1) {
			args->scalarOutput[0] = args->scalarInput[1];
			args->scalarOutputCount = 1;
		}
		ret = sweet_aks_struct_out(args, wrapped, (uint32_t)(8 + len));
		break;
	}
	case kAKSUnwrapKey: {
		uint8_t key[64];
		const uint8_t *w = (const uint8_t *)args->structureInput;
		size_t len = args->structureInputSize;
		if (w == NULL || len <= 8 || len > 8 + sizeof(key) || memcmp(w, "HKwr", 4) != 0) {
			ret = kIOReturnBadArgument;
			break;
		}
		memcpy(key, w + 8, len - 8);
		for (size_t i = 0; i < len - 8; i++) {
			key[i] ^= (uint8_t)(0x5a + 37 * i);
		}
		args->scalarOutputCount = 0;
		ret = sweet_aks_struct_out(args, key, (uint32_t)(len - 8));
		break;
	}
	default:
		ret = kIOReturnUnsupported;
		break;
	}

	static uint32_t logged[128];            /* a few lines per selector */
	if (logall || selector >= 128 || logged[selector]++ < 5) {
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("SWEET AKS: %s sel %u arg0 %llu in %u/%u out %u/%u -> 0x%x\n", name, selector,
		    arg0, in_scalars, in_struct, out_scalars, out_struct, ret);
	}
	return ret;
}
#endif /* SWEET */
