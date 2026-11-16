/*
 * SWEET PM6150L L11C bring-up; see report.md for DT and source evidence.
 * Sole XNU apps-DRV owner, called from thread context with deep idle disabled.
 * No sleep/wake programming, SPMI writes, GIC handler, or timeout recovery.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <sys/sysctl.h>  /* 1131 */

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
int sweet_rpmh_vreg_enable(const char *res, unsigned mv);

/* Live downstream DT has two positional reg entries, no reg-names. */
#define RH_RSC_PA       0x18220000u
#define RH_TCS_PA       0x18220d00u
#define RH_TCS_SIZE     0x3000u
#define RH_DICT_PA      0x0c3f000cu
#define RH_DB_LITO_PA   0x80860000u
#define RH_DB_LITO_SIZE 0x20000u
#define RH_DRV          2u
#define RH_ACTIVE_N     2u       /* DT order: ACTIVE 2, SLEEP 3, WAKE 3 */
#define RH_NORMAL_N     8u       /* hidden CONTROL TCS is not in this bank */
#define RH_TCS_STRIDE   0x2a0u
#define RH_CMD_STRIDE   0x14u
#define RH_IRQ_EN       0x00u
#define RH_IRQ_STATUS   0x04u
#define RH_IRQ_CLEAR    0x08u
#define RH_WAIT         0x10u
#define RH_CONTROL      0x14u
#define RH_STATUS       0x18u
#define RH_ENABLE       0x1cu
#define RH_MSGID        0x30u
#define RH_ADDR         0x34u
#define RH_DATA         0x38u
#define RH_CMD_STATUS   0x3cu
#define RH_AMC_EN       (1u << 16)
#define RH_TRIGGER      (1u << 24)
#define RH_ISSUED       (1u << 8)
#define RH_COMPLETE     (1u << 16)
#define RH_WRITE_ACK    (8u | (1u << 8) | (1u << 16))
#define RH_DB_MAGIC     0x0c0330dbu
#define RH_DB_HEADER    144u
#define RH_DB_ENTRY     24u
#define RH_DB_MAX       0x100000u /* policy bound, not a DT size */
#define RH_VRM          4u
#define RH_ARC          3u
#define RH_BCM          5u
#define RH_TIMEOUT_US   100000u

static vm_offset_t rh_rsc, rh_tcs, rh_dict, rh_db;
/*
 * 1131: the apps DRV's last completed ACTIVE value per RPMh address (resource + offset), for every vote this
 * driver sent since boot (PMEM_RPMH: wland cx/mss, venus BCMs and rails, adsp lcx/lmx, ...; touch rails).
 * Bookkeeping only: nothing about a vote changes. kern.sweet_rpmh_votes (RD string) = "ADDR=VAL ..." in hex,
 * oldest first. RPMh keeps one vote per DRV and resource (the last write wins), and the aggregated state cannot be
 * read from the apps side, so this is how venusctl (VENUSCTL_RAILS) learns the current apps vote of cx.lvl/mx.lvl
 * before it raises one and what to put back. Votes before XNU (ABL) are not in it.
 */
#define RH_LAST_N 32u
static struct {
	uint32_t addr, val;
} rh_last[RH_LAST_N];
static unsigned rh_last_n;

static void
rh_last_note(uint32_t addr, uint32_t val)      /* under rh_lock */
{
	unsigned i;
	for (i = 0; i < rh_last_n && rh_last[i].addr != addr; i++) {
	}
	if (i == rh_last_n && rh_last_n < RH_LAST_N) {
		rh_last_n++;
	}
	if (i == RH_LAST_N) {
		i = RH_LAST_N - 1;     /* full: drop the oldest */
		memmove(&rh_last[0], &rh_last[1], sizeof(rh_last[0]) * (RH_LAST_N - 1));
	} else {
		memmove(&rh_last[i], &rh_last[i + 1], sizeof(rh_last[0]) * (rh_last_n - 1 - i));
		i = rh_last_n - 1;     /* newest last */
	}
	rh_last[i].addr = addr;
	rh_last[i].val = val;
}

