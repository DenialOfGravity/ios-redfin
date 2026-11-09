/*
 * redfin 1061: AppleBasebandPCI stand-in, BSD half (the IOKit half is iokit/Kernel/arm/SweetABP.cpp).
 *
 * Boot-arg sweetabp (bitmask, default 0 = nothing at all):
 *   1  publish the stand-in services (baseband nub, AppleBaseband, AppleBasebandPCIControl, the
 *      AppleBasebandPCIInterface pipes qmux1/qmux2) and /dev/abpbridge;
 *   2  "fake boot": in every process named CommCenter, capabilities::bb::abm::shouldBoot() (dyld shared
 *      cache, libBasebandCapabilities) answers 0 instead of 1, so ABM takes its existing "Faking bootup"
 *      path instead of pushing modem firmware over Sahara. Also enabled by boot-arg sweetabmfake=1.
 *   4  IOKit trace (IOUserClient.cpp "IOK CommCenter: ..." lines) of CommCenter's matching/registry calls.
 *   8  (1063) deferred publish: /dev/abpbridge is made at boot, but the baseband services are published only when
 *      the bridge daemon (wland) has the device open and says ABPB_IOC_READY 1, so CommCenter/ABM never sees a
 *      baseband nobody answers for. ABPB_IOC_READY 0 or closing the device marks it not ready again (see 0x20).
 *   0x10 (1063) leave out AppleBasebandPCIPDPManagerBase (A/B for the 30 s "Start TIMEOUT: boot.mod" seen once in P1).
 *   0x20 (1063) with 8: when wland says not ready / closes the device, also un-publish (terminate) the services;
 *      without it they stay published (CommCenter keeps its sessions, frames queue in the ring until wland is back).
 * kern.sweet_abp can be written (root) to turn bits on at run time (never off).
 * kern.sweet_abp_publish (1063): reads 1 when the services are published; write 1 = publish now, 0 = un-publish now
 * (debugging; does not change the bits).
 *
 * /dev/abpbridge (root, 0600, one opener): wland reads every frame CommCenter writes to a pipe and writes
 * frames that complete CommCenter's reads on that pipe. Records, little-endian:
 *     u16 magic 0xAB51 | u8 pipe (0 qmux1, 1 qmux2) | u8 type | u32 session | u32 len | len bytes
 *     type 0 DATA, 1 OPEN, 2 CLOSE, 3 START, 4 ABORT (1-4 only kernel -> wland; data = the selector's scalar)
 *     session: one per user client that opened the pipe (ABM opens qmux2 from several QMux instances at
 *     once, each its own QMUX stream); wland answers a frame with the same pipe + session.
 * ioctl ABPB_IOC_READY (_IOW('B', 0x51, uint32_t)): 1 = wland can answer now, 0 = not any more (1063).
 * read() returns whole records only (as many as fit; EMSGSIZE if the first does not), blocks unless
 * O_NONBLOCK; select() works. write() takes one or more whole DATA records. The kernel -> wland queue is a
 * 256 KiB ring that also fills while nobody has the device open; when it is full, frames are dropped and
 * counted (CommCenter's write still succeeds, like a modem that loses a frame). Nothing here blocks the
 * kernel: CommCenter's writes never wait.
 *
 * The fake-boot patch (bit 2): from shared_region_check_np() (dyld asks for the shared cache address as
 * one of the first things a process does), for a process named "CommCenter": compute the cache slide from
 * the mapped cache header, read the 10 instructions of shouldBoot() at cache VA 0x1ae1df4a8 (iOS 16.7
 * 20H392 iPhone10,3 cache) and compare them with the expected bytes (refuse and log on any difference),
 * then do what a debugger does to set a breakpoint: cs_allow_invalid() (CS_DEBUGGED), a private
 * copy-on-write copy of that one page (vm_protect VM_PROT_COPY), write "mov w0,#0" over "mov w0,#1",
 * back to r-x, I-cache sync, read back. Only that process's map changes; no file is touched.
 * Log lines "SWEET ABP:".
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/uio.h>
#include <sys/select.h>
#include <sys/ioccom.h>
#include <sys/sysctl.h>
#include <sys/codesign.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <kern/kalloc.h>
#include <pexpert/pexpert.h>
#include <mach/vm_param.h>
#include <mach/vm_attributes.h>
#include <vm/vm_map.h>
#include <libkern/libkern.h>

void sweet_abp_init(void);
void sweet_abp_c2w(unsigned pipe, uint32_t sess, unsigned type, const void *data, uint32_t len);
void sweet_abp_dyld_hook(void *p, uint64_t cache_start);

/* SweetABP.cpp */
extern void SweetABPStart(void);
extern int SweetABPDeliver(unsigned pipe, uint32_t sess, const void *data, uint32_t len);
extern int SweetABPStats(char *buf, size_t len);
extern void SweetABPSetBootstage(uint32_t stage);
extern void SweetABPMessage(uint32_t code);
extern void SweetABPBasebandMessage(uint32_t code);
uint32_t sweet_abp_readyms;     /* 1064: ms the baseband stays "in reset" after ABM starts (0 = never; SweetABP.cpp) */
extern int SweetABPPublish(bool on, const char *why);
extern int SweetABPPublished(void);

