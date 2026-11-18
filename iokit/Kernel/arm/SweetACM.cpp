/*
 * redfin 1195: SweetACM, a stand-in for Apple's credential manager (AppleCredentialManager +
 * AppleCredentialManagerUserClient) for libACM (LocalAuthentication SharedUtils: coreauthd, ModuleACM,
 * Security). Design: logs/research/passcode-acm-design-2026-11-18.md.
 *
 * Published on IOResources only when kern.sweet_passcode != 0 (boot-arg sweetpasscode=1..3 or the sysctl);
 * at 0 it does not exist (as today), and if the mode drops back to 0 every call answers kIOReturnNotPermitted.
 *
 * User client (iPhone 8 kernelcache): selector 0 = command channel (struct in/out), 1 and 2 = one scalar out.
 * Command buffer (libACM LibCall_BuildCommand): le32 'DRCS' | u8 cmd | u8 0 | u8 arg | u8 0 | payload.
 * Errors are the IOReturn itself in libACM's numbering (-3: unsupported / bad parameter; ContextCreate falls
 * back from cmd 36 to cmd 1 on -3).
 *
 * Emulated: contexts (1, 2, 18), AddCredential (5): PassphraseEntered is checked against the stored passcode
 * verifier (sweet_passcode.c, same failure counter/backoff), PasscodeValidated cannot be added from userland,
 * BiometryMatched only when the kernel armed a verified match (sweet_acm_note_verified_match, no caller yet:
 * SweetMesa calls it later, only for keymaster-verified matches); RemoveCredentials (6, 9); VerifyPolicy (3):
 * satisfied iff the context holds a fresh passcode-validated or verified biometric credential.
 * Accepted no-ops: 26 KernelControl, 28 DoubleClickNotify, 44 SetEnvironmentVariable. Everything else -3.
 * Every command is logged; payload bytes only for commands that cannot carry a secret (never cmd 5).
 */
#include <pexpert/arm64/board_config.h>

#if defined(SWEET)
#include <sys/sysctl.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/OSAtomic.h>

extern "C" {
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/random.h>
#include <kern/clock.h>
int sweet_pc_mode(void);
int sweet_pc_check_kbuf(const uint8_t *pw, uint32_t len, int unlock);
void sweet_acm_publish(void);
void sweet_acm_note_verified_match(void);
}

#define SAY(fmt, ...) IOLog("SWEET ACM: " fmt, ##__VA_ARGS__)

#define ACM_MAGIC       0x53435244u     /* 'DRCS' */
#define ACM_HDR         8
#define ACM_IO_MAX      16384
#define ACM_CTX_MAX     64
#define ACM_ID          16
#define ACM_CRED_HDR    32              /* u32 type @0 ... u32 data length @0x1c */
#define ACM_TTL_S       600             /* passcode / biometric credential freshness */
#define ACM_BIO_ARM_S   5               /* a verified match must be consumed within this */
#define ACM_ERR         ((IOReturn)-3)

/* credential types (kernel ACM name table order) */
enum {
	kCredPasscodeValidated = 1, kCredPassphraseEntered = 2, kCredBiometryMatched = 3, kCredUserConsent = 4,
	kCredPasscodeValidated2 = 10, kCredDeviceRestrictedMode = 11, kCredMax = 21,
};

/* libACM commands (SharedUtils LibCall_*) */
static const char *
acm_name(uint32_t c)
{
	switch (c) {
	case 1: return "ContextCreate";
	case 2: return "ContextDelete";
	case 3: return "VerifyPolicy";
	case 5: return "AddCredential";
	case 6: return "RemoveCredentialsByValue";
	case 9: return "RemoveCredentialsByType";
	case 18: return "ContextCreateWithExternalForm";
	case 25: return "GetEnvironmentVariable";
	case 26: return "KernelControl";
	case 27: return "GlobalContextCredentialGetProperty";
	case 28: return "KernDoubleClickNotify";
	case 31: return "GetAclAuthMethod";
	case 33: return "ContextCredentialGetProperty";
	case 36: return "ContextCreate2";
	case 37: return "ContextCreateWithExternalForm2";
	case 38: return "TRMLoadState";
	case 40: return "ContextSetData";
	case 41: return "ContextGetData";
	case 44: return "SetEnvironmentVariable";
	case 45: return "PublishTrustedAccessories";
	case 46: return "ContextGetInfo";
	case 48: return "ContextLoadFromImage";
	case 49: return "ContextUnloadToImage";
	default: return "?";
	}
}