static int
sysctl_sweet_rpmh_votes SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char buf[RH_LAST_N * 24 + 1];
	size_t o = 0;
	buf[0] = 0;
	unsigned n = __atomic_load_n(&rh_last_n, __ATOMIC_ACQUIRE);
	for (unsigned i = 0; i < n && o < sizeof(buf); i++) {
		o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%s%x=%x", i ? " " : "", rh_last[i].addr, rh_last[i].val);
	}
	return SYSCTL_OUT(req, buf, strlen(buf) + 1);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_rpmh_votes, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
    sysctl_sweet_rpmh_votes, "A", "1131: last ACTIVE value per RPMh address sent by XNU (hex ADDR=VAL)");
static uint32_t rh_db_size;
static unsigned rh_ncpt;
static unsigned rh_lock;
static bool rh_poisoned;
/*
 * 1135: kern.sweet_rpmh_recover (default 0 = unchanged: after a timeout the TCS is quarantined and every later
 * vote fails EIO for the rest of the boot). Linux rpmh-rsc has no abort for an AMC in flight either: a request
 * whose completion never comes keeps its TCS in tcs_in_use (it is never rewritten or cleared); it is freed only by
 * the late tx-done IRQ, and other requests go on in the other ACTIVE TCS, a request for an address still in flight
 * gets -EBUSY (check_for_req_inflight). Same here, never touching an incomplete TCS:
 *   bit 0 (1): late completion. Before each vote/read entry, if the timed-out TCS now shows every command
 *              ISSUED|COMPLETE and the TCS idle (it was not complete at the timeout, so this is fresh), it is
 *              cleaned up exactly as a vote that completed in time (CONTROL, ENABLE, WAIT, own IRQ bit) and the
 *              quarantine is lifted; the vote is recorded (1131). Still in flight: stays quarantined.
 *   bit 1 (2): while exactly one TCS is quarantined after a clean timeout (all writes read back), later votes use
 *              the other ACTIVE TCS (the stuck one is never selected: ENABLE != 0; a vote for an address still
 *              enabled in it is refused EBUSY by the existing in-flight check). An uncertain write (EIO in the
 *              middle of programming) or a second timeout keeps everything refused, as before.
 */
static unsigned rh_recover;
static int rh_q_tcs = -1;               /* the TCS quarantined by a clean timeout, -1 none */
static bool rh_q_hard;                  /* a state bit 1 must not bypass (uncertain write, second timeout) */
static unsigned rh_q_n;
static uint32_t rh_q_addr, rh_q_off[4], rh_q_val[4], rh_q_irqbit;
SYSCTL_UINT(_kern, OID_AUTO, sweet_rpmh_recover, CTLFLAG_RW | CTLFLAG_LOCKED, &rh_recover, 0,
    "1135: RPMh TCS timeout handling: 0 quarantine for the boot | 1 reclaim on late completion | 2 use the other ACTIVE TCS");