extern kern_return_t mach_vm_machine_attribute(vm_map_t map, mach_vm_address_t addr, mach_vm_size_t size,
    vm_machine_attribute_t attribute, vm_machine_attribute_val_t *value);

#define ABP_BIT_SERVICES        1u
#define ABP_BIT_FAKEBOOT        2u
#define ABP_BIT_TRACE           4u
#define ABP_BIT_DEFER           8u
#define ABP_BIT_NOPDP           0x10u
#define ABP_BIT_UNPUB_ON_CLOSE  0x20u
#define ABP_BITS_ALL            0x3fu
#define ABPB_IOC_READY          _IOW('B', 0x51, uint32_t)
#define ABPB_MAGIC              0xAB51
#define ABPB_RING               (256u * 1024u)
#define ABPB_MAXFRAME           65536u
#define ABP_NPIPES              2u

uint32_t sweet_abp_flags;
uint32_t sweet_abp_bootstage = 2;       /* AppleBasebandPCIControl "bootstage": < 2 = "boot stage down" for ABM */

struct abpb_hdr {
	uint16_t magic;
	uint8_t pipe;
	uint8_t type;
	uint32_t sess;
	uint32_t len;
};

static LCK_GRP_DECLARE(abp_grp, "sweet-abp");
static LCK_MTX_DECLARE(abp_lock, &abp_grp);

static struct {
	uint8_t buf[ABPB_RING];
	uint32_t head, tail;            /* head - tail = bytes present */
	struct selinfo sel;
	bool open;
	bool node;
	bool ready;                     /* 1063: wland said ABPB_IOC_READY 1 (cleared on 0 / close) */
	uint32_t readies;
	uint32_t records, dropped, dropped_bytes;
	uint64_t to_wland, from_wland;
} ab;

/* ---- kernel -> wland ring ---- */

static uint32_t
ab_used(void)
{
	return ab.head - ab.tail;
}

static void
ab_put(const void *src, uint32_t len)
{
	const uint8_t *s = src;
	while (len) {
		uint32_t off = ab.head % ABPB_RING;
		uint32_t run = ABPB_RING - off;
		if (run > len) {
			run = len;
		}
		memcpy(ab.buf + off, s, run);
		ab.head += run;
		s += run;
		len -= run;
	}
}

static void
ab_peek(void *dst, uint32_t at, uint32_t len)
{
	uint8_t *d = dst;
	while (len) {
		uint32_t off = at % ABPB_RING;
		uint32_t run = ABPB_RING - off;
		if (run > len) {
			run = len;
		}
		memcpy(d, ab.buf + off, run);
		at += run;
		d += run;
		len -= run;
	}
}

