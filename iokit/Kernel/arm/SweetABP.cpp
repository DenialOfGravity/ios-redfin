/*
 * redfin 1061: stand-ins for Apple's baseband IOKit drivers, so the real CommCenter (AppleBasebandManager runs
 * inside it) finds a baseband. Published only with boot-arg sweetabp & 1 (bsd/dev/sweet_abp.c), under the
 * platform expert:
 *
 *   "baseband" nub (SweetABPNub)          IOServiceNameMatching("baseband"): compatible "baseband,n41" and the
 *                                         D22AP 40-byte `config` (timings in ms), as the iPhone X device tree
 *                                         /baseband node (libTelephonyBasebandDynamic reads `config`).
 *     AppleBaseband                       power/GPIO controller. User client type 0: sel 0xf self-identify
 *                                         (struct 0x40), every scalar selector succeeds; reads report "not in
 *                                         reset, PMU ext on, no reset request", manager/baseband state are
 *                                         stored and read back (Get/Set 0x17/0x16, 0x1a/0x1b).
 *   AppleBasebandPCIControl               `bootstage` = kern.sweet_abp_bootstage (default 2; ABM treats < 2 as
 *                                         "boot stage down"). sel 0 open, 1 close, 2 readLogsAsync (held,
 *                                         never completed: no logs), 3 loggerTune.
 *     AppleBasebandPCIInterface x2        ABPInterfaceProtocol "qmux1" (pipe 0), "qmux2" (pipe 1). User client:
 *                                         sel 0 open, 1 close, 2 write (sync struct / async {addr,len}),
 *                                         3 read (sync struct, waits <= 2 s / async {addr,size}, completion
 *                                         args {length}), 6/7 abort/start channel (completed at once),
 *                                         anything else kIOReturnUnsupported.
 *     AppleBasebandPCITrace               accept-all stub (every selector succeeds, zeroed outputs).
 *     AppleBasebandPCIPDPManagerBase      empty (libPCITransport's PDPListener matches it).
 *   AppleIPAppender                       accept-all stub for ApplePDPHelper (it crashes on a NULL mutex in
 *                                         callScalarMethod when the service is missing).
 *
 * 1063: SweetABPStart only sets up the pipes; SweetABPPublish(true/false) publishes / un-publishes the set (at boot,
 * or with sweetabp & 8 when wland says it is ready on /dev/abpbridge; sweetabp & 0x10 leaves out PDPManagerBase).
 *
 * Each user client that opens a pipe is a session (ABM opens qmux2 from several QMux instances at once).
 * Frames CommCenter writes go to /dev/abpbridge tagged with pipe + session (sweet_abp_c2w); frames wland writes
 * there for a session complete that session's oldest pending read or wait in a 128-frame queue (SweetABPDeliver). Every selector is logged the
 * first times it is used, every property read from these services is logged (IOUserClient.cpp), and the
 * first 64 frames per pipe and direction are logged with their first 24 bytes. "SWEET ABP:".
 */
#include <pexpert/arm64/board_config.h>

#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSNumber.h>

extern "C" {
#include <pexpert/pexpert.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <kern/clock.h>
void SweetABPStart(void);
int SweetABPPublish(bool on, const char *why);
int SweetABPPublished(void);
int SweetABPDeliver(unsigned pipe, uint32_t sess, const void *data, uint32_t len);
int SweetABPStats(char *buf, size_t len);
void SweetABPSetBootstage(uint32_t stage);
void SweetABPMessage(uint32_t code);
bool SweetABPIs(const void *entry);
void sweet_abp_c2w(unsigned pipe, uint32_t sess, unsigned type, const void *data, uint32_t len);
extern uint32_t sweet_abp_bootstage;
extern uint32_t sweet_abp_flags;
}

/* 1063: sweetabp bits read here (bsd/dev/sweet_abp.c has the full list) */
#define ABP_BIT_NOPDP   0x10u   /* do not publish AppleBasebandPCIPDPManagerBase (A/B for the boot.mod start timeout) */

#define ABP_NPIPES      2u
#define ABP_MAXREADS    32u
#define ABP_RXQ         128u
#define ABP_MAXFRAME    65536u
#define ABP_LOGFRAMES   64u
#define ABP_LOGLINES    600u

enum { ABPB_DATA = 0, ABPB_OPEN = 1, ABPB_CLOSE = 2, ABPB_START = 3, ABPB_ABORT = 4 };

static const char * const abp_pipe_names[ABP_NPIPES] = { "qmux1", "qmux2" };

static uint32_t abp_lines;
#define ABPLOG(fmt, ...) do { if (abp_lines < ABP_LOGLINES) { abp_lines++; IOLog("SWEET ABP: " fmt "\n", ##__VA_ARGS__); } } while (0)

class AppleBasebandPCIUserClient;

/* sendAsyncResult64 is protected in IOUserClient: reached through a subclass (never instantiated) */
class SweetABPAsync : public IOUserClient
{
public:
	static IOReturn
	done(OSAsyncReference64 ref, IOReturn result, io_user_reference_t *args, UInt32 n)
	{
		IOReturn r = sendAsyncResult64(ref, result, args, n);
		releaseAsyncReference64(ref);
		return r;
	}
	static void
	drop(OSAsyncReference64 ref)
	{
		releaseAsyncReference64(ref);
	}
};

struct abp_read {
	OSAsyncReference64 ref;
	mach_vm_address_t addr;
	uint32_t size;
	uint32_t sess;
	task_t task;
};