struct acm_ctx {
	bool used;
	uint8_t id[ACM_ID];
	uint64_t serial;
	clock_sec_t pass_at, bio_at;
	uint32_t creds;                 /* bit per credential type added */
};

static acm_ctx gCtx[ACM_CTX_MAX];
static acm_ctx gGlobal;                 /* id all zero */
static IOLock *gL;
static uint64_t gSerial;
static clock_sec_t gBioArmedAt;
static volatile UInt32 gPublished;
static uint32_t nCmds, nUnsup, nRefused, nPassOk, nPassBad, nBioOk, nBioRefused, nCtx, nPolicyYes, nPolicyNo;
static uint32_t gLastCmd;
static IOReturn gLastRet;
static uint32_t gLogged[64];

static clock_sec_t
acm_now(void)
{
	clock_sec_t s;
	clock_usec_t us;
	clock_get_system_microtime(&s, &us);
	return s;
}

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* gL held */
static acm_ctx *
ctx_find(const uint8_t *id)
{
	static const uint8_t zero[ACM_ID] = { 0 };
	if (memcmp(id, zero, ACM_ID) == 0) {
		return &gGlobal;
	}
	for (int i = 0; i < ACM_CTX_MAX; i++) {
		if (gCtx[i].used && memcmp(gCtx[i].id, id, ACM_ID) == 0) {
			return &gCtx[i];
		}
	}
	return NULL;
}

/* gL held: a free slot, else the oldest */
static acm_ctx *
ctx_new(void)
{
	acm_ctx *c = NULL;
	for (int i = 0; i < ACM_CTX_MAX && c == NULL; i++) {
		if (!gCtx[i].used) {
			c = &gCtx[i];
		}
	}
	if (c == NULL) {
		c = &gCtx[0];
		for (int i = 1; i < ACM_CTX_MAX; i++) {
			if (gCtx[i].serial < c->serial) {
				c = &gCtx[i];
			}
		}
	}
	bzero(c, sizeof(*c));
	c->used = true;
	c->serial = ++gSerial;
	read_random(c->id, ACM_ID);
	c->id[0] |= 1;                  /* never the all-zero global id */
	nCtx++;
	return c;
}

static bool
fresh(clock_sec_t t, clock_sec_t now)
{
	return t != 0 && now - t <= ACM_TTL_S;
}