/* CommCenter wrote a frame (or opened/closed/started/aborted a pipe): queue it for wland. Never blocks. */
void
sweet_abp_c2w(unsigned pipe, uint32_t sess, unsigned type, const void *data, uint32_t len)
{
	struct abpb_hdr h = { .magic = ABPB_MAGIC, .pipe = (uint8_t)pipe, .type = (uint8_t)type, .sess = sess, .len = len };
	if (len > ABPB_MAXFRAME) {
		return;
	}
	lck_mtx_lock(&abp_lock);
	if (ABPB_RING - ab_used() < sizeof(h) + len) {
		if (ab.dropped++ < 20) {
			printf("SWEET ABP: bridge queue full (%u bytes, wland %s): dropped a %u-byte frame of pipe %u\n",
			    ab_used(), ab.open ? "open" : "not open", len, pipe);
		}
		ab.dropped_bytes += len;
		lck_mtx_unlock(&abp_lock);
		return;
	}
	ab_put(&h, sizeof(h));
	if (len) {
		ab_put(data, len);
	}
	ab.records++;
	ab.to_wland += len;
	wakeup(&ab);
	selwakeup(&ab.sel);
	lck_mtx_unlock(&abp_lock);
}

static int
ab_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	lck_mtx_lock(&abp_lock);
	if (ab.open) {
		lck_mtx_unlock(&abp_lock);
		return EBUSY;
	}
	ab.open = true;
	printf("SWEET ABP: /dev/abpbridge opened (%u bytes / %u records waiting)\n", ab_used(), ab.records);
	lck_mtx_unlock(&abp_lock);
	return 0;
}

static int
ab_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	bool was_ready;
	lck_mtx_lock(&abp_lock);
	ab.open = false;
	was_ready = ab.ready;
	ab.ready = false;
	wakeup(&ab);
	selwakeup(&ab.sel);
	lck_mtx_unlock(&abp_lock);
	printf("SWEET ABP: /dev/abpbridge closed%s\n", was_ready ? " (was ready: now not ready)" : "");
	if (was_ready && (sweet_abp_flags & ABP_BIT_DEFER) && (sweet_abp_flags & ABP_BIT_UNPUB_ON_CLOSE)) {
		SweetABPPublish(false, "bridge closed, sweetabp & 0x20");
	}
	return 0;
}

/* 1063: wland says it can (1) or cannot (0) answer for the modem now. */
static int
ab_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	if (cmd != ABPB_IOC_READY) {
		return ENOTTY;
	}
	uint32_t v = *(uint32_t *)(void *)data;
	bool was;
	lck_mtx_lock(&abp_lock);
	was = ab.ready;
	ab.ready = v != 0;
	if (ab.ready && !was) {
		ab.readies++;
	}
	lck_mtx_unlock(&abp_lock);
	printf("SWEET ABP: wland says %s (was %s)\n", v ? "READY" : "not ready", was ? "ready" : "not ready");
	if (!(sweet_abp_flags & ABP_BIT_DEFER)) {
		return 0;               /* published at boot already: nothing to do */
	}
	if (v) {
		int e = SweetABPPublish(true, "wland ready");
		return e == EALREADY ? 0 : e;
	}
	if (sweet_abp_flags & ABP_BIT_UNPUB_ON_CLOSE) {
		int e = SweetABPPublish(false, "wland not ready, sweetabp & 0x20");
		return e == EALREADY ? 0 : e;
	}
	return 0;
}