/*
 * Several user clients can have the same pipe open at once (ABM opens qmux2 from more than one QMux instance,
 * each its own QMUX stream with its own CTL transaction ids). Each open user client is a session: its writes
 * go to wland tagged with the session id, and wland's frames for a session complete only that session's reads.
 */
struct abp_pipe {
	IOLock *lock;
	abp_read reads[ABP_MAXREADS];
	uint32_t nreads;
	struct {
		uint8_t *buf;
		uint32_t len;
		uint32_t sess;
	} rxq[ABP_RXQ];
	uint32_t rxn;
	uint32_t sync_waiters;
	uint32_t next_sess, live;
	uint32_t live_ids[32];          /* open sessions (0 = free slot) */
	uint64_t wr_frames, wr_bytes, rd_frames, rd_bytes;
	uint32_t drops, truncs, opens, stale;
};

static abp_pipe abp_pipes[ABP_NPIPES];
static IOService *abp_services[10];
static unsigned abp_nservices;
static bool abp_started;                /* pipes set up (locks); the services may still be unpublished (1063 deferred publish) */
static IOLock *abp_svc_lock;            /* 1063: publish / unpublish against each other and the service loops */
static uint32_t abp_publishes, abp_unpublishes;

static void
abp_hex(char *out, size_t outlen, const uint8_t *p, uint32_t len)
{
	size_t o = 0;
	for (uint32_t i = 0; i < len && o + 3 < outlen; i++) {
		o += snprintf(out + o, outlen - o, "%02x", p[i]);
	}
	out[o < outlen ? o : outlen - 1] = 0;
}

static void
abp_log_frame(unsigned pipe, uint32_t sess, const char *dir, const uint8_t *p, uint32_t len, uint64_t nth)
{
	if (nth >= ABP_LOGFRAMES) {
		return;
	}
	char h[64];
	abp_hex(h, sizeof(h), p, len < 24 ? len : 24);
	ABPLOG("%s/%u %s #%llu len %u: %s%s", abp_pipe_names[pipe], sess, dir, nth, len, h, len > 24 ? "..." : "");
}

/* Copy a frame into a pending async read's user buffer and complete it (no locks held). */
static void
abp_complete_read(unsigned pipe, abp_read *rd, const uint8_t *data, uint32_t len)
{
	uint32_t n = len;
	if (n > rd->size) {
		abp_pipes[pipe].truncs++;
		ABPLOG("%s: a %u-byte frame truncated to the read's %u bytes", abp_pipe_names[pipe], len, rd->size);
		n = rd->size;
	}
	IOReturn r = kIOReturnSuccess;
	if (n) {
		IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(rd->addr, n, kIODirectionIn, rd->task);
		r = md ? md->prepare() : kIOReturnNoMemory;
		if (r == kIOReturnSuccess) {
			if (md->writeBytes(0, data, n) != n) {
				r = kIOReturnVMError;
			}
			md->complete();
		}
		OSSafeReleaseNULL(md);
	}
	io_user_reference_t a[1] = { r == kIOReturnSuccess ? n : 0 };
	IOReturn s = SweetABPAsync::done(rd->ref, r, a, 1);
	if (r != kIOReturnSuccess || s != kIOReturnSuccess) {
		ABPLOG("%s: read completion copy 0x%x send 0x%x", abp_pipe_names[pipe], r, s);
	}
}

/* A session ends (close, client died): its pending reads are aborted (or just released when the port is gone)
 * and frames queued for it are dropped. */
static void
abp_end_session(unsigned pipe, uint32_t sess, bool send)
{
	abp_pipe *pp = &abp_pipes[pipe];
	abp_read rs[ABP_MAXREADS];
	uint32_t n = 0, q = 0;
	IOLockLock(pp->lock);
	for (uint32_t i = 0; i < pp->nreads;) {
		if (pp->reads[i].sess == sess) {
			rs[n++] = pp->reads[i];
			pp->reads[i] = pp->reads[--pp->nreads];
		} else {
			i++;
		}
	}
	for (uint32_t i = 0; i < pp->rxn;) {
		if (pp->rxq[i].sess == sess) {
			IOFreeData(pp->rxq[i].buf, pp->rxq[i].len ? pp->rxq[i].len : 1);
			memmove(&pp->rxq[i], &pp->rxq[i + 1], (pp->rxn - i - 1) * sizeof(pp->rxq[0]));
			pp->rxn--;
			q++;
		} else {
			i++;
		}
	}
	IOLockUnlock(pp->lock);
	for (uint32_t i = 0; i < n; i++) {
		if (send) {
			io_user_reference_t a[1] = { 0 };
			SweetABPAsync::done(rs[i].ref, kIOReturnAborted, a, 1);
		} else {
			SweetABPAsync::drop(rs[i].ref);
		}
	}
	if (n || q) {
		ABPLOG("%s/%u: %u pending read(s) %s, %u queued frame(s) dropped", abp_pipe_names[pipe], sess, n,
		    send ? "aborted" : "released (client gone)", q);
	}
}