static uint32_t
rh_read(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
rh_write(vm_offset_t base, uint32_t off, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static bool
rh_sync(uint32_t off, uint32_t value)
{
	rh_write(rh_tcs, off, value);
	for (unsigned i = 0; i < 1000; ++i) {
		if (rh_read(rh_tcs, off) == value) {
			return true;
		}
		delay(1);
	}
	printf("SWEET RPMH: readback timeout off=%#x want=%#x got=%#x\n",
	    off, value, rh_read(rh_tcs, off));
	return false;
}

/* DB is little endian; avoid packed structs and unaligned Device loads. */
static uint16_t
rh_db16(uint32_t off)
{
	uint32_t v = rh_read(rh_db, off & ~3u);
	return (uint16_t)(v >> (8u * (off & 3u)));
}

static bool
rh_range(uint32_t off, uint32_t size)
{
	return off <= rh_db_size && size <= rh_db_size - off;
}

static int
rh_map(void)
{
	if (!rh_rsc) {
		rh_rsc = ml_io_map(RH_RSC_PA, 0x100);
	}
	if (!rh_tcs) {
		rh_tcs = ml_io_map(RH_TCS_PA, RH_TCS_SIZE);
	}
	if (!rh_dict) {
		rh_dict = ml_io_map(RH_DICT_PA, 8);
	}
	if (!rh_rsc || !rh_tcs || !rh_dict) {
		printf("SWEET RPMH: RSC/locator mapping failed\n");
		return ENOMEM;
	}
	uint32_t id = rh_read(rh_rsc, 0);
	uint32_t solver = rh_read(rh_rsc, 4);
	uint32_t config = rh_read(rh_rsc, 0x0c);
	unsigned major = (id >> 16) & 0xffu;
	unsigned count = (config >> (6u * RH_DRV)) & 0x3fu;
	rh_ncpt = (config >> 27) & 0x1fu;
	printf("SWEET RPMH: rsc=%#x tcs=%#x DRV=%u id=%#x solver=%#x config=%#x TCS=%u ncpt=%u\n",
	    RH_RSC_PA, RH_TCS_PA, RH_DRV, id, solver, config, count, rh_ncpt);
	/* This client implements the pre-v3 layout only; do not guess v3 offsets. */
	if ((major != 1 && major != 2) || config == 0xffffffffu ||
	    solver == 0xffffffffu || count < RH_NORMAL_N ||
	    count > RH_TCS_SIZE / RH_TCS_STRIDE || rh_ncpt < 3 || rh_ncpt > 16) {
		printf("SWEET RPMH: unsupported RSC geometry/revision\n");
		return ENODEV;
	}
	if (!rh_db) {
		uint32_t pa = rh_read(rh_dict, 0);
		uint32_t size = rh_read(rh_dict, 4);
		printf("SWEET RPMH: cmd-db locator=%#x pa=%#x size=%#x\n", RH_DICT_PA, pa, size);
		/* Firmware supplies the reserved RAM address; never scan physical RAM. */
		if (pa < 0x80000000u || (pa & 3u) || (size & 3u) ||
		    size < RH_DB_HEADER || size > RH_DB_MAX ||
		    (uint64_t)pa + size > 0x100000000ULL) {
			/*
			 * redfin (lito): cmd-db is a fixed reserved-memory region
			 * (lito.dtsi qcom,cmd-db reg <0x80860000 0x20000>), not an AOP
			 * locator. Use it only if the locator is unusable; the magic and
			 * version check below still has to pass.
			 */
			printf("SWEET RPMH: invalid cmd-db locator; trying lito cmd-db %#x\n", RH_DB_LITO_PA);
			pa = RH_DB_LITO_PA;
			size = RH_DB_LITO_SIZE;
		}
		rh_db = ml_io_map(pa, size);
		if (!rh_db) {
			return ENOMEM;
		}
		rh_db_size = size;
	}
	uint32_t db_version = rh_read(rh_db, 0);
	uint32_t magic = rh_read(rh_db, 4);
	uint32_t flags = rh_read(rh_db, 140);
	printf("SWEET RPMH: cmd-db version=%#x magic=%#x checksum=%#x flags=%#x\n",
	    db_version, magic, rh_read(rh_db, 136), flags);
	/* Linux only checks magic; version 1 is an intentionally stricter policy. */
	if (magic != RH_DB_MAGIC || db_version != 1 || (flags & 1u)) {
		printf("SWEET RPMH: unsupported/standalone cmd-db\n");
		return EINVAL;
	}
	return 0;
}

static int
rh_lookup(const char *res, uint32_t *address)
{
	uint32_t key[2] = {0, 0};
	for (unsigned i = 0; i < 8 && res[i]; ++i) {
		key[i / 4] |= (uint32_t)(uint8_t)res[i] << (8u * (i % 4));
	}
	unsigned matches = 0;
	for (unsigned i = 0; i < 8; ++i) {
		uint32_t h = 8u + 16u * i;
		unsigned slave = rh_db16(h);
		if (!slave) {
			break;
		}
		uint32_t entries = RH_DB_HEADER + rh_db16(h + 2);
		uint32_t aux = RH_DB_HEADER + rh_db16(h + 4);
		unsigned count = rh_db16(h + 6);
		if (slave < 3 || slave > 5 || (entries & 3u) ||
		    !rh_range(entries, count * RH_DB_ENTRY) || !rh_range(aux, 0)) {
			printf("SWEET RPMH: malformed cmd-db slave header %u\n", i);
			return EINVAL;
		}
		for (unsigned j = 0; j < count; ++j) {
			uint32_t e = entries + RH_DB_ENTRY * j;
			if (rh_read(rh_db, e) != key[0] || rh_read(rh_db, e + 4) != key[1]) {
				continue;
			}
			uint32_t addr = rh_read(rh_db, e + 16);
			if ((slave != RH_VRM && slave != RH_ARC && slave != RH_BCM) || (addr >> 16) != slave || (addr & 3u) ||
			    !rh_range(aux + rh_db16(e + 22), rh_db16(e + 20))) {
				printf("SWEET RPMH: invalid VRM/ARC/BCM entry for %s\n", res);
				return EINVAL;
			}
			*address = addr;
			++matches;
			printf("SWEET RPMH: %s addr=%#x slave=%u aux-version=%#x priority=%#x:%#x\n",
			    res, addr, slave, rh_db16(h + 8),
			    rh_read(rh_db, e + 8), rh_read(rh_db, e + 12));
		}
	}
	if (matches != 1) {
		printf("SWEET RPMH: %s lookup matches=%u; refusing\n", res, matches);
		return ENODEV;
	}
	return 0;
}

static bool
rh_free(unsigned t)
{
	uint32_t b = t * RH_TCS_STRIDE;
	/* Idle alone is insufficient: a sleep/wake payload can be armed and idle. */
	return rh_read(rh_tcs, b + RH_STATUS) != 0 &&
	    rh_read(rh_tcs, b + RH_STATUS) != 0xffffffffu &&
	    rh_read(rh_tcs, b + RH_CONTROL) == 0 &&
	    rh_read(rh_tcs, b + RH_ENABLE) == 0 &&
	    rh_read(rh_tcs, b + RH_WAIT) == 0 &&
	    !(rh_read(rh_tcs, RH_IRQ_STATUS) & (1u << t));
}

static int
rh_send_cmds(uint32_t address, unsigned n, const uint32_t *offsets, const uint32_t *values)
{
	int chosen = -1;
	for (unsigned t = 0; t < RH_ACTIVE_N; ++t) {
		uint32_t b = t * RH_TCS_STRIDE;
		uint32_t enabled = rh_read(rh_tcs, b + RH_ENABLE);
		printf("SWEET RPMH: ACTIVE TCS%u idle=%#x control=%#x enable=%#x wait=%#x\n",
		    t, rh_read(rh_tcs, b + RH_STATUS), rh_read(rh_tcs, b + RH_CONTROL),
		    enabled, rh_read(rh_tcs, b + RH_WAIT));
		if (enabled & ~((1u << rh_ncpt) - 1u)) {
			return EIO;
		}
		for (unsigned c = 0; c < rh_ncpt; ++c) {
			if ((enabled & (1u << c)) &&
			    (rh_read(rh_tcs, b + RH_ADDR + c * RH_CMD_STRIDE) & ~15u) == address) {
				printf("SWEET RPMH: conflicting inherited L11C request in TCS%u\n", t);
				return EBUSY;
			}
		}
		if (chosen < 0 && rh_free(t)) {
			chosen = (int)t;
		}
	}
	if (chosen < 0) {
		printf("SWEET RPMH: no empty idle ACTIVE TCS; leaving inherited state intact\n");
		return EBUSY;
	}
	unsigned t = (unsigned)chosen;
	uint32_t b = t * RH_TCS_STRIDE, bit = 1u << t;
	uint32_t irq_en = rh_read(rh_tcs, RH_IRQ_EN);
	if (irq_en == 0xffffffffu || !rh_free(t)) {
		return EBUSY;
	}
	/* Only this TCS's source is masked. Other sources/CPU IRQs remain intact. */
	if (!rh_sync(RH_IRQ_EN, irq_en & ~bit)) {
		rh_poisoned = true;
		return EIO;
	}
	if (!rh_free(t)) {
		/* No payload was touched. Do not acknowledge somebody else's IRQ. */
		if (!rh_sync(RH_IRQ_EN, (rh_read(rh_tcs, RH_IRQ_EN) & ~bit) | (irq_en & bit))) {
			rh_poisoned = true;
		}
		return EBUSY;
	}
	/* From here an uncertain write/timeout quarantines our slot for this boot. */
	rh_poisoned = true;
	bool prev_hard = rh_q_hard;             /* 1135: an exit below other than success/clean timeout stays hard */
	rh_q_hard = true;
	uint32_t mask = (1u << n) - 1u;
	printf("SWEET RPMH: TCS%u ACTIVE %u commands at %#x (first %#x=%u) irq-enable=%#x\n", t, n, address,
	    offsets[0], values[0], irq_en);
	for (unsigned c = 0; c < n; ++c) {
		uint32_t cmd = b + c * RH_CMD_STRIDE;
		if (!rh_sync(cmd + RH_MSGID, RH_WRITE_ACK) ||
		    !rh_sync(cmd + RH_ADDR, address + offsets[c]) ||
		    !rh_sync(cmd + RH_DATA, values[c])) {
			return EIO;
		}
	}
	if (!rh_sync(b + RH_WAIT, mask) || !rh_sync(b + RH_ENABLE, mask) ||
	    !rh_sync(b + RH_CONTROL, 0) || !rh_sync(b + RH_CONTROL, RH_AMC_EN)) {
		return EIO;
	}
	rh_write(rh_tcs, b + RH_CONTROL, RH_AMC_EN | RH_TRIGGER);
	uint32_t irq = 0, idle = 0, status[4] = {0, 0, 0, 0};
	bool done = false, progress = false;
	for (unsigned us = 0; us < RH_TIMEOUT_US; ++us) {
		irq = rh_read(rh_tcs, RH_IRQ_STATUS);
		idle = rh_read(rh_tcs, b + RH_STATUS);
		bool complete = true;
		for (unsigned c = 0; c < n; ++c) {
			status[c] = rh_read(rh_tcs, b + RH_CMD_STATUS + c * RH_CMD_STRIDE);
			complete &= status[c] != 0xffffffffu &&
			    (status[c] & (RH_ISSUED | RH_COMPLETE)) == (RH_ISSUED | RH_COMPLETE);
		}
		/* A fresh IRQ or observed execution transition excludes stale CMD_STATUS. */
		progress |= idle == 0 || !complete;
		if (idle && idle != 0xffffffffu && irq != 0xffffffffu &&
		    ((irq & bit) || progress) && complete) {
			done = true;
			break;
		}
		delay(1);
	}
	printf("SWEET RPMH: TCS%u done=%d irq=%#x idle=%#x control=%#x cmd-status=%#x/%#x/%#x\n",
	    t, done, irq, idle, rh_read(rh_tcs, b + RH_CONTROL), status[0], status[1], status[2]);
	if (!done) {
		printf("SWEET RPMH: timeout; TCS quarantined, IRQ remains masked; no reset/retry\n");
		if (rh_q_tcs < 0 && n <= 4 && !prev_hard) {   /* 1135: a clean timeout, the first one */
			rh_q_tcs = (int)t;
			rh_q_n = n;
			rh_q_addr = address;
			rh_q_irqbit = irq_en & bit;
			for (unsigned c = 0; c < n; ++c) {
				rh_q_off[c] = offsets[c];
				rh_q_val[c] = values[c];
			}
			rh_q_hard = false;
			printf("SWEET RPMH: 1135: TCS%u in flight at %#x; kern.sweet_rpmh_recover=%u (1 reclaim on late "
			    "completion, 2 use the other ACTIVE TCS)\n", t, address, rh_recover);
		}
		return ETIMEDOUT;
	}
	/* Proven completion: clear trigger before mode, then free only our commands. */
	if (!rh_sync(b + RH_CONTROL, RH_AMC_EN) || !rh_sync(b + RH_CONTROL, 0) ||
	    !rh_sync(b + RH_ENABLE, 0) || !rh_sync(b + RH_WAIT, 0)) {
		return EIO;
	}
	rh_write(rh_tcs, RH_IRQ_CLEAR, bit); /* W1C: never clear other TCS bits */
	if ((rh_read(rh_tcs, RH_IRQ_STATUS) & bit) ||
	    !rh_sync(RH_IRQ_EN, (rh_read(rh_tcs, RH_IRQ_EN) & ~bit) | (irq_en & bit))) {
		return EIO;
	}
	rh_q_hard = prev_hard;                  /* 1135 */
	rh_poisoned = rh_q_hard || rh_q_tcs >= 0;
	for (unsigned c = 0; c < n; ++c) {
		rh_last_note(address + offsets[c], values[c]);   /* 1131 */
	}
	printf("SWEET RPMH: ACTIVE vote complete (rail voltage requires hardware verification)\n");
	return 0;
}

static int
rh_send(uint32_t address, unsigned mv)
{
	/* HPM (raw 7) is supported by this PMIC5 LDO; enable comes last. */
	const uint32_t offsets[3] = {0, 8, 4};
	const uint32_t values[3] = {mv, 7, 1};
	return rh_send_cmds(address, 3, offsets, values);
}

/* 1135: lift the quarantine of a timed-out TCS whose commands completed late (bit 0). Under rh_lock. */
static void
rh_reclaim(void)
{
	if (!(rh_recover & 1) || rh_q_tcs < 0 || !rh_tcs) {
		return;
	}
	unsigned t = (unsigned)rh_q_tcs;
	uint32_t b = t * RH_TCS_STRIDE, bit = 1u << t;
	uint32_t idle = rh_read(rh_tcs, b + RH_STATUS), irq = rh_read(rh_tcs, RH_IRQ_STATUS);
	bool complete = idle != 0 && idle != 0xffffffffu && irq != 0xffffffffu;
	for (unsigned c = 0; c < rh_q_n && complete; ++c) {
		uint32_t st = rh_read(rh_tcs, b + RH_CMD_STATUS + c * RH_CMD_STRIDE);
		complete = st != 0xffffffffu && (st & (RH_ISSUED | RH_COMPLETE)) == (RH_ISSUED | RH_COMPLETE) &&
		    (rh_read(rh_tcs, b + c * RH_CMD_STRIDE + RH_ADDR) & ~3u) == ((rh_q_addr + rh_q_off[c]) & ~3u);
	}
	if (!complete) {
		return;
	}
	if (!rh_sync(b + RH_CONTROL, RH_AMC_EN) || !rh_sync(b + RH_CONTROL, 0) ||
	    !rh_sync(b + RH_ENABLE, 0) || !rh_sync(b + RH_WAIT, 0)) {
		rh_q_hard = true;
		printf("SWEET RPMH: 1135: TCS%u late completion but cleanup did not read back; stays quarantined\n", t);
		return;
	}
	rh_write(rh_tcs, RH_IRQ_CLEAR, bit);
	if ((rh_read(rh_tcs, RH_IRQ_STATUS) & bit) ||
	    !rh_sync(RH_IRQ_EN, (rh_read(rh_tcs, RH_IRQ_EN) & ~bit) | rh_q_irqbit)) {
		rh_q_hard = true;
		printf("SWEET RPMH: 1135: TCS%u late completion but IRQ cleanup failed; stays quarantined\n", t);
		return;
	}
	for (unsigned c = 0; c < rh_q_n; ++c) {
		rh_last_note(rh_q_addr + rh_q_off[c], rh_q_val[c]);
	}
	rh_q_tcs = -1;
	rh_poisoned = rh_q_hard;
	printf("SWEET RPMH: 1135: TCS%u completed late (irq=%#x idle=%#x); cleaned up, quarantine lifted%s\n", t, irq,
	    idle, rh_poisoned ? " (an earlier uncertain write still blocks)" : "");
}

/* 1135: is every vote refused? (rh_poisoned, unless bit 1 and only one clean-timeout TCS is stuck). Under rh_lock. */
static bool
rh_blocked(void)
{
	rh_reclaim();
	return rh_poisoned && !((rh_recover & 2) && rh_q_tcs >= 0 && !rh_q_hard);
}

/*
 * Any VRM, ARC or BCM resource, 1-4 commands (offset from the resource address,
 * value): for /dev/pmem's PMEM_RPMH (userland bring-up of the modem and
 * WLAN: ARC levels of cx/mss, WLAN rails). No policy: a debugging path.
 */
int sweet_rpmh_write(const char *res, unsigned n, const uint32_t *offsets, const uint32_t *values);

int
sweet_rpmh_write(const char *res, unsigned n, const uint32_t *offsets, const uint32_t *values)
{
	if (!res || n == 0 || n > 4) {
		return EINVAL;
	}
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) {
		return EBUSY;
	}
	int error = EIO;
	uint32_t address = 0;
	if (!rh_blocked()) {                    /* 1135 (was !rh_poisoned) */
		error = rh_map();
		if (!error) {
			error = rh_lookup(res, &address);
		}
		if (!error) {
			error = rh_send_cmds(address, n, offsets, values);
		}
	}
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}

int
sweet_rpmh_vreg_enable(const char *res, unsigned mv)
{
	/* Board-specific helper: do not apply PMIC5-LDO policy to arbitrary resources. */
	/* Redmi touch L11C; flame touch L14A (DVDD) and L17A (AVDD), DT ranges;
	 * redfin touch AVDD L17A fixed at 3304 mV (lito-redfin-touch.dtsi). */
	bool ok = res && ((!strcmp(res, "ldoc11") && mv >= 2950 && mv <= 3400) ||
	    (!strcmp(res, "ldoa14") && mv >= 1800 && mv <= 1840) ||
	    (!strcmp(res, "ldoa17") && ((mv >= 2856 && mv <= 3008) || mv == 3304)));
	if (!ok) {
		printf("SWEET RPMH: %s %u mV is not an allowed touch rail\n", res ? res : "?", mv);
		return EINVAL;
	}
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) {
		return EBUSY;
	}
	int error = EIO;
	uint32_t address = 0;
	if (rh_blocked()) {                     /* 1135 (was rh_poisoned) */
		printf("SWEET RPMH: previous uncertain transaction; refusing further access\n");
		goto out;
	}
	error = rh_map();
	if (!error) {
		error = rh_lookup(res, &address);
	}
	if (!error) {
		error = rh_send(address, mv);
	}
out:
	if (error) {
		printf("SWEET RPMH: enable failed errno=%d\n", error);
	}
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}
/*
 * redfin 1077: read-only views for the UFS diagnostics (sweet_ufs.c, kern.sweet_ufs_diag). Nothing here writes a TCS
 * register or sends a vote. The first call maps and checks the RSC and cmd-db as a vote would (rh_map, under the
 * lock, without waiting: EBUSY if a vote is in flight); after that the reads take no lock, so a vote is never refused
 * because of them. The TCS command registers hold what this DRV last programmed into each TCS (lito DT order: ACTIVE
 * 0-1, SLEEP 2-4, WAKE 5-7); the aggregated RPMh state of a resource cannot be read from the APPS side.
 */