static int
ab_read(__unused dev_t dev, struct uio *uio, int ioflag)
{
	int e = 0, moved = 0;
	uint8_t *tmp = NULL;
	lck_mtx_lock(&abp_lock);
	while (ab_used() == 0) {
		if (ioflag & IO_NDELAY) {
			lck_mtx_unlock(&abp_lock);
			return EWOULDBLOCK;
		}
		e = msleep(&ab, &abp_lock, PZERO | PCATCH, "abpbrd", NULL);
		if (e) {
			lck_mtx_unlock(&abp_lock);
			return e;
		}
	}
	while (ab_used() >= sizeof(struct abpb_hdr)) {
		struct abpb_hdr h;
		ab_peek(&h, ab.tail, sizeof(h));
		uint32_t rec = (uint32_t)sizeof(h) + h.len;
		if (rec > uio_resid(uio)) {
			if (moved == 0) {
				e = EMSGSIZE;
			}
			break;
		}
		if (tmp == NULL) {
			tmp = kalloc_data(sizeof(h) + ABPB_MAXFRAME, Z_WAITOK);
			if (tmp == NULL) {
				e = ENOMEM;
				break;
			}
		}
		ab_peek(tmp, ab.tail, rec);
		ab.tail += rec;
		ab.records--;
		lck_mtx_unlock(&abp_lock);
		e = uiomove((caddr_t)tmp, (int)rec, uio);
		lck_mtx_lock(&abp_lock);
		if (e) {
			break;
		}
		moved++;
	}
	lck_mtx_unlock(&abp_lock);
	if (tmp) {
		kfree_data(tmp, sizeof(struct abpb_hdr) + ABPB_MAXFRAME);
	}
	return e;
}

static int
ab_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	user_ssize_t n = uio_resid(uio);
	if (n <= 0) {
		return 0;
	}
	if (n > (user_ssize_t)(sizeof(struct abpb_hdr) + ABPB_MAXFRAME)) {
		return EMSGSIZE;
	}
	uint32_t total = (uint32_t)n;
	uint8_t *buf = kalloc_data(total, Z_WAITOK);
	if (buf == NULL) {
		return ENOMEM;
	}
	int e = uiomove((caddr_t)buf, (int)total, uio);
	uint32_t off = 0;
	while (e == 0 && off + sizeof(struct abpb_hdr) <= total) {
		struct abpb_hdr h;
		memcpy(&h, buf + off, sizeof(h));
		if (h.magic != ABPB_MAGIC || h.len > total - off - sizeof(h) || h.pipe >= ABP_NPIPES) {
			e = EINVAL;
			break;
		}
		if (h.type == 0) {
			int d = SweetABPDeliver(h.pipe, h.sess, buf + off + sizeof(h), h.len);
			if (d) {
				e = d;
				break;
			}
			lck_mtx_lock(&abp_lock);
			ab.from_wland += h.len;
			lck_mtx_unlock(&abp_lock);
		}
		off += (uint32_t)sizeof(h) + h.len;
	}
	if (e == 0 && off != total) {
		e = EINVAL;
	}
	kfree_data(buf, total);
	return e;
}

static int
ab_select(__unused dev_t dev, int which, void *wql, proc_t p)
{
	int ready = 0;
	lck_mtx_lock(&abp_lock);
	if (which == FREAD) {
		if (ab_used()) {
			ready = 1;
		} else {
			selrecord(p, &ab.sel, wql);
		}
	} else if (which == FWRITE) {
		ready = 1;
	}
	lck_mtx_unlock(&abp_lock);
	return ready;
}

static const struct cdevsw ab_cdevsw = {
	.d_open       = ab_open,
	.d_close      = ab_close,
	.d_read       = ab_read,
	.d_write      = ab_write,
	.d_ioctl      = ab_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = ab_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = 0,
};

static void
sweet_abp_apply(uint32_t bits)
{
	sweet_abp_flags |= bits;
	if (!(sweet_abp_flags & ABP_BIT_SERVICES) || ab.node) {
		return;
	}
	ab.node = true;
	int major = cdevsw_add(-1, &ab_cdevsw);
	void *n = major < 0 ? NULL : devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, "abpbridge");
	printf("SWEET ABP: /dev/abpbridge %s (major %d)\n", n ? "made" : "FAILED", major);
	SweetABPStart();
	if (sweet_abp_flags & ABP_BIT_DEFER) {
		printf("SWEET ABP: deferred publish (sweetabp & 8): the services appear when wland says ready\n");
	} else {
		SweetABPPublish(true, "boot");
	}
}