int
SweetABPDeliver(unsigned pipe, uint32_t sess, const void *data, uint32_t len)
{
	if (pipe >= ABP_NPIPES || !abp_started || len > ABP_MAXFRAME) {
		return EINVAL;
	}
	abp_pipe *pp = &abp_pipes[pipe];
	IOLockLock(pp->lock);
	bool known = false;
	for (unsigned i = 0; sess && i < 32; i++) {
		known |= pp->live_ids[i] == sess;
	}
	if (!known) {           /* the session ended (CommCenter gave up or restarted): dropped */
		uint32_t st = pp->stale++;
		IOLockUnlock(pp->lock);
		if (st < 20) {
			ABPLOG("%s/%u: session not open: wland frame of %u bytes dropped", abp_pipe_names[pipe], sess, len);
		}
		return 0;
	}
	uint64_t nth = pp->rd_frames++;
	pp->rd_bytes += len;
	for (uint32_t i = 0; i < pp->nreads; i++) {
		if (pp->reads[i].sess != sess) {
			continue;
		}
		abp_read rd = pp->reads[i];
		memmove(&pp->reads[i], &pp->reads[i + 1], (pp->nreads - i - 1) * sizeof(pp->reads[0]));
		pp->nreads--;
		IOLockUnlock(pp->lock);
		abp_log_frame(pipe, sess, "<- wland (to a pending read)", (const uint8_t *)data, len, nth);
		abp_complete_read(pipe, &rd, (const uint8_t *)data, len);
		return 0;
	}
	if (pp->rxn >= ABP_RXQ) {
		pp->drops++;
		pp->rd_frames--;
		pp->rd_bytes -= len;
		IOLockUnlock(pp->lock);
		ABPLOG("%s/%u: no read pending and %u frames queued: wland frame of %u bytes refused", abp_pipe_names[pipe], sess, ABP_RXQ, len);
		return ENOSPC;
	}
	uint8_t *copy = (uint8_t *)IOMallocData(len ? len : 1);
	if (copy == NULL) {
		IOLockUnlock(pp->lock);
		return ENOMEM;
	}
	memcpy(copy, data, len);
	pp->rxq[pp->rxn].buf = copy;
	pp->rxq[pp->rxn].len = len;
	pp->rxq[pp->rxn].sess = sess;
	pp->rxn++;
	if (pp->sync_waiters) {
		IOLockWakeup(pp->lock, &pp->rxn, false);
	}
	IOLockUnlock(pp->lock);
	abp_log_frame(pipe, sess, "<- wland (queued)", (const uint8_t *)data, len, nth);
	return 0;
}

/* Lock held. Takes the oldest frame queued for sess; the caller frees it with IOFreeData(buf, len ? len : 1). */
static bool
abp_rx_pop(abp_pipe *pp, uint32_t sess, uint8_t **buf, uint32_t *len)
{
	for (uint32_t i = 0; i < pp->rxn; i++) {
		if (pp->rxq[i].sess != sess) {
			continue;
		}
		*buf = pp->rxq[i].buf;
		*len = pp->rxq[i].len;
		memmove(&pp->rxq[i], &pp->rxq[i + 1], (pp->rxn - i - 1) * sizeof(pp->rxq[0]));
		pp->rxn--;
		return true;
	}
	return false;
}

/* Output structure (inband or descriptor) helpers. */
static uint32_t
abp_struct_in(IOExternalMethodArguments *args, uint8_t *dst, uint32_t max)
{
	if (args->structureInputDescriptor != NULL) {
		uint32_t n = (uint32_t)args->structureInputDescriptor->getLength();
		if (n > max) {
			n = max;
		}
		return (uint32_t)args->structureInputDescriptor->readBytes(0, dst, n);
	}
	uint32_t n = args->structureInputSize;
	if (n > max) {
		n = max;
	}
	if (n && args->structureInput) {
		memcpy(dst, args->structureInput, n);
	}
	return n;
}

static IOReturn
abp_struct_out(IOExternalMethodArguments *args, const void *buf, uint32_t len)
{
	if (args->structureOutputDescriptor != NULL) {
		if (len > args->structureOutputDescriptor->getLength()) {
			len = (uint32_t)args->structureOutputDescriptor->getLength();
		}
		args->structureOutputDescriptor->writeBytes(0, buf, len);
		args->structureOutputDescriptorSize = len;
		return kIOReturnSuccess;
	}
	if (args->structureOutput == NULL) {
		return kIOReturnBadArgument;
	}
	if (len > args->structureOutputSize) {
		len = args->structureOutputSize;
	}
	memcpy(args->structureOutput, buf, len);
	args->structureOutputSize = len;
	return kIOReturnSuccess;
}

static void
abp_log_call(const char *who, uint32_t selector, IOExternalMethodArguments *args, IOReturn r)
{
	char p[32];
	proc_selfname(p, (int)sizeof(p));
	ABPLOG("%s %s sel 0x%x%s in[%u]={0x%llx,0x%llx,0x%llx} struct %u out[%u] -> 0x%x", p, who, selector,
	    args->asyncWakePort != MACH_PORT_NULL ? " (async)" : "", args->scalarInputCount,
	    args->scalarInputCount > 0 ? args->scalarInput[0] : 0ULL, args->scalarInputCount > 1 ? args->scalarInput[1] : 0ULL,
	    args->scalarInputCount > 2 ? args->scalarInput[2] : 0ULL,
	    args->structureInputDescriptor ? (uint32_t)args->structureInputDescriptor->getLength() : args->structureInputSize,
	    args->scalarOutputCount, r);
}

/* ---------------- services ---------------- */

class SweetABPNub : public IOService
{
	OSDeclareDefaultStructors(SweetABPNub);
};
OSDefineMetaClassAndStructors(SweetABPNub, IOService);

class AppleBaseband : public IOService
{
	OSDeclareDefaultStructors(AppleBaseband);
};
OSDefineMetaClassAndStructors(AppleBaseband, IOService);