static bool rh_ro_ok;

static int
rh_ro_ready(void)
{
	if (rh_ro_ok) {
		return 0;
	}
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) {
		return EBUSY;
	}
	int error = rh_blocked() ? EIO : rh_map();      /* 1135 */
	if (!error) {
		rh_ro_ok = true;
	}
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}

int sweet_rpmh_tcs_peek(unsigned t, uint32_t *enable, uint32_t *addr, uint32_t *data, unsigned max, unsigned *ncmd);
int
sweet_rpmh_tcs_peek(unsigned t, uint32_t *enable, uint32_t *addr, uint32_t *data, unsigned max, unsigned *ncmd)
{
	int error = rh_ro_ready();
	if (error) {
		return error;
	}
	if (t >= RH_NORMAL_N) {
		return EINVAL;
	}
	uint32_t b = t * RH_TCS_STRIDE;
	unsigned n = MIN(rh_ncpt, max);
	*enable = rh_read(rh_tcs, b + RH_ENABLE);
	for (unsigned i = 0; i < n; i++) {
		addr[i] = rh_read(rh_tcs, b + RH_ADDR + i * RH_CMD_STRIDE);
		data[i] = rh_read(rh_tcs, b + RH_DATA + i * RH_CMD_STRIDE);
	}
	*ncmd = n;
	return 0;
}