void
sweet_abp_init(void)
{
	uint32_t on = 0, fake = 0;
	PE_parse_boot_argn("sweetabp", &on, sizeof(on));
	PE_parse_boot_argn("sweetabmfake", &fake, sizeof(fake));
	PE_parse_boot_argn("sweetabpstage", &sweet_abp_bootstage, sizeof(sweet_abp_bootstage));
	if (fake) {
		on |= ABP_BIT_FAKEBOOT;
	}
	if (!on) {
		return;
	}
	printf("SWEET ABP: sweetabp=0x%x (services %s%s%s%s, fake boot %s), bootstage %u\n", on,
	    (on & ABP_BIT_SERVICES) ? "on" : "off", (on & ABP_BIT_DEFER) ? ", deferred to wland ready" : "",
	    (on & ABP_BIT_NOPDP) ? ", no PDPManagerBase" : "", (on & ABP_BIT_UNPUB_ON_CLOSE) ? ", un-publish on close" : "",
	    (on & ABP_BIT_FAKEBOOT) ? "on" : "off", sweet_abp_bootstage);
	sweet_abp_apply(on);
}

/* ---- fake boot: capabilities::bb::abm::shouldBoot() -> 0 in CommCenter ---- */

#define ABM_SHOULDBOOT_VA       0x1ae1df4a8ULL          /* unslid cache VA, iOS 16.7 20H392 iPhone10,3 */
#define ABM_PATCH_WORD          6                        /* 0x1ae1df4c0: mov w0, #0x1 */
#define ABM_INSN_RET1           0x52800020u
#define ABM_INSN_RET0           0x52800000u
static const uint32_t abm_expect[10] = {
	0xa9bf7bfd,     /* stp fp, lr, [sp, #-0x10]! */
	0x910003fd,     /* mov fp, sp */
	0xf019f5c8,     /* adrp x8, sOnce@page */
	0xf9421908,     /* ldr x8, [x8, #0x430] */
	0xb100051f,     /* cmn x8, #1 */
	0x54000081,     /* b.ne +0x10 (dispatch_once) */
	0x52800020,     /* mov w0, #1   <- becomes mov w0, #0 */
	0xa8c17bfd,     /* ldp fp, lr, [sp], #0x10 */
	0xd65f03c0,     /* ret */
	0xf019f5c0,     /* adrp x0, sOnce@page */
};

static int abm_state;                   /* 0 never tried, 1 patched, 2 already patched, < 0 the step that failed */
static int abm_pid;
static uint32_t abm_count;

static void
abm_words(const char *what, const uint32_t *w)
{
	printf("SWEET ABP: fake boot: %s %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n", what,
	    w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9]);
}