class AppleBasebandPCIControl : public IOService
{
	OSDeclareDefaultStructors(AppleBasebandPCIControl);
};
OSDefineMetaClassAndStructors(AppleBasebandPCIControl, IOService);

class AppleBasebandPCIInterface : public IOService
{
	OSDeclareDefaultStructors(AppleBasebandPCIInterface);
public:
	unsigned fPipe;
};
OSDefineMetaClassAndStructors(AppleBasebandPCIInterface, IOService);

/* ---------------- AppleBaseband user client ---------------- */

static uint64_t abb_manager_state;
static uint64_t abb_baseband_state;
static uint32_t abb_calls[0x40];

class AppleBasebandUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AppleBasebandUserClient);
public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
	    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AppleBasebandUserClient, IOUserClient);

IOReturn
AppleBasebandUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn
AppleBasebandUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	IOReturn r = kIOReturnSuccess;
	uint64_t in0 = args->scalarInputCount > 0 ? args->scalarInput[0] : 0;
	uint64_t out = 0;
	bool has_out = true;
	switch (selector) {
	case 0x0f: {    /* self-identify: struct in 0x40 */
		uint8_t id[0x40];
		bzero(id, sizeof(id));
		uint32_t n = abp_struct_in(args, id, sizeof(id));
		char h[100];
		abp_hex(h, sizeof(h), id, n < 32 ? n : 32);
		ABPLOG("AppleBaseband identify (%u bytes): %s", n, h);
		has_out = false;
		break;
	}
	case 0x03:      /* get reset: not in reset */
	case 0x12:      /* get reset request: none */
	case 0x11:      /* get EFS erase done */
	case 0x1c:      /* get property */
	case 0x2a:      /* coredump-ready status */
		out = 0;
		break;
	case 0x0b:      /* PMU ext-on */
	case 0x10:      /* diag ready */
		out = 1;
		break;
	case 0x16:
		abb_manager_state = in0;
		has_out = false;
		break;
	case 0x17:
		out = abb_manager_state;
		break;
	case 0x1b:
		abb_baseband_state = in0;
		has_out = false;
		break;
	case 0x1a:
		out = abb_baseband_state;
		break;
	default:        /* reset, power on, powercycle, VBUS, mux, wake votes, ...: done */
		out = 0;
		break;
	}
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = (i == 0 && has_out) ? out : 0;
	}
	if (args->structureOutputDescriptor == NULL && args->structureOutput && args->structureOutputSize) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	uint32_t slot = selector < 0x40 ? selector : 0x3f;
	if (abb_calls[slot]++ < 6 || selector == 0x16 || selector == 0x1b) {
		abp_log_call("AppleBaseband", selector, args, r);
		if (args->scalarOutputCount) {
			ABPLOG("AppleBaseband sel 0x%x answers 0x%llx", selector, args->scalarOutput[0]);
		}
	}
	return r;
}

/* ---------------- AppleBasebandPCIControl user client ---------------- */

static uint32_t abc_calls[0x10];

class AppleBasebandPCIControlUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AppleBasebandPCIControlUserClient);
	OSAsyncReference64 fLogRefs[4];
	uint32_t fNLogRefs;
	IOLock *fLock;
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
	    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AppleBasebandPCIControlUserClient, IOUserClient);