/* One command. p/n = payload after the 8-byte header. Fills o (cap ocap), *olen. */
static IOReturn
acm_cmd(uint32_t cmd, const uint8_t *p, uint32_t n, uint8_t *o, uint32_t ocap, uint32_t *olen, char *note,
    size_t notesz)
{
	IOReturn r = kIOReturnSuccess;
	clock_sec_t now = acm_now();
	acm_ctx *c;
	*olen = 0;
	switch (cmd) {
	case 1:                                 /* ContextCreate: out id16 + u8 */
		if (ocap < ACM_ID + 1) {
			return ACM_ERR;
		}
		IOLockLock(gL);
		c = ctx_new();
		memcpy(o, c->id, ACM_ID);
		o[ACM_ID] = 0;
		IOLockUnlock(gL);
		*olen = ACM_ID + 1;
		snprintf(note, notesz, "ctx #%llu", c->serial);
		return r;
	case 18:                                /* ContextCreateWithExternalForm: id16 ... */
		if (n < ACM_ID || ocap < ACM_ID + 1) {
			return ACM_ERR;
		}
		IOLockLock(gL);
		c = ctx_find(p);
		if (c != NULL && c != &gGlobal) {
			memcpy(o, c->id, ACM_ID);
			o[ACM_ID] = 0;
			*olen = ACM_ID + 1;
			snprintf(note, notesz, "ctx #%llu", c->serial);
		} else {
			r = ACM_ERR;
			snprintf(note, notesz, "unknown context");
		}
		IOLockUnlock(gL);
		return r;
	case 2:                                 /* ContextDelete: id16 */
		if (n < ACM_ID) {
			return ACM_ERR;
		}
		IOLockLock(gL);
		c = ctx_find(p);
		if (c != NULL && c != &gGlobal) {
			snprintf(note, notesz, "ctx #%llu", c->serial);
			bzero(c, sizeof(*c));
		}
		IOLockUnlock(gL);
		return r;
	case 5: {                               /* AddCredential: id16 | cred hdr32 + data | u32 scope */
		if (n < ACM_ID + ACM_CRED_HDR) {
			return ACM_ERR;
		}
		uint32_t type = le32(p + ACM_ID), dlen = le32(p + ACM_ID + 0x1c);
		const uint8_t *data = p + ACM_ID + ACM_CRED_HDR;
		if (dlen > n - ACM_ID - ACM_CRED_HDR || type > kCredMax) {
			return ACM_ERR;
		}
		if (type == kCredPasscodeValidated || type == kCredPasscodeValidated2 || type == kCredDeviceRestrictedMode) {
			nRefused++;
			snprintf(note, notesz, "type %u REFUSED (only SweetACM grants it)", type);
			return ACM_ERR;
		}
		if (type == kCredPassphraseEntered) {
			int e = sweet_pc_check_kbuf(data, dlen, 0);     /* never logged: length only */
			IOLockLock(gL);
			c = ctx_find(p);
			if (c != NULL && e == 0) {
				c->pass_at = now;
				c->creds |= 1u << kCredPasscodeValidated;
			}
			IOLockUnlock(gL);
			if (e == 0) {
				nPassOk++;
			} else {
				nPassBad++;
			}
			snprintf(note, notesz, "passphrase len %u -> %s%s", dlen, e == 0 ? "validated" : e == EBUSY ? "backoff" :
			    "WRONG (not granted)", c == NULL ? " (unknown context)" : "");
			return c == NULL ? ACM_ERR : kIOReturnSuccess;
		}
		if (type == kCredBiometryMatched) {
			IOLockLock(gL);
			bool armed = gBioArmedAt != 0 && now - gBioArmedAt <= ACM_BIO_ARM_S;
			c = ctx_find(p);
			if (armed && c != NULL) {
				gBioArmedAt = 0;                        /* one match, one credential */
				c->bio_at = now;
				c->creds |= 1u << kCredBiometryMatched;
			}
			IOLockUnlock(gL);
			if (!armed || c == NULL) {
				nBioRefused++;
				snprintf(note, notesz, "BiometryMatched REFUSED: no kernel-verified match armed");
				return ACM_ERR;
			}
			nBioOk++;
			snprintf(note, notesz, "BiometryMatched from a verified match");
			return r;
		}
		IOLockLock(gL);
		c = ctx_find(p);
		if (c != NULL) {
			c->creds |= 1u << type;
		}
		IOLockUnlock(gL);
		snprintf(note, notesz, "type %u len %u recorded%s", type, dlen, c == NULL ? " (unknown context)" : "");
		return c == NULL ? ACM_ERR : r;
	}
	case 6:                                 /* RemoveCredentialsByValue(AndScope) */
	case 9:                                 /* RemoveCredentialsByType(AndScope) */
		if (n < ACM_ID) {
			return ACM_ERR;
		}
		IOLockLock(gL);
		c = ctx_find(p);
		if (c != NULL) {
			c->creds = 0;
			c->pass_at = 0;
			c->bio_at = 0;
		}
		IOLockUnlock(gL);
		return r;
	case 3: {                               /* VerifyPolicy: id16 | policy cstring | u8 | u32 | params */
		if (n < ACM_ID + 1) {
			return ACM_ERR;
		}
		char pol[48];
		uint32_t pl = 0;
		while (ACM_ID + pl < n && pl < sizeof(pol) - 1 && p[ACM_ID + pl] >= 0x20 && p[ACM_ID + pl] < 0x7f) {
			pol[pl] = (char)p[ACM_ID + pl];
			pl++;
		}
		pol[pl] = 0;
		IOLockLock(gL);
		c = ctx_find(p);
		bool sat = c != NULL && (fresh(c->pass_at, now) || fresh(c->bio_at, now));
		IOLockUnlock(gL);
		if (sat) {
			nPolicyYes++;
		} else {
			nPolicyNo++;
		}
		snprintf(note, notesz, "policy '%s' -> %s%s", pol, sat ? "satisfied" : "NOT satisfied",
		    c == NULL ? " (unknown context)" : "");
		if (ocap < 4 + 16) {
			return ACM_ERR;
		}
		bzero(o, 20);
		put32(o, sat ? 1 : 0);
		put32(o + 4, 1);                /* requirement: type 1 (passcode) */
		put32(o + 8, sat ? 1 : 0);      /* state */
		*olen = 20;
		return r;
	}
	case 26:
	case 28:
	case 44:
		snprintf(note, notesz, "accepted (no-op)");
		return r;
	default:
		nUnsup++;
		return ACM_ERR;
	}
}