void
sweet_abp_dyld_hook(void *pv, uint64_t cache_start)
{
	proc_t p = (proc_t)pv;
	char name[32];
	if (!(sweet_abp_flags & ABP_BIT_FAKEBOOT) || cache_start == 0) {
		return;
	}
	proc_selfname(name, (int)sizeof(name));
	if (strcmp(name, "CommCenter") != 0) {
		return;
	}
	int pid = proc_selfpid();
	uint8_t hdr[0x20];
	uint64_t map0 = 0;
	uint32_t w[10], back = 0;
	int st = 0;
	kern_return_t kr = KERN_SUCCESS;
	abm_count++;
	if (copyin((user_addr_t)cache_start, hdr, sizeof(hdr)) || memcmp(hdr, "dyld_v1", 7) != 0) {
		printf("SWEET ABP: fake boot: pid %d: no dyld cache header at 0x%llx: refused\n", pid, cache_start);
		st = -1;
		goto out;
	}
	uint32_t moff;
	memcpy(&moff, hdr + 0x10, sizeof(moff));
	if (moff < 0x18 || moff > 0x1000 || copyin((user_addr_t)(cache_start + moff), &map0, sizeof(map0))) {
		printf("SWEET ABP: fake boot: pid %d: bad mapping offset 0x%x: refused\n", pid, moff);
		st = -2;
		goto out;
	}
	uint64_t slide = cache_start - map0;
	uint64_t fn = ABM_SHOULDBOOT_VA + slide;
	if (copyin((user_addr_t)fn, w, sizeof(w))) {
		printf("SWEET ABP: fake boot: pid %d: cannot read shouldBoot at 0x%llx (slide 0x%llx): refused\n", pid, fn, slide);
		st = -3;
		goto out;
	}
	bool same = true;
	for (int i = 0; i < 10; i++) {
		if (w[i] != abm_expect[i] && !(i == ABM_PATCH_WORD && w[i] == ABM_INSN_RET0)) {
			same = false;
		}
	}
	if (!same) {
		printf("SWEET ABP: fake boot: pid %d: shouldBoot at 0x%llx (cache 0x%llx, slide 0x%llx) differs: refused\n",
		    pid, fn, cache_start, slide);
		abm_words("found   ", w);
		abm_words("expected", abm_expect);
		st = -4;
		goto out;
	}
	if (w[ABM_PATCH_WORD] == ABM_INSN_RET0) {
		printf("SWEET ABP: fake boot: pid %d: shouldBoot at 0x%llx already returns 0\n", pid, fn);
		st = 2;
		goto out;
	}
	if (!cs_allow_invalid(p)) {
		printf("SWEET ABP: fake boot: pid %d: cs_allow_invalid refused: refused\n", pid);
		st = -5;
		goto out;
	}
	vm_map_t map = current_map();
	uint64_t pmask = (uint64_t)vm_map_page_mask(map);
	uint64_t pg = (fn + 4 * ABM_PATCH_WORD) & ~pmask;
	uint64_t pend = pg + pmask + 1;
	kr = vm_map_protect(map, pg, pend, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY, FALSE);
	if (kr != KERN_SUCCESS) {
		printf("SWEET ABP: fake boot: pid %d: vm_protect(COPY|rw) of 0x%llx failed (%d)\n", pid, pg, kr);
		st = -6;
		goto out;
	}
	uint32_t ret0 = ABM_INSN_RET0;
	int ce = copyout(&ret0, (user_addr_t)(fn + 4 * ABM_PATCH_WORD), sizeof(ret0));
	kr = vm_map_protect(map, pg, pend, VM_PROT_READ | VM_PROT_EXECUTE, FALSE);
	if (ce || kr != KERN_SUCCESS) {
		printf("SWEET ABP: fake boot: pid %d: write %d / vm_protect(r-x) %d failed\n", pid, ce, kr);
		st = -7;
		goto out;
	}
	vm_machine_attribute_val_t v = MATTR_VAL_CACHE_SYNC;
	kr = mach_vm_machine_attribute(map, fn + 4 * ABM_PATCH_WORD, 4, MATTR_CACHE, &v);
	if (copyin((user_addr_t)(fn + 4 * ABM_PATCH_WORD), &back, sizeof(back)) || back != ABM_INSN_RET0) {
		printf("SWEET ABP: fake boot: pid %d: read-back 0x%08x: patch not in place\n", pid, back);
		st = -8;
		goto out;
	}
	printf("SWEET ABP: fake boot: pid %d CommCenter: shouldBoot() at 0x%llx (cache 0x%llx, slide 0x%llx) now returns 0 "
	    "(word 0x%llx 0x%08x -> 0x%08x, private copy of page 0x%llx, cache sync %d, csflags 0x%llx)\n", pid, fn, cache_start,
	    slide, fn + 4 * ABM_PATCH_WORD, ABM_INSN_RET1, back, pg, kr, (unsigned long long)proc_getcsflags(p));
	st = 1;
out:
	abm_state = st;
	abm_pid = pid;
}

/* ---- sysctls ---- */

static int
sysctl_abp(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int v = (int)sweet_abp_flags, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);
	if (e == 0 && changed) {
		if (proc_suser(req->p) != 0) {
			return EPERM;
		}
		printf("SWEET ABP: kern.sweet_abp 0x%x -> adds 0x%x\n", sweet_abp_flags, (uint32_t)v);
		sweet_abp_apply((uint32_t)v & ABP_BITS_ALL);
	}
	return e;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_abp, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_abp, "I",
    "redfin 1061/1063: AppleBasebandPCI stand-in (1 services + /dev/abpbridge, 2 fake ABM boot, 4 trace, 8 publish on wland ready, "
    "0x10 no PDPManagerBase, 0x20 un-publish on bridge close); only adds bits");