bool
AppleBasebandPCIControlUserClient::start(IOService *provider)
{
	if (!IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	return fLock != NULL;
}

void
AppleBasebandPCIControlUserClient::free(void)
{
	if (fLock) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
AppleBasebandPCIControlUserClient::clientClose(void)
{
	if (fLock) {
		IOLockLock(fLock);
		uint32_t n = fNLogRefs;
		fNLogRefs = 0;
		IOLockUnlock(fLock);
		for (uint32_t i = 0; i < n; i++) {
			IOUserClient::releaseAsyncReference64(fLogRefs[i]);
		}
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
AppleBasebandPCIControlUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	IOReturn r = kIOReturnSuccess;
	switch (selector) {
	case 0:         /* open */
	case 1:         /* close */
	case 3:         /* loggerTune */
		break;
	case 2:         /* readLogsAsync: there are no modem logs; hold the request */
		if (args->asyncWakePort == MACH_PORT_NULL) {
			r = kIOReturnUnsupported;
			break;
		}
		IOLockLock(fLock);
		if (fNLogRefs < 4) {
			bcopy(args->asyncReference, fLogRefs[fNLogRefs], sizeof(OSAsyncReference64));
			fNLogRefs++;
		} else {
			r = kIOReturnNoResources;
		}
		IOLockUnlock(fLock);
		break;
	default:
		r = kIOReturnUnsupported;
		break;
	}
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = 0;
	}
	if (abc_calls[selector < 0x10 ? selector : 0xf]++ < 6) {
		abp_log_call("AppleBasebandPCIControl", selector, args, r);
	}
	return r;
}

/* ---------------- AppleBasebandPCIInterface user client ---------------- */

class AppleBasebandPCIUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AppleBasebandPCIUserClient);
	task_t fTask;
	unsigned fPipe;
	uint32_t fSess;                 /* 0 = not open */
	uint32_t fCalls[0x10];
	void detachFromPipe(bool send);
public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn clientDied(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
	    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AppleBasebandPCIUserClient, IOUserClient);

bool
AppleBasebandPCIUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
AppleBasebandPCIUserClient::start(IOService *provider)
{
	AppleBasebandPCIInterface *ifc = OSDynamicCast(AppleBasebandPCIInterface, provider);
	if (ifc == NULL || ifc->fPipe >= ABP_NPIPES || !IOUserClient::start(provider)) {
		return false;
	}
	fPipe = ifc->fPipe;
	return true;
}

void
AppleBasebandPCIUserClient::detachFromPipe(bool send)
{
	abp_pipe *pp = &abp_pipes[fPipe];
	uint32_t sess;
	IOLockLock(pp->lock);
	sess = fSess;
	fSess = 0;
	if (sess) {
		pp->live--;
		for (unsigned i = 0; i < 32; i++) {
			if (pp->live_ids[i] == sess) {
				pp->live_ids[i] = 0;
			}
		}
	}
	IOLockUnlock(pp->lock);
	if (sess) {
		abp_end_session(fPipe, sess, send);
		sweet_abp_c2w(fPipe, sess, ABPB_CLOSE, NULL, 0);
	}
}

IOReturn
AppleBasebandPCIUserClient::clientClose(void)
{
	detachFromPipe(true);
	terminate();
	return kIOReturnSuccess;
}

IOReturn
AppleBasebandPCIUserClient::clientDied(void)
{
	ABPLOG("%s: client died", abp_pipe_names[fPipe]);
	detachFromPipe(false);
	return IOUserClient::clientDied();
}

IOReturn
AppleBasebandPCIUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	abp_pipe *pp = &abp_pipes[fPipe];
	bool async = args->asyncWakePort != MACH_PORT_NULL;
	bool completed = false;         /* async request finished here (completion sent, reference released) */
	IOReturn r = kIOReturnSuccess;
	uint64_t in0 = args->scalarInputCount > 0 ? args->scalarInput[0] : 0;
	uint64_t in1 = args->scalarInputCount > 1 ? args->scalarInput[1] : 0;

	switch (selector) {
	case 0: {       /* open: a new session */
		bool fresh = false;
		IOLockLock(pp->lock);
		if (fSess == 0) {
			fSess = ++pp->next_sess;
			pp->live++;
			fresh = true;
			for (unsigned i = 0; i < 32; i++) {
				if (pp->live_ids[i] == 0) {
					pp->live_ids[i] = fSess;
					break;
				}
			}
		}
		pp->opens++;
		uint32_t live = pp->live;
		IOLockUnlock(pp->lock);
		uint64_t v = in0;
		sweet_abp_c2w(fPipe, fSess, ABPB_OPEN, &v, sizeof(v));
		ABPLOG("%s/%u: open%s (%u session(s) open)", abp_pipe_names[fPipe], fSess, fresh ? "" : " again", live);
		break;
	}
	case 1:         /* close */
		detachFromPipe(true);
		break;
	case 2: {       /* write */
		uint8_t *buf;
		uint32_t len;
		if (async) {
			len = (uint32_t)in1;
			if (len > ABP_MAXFRAME) {
				r = kIOReturnBadArgument;
				break;
			}
			buf = (uint8_t *)IOMallocData(len ? len : 1);
			if (buf == NULL) {
				r = kIOReturnNoMemory;
				break;
			}
			if (len) {
				IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(in0, len, kIODirectionOut, fTask);
				r = md ? md->prepare() : kIOReturnNoMemory;
				if (r == kIOReturnSuccess) {
					if (md->readBytes(0, buf, len) != len) {
						r = kIOReturnVMError;
					}
					md->complete();
				}
				OSSafeReleaseNULL(md);
			}
		} else {
			len = args->structureInputDescriptor ? (uint32_t)args->structureInputDescriptor->getLength() : args->structureInputSize;
			if (len > ABP_MAXFRAME) {
				r = kIOReturnBadArgument;
				break;
			}
			buf = (uint8_t *)IOMallocData(len ? len : 1);
			if (buf == NULL) {
				r = kIOReturnNoMemory;
				break;
			}
			len = abp_struct_in(args, buf, len);
		}
		if (r == kIOReturnSuccess) {
			IOLockLock(pp->lock);
			uint64_t nth = pp->wr_frames++;
			pp->wr_bytes += len;
			IOLockUnlock(pp->lock);
			abp_log_frame(fPipe, fSess, "-> wland", buf, len, nth);
			sweet_abp_c2w(fPipe, fSess, ABPB_DATA, buf, len);
		}
		IOFreeData(buf, len ? len : 1);
		if (r == kIOReturnSuccess && async) {
			io_user_reference_t a[1] = { len };
			sendAsyncResult64(args->asyncReference, kIOReturnSuccess, a, 1);
			releaseAsyncReference64(args->asyncReference);
			completed = true;
		}
		break;
	}
	case 3: {       /* read */
		if (async) {
			abp_read rd;
			bcopy(args->asyncReference, rd.ref, sizeof(rd.ref));
			rd.addr = in0;
			rd.size = (uint32_t)in1;
			rd.task = fTask;
			rd.sess = fSess;
			uint8_t *fb = NULL;
			uint32_t fl = 0;
			IOLockLock(pp->lock);
			if (abp_rx_pop(pp, fSess, &fb, &fl)) {
				IOLockUnlock(pp->lock);
				abp_complete_read(fPipe, &rd, fb, fl);
				IOFreeData(fb, fl ? fl : 1);
				completed = true;
			} else if (pp->nreads < ABP_MAXREADS) {
				pp->reads[pp->nreads++] = rd;
				IOLockUnlock(pp->lock);
			} else {
				IOLockUnlock(pp->lock);
				r = kIOReturnNoResources;
			}
		} else {
			uint8_t *fb = NULL;
			uint32_t fl = 0;
			IOLockLock(pp->lock);
			if (!abp_rx_pop(pp, fSess, &fb, &fl)) {
				AbsoluteTime dl;
				clock_interval_to_deadline(2000, kMillisecondScale, &dl);
				pp->sync_waiters++;
				IOLockSleepDeadline(pp->lock, &pp->rxn, dl, THREAD_ABORTSAFE);
				pp->sync_waiters--;
				abp_rx_pop(pp, fSess, &fb, &fl);
			}
			IOLockUnlock(pp->lock);
			if (fb == NULL) {
				r = kIOReturnTimeout;
				args->structureOutputSize = 0;
			} else {
				r = abp_struct_out(args, fb, fl);
				IOFreeData(fb, fl ? fl : 1);
			}
		}
		break;
	}
	case 6:         /* abortChannelAsync */
	case 7: {       /* startChannelAsync */
		uint64_t v = in0;
		sweet_abp_c2w(fPipe, fSess, selector == 7 ? ABPB_START : ABPB_ABORT, &v, sizeof(v));
		if (selector == 6 && fSess) {
			/* abort: this session's pending reads end (aborted); queued frames stay */
			abp_read rs[ABP_MAXREADS];
			uint32_t n = 0;
			IOLockLock(pp->lock);
			for (uint32_t i = 0; i < pp->nreads;) {
				if (pp->reads[i].sess == fSess) {
					rs[n++] = pp->reads[i];
					memmove(&pp->reads[i], &pp->reads[i + 1], (pp->nreads - i - 1) * sizeof(pp->reads[0]));
					pp->nreads--;
				} else {
					i++;
				}
			}
			IOLockUnlock(pp->lock);
			for (uint32_t i = 0; i < n; i++) {
				io_user_reference_t a0[1] = { 0 };
				SweetABPAsync::done(rs[i].ref, kIOReturnAborted, a0, 1);
			}
		}
		if (async) {
			io_user_reference_t a[1] = { 0 };
			sendAsyncResult64(args->asyncReference, kIOReturnSuccess, a, 1);
			releaseAsyncReference64(args->asyncReference);
			completed = true;
		}
		break;
	}
	default:
		r = kIOReturnUnsupported;
		break;
	}
	(void)completed;
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = 0;
	}
	if (selector != 2 && selector != 3 && fCalls[selector < 0x10 ? selector : 0xf]++ < 8) {
		abp_log_call(abp_pipe_names[fPipe], selector, args, r);
	} else if ((selector == 2 || selector == 3) && fCalls[selector]++ < 3) {
		abp_log_call(abp_pipe_names[fPipe], selector, args, r);
	}
	return r;
}