class AppleCredentialManager : public IOService {
	OSDeclareDefaultStructors(AppleCredentialManager);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleCredentialManager, IOService);

bool
AppleCredentialManager::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "AppleCredentialManagerUserClient");
	return true;
}

class AppleCredentialManagerUserClient : public IOUserClient {
	OSDeclareDefaultStructors(AppleCredentialManagerUserClient);

public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleCredentialManagerUserClient, IOUserClient);

IOReturn
AppleCredentialManagerUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

/* descriptor copies are prepare()d (1194) */
static bool
md_copy(IOMemoryDescriptor *md, void *buf, uint32_t len, bool out)
{
	if (len == 0) {
		return true;
	}
	if (md->prepare() != kIOReturnSuccess) {
		return false;
	}
	IOByteCount k = out ? md->writeBytes(0, buf, len) : md->readBytes(0, buf, len);
	md->complete();
	return k == len;
}

IOReturn
AppleCredentialManagerUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	char pn[32], note[112];
	IOReturn r = kIOReturnSuccess;
	uint32_t in_len = args->structureInputDescriptor != NULL ?
	    (uint32_t)args->structureInputDescriptor->getLength() : args->structureInputSize;
	uint32_t out_cap = args->structureOutputDescriptor != NULL ?
	    (uint32_t)args->structureOutputDescriptor->getLength() : args->structureOutputSize;
	uint32_t cmd = 0, olen = 0;
	uint8_t *in = NULL, *out = NULL;
	bool dump = false;
	note[0] = 0;
	proc_selfname(pn, (int)sizeof(pn));

	if (sweet_pc_mode() == 0) {
		r = kIOReturnNotPermitted;
		snprintf(note, sizeof(note), "kern.sweet_passcode=0");
		goto done;
	}
	if (selector == 1 || selector == 2) {
		for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
			args->scalarOutput[i] = 0;
		}
		goto done;
	}
	if (selector != 0) {
		r = kIOReturnUnsupported;
		goto done;
	}
	if (in_len < ACM_HDR || in_len > ACM_IO_MAX || out_cap > ACM_IO_MAX) {
		r = ACM_ERR;
		goto done;
	}
	in = (uint8_t *)IOMallocData(in_len);
	out = out_cap ? (uint8_t *)IOMallocZeroData(out_cap) : NULL;
	if (in == NULL || (out_cap && out == NULL)) {
		r = kIOReturnNoMemory;
		goto done;
	}
	if (args->structureInputDescriptor != NULL) {
		if (!md_copy(args->structureInputDescriptor, in, in_len, false)) {
			r = kIOReturnVMError;
			goto done;
		}
	} else {
		memcpy(in, args->structureInput, in_len);
	}
	if (le32(in) != ACM_MAGIC) {
		r = ACM_ERR;
		snprintf(note, sizeof(note), "bad magic 0x%08x", le32(in));
		goto done;
	}
	cmd = in[4];
	nCmds++;
	r = acm_cmd(cmd, in + ACM_HDR, in_len - ACM_HDR, out, out_cap, &olen, note, sizeof(note));
	dump = (cmd != 5 && r == ACM_ERR && note[0] == 0);
	if (olen > 0) {
		if (args->structureOutputDescriptor != NULL) {
			if (md_copy(args->structureOutputDescriptor, out, olen, true)) {
				args->structureOutputDescriptorSize = olen;
			} else {
				r = kIOReturnVMError;
			}
		} else {
			memcpy(args->structureOutput, out, olen);
			args->structureOutputSize = olen;
		}
	} else if (args->structureOutputDescriptor != NULL) {
		args->structureOutputDescriptorSize = 0;
	} else {
		args->structureOutputSize = 0;
	}
	gLastCmd = cmd;
	gLastRet = r;
