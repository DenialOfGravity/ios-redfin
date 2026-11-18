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
 *
 * 1071 (P4 phase 2b, boot-arg sweetbb & 0x40, default off): ABM's real baseband boot (BBUpdater performBootup, Mav17 =
 * EUR16 update source + EUR13 loader + EUR10 Sahara programmer) runs against an emulated modem boot ROM:
 *   AppleBasebandPCIInterface "pbl"    (ABPInterfaceIndex 0 = PCITransportInterface 0; BBU stage 2 = first stage, BHI)
 *       sel 5 readRegister: scalar {reg}, struct out = the register: reg 0 SNUM (4), reg 3 MSM HW ID (0x18, chip id u32 at +4),
 *             reg 4 OEM PK hash (0x60; first 0x20 = the loader's default hash => "Detect as unfused BB"), others zero.
 *       sel 4 sendImage: sync = struct in (the image), 2 scalar outs {bytes taken, status 0}; async = scalars {addr, len},
 *             completion args {len, 0}. The image (sbl1.mbn) is not copied: its first 16 bytes are logged, then dropped.
 *   AppleBasebandPCIInterface "sahara" (ABPInterfaceIndex 1 = PCITransportInterface 1; BBU stage 4 = second stage)
 *       read/write as the qmux pipes (sel 3/2), but answered here: the target side of Sahara. On open: HELLO (v2, mode 0
 *       image transfer). HELLO_RESP -> READ_DATA(kern.sweet_abm_img, 0, kern.sweet_abm_imglen) -> the image bytes are
 *       counted and dropped -> END_IMAGE_TX(success) -> DONE -> DONE_RESP(1 = image transfer complete; BBU logs "All
 *       images transferred"). RESET -> RESET_RESP. Nothing reaches the SM7250 (MPSS is PAS-loaded from its own partition).
 *   AppleBaseband: power on (sel 1) waits (<= kern.sweet_abm_upwait_ms) until the real modem is up (kern.sweet_mdm_up == 2,
 *       needs sweetbb & 1); "get reset" reads 0 and 0xe3ff8000 (reset detected) is held back while the push runs (BBU
 *       would powercycle); after DONE_RESP the 0xe3ff8001 "alive" message follows (after kern.sweet_abp_readyms, >= 200 ms).
 * Every stage logs "SWEET ABM:". kern.sweet_abm_stats has the counters.
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
#include <kern/thread_call.h>
void SweetABPStart(void);
int SweetABPPublish(bool on, const char *why);
int SweetABPPublished(void);
int SweetBBVPublish(bool on, const char *why);       /* 1092 */
int SweetBBVPublished(void);
int SweetABPDeliver(unsigned pipe, uint32_t sess, const void *data, uint32_t len);
int SweetABPStats(char *buf, size_t len);
void SweetABPSetBootstage(uint32_t stage);
void SweetABPMessage(uint32_t code);
void SweetABPBasebandMessage(uint32_t code);
bool SweetABPIs(const void *entry);
void sweet_abp_c2w(unsigned pipe, uint32_t sess, unsigned type, const void *data, uint32_t len);
int SweetABMStats(char *buf, size_t len);
extern uint32_t sweet_bb;               /* 1071: sweet_bb.c */
int sbb_link_state(void);
extern uint32_t sweet_abm_upwait_ms, sweet_abm_img, sweet_abm_imglen, sweet_abm_chipid, sweet_abm_snum;
extern uint32_t sweet_abp_bootstage;
extern uint32_t sweet_abp_flags;
extern uint32_t sweet_abp_readyms;
/* 1075: bsd/dev/sweet_pdp.c (pdp_ip0..7, sweetbb & 0x10) */
bool sweet_pdp_enabled(void);
void sweet_pdp_publish(void);
void sweet_pdp_block(uint32_t index, int on);
void sweet_pdp_unblock_all(void);
}

/* 1063: sweetabp bits read here (bsd/dev/sweet_abp.c has the full list) */
#define ABP_BIT_NOPDP   0x10u   /* do not publish AppleBasebandPCIPDPManagerBase (A/B for the boot.mod start timeout) */

#define ABP_NQMUX       2u      /* qmux1, qmux2: frames go to wland / the kernel router */
#define ABP_PIPE_PBL    2u      /* 1071: emulated BHI (published only with sweetbb & 0x40) */
#define ABP_PIPE_SAHARA 3u      /* 1071: emulated Sahara target */
#define ABP_NPIPES      4u
#define SBB_REALBOOT    0x40u   /* sweet_bb.h */
#define ABP_MAXREADS    32u
#define ABP_RXQ         128u
#define ABP_MAXFRAME    65536u
#define ABP_LOGFRAMES   64u
#define ABP_LOGLINES    600u

enum { ABPB_DATA = 0, ABPB_OPEN = 1, ABPB_CLOSE = 2, ABPB_START = 3, ABPB_ABORT = 4 };

static const char * const abp_pipe_names[ABP_NPIPES] = { "qmux1", "qmux2", "pbl", "sahara" };

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
static IOService *abp_services[12];
static unsigned abp_nservices;
static bool abp_started;                /* pipes set up (locks); the services may still be unpublished (1063 deferred publish) */
static IOLock *abp_svc_lock;            /* 1063: publish / unpublish against each other and the service loops */
static IOLock *bbv_lock;                /* 1092: "Baseband Voice" publish / terminate */
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

/*
 * 1194: struct descriptors (> 4096 bytes) arrive unprepared: wire them around each copy (prepare/complete).
 * An unprepared writeBytes hung the phone in SweetMesa's bridge (fingerprint-freeze-bisect-2026-11-18.md).
 */
static IOByteCount
abp_md_copy(IOMemoryDescriptor *md, IOByteCount off, void *buf, IOByteCount len, bool out)
{
	if (len == 0 || md->prepare() != kIOReturnSuccess) {
		return 0;
	}
	IOByteCount n = out ? md->writeBytes(off, buf, len) : md->readBytes(off, buf, len);
	md->complete();
	return n;
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
		return (uint32_t)abp_md_copy(args->structureInputDescriptor, 0, dst, n, false);
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
		abp_md_copy(args->structureOutputDescriptor, 0, (void *)(uintptr_t)buf, len, true);
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

/* ---------------- 1071: real ABM boot emulation (BHI + Sahara target), forward ---------------- */

static IOLock *abm_lock;
static bool abm_on(void) { return (sweet_bb & SBB_REALBOOT) != 0; }
static void abm_on_open(unsigned pipe, uint32_t sess);
static void abm_on_close(unsigned pipe, uint32_t sess);
static void abm_on_write(unsigned pipe, uint32_t sess, const uint8_t *buf, uint32_t len);
static bool abm_push_active(void);
static void abm_reset(const char *why);
static bool abm_wait_up(uint32_t ms, const char *why);

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

/*
 * 1064: AppleBaseband reset state + reset-detect messages (Claude 2026-11-09).
 * ABM's ResetDetection polls "get reset" (sel 0x03) and listens for general-interest messages on AppleBaseband
 * (libTelephonyBaseband _TelephonyBasebandResetCallback -> ABM ResetDetection::resetDetectCallback):
 *   0xe3ff8000 = baseband reset detected (blocks added with reset=1, e.g. 'Baseband Crash Recovery'),
 *   0xe3ff8001 = baseband alive (blocks added with reset=0, e.g. 'Wait for Ready on Fake Boot').
 * With the fake boot, "Wait for Ready on Fake Boot" was satisfied by the first poll, so ABM's boot finished ~30 ms after
 * the manager started, often BEFORE ipc.mod / radio.mod registered for EventBasebandBootStateChange: they never saw
 * "booted", the QMI transport was never engaged (no "Engaging"), boot.mod hit its 30 s start timeout and CommCenter
 * never opened the pipes (P2 runs: about half of all CommCenter starts).
 * kern.sweet_abp_readyms = N > 0: when ABM starts (AppleBaseband sel 0x16 manager state 1) the baseband reads as "in
 * reset" for N ms, then comes out of reset with the 0xe3ff8001 message, like a real modem finishing its boot.
 * 0 (default) = the old behaviour (never in reset). kern.sweet_abp_bbmsg sends one message by hand (see sweet_abp.c).
 */
static volatile uint32_t abb_in_reset;
static thread_call_t abb_ready_tc;
static uint32_t abb_readies;

static void abb_ready_fire(thread_call_param_t, thread_call_param_t);

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
	case 0x03:      /* get reset: 1064 in reset while kern.sweet_abp_readyms runs, else not; 1071: never during the push */
		out = (abm_on() && abm_push_active()) ? 0 : abb_in_reset;
		break;
	case 0x00:      /* reset modem */
		if (abm_on()) {
			abm_reset("AppleBaseband reset (sel 0)");
		}
		out = 0;
		break;
	case 0x01:      /* power on modem: 1071 answers when the real modem is up */
		if (abm_on()) {
			IOLog("SWEET ABM: power on (AppleBaseband sel 1): modem link state %d\n", sbb_link_state());
			abm_wait_up(sweet_abm_upwait_ms, "power on");
		}
		out = 0;
		break;
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
		if (in0 == 1 && abm_on()) {
			IOLog("SWEET ABM: ABM starting (manager state 1): baseband reads as out of reset; booted/ready follows DONE_RESP\n");
		} else if (in0 == 1 && sweet_abp_readyms > 0) {
			/* 1064: ABM starting: the baseband is "in reset" for readyms, then alive */
			uint32_t ms = sweet_abp_readyms > 30000 ? 30000 : sweet_abp_readyms;
			if (abb_ready_tc == NULL) {
				abb_ready_tc = thread_call_allocate(abb_ready_fire, NULL);
			}
			if (abb_ready_tc != NULL) {
				uint64_t deadline;
				abb_in_reset = 1;
				clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
				thread_call_enter_delayed(abb_ready_tc, deadline);
				IOLog("SWEET ABP: ABM starting: baseband in reset for %u ms (kern.sweet_abp_readyms)\n", ms);
			}
		}
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
	IOReturn abmReadRegister(uint32_t reg, IOExternalMethodArguments *args);
	static void abmImageTaken(uint32_t len, const uint8_t *head, uint32_t hn, bool async);
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
		if (fPipe < ABP_NQMUX) {
			sweet_abp_c2w(fPipe, sess, ABPB_CLOSE, NULL, 0);
		} else {
			abm_on_close(fPipe, sess);
		}
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
	uint64_t outv[2] = { 0, 0 };    /* 1071: scalar outputs of sel 4 */
	bool has_outv = false;

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
		ABPLOG("%s/%u: open%s (%u session(s) open)", abp_pipe_names[fPipe], fSess, fresh ? "" : " again", live);
		if (fPipe < ABP_NQMUX) {
			sweet_abp_c2w(fPipe, fSess, ABPB_OPEN, &v, sizeof(v));
		} else {
			abm_on_open(fPipe, fSess);
		}
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
			if (fPipe < ABP_NQMUX) {
				abp_log_frame(fPipe, fSess, "-> wland", buf, len, nth);
				sweet_abp_c2w(fPipe, fSess, ABPB_DATA, buf, len);
			} else {
				abm_on_write(fPipe, fSess, buf, len);
			}
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
		if (fPipe < ABP_NQMUX) {
			sweet_abp_c2w(fPipe, fSess, selector == 7 ? ABPB_START : ABPB_ABORT, &v, sizeof(v));
		}
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
	case 4:         /* 1071: sendImage (BHI): taken and dropped */
	case 5: {       /* 1071: readRegister (BHI) */
		if (fPipe != ABP_PIPE_PBL) {
			r = kIOReturnUnsupported;
			break;
		}
		if (selector == 5) {
			r = abmReadRegister((uint32_t)in0, args);
			break;
		}
		uint8_t head[16];
		uint32_t hn = 0, len;
		bzero(head, sizeof(head));
		if (async) {
			len = (uint32_t)in1;
			IOMemoryDescriptor *md = len ? IOMemoryDescriptor::withAddressRange(in0, len < 16 ? len : 16, kIODirectionOut, fTask) : NULL;
			if (md && md->prepare() == kIOReturnSuccess) {
				hn = (uint32_t)md->readBytes(0, head, len < 16 ? len : 16);
				md->complete();
			}
			OSSafeReleaseNULL(md);
		} else {
			len = args->structureInputDescriptor ? (uint32_t)args->structureInputDescriptor->getLength() : args->structureInputSize;
			hn = abp_struct_in(args, head, 16);
		}
		abmImageTaken(len, head, hn, async);
		outv[0] = len;
		outv[1] = 0;
		has_outv = true;
		if (async) {
			io_user_reference_t a[2] = { len, 0 };
			sendAsyncResult64(args->asyncReference, kIOReturnSuccess, a, 2);
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
		args->scalarOutput[i] = (has_outv && i < 2) ? outv[i] : 0;
	}
	if (selector != 2 && selector != 3 && fCalls[selector < 0x10 ? selector : 0xf]++ < 8) {
		abp_log_call(abp_pipe_names[fPipe], selector, args, r);
	} else if ((selector == 2 || selector == 3) && fCalls[selector]++ < 3) {
		abp_log_call(abp_pipe_names[fPipe], selector, args, r);
	}
	return r;
}

/* ---------------- 1071: real ABM boot emulation (BHI + Sahara target) ---------------- */

/*
 * The loader's default OEM PK hash (BBUEUR13Loader, 32 bytes; it is Qualcomm's public default/test root hash). The BHI
 * PK hash register reads back this value, so BBU's queryChipInfo logs "Detect as unfused BB" (fusing state 1) and
 * needs no BBTicket, manifest or nonce.
 */
static const uint8_t abm_default_pkhash[32] = {
	0xcc, 0x31, 0x53, 0xa8, 0x02, 0x93, 0x93, 0x9b, 0x90, 0xd0, 0x2d, 0x3b, 0xf8, 0xb2, 0x3e, 0x02,
	0x92, 0xe4, 0x52, 0xfe, 0xf6, 0x62, 0xc7, 0x49, 0x98, 0x42, 0x1a, 0xda, 0xd4, 0x2a, 0x38, 0x0f,
};

/* Sahara commands (little-endian u32 words: cmd, length, ...) */
enum {
	SAH_HELLO = 1, SAH_HELLO_RESP = 2, SAH_READ_DATA = 3, SAH_END_IMAGE_TX = 4, SAH_DONE = 5, SAH_DONE_RESP = 6,
	SAH_RESET = 7, SAH_RESET_RESP = 8,
};
enum { ABM_IDLE, ABM_WAIT_HELLO_RESP, ABM_WAIT_DATA, ABM_WAIT_DONE, ABM_COMPLETE };
static const char * const abm_state_names[] = { "idle", "wait HELLO_RESP", "wait image data", "wait DONE", "complete" };

static struct {
	uint32_t state, sess, img, want, got;
	bool active;                    /* the push runs (BHI or Sahara seen, not complete yet) */
	uint64_t t_active;              /* ms */
	uint32_t opens, hellos, hello_resps, reads, ends, dones, completes, resets, odd;
	uint32_t regreads, images, image_bytes, upwaits, uptimeouts;
	uint64_t data_bytes;
} abm;
static uint32_t abm_lines;
#define ABMLOG(fmt, ...) do { if (abm_lines < 400) { abm_lines++; IOLog("SWEET ABM: " fmt "\n", ##__VA_ARGS__); } } while (0)

static uint64_t
abm_ms(void)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000000ULL;
}

static void
abm_mark_active(void)
{
	if (abm_lock) {
		IOLockLock(abm_lock);
	}
	abm.active = true;
	abm.t_active = abm_ms();
	if (abm_lock) {
		IOLockUnlock(abm_lock);
	}
}

/* The push counts as running for 60 s after its last BHI/Sahara activity, until DONE_RESP(complete). */
static bool
abm_push_active(void)
{
	return abm.active && abm_ms() - abm.t_active < 60000;
}

/* Waits (in the caller's thread, no locks held) until kern.sweet_mdm_up == 2. True when up. */
static bool
abm_wait_up(uint32_t ms, const char *why)
{
	if (!(sweet_bb & 1)) {
		ABMLOG("%s: modem state not tracked (sweetbb & 1 off, wland owns the link): not waiting", why);
		return true;
	}
	uint64_t t0 = abm_ms();
	int st = sbb_link_state();
	if (st == 2) {
		return true;
	}
	abm.upwaits++;
	ABMLOG("%s: modem not up yet (kern.sweet_mdm_up = %d): waiting up to %u ms", why, st, ms);
	while ((st = sbb_link_state()) != 2 && abm_ms() - t0 < ms) {
		IOSleep(50);
	}
	if (st == 2) {
		ABMLOG("%s: modem up after %llu ms", why, abm_ms() - t0);
		return true;
	}
	abm.uptimeouts++;
	ABMLOG("%s: modem still not up after %u ms (state %d): answering anyway", why, ms, st);
	return false;
}

static void
abm_send(unsigned pipe, uint32_t sess, const uint32_t *w, uint32_t len, const char *what)
{
	int e = SweetABPDeliver(pipe, sess, w, len);
	ABMLOG("sahara/%u: -> %s (%u bytes)%s", sess, what, len, e ? " NOT delivered" : "");
}

static void
abm_reset(const char *why)
{
	if (abm_lock == NULL) {
		return;
	}
	IOLockLock(abm_lock);
	uint32_t was = abm.state;
	abm.state = ABM_IDLE;
	abm.resets++;
	IOLockUnlock(abm_lock);
	ABMLOG("%s: Sahara state %s -> idle", why, abm_state_names[was]);
}

static void
abm_on_open(unsigned pipe, uint32_t sess)
{
	if (abm_lock == NULL) {
		return;
	}
	abm_mark_active();
	if (pipe == ABP_PIPE_PBL) {
		ABMLOG("BHI stage: pbl/%u opened by the EUR13 loader (power on, reset detect: modem link state %d)", sess,
		    sbb_link_state());
		return;
	}
	uint32_t img = sweet_abm_img, want = sweet_abm_imglen;
	if (want == 0 || want > 0x1000) {
		want = 0x40;
	}
	IOLockLock(abm_lock);
	abm.state = ABM_WAIT_HELLO_RESP;
	abm.sess = sess;
	abm.img = img;
	abm.want = want;
	abm.got = 0;
	abm.opens++;
	abm.hellos++;
	IOLockUnlock(abm_lock);
	/* HELLO: version 2, compatible 1, max command packet 0x1000, mode 0 (image transfer pending), reserved 0 */
	uint32_t hello[12] = { SAH_HELLO, 0x30, 2, 1, 0x1000, 0, 0, 0, 0, 0, 0, 0 };
	ABMLOG("Sahara stage: sahara/%u opened by the EUR10 programmer: HELLO (v2, mode 0)", sess);
	abm_send(pipe, sess, hello, sizeof(hello), "HELLO");
}

static void
abm_on_close(unsigned pipe, uint32_t sess)
{
	ABMLOG("%s/%u closed (Sahara state %s)", abp_pipe_names[pipe], sess, abm_state_names[abm.state]);
}

static void abm_booted(void);

static void
abm_on_write(unsigned pipe, uint32_t sess, const uint8_t *buf, uint32_t len)
{
	uint32_t w[8];
	bzero(w, sizeof(w));
	memcpy(w, buf, len < sizeof(w) ? len : sizeof(w));
	if (pipe != ABP_PIPE_SAHARA || abm_lock == NULL) {
		ABMLOG("%s/%u: unexpected write of %u bytes (cmd word 0x%x): dropped", abp_pipe_names[pipe], sess, len, w[0]);
		return;
	}
	abm_mark_active();
	bool cmd = len >= 8 && w[1] == len && w[0] >= SAH_HELLO && w[0] <= 0x20;
	if (cmd && w[0] == SAH_RESET) {
		abm_reset("Sahara RESET from the host");
		uint32_t rr[2] = { SAH_RESET_RESP, 8 };
		abm_send(pipe, sess, rr, sizeof(rr), "RESET_RESP");
		return;
	}
	IOLockLock(abm_lock);
	uint32_t st = abm.state;
	switch (st) {
	case ABM_WAIT_HELLO_RESP:
		if (cmd && w[0] == SAH_HELLO_RESP && len >= 0x18) {
			abm.hello_resps++;
			abm.state = ABM_WAIT_DATA;
			abm.got = 0;
			abm.reads++;
			uint32_t img = abm.img, want = abm.want;
			IOLockUnlock(abm_lock);
			ABMLOG("sahara/%u: <- HELLO_RESP version %u compat %u status %u mode %u", sess, w[2], w[3], w[4], w[5]);
			uint32_t rd[5] = { SAH_READ_DATA, 0x14, img, 0, want };
			ABMLOG("sahara/%u: READ_DATA image 0x%x offset 0 length 0x%x", sess, img, want);
			abm_send(pipe, sess, rd, sizeof(rd), "READ_DATA");
			return;
		}
		break;
	case ABM_WAIT_DATA: {
		uint32_t first = abm.got == 0;
		abm.got += len;
		abm.data_bytes += len;
		bool full = abm.got >= abm.want;
		uint32_t img = abm.img, got = abm.got;
		if (full) {
			abm.state = ABM_WAIT_DONE;
			abm.ends++;
		}
		IOLockUnlock(abm_lock);
		if (first) {
			char h[40];
			abp_hex(h, sizeof(h), buf, len < 16 ? len : 16);
			ABMLOG("sahara/%u: <- image 0x%x data %u bytes (%s...): counted, dropped", sess, img, len, h);
		}
		if (full) {
			uint32_t end[4] = { SAH_END_IMAGE_TX, 0x10, img, 0 };
			ABMLOG("sahara/%u: image 0x%x: %u bytes taken: END_IMAGE_TX success", sess, img, got);
			abm_send(pipe, sess, end, sizeof(end), "END_IMAGE_TX");
		}
		return;
	}
	case ABM_WAIT_DONE:
	case ABM_COMPLETE:
		if (cmd && w[0] == SAH_DONE) {
			abm.dones++;
			IOLockUnlock(abm_lock);
			ABMLOG("sahara/%u: <- DONE", sess);
			/* the final answer: the modem must be up (short wait: BBU's programmer reads time out after 5 s) */
			abm_wait_up(3000, "DONE");
			IOLockLock(abm_lock);
			abm.state = ABM_COMPLETE;
			abm.completes++;
			abm.active = false;
			IOLockUnlock(abm_lock);
			uint32_t dr[3] = { SAH_DONE_RESP, 0xc, 1 };
			ABMLOG("sahara/%u: DONE_RESP image transfer complete (BBU: \"All images transferred\")", sess);
			abm_send(pipe, sess, dr, sizeof(dr), "DONE_RESP");
			abm_booted();
			return;
		}
		break;
	default:
		break;
	}
	abm.odd++;
	IOLockUnlock(abm_lock);
	ABMLOG("sahara/%u: unexpected %u bytes in state %s (words 0x%x 0x%x 0x%x 0x%x): ignored", sess, len,
	    abm_state_names[st], w[0], w[1], w[2], w[3]);
}

/* After DONE_RESP: booted/ready. The modem is up; ABM's ResetDetection gets the "alive" message like after a real boot. */
static void
abm_booted(void)
{
	uint32_t ms = sweet_abp_readyms < 200 ? 200 : (sweet_abp_readyms > 30000 ? 30000 : sweet_abp_readyms);
	if (abb_ready_tc == NULL) {
		abb_ready_tc = thread_call_allocate(abb_ready_fire, NULL);
	}
	abb_in_reset = 0;
	if (abb_ready_tc != NULL) {
		uint64_t deadline;
		clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
		thread_call_enter_delayed(abb_ready_tc, deadline);
	}
	ABMLOG("booted: 'alive' (0xe3ff8001) to AppleBaseband clients in %u ms; modem link state %d", ms, sbb_link_state());
}

IOReturn
AppleBasebandPCIUserClient::abmReadRegister(uint32_t reg, IOExternalMethodArguments *args)
{
	uint8_t v[0x100];
	uint32_t want = args->structureOutputDescriptor ? (uint32_t)args->structureOutputDescriptor->getLength() : args->structureOutputSize;
	uint32_t n = want < sizeof(v) ? want : (uint32_t)sizeof(v);
	bzero(v, sizeof(v));
	const char *what = "unknown (zeros)";
	switch (reg) {
	case 0:
		memcpy(v, &sweet_abm_snum, 4);
		what = "SNUM";
		break;
	case 3:
		memcpy(v + 4, &sweet_abm_chipid, 4);
		what = "MSM HW ID";
		break;
	case 4:
		memcpy(v, abm_default_pkhash, sizeof(abm_default_pkhash));
		what = "OEM PK hash = default (unfused)";
		break;
	default:
		break;
	}
	abm_mark_active();
	abm.regreads++;
	IOReturn r = abp_struct_out(args, v, n);
	ABMLOG("BHI readRegister %u (%s): %u of %u bytes -> 0x%x", reg, what, n, want, r);
	return r;
}

void
AppleBasebandPCIUserClient::abmImageTaken(uint32_t len, const uint8_t *head, uint32_t hn, bool async)
{
	char h[40];
	abp_hex(h, sizeof(h), head, hn);
	abm_mark_active();
	abm.images++;
	abm.image_bytes += len;
	ABMLOG("BHI sendImage%s #%u: %u bytes (%s...) taken and dropped (never reaches the SM7250)", async ? " (async)" : "",
	    abm.images, len, h);
}

int
SweetABMStats(char *buf, size_t len)
{
	int n = snprintf(buf, len, "sweetbb & 0x40 %s | sahara %s (image 0x%x want %u got %u) push %s | BHI regreads %u images %u (%u B) "
	    "| sahara opens %u hellos %u hello_resp %u read_data %u end_tx %u done %u complete %u resets %u odd %u data %llu B "
	    "| modem link %d, up-waits %u timeouts %u",
	    abm_on() ? "on" : "off", abm_state_names[abm.state < 5 ? abm.state : 0], abm.img, abm.want, abm.got,
	    abm_push_active() ? "running" : "idle", abm.regreads, abm.images, abm.image_bytes, abm.opens, abm.hellos,
	    abm.hello_resps, abm.reads, abm.ends, abm.dones, abm.completes, abm.resets, abm.odd, abm.data_bytes,
	    sbb_link_state(), abm.upwaits, abm.uptimeouts);
	return n < (int)len ? n : (int)len - 1;
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

/* Every selector succeeds with zeroed outputs; async requests are accepted and never completed.
 * 1075: with sweetbb & 0x10, AppleIPAppender's selectors go to ipa_method() instead (below). */
static IOReturn ipa_method(uint32_t selector, IOExternalMethodArguments *args);
static volatile SInt32 ipa_clients;
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
	if (fKind == 0 && sweet_pdp_enabled() && ipa_clients > 0 && OSDecrementAtomic(&ipa_clients) == 1) {
		sweet_pdp_unblock_all();        /* 1075: the last AppleIPAppender client is gone: no stale blockPDPTraffic */
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
SweetABPStubUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	if (fKind == 0 && sweet_pdp_enabled()) {
		return ipa_method(selector, args);
	}
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = 0;
	}
	if (args->structureOutputDescriptor != NULL) {
		uint32_t n = (uint32_t)args->structureOutputDescriptor->getLength();
		uint8_t z[256];
		bzero(z, sizeof(z));
		for (uint32_t o = 0; o < n; o += sizeof(z)) {
			abp_md_copy(args->structureOutputDescriptor, o, z, n - o < sizeof(z) ? n - o : sizeof(z), true);
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

/*
 * 1075: AppleIPAppender selectors (only with sweetbb & 0x10; research/cc/P4-u2-classes.md Q3). Sizes are checked
 * (kIOReturnBadArgument on a mismatch, which ApplePDPHelper only logs); answers: success with zeroed outputs,
 * 0x02 getFlow returns what 0x00 addFlow stored, 0x0a no wake buffer (0 bytes), 0x11 trailer not enabled (0),
 * 0x12 no keepalive frames (0 bytes), 0x10 blockPDPTraffic sets the pdp_ip output block flag, 0x14 accepted and never
 * completed. One klog line per selector, the first time it is called.
 */
#define IPA_SO_FIXED    0u      /* struct out: caller gives >= sout, gets sout zeroed bytes */
#define IPA_SO_EMPTY    1u      /* struct out: any size (multiple of sout if sout), answered with 0 bytes */
#define IPA_SO_UPTO     2u      /* struct out: caller gives <= sout, all of it zeroed */
struct ipa_sel {
	const char *name;
	uint8_t nin, nout, somode;
	uint16_t sin, sout;
};
static const struct ipa_sel ipa_sels[] = {
	{ "addFlow", 0, 0, IPA_SO_FIXED, 8, 0 },
	{ "removeFlow", 1, 0, IPA_SO_FIXED, 0, 0 },
	{ "getFlow", 1, 1, IPA_SO_FIXED, 0, 0 },
	{ "enable", 1, 1, IPA_SO_FIXED, 0, 0 },
	{ "setDormancyInterval", 0, 0, IPA_SO_FIXED, 8, 0 },
	{ "getDormancyTime", 0, 1, IPA_SO_FIXED, 0, 0 },
	{ "getTimeAdjustment", 0, 0, IPA_SO_FIXED, 0, 8 },
	{ "getStatus", 0, 0, IPA_SO_FIXED, 0, 0xb8 },
	{ "setLinkQualityMetric", 1, 0, IPA_SO_FIXED, 0, 0 },
	{ "restartDormancyCheck", 0, 0, IPA_SO_FIXED, 0, 0 },
	{ "copyWakeBuffer", 1, 0, IPA_SO_EMPTY, 0, 0 },
	{ "reportLinkThroughput", 0, 0, IPA_SO_FIXED, 0x20, 0 },
	{ "getOpenPortsExt", 3, 0, IPA_SO_UPTO, 0, 0x1000 },
	{ "enableLowPowerPDPThrottle", 1, 0, IPA_SO_FIXED, 0, 0 },
	{ "activateLowPowerPDPThrottle", 1, 0, IPA_SO_FIXED, 0, 0 },
	{ "getNumLowPowerPDPThrottleStats", 0, 0, IPA_SO_FIXED, 0, 0xc },
	{ "blockPDPTraffic", 2, 0, IPA_SO_FIXED, 0, 0 },
	{ "enableMetadataTrailer", 1, 1, IPA_SO_FIXED, 0, 0 },
	{ "getKeepaliveOffloadFrames", 1, 0, IPA_SO_EMPTY, 0, 0x84 },
	{ "notifyDedicatedBearer", 0, 0, IPA_SO_FIXED, 0x10, 0x10 },
	{ "registerFilterRuleCallBack", 2, 0, IPA_SO_FIXED, 0, 0 },
};
#define IPA_NSELS       (sizeof(ipa_sels) / sizeof(ipa_sels[0]))
static uint8_t ipa_seen[0x20], ipa_bad_seen[0x20];
static uint32_t ipa_flow[16];           /* addFlow: traffic class -> flow id (getFlow only) */

static IOReturn
ipa_method(uint32_t selector, IOExternalMethodArguments *args)
{
	const struct ipa_sel *s = selector < IPA_NSELS ? &ipa_sels[selector] : NULL;
	uint32_t sin = args->structureInputDescriptor ? (uint32_t)args->structureInputDescriptor->getLength() : args->structureInputSize;
	uint32_t soc = args->structureOutputDescriptor ? (uint32_t)args->structureOutputDescriptor->getLength() : args->structureOutputSize;
	uint32_t sout = 0;
	IOReturn r = kIOReturnSuccess;
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = 0;
	}
	if (s == NULL) {
		sout = soc;     /* not in Q3: today's answer (success, zeroed outputs) */
	} else if (args->scalarInputCount != s->nin || args->scalarOutputCount != s->nout || sin != s->sin) {
		r = kIOReturnBadArgument;
	} else if (s->somode == IPA_SO_FIXED) {
		if (soc < s->sout) {
			r = kIOReturnBadArgument;
		}
		sout = s->sout;
	} else if (s->somode == IPA_SO_EMPTY) {
		if (s->sout && soc % s->sout) {
			r = kIOReturnBadArgument;
		}
		sout = 0;
	} else {
		if (soc > s->sout) {
			r = kIOReturnBadArgument;
		}
		sout = soc;
	}
	if (r == kIOReturnSuccess) {
		uint8_t in[0x20];
		bzero(in, sizeof(in));
		(void)abp_struct_in(args, in, sizeof(in));
		switch (selector) {
		case 0x00: {    /* addFlow {u32 trafficClass, u32 flowId} */
			uint32_t tc, fl;
			memcpy(&tc, in, 4);
			memcpy(&fl, in + 4, 4);
			ipa_flow[tc & 15] = fl;
			break;
		}
		case 0x01:
			ipa_flow[args->scalarInput[0] & 15] = 0;
			break;
		case 0x02:
			args->scalarOutput[0] = ipa_flow[args->scalarInput[0] & 15];
			break;
		case 0x10: {    /* blockPDPTraffic(pdp index, block) */
			uint64_t idx = args->scalarInput[0], on = args->scalarInput[1] != 0;
			if (idx < 32) {
				sweet_pdp_block((uint32_t)idx, (int)on);
				IOLog("SWEET ABP: 1075 blockPDPTraffic pdp %llu %s\n", idx, on ? "blocked" : "unblocked");
			} else {
				r = kIOReturnBadArgument;
			}
			break;
		}
		default:
			break;
		}
	}
	if (r == kIOReturnSuccess && sout) {
		uint8_t z[256];
		bzero(z, sizeof(z));
		if (args->structureOutputDescriptor != NULL) {
			for (uint32_t o = 0; o < sout; o += sizeof(z)) {
				abp_md_copy(args->structureOutputDescriptor, o, z, sout - o < sizeof(z) ? sout - o : sizeof(z), true);
			}
		} else if (args->structureOutput != NULL) {
			bzero(args->structureOutput, sout);
		}
	}
	if (args->structureOutputDescriptor != NULL) {
		args->structureOutputDescriptorSize = r == kIOReturnSuccess ? sout : 0;
	} else {
		args->structureOutputSize = r == kIOReturnSuccess ? sout : 0;
	}
	uint32_t k = selector < 0x20 ? selector : 0x1f;
	uint8_t *seen = r == kIOReturnSuccess ? ipa_seen : ipa_bad_seen;
	if (!seen[k]) {
		seen[k] = 1;
		char p[32];
		proc_selfname(p, (int)sizeof(p));
		IOLog("SWEET ABP: 1075 %s AppleIPAppender sel 0x%x %s%s in[%u]={0x%llx,0x%llx} struct in %u out %u/%u -> 0x%x\n", p, selector,
		    s ? s->name : "unknown", args->asyncWakePort != MACH_PORT_NULL ? " (async)" : "", args->scalarInputCount,
		    args->scalarInputCount > 0 ? args->scalarInput[0] : 0ULL, args->scalarInputCount > 1 ? args->scalarInput[1] : 0ULL,
		    sin, sout, soc, r);
	}
	return r;
}

class AppleIPAppenderUserClient : public SweetABPStubUserClient
{
	OSDeclareDefaultStructors(AppleIPAppenderUserClient);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE
	{
		fKind = 0;
		if (!SweetABPStubUserClient::start(provider)) {
			return false;
		}
		if (sweet_pdp_enabled()) {
			OSIncrementAtomic(&ipa_clients);
		}
		return true;
	}
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
	abm_lock = IOLockAlloc();
	bbv_lock = IOLockAlloc();      /* 1092 */
	abp_started = true;
	/* 1092: "Baseband Voice" from boot only with boot-arg sweetbbv=1 (before mediaserverd loads BasebandVoice.driver) */
	uint32_t bbv = 0;
	PE_parse_boot_argn("sweetbbv", &bbv, sizeof(bbv));
	if (bbv) {
		SweetBBVPublish(true, "boot-arg sweetbbv");
	} else {
		IOLog("SWEET BBV: \"Baseband Voice\" not published (default; boot-arg sweetbbv=1 or kern.sweet_bbv_service=1)\n");
	}
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
			if (i >= ABP_NQMUX && !abm_on()) {
				continue;       /* 1071: the boot pipes exist only for the real-boot emulation */
			}
			AppleBasebandPCIInterface *ifc = OSTypeAlloc(AppleBasebandPCIInterface);
			if (ifc && !ifc->init()) {
				OSSafeReleaseNULL(ifc);
			}
			if (ifc == NULL) {
				continue;
			}
			ifc->fPipe = i;
			ifc->setProperty("ABPInterfaceProtocol", abp_pipe_names[i]);
			/* qmux1/qmux2 = PCITransportInterface 4/5; 1071: pbl/sahara = 0/1 */
			ifc->setProperty("ABPInterfaceIndex", (unsigned long long)(i < ABP_NQMUX ? i + 4 : i - ABP_PIPE_PBL), 32);
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
		sweet_pdp_publish();            /* 1075: pdp_ip0..7 once, only with sweetbb & 0x10 */
		return 0;
	}
	IOService *svcs[12];
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

/*
 * 1092: "Baseband Voice". BasebandVoice.driver (HAL plug-in in mediaserverd) decides once, when it loads, whether it
 * publishes its own PCIe baseband audio device: IOServiceGetMatchingService(IOServiceNameMatching("Baseband Voice"))
 * finding a service = "BVCanPublishBasebandVoice: NO (I2S device present)", as on a real D22 whose kernel I2S device
 * has that name. The stock driver reads nothing from the service (BBA study A1: existence only), so the service has
 * no properties of its own beyond "SweetABP" (marks it as ours) and opens nothing on it; an IOServiceOpen is logged
 * (first 8) and refused with kIOReturnUnsupported. Our RedwoodAudio plug-in provides the HAL device of that name.
 * Published under the platform expert, independent of the stand-in set (SweetABPPublish never touches it).
 * Default off; boot-arg sweetbbv=1 publishes it from SweetABPStart; kern.sweet_bbv_service 1/0 publishes/terminates
 * it at run time (mediaserverd decides only when it starts: restart it after a change).
 */
class SweetBasebandVoice : public IOService
{
	OSDeclareDefaultStructors(SweetBasebandVoice);
public:
	using IOService::newUserClient;
	virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
	    IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(SweetBasebandVoice, IOService);

static IOService *bbv_service;
static uint32_t bbv_publishes, bbv_unpublishes, bbv_opens;

IOReturn
SweetBasebandVoice::newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
    IOUserClient **handler)
{
	(void)owningTask; (void)securityID; (void)properties;
	uint32_t n = OSIncrementAtomic((SInt32 *)&bbv_opens) + 1;
	if (n <= 8) {
		char pname[32];
		proc_selfname(pname, sizeof(pname));
		IOLog("SWEET BBV: open of \"Baseband Voice\" (type %u) by pid %d %s: unsupported (#%u)\n", (unsigned)type,
		    proc_selfpid(), pname, n);
	}
	if (handler) {
		*handler = NULL;
	}
	return kIOReturnUnsupported;
}

/* 0 ok, EALREADY when already in that state, ENXIO before SweetABPStart / without a platform expert, ENOMEM. */
extern "C" int
SweetBBVPublish(bool on, const char *why)
{
	if (!abp_started || bbv_lock == NULL) {
		return ENXIO;
	}
	IOLockLock(bbv_lock);
	if (on == (bbv_service != NULL)) {
		IOLockUnlock(bbv_lock);
		return EALREADY;
	}
	if (!on) {
		IOService *svc = bbv_service;
		bbv_service = NULL;
		bbv_unpublishes++;
		IOLockUnlock(bbv_lock);
		svc->terminate();
		svc->release();
		IOLog("SWEET BBV: \"Baseband Voice\" terminated (%s, #%u)\n", why, bbv_unpublishes);
		return 0;
	}
	IOService *platform = IOService::getPlatform();
	if (platform == NULL) {
		IOLockUnlock(bbv_lock);
		IOLog("SWEET BBV: no platform expert yet: \"Baseband Voice\" not published (%s)\n", why);
		return ENXIO;
	}
	SweetBasebandVoice *svc = OSTypeAlloc(SweetBasebandVoice);
	if (svc == NULL || !svc->init()) {
		OSSafeReleaseNULL(svc);
		IOLockUnlock(bbv_lock);
		return ENOMEM;
	}
	svc->setProperty("SweetABP", true);
	if (!svc->attach(platform)) {
		svc->release();
		IOLockUnlock(bbv_lock);
		return ENOMEM;
	}
	svc->setName("Baseband Voice");
	if (!svc->start(platform)) {
		svc->detach(platform);
		svc->release();
		IOLockUnlock(bbv_lock);
		return ENXIO;
	}
	svc->registerService();
	bbv_service = svc;              /* the attach keeps the registry's reference; this one is ours */
	bbv_publishes++;
	IOLockUnlock(bbv_lock);
	IOLog("SWEET BBV: published \"Baseband Voice\" (SweetBasebandVoice) under %s (%s, #%u)\n", platform->getName(), why,
	    bbv_publishes);
	return 0;
}

extern "C" int
SweetBBVPublished(void)
{
	return bbv_service != NULL;
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

/* 1064: a general-interest message to AppleBaseband's clients (ABM ResetDetection); tracks the reset state it implies */
void
SweetABPBasebandMessage(uint32_t code)
{
	IOService *bb = NULL;
	if (!abp_started) {
		return;
	}
	if (code == 0xe3ff8000u && abm_on() && abm_push_active()) {
		IOLog("SWEET ABM: 'reset detected' (0xe3ff8000) held back: BBUpdater is pushing (it would powercycle)\n");
		return;
	}
	if (code == 0xe3ff8000u) {
		abb_in_reset = 1;
	} else if (code == 0xe3ff8001u) {
		abb_in_reset = 0;
	}
	IOLockLock(abp_svc_lock);
	for (unsigned i = 0; i < abp_nservices; i++) {
		if (OSDynamicCast(AppleBaseband, abp_services[i])) {
			bb = abp_services[i];
			bb->retain();
			break;
		}
	}
	IOLockUnlock(abp_svc_lock);
	if (bb) {
		IOReturn r = bb->messageClients(code, NULL, 0);
		IOLog("SWEET ABP: message 0x%x to AppleBaseband clients -> 0x%x (in reset %u)\n", code, r, abb_in_reset);
		bb->release();
	} else {
		IOLog("SWEET ABP: message 0x%x: AppleBaseband not published\n", code);
	}
}

static void
abb_ready_fire(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	abb_readies++;
	IOLog("SWEET ABP: baseband out of reset (#%u): alive message\n", abb_readies);
	SweetABPBasebandMessage(0xe3ff8001u);
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
	o += snprintf(buf + o, len - o, "%s (%u services, published %u x, un-published %u x) | bootstage %u mgr %llu bb %llu reset %u readyms %u readies %u",
	    abp_nservices ? "published" : "NOT published", abp_nservices, abp_publishes, abp_unpublishes, sweet_abp_bootstage,
	    abb_manager_state, abb_baseband_state, abb_in_reset, sweet_abp_readyms, abb_readies);
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