/* ---------------- accept-all stand-ins: AppleIPAppender (ApplePDPHelper), AppleBasebandPCITrace ---------------- */

class AppleIPAppender : public IOService
{
	OSDeclareDefaultStructors(AppleIPAppender);
};
OSDefineMetaClassAndStructors(AppleIPAppender, IOService);

class AppleBasebandPCITrace : public IOService
{
	OSDeclareDefaultStructors(AppleBasebandPCITrace);
};
OSDefineMetaClassAndStructors(AppleBasebandPCITrace, IOService);

/* libPCITransport's PDPListener matches this (data path, later P4); only its interest notifications are used. */
class AppleBasebandPCIPDPManagerBase : public IOService
{
	OSDeclareDefaultStructors(AppleBasebandPCIPDPManagerBase);
};
OSDefineMetaClassAndStructors(AppleBasebandPCIPDPManagerBase, IOService);

/* Every selector succeeds with zeroed outputs; async requests are accepted and never completed. */
static uint32_t abs_calls[2][0x20];

class SweetABPStubUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(SweetABPStubUserClient);
protected:
	unsigned fKind;         /* 0 AppleIPAppender, 1 AppleBasebandPCITrace */
public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
	    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(SweetABPStubUserClient, IOUserClient);

IOReturn
SweetABPStubUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn
SweetABPStubUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = 0;
	}
	if (args->structureOutputDescriptor != NULL) {
		uint32_t n = (uint32_t)args->structureOutputDescriptor->getLength();
		uint8_t z[256];
		bzero(z, sizeof(z));
		for (uint32_t o = 0; o < n; o += sizeof(z)) {
			args->structureOutputDescriptor->writeBytes(o, z, n - o < sizeof(z) ? n - o : sizeof(z));
		}
		args->structureOutputDescriptorSize = n;
	} else if (args->structureOutput && args->structureOutputSize) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	if (abs_calls[fKind & 1][selector < 0x20 ? selector : 0x1f]++ < 4) {
		abp_log_call(fKind ? "AppleBasebandPCITrace" : "AppleIPAppender", selector, args, kIOReturnSuccess);
	}
	return kIOReturnSuccess;
}

class AppleIPAppenderUserClient : public SweetABPStubUserClient
{
	OSDeclareDefaultStructors(AppleIPAppenderUserClient);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE { fKind = 0; return SweetABPStubUserClient::start(provider); }
};
OSDefineMetaClassAndStructors(AppleIPAppenderUserClient, SweetABPStubUserClient);

class AppleBasebandPCITraceUserClient : public SweetABPStubUserClient
{
	OSDeclareDefaultStructors(AppleBasebandPCITraceUserClient);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE { fKind = 1; return SweetABPStubUserClient::start(provider); }
};
OSDefineMetaClassAndStructors(AppleBasebandPCITraceUserClient, SweetABPStubUserClient);