/* cmd-db walk: exact (name -> address) or nearest (address -> "name+offset", offset <= 0xc within the same slave) */
static int
rh_ro_walk(const char *want, uint32_t address, uint32_t *found, char *out, unsigned len)
{
	uint32_t best = 0, best_e = 0;
	for (unsigned i = 0; i < 8; ++i) {
		uint32_t h = 8u + 16u * i;
		unsigned slave = rh_db16(h);
		if (!slave) {
			break;
		}
		uint32_t entries = RH_DB_HEADER + rh_db16(h + 2);
		unsigned count = rh_db16(h + 6);
		if ((entries & 3u) || !rh_range(entries, count * RH_DB_ENTRY)) {
			return EINVAL;
		}
		for (unsigned j = 0; j < count; ++j) {
			uint32_t e = entries + RH_DB_ENTRY * j;
			uint32_t a = rh_read(rh_db, e + 16);
			if (want) {
				char id[9];
				uint32_t k0 = rh_read(rh_db, e), k1 = rh_read(rh_db, e + 4);
				for (unsigned c = 0; c < 8; c++) {
					id[c] = (char)((c < 4 ? k0 : k1) >> (8u * (c % 4)));
				}
				id[8] = 0;
				if (!strncmp(id, want, 8)) {
					*found = a;
					return 0;
				}
			} else if (a <= address && address - a <= 0xcu && (a >> 16) == (address >> 16) && (!best_e || a > best)) {
				best = a;
				best_e = e;
			}
		}
	}
	if (want || !best_e) {
		return ENODEV;
	}
	char id[9];
	uint32_t k0 = rh_read(rh_db, best_e), k1 = rh_read(rh_db, best_e + 4);
	for (unsigned c = 0; c < 8; c++) {
		id[c] = (char)((c < 4 ? k0 : k1) >> (8u * (c % 4)));
	}
	id[8] = 0;
	if (address == best) {
		snprintf(out, len, "%s", id);
	} else {
		snprintf(out, len, "%s+%x", id, address - best);
	}
	return 0;
}

int sweet_rpmh_name(uint32_t address, char *out, unsigned len);
int
sweet_rpmh_name(uint32_t address, char *out, unsigned len)
{
	int error = rh_ro_ready();
	return error ? error : rh_ro_walk(NULL, address, NULL, out, len);
}

int sweet_rpmh_find(const char *res, uint32_t *address);
int
sweet_rpmh_find(const char *res, uint32_t *address)
{
	int error = rh_ro_ready();
	return error ? error : rh_ro_walk(res, 0, address, NULL, 0);
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