static int
sysctl_abp_publish(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int v = SweetABPPublished(), changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);
	if (e == 0 && changed) {
		if (proc_suser(req->p) != 0) {
			return EPERM;
		}
		if (!ab.node) {
			return ENXIO;
		}
		e = SweetABPPublish(v != 0, v ? "kern.sweet_abp_publish=1" : "kern.sweet_abp_publish=0");
		if (e == EALREADY) {
			e = 0;
		}
	}
	return e;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_abp_publish, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_abp_publish, "I",
    "redfin 1063: 1 = baseband stand-ins published; write 1/0 to publish/un-publish now (debugging)");

static int
sysctl_abp_stage(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int v = (int)sweet_abp_bootstage, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);
	if (e == 0 && changed) {
		sweet_abp_bootstage = (uint32_t)v;
		SweetABPSetBootstage((uint32_t)v);
	}
	return e;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_abp_bootstage, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_abp_stage, "I",
    "redfin 1061: AppleBasebandPCIControl bootstage property (ABM: < 2 = down)");

static int
sysctl_abp_msg(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int v = 0, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);
	if (e == 0 && changed) {
		SweetABPMessage((uint32_t)v);
	}
	return e;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_abp_msg, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_abp_msg, "I",
    "redfin 1061: write N = general-interest message N to AppleBasebandPCIControl's clients (experiments)");

/* 1064 */
static int
sysctl_abp_readyms(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int v = (int)sweet_abp_readyms, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);
	if (e == 0 && changed) {
		sweet_abp_readyms = v < 0 ? 0 : (uint32_t)v;
		printf("SWEET ABP: readyms = %u\n", sweet_abp_readyms);
	}
	return e;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_abp_readyms, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_abp_readyms, "I",
    "redfin 1064: ms AppleBaseband reads as in reset after ABM starts, then 'alive' (0 = never in reset, the 1061 behaviour)");

static int
sysctl_abp_bbmsg(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int v = 0, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);
	if (e == 0 && changed) {
		uint32_t code = (uint32_t)v;
		if (v == 1) {
			code = 0xe3ff8000u;     /* reset detected */
		} else if (v == 2) {
			code = 0xe3ff8001u;     /* alive */
		}
		SweetABPBasebandMessage(code);
	}
	return e;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_abp_bbmsg, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_abp_bbmsg, "I",
    "redfin 1064: write 1 = 'reset detected' (0xe3ff8000), 2 = 'alive' (0xe3ff8001), other N = message N to AppleBaseband's clients");

static int
sysctl_abp_stats(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char *s = kalloc_data(2048, Z_WAITOK | Z_ZERO);
	if (s == NULL) {
		return ENOMEM;
	}
	int n = SweetABPStats(s, 2048);
	lck_mtx_lock(&abp_lock);
	snprintf(s + n, 2048 - (size_t)n, " | bridge %s %s (ready %u x) queued %u B/%u rec, dropped %u (%u B), to wland %llu B, from wland %llu B"
	    " | fakeboot state %d pid %d calls %u", ab.open ? "open" : "closed", ab.ready ? "READY" : "not ready", ab.readies, ab_used(), ab.records, ab.dropped,
	    ab.dropped_bytes, ab.to_wland, ab.from_wland, abm_state, abm_pid, abm_count);
	lck_mtx_unlock(&abp_lock);
	int e = SYSCTL_OUT(req, s, strlen(s) + 1);
	kfree_data(s, 2048);
	return e;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_abp_stats, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_abp_stats, "A",
    "redfin 1061: AppleBasebandPCI stand-in counters");
#endif /* ARM64_BOARD_CONFIG_SWEET */