/* ---------------- publishing ---------------- */

static void
put_data(IORegistryEntry *e, const char *key, const void *bytes, unsigned len)
{
	OSData *v = OSData::withBytes(bytes, len);
	if (v) {
		e->setProperty(key, v);
		v->release();
	}
}

static bool
abp_publish(IOService *svc, IOService *provider, const char *name, const char *ucclass)
{
	if (svc == NULL) {        /* already init()ed by the caller */
		return false;
	}
	svc->setProperty("SweetABP", true);
	if (ucclass) {
		svc->setProperty(kIOUserClientClassKey, ucclass);
	}
	if (!svc->attach(provider)) {
		svc->release();
		return false;
	}
	if (name) {
		svc->setName(name);
	}
	if (!svc->start(provider)) {
		svc->detach(provider);
		svc->release();
		return false;
	}
	svc->registerService();
	if (abp_nservices < sizeof(abp_services) / sizeof(abp_services[0])) {
		abp_services[abp_nservices++] = svc;
	}
	IOLog("SWEET ABP: published %s (%s) under %s\n", svc->getName(), svc->getMetaClass()->getClassName(), provider->getName());
	return true;
}

/* 1063: sets up the pipes only; the services are published by SweetABPPublish (at once, or when wland is ready). */
void
SweetABPStart(void)
{
	if (abp_started) {
		return;
	}
	for (unsigned i = 0; i < ABP_NPIPES; i++) {
		abp_pipes[i].lock = IOLockAlloc();
	}
	abp_svc_lock = IOLockAlloc();
	abp_started = true;
}

/* Lock (abp_svc_lock) held. Publishes the whole set under the platform expert. */
static void
abp_publish_all(void)
{
	IOService *platform = IOService::getPlatform();
	if (platform == NULL) {
		IOLog("SWEET ABP: no platform expert yet: nothing published\n");
		return;
	}
	/* /baseband of the D22AP (iPhone X) device tree */
	SweetABPNub *nub = OSTypeAlloc(SweetABPNub);
	IOService *bbparent = platform;
	if (nub) {
		static const char compat[] = "baseband,n41";
		static const uint32_t config[10] = { 0xfa, 0x1f4, 0x4844, 0x3e8, 0x64, 0x64, 0x83, 0x25, 0x14, 0x64 };
		if (nub->init()) {
			put_data(nub, "compatible", compat, sizeof(compat));
			put_data(nub, "device_type", "baseband", 9);
			put_data(nub, "config", config, sizeof(config));
			nub->setProperty("SweetABP", true);
			if (nub->attach(platform)) {
				nub->setName("baseband");
				if (nub->start(platform)) {
					nub->registerService();
					bbparent = nub;
					if (abp_nservices < sizeof(abp_services) / sizeof(abp_services[0])) {
						abp_services[abp_nservices++] = nub;
					}
					IOLog("SWEET ABP: published baseband nub (config 40 bytes, baseband,n41) under %s\n", platform->getName());
				} else {
					nub->detach(platform);
					nub->release();
				}
			} else {
				nub->release();
			}
		} else {
			nub->release();
		}
	}
	AppleBaseband *abb = OSTypeAlloc(AppleBaseband);
	if (abb && !abb->init()) {
		OSSafeReleaseNULL(abb);
	}
	abp_publish(abb, bbparent, NULL, "AppleBasebandUserClient");
	AppleBasebandPCIControl *ctl = OSTypeAlloc(AppleBasebandPCIControl);
	if (ctl && !ctl->init()) {
		OSSafeReleaseNULL(ctl);
	}
	if (ctl) {
		ctl->setProperty("bootstage", (unsigned long long)sweet_abp_bootstage, 32);
	}
	if (ctl && abp_publish(ctl, platform, NULL, "AppleBasebandPCIControlUserClient")) {
		for (unsigned i = 0; i < ABP_NPIPES; i++) {
			AppleBasebandPCIInterface *ifc = OSTypeAlloc(AppleBasebandPCIInterface);
			if (ifc && !ifc->init()) {
				OSSafeReleaseNULL(ifc);
			}
			if (ifc == NULL) {
				continue;
			}
			ifc->fPipe = i;
			ifc->setProperty("ABPInterfaceProtocol", abp_pipe_names[i]);
			ifc->setProperty("ABPInterfaceIndex", (unsigned long long)(i + 4), 32);
			abp_publish(ifc, ctl, NULL, "AppleBasebandPCIUserClient");
		}
		AppleBasebandPCITrace *tr = OSTypeAlloc(AppleBasebandPCITrace);
		if (tr && !tr->init()) {
			OSSafeReleaseNULL(tr);
		}
		abp_publish(tr, ctl, NULL, "AppleBasebandPCITraceUserClient");
		if (sweet_abp_flags & ABP_BIT_NOPDP) {
			IOLog("SWEET ABP: AppleBasebandPCIPDPManagerBase not published (sweetabp & 0x10)\n");
		} else {
			AppleBasebandPCIPDPManagerBase *pdp = OSTypeAlloc(AppleBasebandPCIPDPManagerBase);
			if (pdp && !pdp->init()) {
				OSSafeReleaseNULL(pdp);
			}
			abp_publish(pdp, ctl, NULL, "AppleIPAppenderUserClient");
		}
	}
	AppleIPAppender *ipa = OSTypeAlloc(AppleIPAppender);
	if (ipa && !ipa->init()) {
		OSSafeReleaseNULL(ipa);
	}
	abp_publish(ipa, platform, NULL, "AppleIPAppenderUserClient");
}