done:
	if (selector != 0 || cmd >= 64 || gLogged[cmd]++ < 40 || r != kIOReturnSuccess || (gLogged[cmd] % 100) == 0) {
		char hx[3 * 24 + 1];
		hx[0] = 0;
		if (dump && in != NULL) {
			uint32_t k = in_len - ACM_HDR < 24 ? in_len - ACM_HDR : 24;
			for (uint32_t i = 0; i < k; i++) {
				snprintf(hx + 3 * i, 4, "%02x ", in[ACM_HDR + i]);
			}
		}
		SAY("%s sel %u cmd %u(%s) arg %u in %u out %u/%u -> 0x%x %s%s%s\n", pn, selector, cmd, acm_name(cmd),
		    in != NULL && in_len >= ACM_HDR ? in[6] : 0, in_len, olen, out_cap, r, note, hx[0] ? " | " : "", hx);
	}
	if (in != NULL) {
		bzero(in, in_len);              /* may hold a passphrase */
		IOFreeData(in, in_len);
	}
	if (out != NULL) {
		IOFreeData(out, out_cap);
	}
	return r;
}

/* ---- publish (once; never withdrawn: at mode 0 calls are refused) ---- */
extern "C" void
sweet_acm_publish(void)
{
	if (!OSCompareAndSwap(0, 1, &gPublished)) {
		return;
	}
	gL = IOLockAlloc();
	IOService *res = IOService::getResourceService();
	AppleCredentialManager *s = OSTypeAlloc(AppleCredentialManager);
	if (gL == NULL || res == NULL || s == NULL || !s->init()) {
		SAY("publish failed (alloc)\n");
		OSSafeReleaseNULL(s);
		return;
	}
	if (!s->attach(res)) {
		s->release();
		SAY("publish failed (attach)\n");
		return;
	}
	if (!s->start(res)) {
		s->detach(res);
		s->release();
		SAY("publish failed (start)\n");
		return;
	}
	s->registerService();
	SAY("AppleCredentialManager published (user client AppleCredentialManagerUserClient)\n");
}

/*
 * Kernel-only: SweetMesa calls this for a match that keymaster verified (1192 6.4) and only with
 * kern.sweet_mesa_match=1. It arms one BiometryMatched credential for ACM_BIO_ARM_S seconds.
 * No userland path reaches it. (No caller in 1195.)
 */
extern "C" void
sweet_acm_note_verified_match(void)
{
	if (gL == NULL) {
		return;
	}
	IOLockLock(gL);
	gBioArmedAt = acm_now();
	IOLockUnlock(gL);
	SAY("verified biometric match armed for %d s\n", ACM_BIO_ARM_S);
}

static int
sysctl_sweet_acm_state(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	char b[320];
	int live = 0;
	for (int i = 0; i < ACM_CTX_MAX; i++) {
		live += gCtx[i].used;
	}
	snprintf(b, sizeof(b), "published=%u cmds=%u unsup=%u contexts=%u live=%d pass_ok=%u pass_bad=%u bio_ok=%u "
	    "bio_refused=%u refused=%u policy_yes=%u policy_no=%u last=%u(%s)->0x%x",
	    gPublished, nCmds, nUnsup, nCtx, live, nPassOk, nPassBad, nBioOk, nBioRefused, nRefused, nPolicyYes,
	    nPolicyNo, gLastCmd, acm_name(gLastCmd), gLastRet);
	return SYSCTL_OUT(req, b, strlen(b) + 1);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_acm_state, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_acm_state, "A", "redfin 1195: SweetACM counters");
#endif /* SWEET */