/* Aborts every pending read on both pipes (all sessions); the sessions stay (their user clients close them). */
static uint32_t
abp_abort_all_reads(void)
{
	uint32_t total = 0;
	for (unsigned p = 0; p < ABP_NPIPES; p++) {
		abp_pipe *pp = &abp_pipes[p];
		abp_read rs[ABP_MAXREADS];
		uint32_t n;
		IOLockLock(pp->lock);
		n = pp->nreads;
		memcpy(rs, pp->reads, n * sizeof(rs[0]));
		pp->nreads = 0;
		IOLockUnlock(pp->lock);
		for (uint32_t i = 0; i < n; i++) {
			io_user_reference_t a[1] = { 0 };
			SweetABPAsync::done(rs[i].ref, kIOReturnAborted, a, 1);
		}
		total += n;
	}
	return total;
}

/*
 * 1063: publish (on) or un-publish (off) the stand-ins. Un-publish terminates the top-level services (the nub, the
 * PCI control with its pipes/trace/PDP children, AppleIPAppender); IOKit terminates their user clients, CommCenter
 * sees its baseband go away. A later publish creates a fresh set. Returns 0, or EALREADY / ENXIO.
 */
int
SweetABPPublish(bool on, const char *why)
{
	if (!abp_started) {
		return ENXIO;
	}
	IOLockLock(abp_svc_lock);
	if (on == (abp_nservices != 0)) {
		IOLockUnlock(abp_svc_lock);
		return EALREADY;
	}
	if (on) {
		abp_publishes++;
		IOLog("SWEET ABP: publishing the baseband services (%s, publish #%u)\n", why, abp_publishes);
		abp_publish_all();
		IOLockUnlock(abp_svc_lock);
		return 0;
	}
	IOService *svcs[10];
	unsigned n = abp_nservices;
	memcpy(svcs, abp_services, n * sizeof(svcs[0]));
	abp_nservices = 0;
	abp_unpublishes++;
	IOLockUnlock(abp_svc_lock);
	uint32_t aborted = abp_abort_all_reads();
	for (unsigned i = 0; i < n; i++) {
		/* top level only: a provider's termination takes its children with it */
		IOService *prov = svcs[i]->getProvider();
		bool top = true;
		for (unsigned j = 0; j < n; j++) {
			top &= prov != svcs[j];
		}
		if (top) {
			svcs[i]->terminate();
		}
	}
	for (unsigned i = 0; i < n; i++) {
		svcs[i]->release();
	}
	IOLog("SWEET ABP: un-published %u baseband services (%s), %u pending read(s) aborted\n", n, why, aborted);
	return 0;
}

int
SweetABPPublished(void)
{
	return abp_nservices != 0;
}

void
SweetABPSetBootstage(uint32_t stage)
{
	if (!abp_started) {
		return;
	}
	IOLockLock(abp_svc_lock);
	for (unsigned i = 0; i < abp_nservices; i++) {
		if (OSDynamicCast(AppleBasebandPCIControl, abp_services[i])) {
			abp_services[i]->setProperty("bootstage", (unsigned long long)stage, 32);
			IOLog("SWEET ABP: bootstage = %u\n", stage);
		}
	}
	IOLockUnlock(abp_svc_lock);
}

void
SweetABPMessage(uint32_t code)
{
	IOService *ctl = NULL;
	if (!abp_started) {
		return;
	}
	IOLockLock(abp_svc_lock);
	for (unsigned i = 0; i < abp_nservices; i++) {
		if (OSDynamicCast(AppleBasebandPCIControl, abp_services[i])) {
			ctl = abp_services[i];
			ctl->retain();
			break;
		}
	}
	IOLockUnlock(abp_svc_lock);
	if (ctl) {
		IOReturn r = ctl->messageClients(code, NULL, 0);
		IOLog("SWEET ABP: general-interest message 0x%x to AppleBasebandPCIControl clients -> 0x%x\n", code, r);
		ctl->release();
	}
}

bool
SweetABPIs(const void *entry)
{
	for (unsigned i = 0; entry != NULL && i < abp_nservices; i++) {
		if (abp_services[i] == entry) {
			return true;
		}
	}
	return false;
}

int
SweetABPStats(char *buf, size_t len)
{
	size_t o = 0;
	o += snprintf(buf + o, len - o, "%s (%u services, published %u x, un-published %u x) | bootstage %u mgr %llu bb %llu",
	    abp_nservices ? "published" : "NOT published", abp_nservices, abp_publishes, abp_unpublishes, sweet_abp_bootstage,
	    abb_manager_state, abb_baseband_state);
	for (unsigned i = 0; i < ABP_NPIPES && o < len; i++) {
		abp_pipe *pp = &abp_pipes[i];
		if (pp->lock == NULL) {
			continue;
		}
		IOLockLock(pp->lock);
		o += snprintf(buf + o, len - o, " | %s opens %u (%u live) %s, CC wrote %llu fr/%llu B, wland gave %llu fr/%llu B, reads pending %u, queued %u, drops %u, truncs %u, stale %u",
		    abp_pipe_names[i], pp->opens, pp->live, pp->live ? "open" : "closed", pp->wr_frames, pp->wr_bytes, pp->rd_frames, pp->rd_bytes,
		    pp->nreads, pp->rxn, pp->drops, pp->truncs, pp->stale);
		IOLockUnlock(pp->lock);
	}
	return (int)(o < len ? o : len - 1);
}
#endif /* SWEET */
