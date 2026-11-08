/*
 * ipagsi.c - the AP side of the IPA v4.7 / GSI v4.5 data path on redfin (Pixel 5), driven from
 * wland.c's net_idle() over /dev/pmem.  See ipagsi.h for the interface, and README.ipagsi.md (the
 * file next to ipagsi.diff, one level up from this directory) for how to run it and for what is
 * verified and what is still open.
 *
 * All GSI register offsets, fields and commands below are taken from mainline Linux
 * drivers/net/ipa/reg/gsi_reg-v4.5.c and drivers/net/ipa/gsi.c (5.15 and 6.7 both checked); they
 * are *not* from the DO-NOT-BOOT drafts.  GSI_EE_AP is 0 in those files, so the per-EE term
 * (0x4000 * GSI_EE_AP) drops out and the offsets below are the EE0 offsets we use.
 *
 * IPAGSI_PHASE picks how far we go:
 *   1  GSI only: check GSI, allocate/program/start our event rings and channels, ring the RX
 *      doorbell, open and configure the utun, dump the IPA register windows we intend to use
 *      later.  No IPA register is written, no immediate command is sent, no table is touched.
 *      A packet cannot flow yet (the IPA still points endpoints 2/16 at the modem's channels),
 *      but nothing here can hurt the modem's setup: the only writes are to EE0 rings and
 *      channels that belong to us.
 *   2  also read and check the IPA endpoint blocks (2, 16 and, with tables=1, 7) against what the
 *      modem programmed, and warn about anything that would stop data flowing.  The IPAGSI_SET_*
 *      knobs turn individual endpoint registers into writes once their values are known good.
 *      The channel 5 table/system init immediate commands (spec step 5) are still not written:
 *      ipagsi_tables() says so and uses the modem's tables as they are.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>

#include "pmem.h"
#include "link.h"
#include "ipagsi.h"
#include "ipnet.h"
#include "cell.h"

#ifndef IPAGSI_PHASE
#define IPAGSI_PHASE	1	/* 1 = GSI only + IPA dump, 2 = also program the IPA endpoints.
					 * -DIPAGSI_PHASE=2 on the ios-cc line switches without editing. */
#endif

#define LOG(fmt, ...)	printf("[%6llu] ipagsi: " fmt "\n", (unsigned long long) link_ms(), ##__VA_ARGS__)

#define IPA_BASE	0x1e40000
#define GSI_BASE	0x1e04000	/* EE0: the app processor's own GSI EE */

/* ------------------------------------------------------------------ GSI registers (v4.5, EE0) */

#define GSI_HW_PARAM_2		(GSI_BASE + 0x12040)	/* IRAM 2:0, NUM_CH_PER_EE 7:3, NUM_EV_PER_EE 12:8 */
#define GSI_STATUS		(GSI_BASE + 0x12000)	/* bit 0 ENABLED */
#define GSI_CH_CMD		(GSI_BASE + 0x12008)	/* ch id 7:0, opcode 31:24 */
#define GSI_EV_CH_CMD		(GSI_BASE + 0x12010)	/* ev id 7:0, opcode 31:24 */
#define GSI_GENERIC_CMD		(GSI_BASE + 0x12018)	/* opcode 4:0, chid 9:5, ee 13:10 */

#define GSI_CH_C_CNTXT_0(ch)	(GSI_BASE + 0x0f000 + (ch) * 0x80)
#define GSI_CH_C_CNTXT_1(ch)	(GSI_BASE + 0x0f004 + (ch) * 0x80)
#define GSI_CH_C_CNTXT_2(ch)	(GSI_BASE + 0x0f008 + (ch) * 0x80)
#define GSI_CH_C_CNTXT_3(ch)	(GSI_BASE + 0x0f00c + (ch) * 0x80)
#define GSI_CH_C_QOS(ch)	(GSI_BASE + 0x0f05c + (ch) * 0x80)
#define GSI_CH_C_SCRATCH_0(ch)	(GSI_BASE + 0x0f060 + (ch) * 0x80)
#define GSI_CH_C_SCRATCH_1(ch)	(GSI_BASE + 0x0f064 + (ch) * 0x80)
#define GSI_CH_C_SCRATCH_2(ch)	(GSI_BASE + 0x0f068 + (ch) * 0x80)
#define GSI_CH_C_SCRATCH_3(ch)	(GSI_BASE + 0x0f06c + (ch) * 0x80)
#define GSI_CH_DB(ch)		(GSI_BASE + 0x11000 + (ch) * 8)

#define GSI_EV_CH_E_CNTXT_0(ev)	(GSI_BASE + 0x10000 + (ev) * 0x80)
#define GSI_EV_CH_E_CNTXT_1(ev)	(GSI_BASE + 0x10004 + (ev) * 0x80)
#define GSI_EV_CH_E_CNTXT_2(ev)	(GSI_BASE + 0x10008 + (ev) * 0x80)
#define GSI_EV_CH_E_CNTXT_3(ev)	(GSI_BASE + 0x1000c + (ev) * 0x80)
#define GSI_EV_CH_E_CNTXT_4(ev)	(GSI_BASE + 0x10010 + (ev) * 0x80)
#define GSI_EV_CH_E_CNTXT_8(ev)	(GSI_BASE + 0x10020 + (ev) * 0x80)
#define GSI_EV_CH_E_CNTXT_9(ev)	(GSI_BASE + 0x10024 + (ev) * 0x80)
#define GSI_EV_CH_E_CNTXT_10(ev) (GSI_BASE + 0x10028 + (ev) * 0x80)
#define GSI_EV_CH_E_CNTXT_11(ev) (GSI_BASE + 0x1002c + (ev) * 0x80)
#define GSI_EV_CH_E_CNTXT_12(ev) (GSI_BASE + 0x10030 + (ev) * 0x80)
#define GSI_EV_CH_E_CNTXT_13(ev) (GSI_BASE + 0x10034 + (ev) * 0x80)
#define GSI_EV_DB(ev)		(GSI_BASE + 0x11100 + (ev) * 8)

/* CH_C_CNTXT_0 fields */
#define CH_CHTYPE_MASK		0x7u		/* CHTYPE_PROTOCOL 2:0 (+ bit 13 MSB, 0 for values < 8) */
#define CH_CHTYPE_GPI		2u		/* GSI_CHANNEL_TYPE_GPI = 0x2 (gsi_reg.h; 0 MHI, 1 XHCI, 3 XDCI) */
#define CH_DIR			(1u << 3)	/* set for a channel going toward the IPA */
#define CH_EE_SHIFT		4
#define CH_ID_SHIFT		8
#define CH_ERINDEX_SHIFT	14		/* GENMASK(18,14): event ring we report to (v4.5) */
#define CH_STATE_SHIFT		20		/* GENMASK(23,20), read only for us */
#define CH_ELEM_SHIFT		24		/* GENMASK(31,24): element size, 16 */
#define CH_LEN_MASK		0xffffu		/* CH_C_CNTXT_1 R_LENGTH, bytes */

/* CH_C_QOS fields: WRR_WEIGHT 3:0, MAX_PREFETCH bit 8, USE_DB_ENG bit 9, PREFETCH_MODE 13:10,
 * EMPTY_LVL_THRSHOLD 23:16.  GSI_ESCAPE_BUF_ONLY = 1 in PREFETCH_MODE (used for v4.5+);
 * WRR_WEIGHT's field maximum (reg_field_max of GENMASK(3,0)) is what mainline gives the command
 * channel, where the lowest weight wins. */
#define CH_QOS_WRR_SHIFT	0
#define CH_QOS_WRR_MAX		7u
#define CH_QOS_PREFETCH_SHIFT	10
#define CH_QOS_ESCAPE_ONLY	1u

/* CH_C_SCRATCH_2/3, GPI protocol (struct gsi_channel_scratch_gpi): words 1 and 2 are reserved
 * (0), word 3 = max_outstanding_tre in 31:16, word 4 = outstanding_threshold in 31:16 with the
 * low 16 bits kept as the hardware has them. */
#define CH_SCR_MAXTRE_SHIFT	16
#define CH_SCR_THRESH_SHIFT	16

/* CH_C_CNTXT_0 CHSTATE (GSI_CHANNEL_STATE_*), and EV_CH_E_CNTXT_0 23:20 (GSI_EVT_RING_STATE_*) */
enum { CH_ST_NOT_ALLOC = 0, CH_ST_ALLOCATED = 1, CH_ST_STARTED = 2, CH_ST_STOPPED = 3,
       CH_ST_STOP_IN_PROC = 4, CH_ST_FLOW_CTRL = 5, CH_ST_ERROR = 0xf };	/* mainline gsi.h */
enum { EV_ST_NOT_ALLOC = 0, EV_ST_ALLOCATED = 1, EV_ST_ERROR = 0xf };	/* mainline gsi.h */

/* CH_CMD / EV_CH_CMD opcodes (reg value = opcode << 24 | id) */
enum { CH_CMD_ALLOCATE = 0x0, CH_CMD_START = 0x1, CH_CMD_STOP = 0x2, CH_CMD_RESET = 0x9,
       CH_CMD_DE_ALLOC = 0xa, CH_CMD_DB_STOP = 0xb };
enum { EV_CMD_ALLOCATE = 0x0, EV_CMD_RESET = 0x9, EV_CMD_DE_ALLOC = 0xa };

/* EV_CH_E_CNTXT_0 fields: EV_CHTYPE 3:0 (GPI = 2), EV_EE 7:4 (0), EV_EVCHID 15:8 (0),
 * EV_INTYPE bit 16 (set), EV_CHSTATE 23:20, EV_ELEMENT_SIZE 31:24 */
#define EV_CHTYPE_GPI		2u		/* same enum: GSI_CHANNEL_TYPE_GPI = 0x2 */
#define EV_INTYPE		(1u << 16)
#define EV_ELEM_SHIFT		24
#define EV_STATE_SHIFT		20
/* EV_CH_E_CNTXT_8: EV_MODT 15:0, EV_MODC 23:16 (1 = moderation comes from the channel),
 * EV_MOD_CNT 31:24 (an event count, 0 = not used).  mainline's GSI_EVT_RING_INT_MODT is 32 "1 ms
 * under a 32 KHz clock"; we poll the rings, so moderation only decides when the hardware writes,
 * and we use the same value. */
#define EV_MODT			32u
#define EV_MODC_SHIFT		16

/* event ring element (16 bytes), gsi.c struct gsi_event */
struct gsi_evt {
	uint64_t xfer_ptr;	/* bus address of the TRE buffer (or the completion's data) */
	uint16_t len;
	uint8_t resv0;
	uint8_t code;		/* CBCM codes: DATA 0x01, CMD 0x02, RELEASE 0x03, THROWAWAY 0x08 */
	uint16_t resv1;
	uint8_t type;		/* 0x00 CBCM, 0x0e FLR, 0x0f ERR */
	uint8_t chid;
} __attribute__((packed));
#define EVT_CBCM	0x00
#define EVT_XFER	0x22	/* transfer completion (Qualcomm GSI event type); code 0x02 = EOT, 0x04 = EOB, 0x01 = success */
#define EVT_FLR		0x0e
#define EVT_ERR		0x0f
#define EVT_CODE_DATA	0x01
#define EVT_CODE_CMD	0x02
#define EVT_CODE_REL	0x03
#define EVT_CODE_THROW	0x08

/* TRE (16 bytes), gsi_trans.c struct gsi_tre: the length is 16 bits, followed by a reserved
 * 16 bits (the drafts' layout was right about that).  A 32-bit length field would give the same
 * bytes for len < 64 KB, but mainline's layout is what we write. */
struct gsi_tre {
	uint64_t addr;		/* bus address (physical for us: the SMMU stream is off) */
	uint16_t len;		/* bytes, or the IPA_CMD_* opcode for an immediate command */
	uint16_t resv;
	uint32_t flags;		/* TRE_FLAGS_*: CHAIN bit 0, IEOT bit 9, BEI bit 10, type 23:16 */
} __attribute__((packed));
#define TRE_CHAIN	(1u << 0)
#define TRE_IEOT	(1u << 9)
#define TRE_BEI		(1u << 10)
/* flags 23:16 is the entry type (enum gsi_tre_type), not a per-endpoint TLV number: a plain
 * transfer is GSI_RE_XFER = 2 (gsi_trans.c: GSI_RE_XFER = 0x2; an earlier version of this file said 0 and
 * so posted type-0 TREs the GSI never consumed), an immediate command is GSI_RE_IMMD_CMD = 3 */
#define TRE_XFER	(2u << 16)
#define TRE_IMMD_CMD	(3u << 16)

#define RING_ELEM	16

/* our channels and event rings (per-EE ids on EE0), from the spec's endpoint table */
#define CH_CMDTX	5	/* AP_COMMAND_TX, endpoint 7 */
#define CH_MDM_TX	2	/* AP_MODEM_TX, endpoint 2 */
#define CH_MDM_RX	7	/* AP_MODEM_RX, endpoint 16 */
#define CH_ALT_TX	3	/* gsistart=ids34 only: stand-ins for ch 2 / 7 */
#define CH_ALT_RX	4
#define EV_CMD		0
#define EV_TX		2
#define EV_RX		3

#define EP_CMDTX	7
#define EP_MDM_TX	2
#define EP_MDM_RX	16
#define EP_LAN_RX	9	/* IPA_ENDPOINT_AP_LAN_RX, channel 14: ep7's dma destination */
/* TLV FIFO sizes from ipa_data-v4.7.c (tlv_count for channels 5, 2 and 7).  This is *not* a
 * value written into a TRE: it is the number of TREs one transaction (one commit + one
 * doorbell) may hold, which is what sets max_outstanding_tre below.  The endpoint a transfer
 * belongs to comes from the channel -> endpoint binding, not from the TRE. */
#define TLV_CMDTX	20
#define TLV_MDM_TX	16
static uint32_t tlv_tx = TLV_MDM_TX, tlv_rx = 9;	/* gsistart=tlvmin overrides */
#define TLV_MDM_RX	9

#define RX_BUFS		32	/* pre-posted RX buffers */
#define RX_BUF_SZ	8192	/* IPA v4.7 endpoint 16 aggregation byte limit */
#define TX_CAP		2048
#define CMD_TRE_CNT	256
#define TX_TRE_CNT	512
#define RX_TRE_CNT	256
#define EV_CNT		64	/* event ring elements (each event ends one transaction) */

struct gsi_ring {			/* one ring: our physical memory plus software pointers */
	uint64_t pa;
	uint32_t cnt;
	uint32_t wp;			/* TREs written (ch), or unused (ev) */
	uint32_t rp;			/* consumed */
};

static struct {
	int active;
	const char *state;
	struct ipagsi_call call;
	int tables;

	struct gsi_ring ev[8];		/* event rings, by id (0, 2, 3; 4-7 for the ev4hi probe) */
	struct gsi_ring ch[8];		/* channels, by id (2, 5, 7) */

	uint64_t rx_buf_pa[RX_BUFS];
	uint32_t rx_wp;		/* RX TREs written (total) */
	uint32_t rx_done;	/* RX completions taken (total) */

	int utun_fd;
	char utun_name[32];

	uint64_t stat_ms;
	struct qmap_stats rx_st, tx_st;
	unsigned rx_pkts, tx_pkts, rx_err, tx_err, tx_other;
} ctx = { .state = "off", .utun_fd = -1 };

static const struct qmap_stats qmap_zero = { 0, 0, 0 };

/* ------------------------------------------------------------------ register helpers */

static unsigned ch_state(int ch)
{
	return rd32(GSI_CH_C_CNTXT_0(ch)) >> CH_STATE_SHIFT & 0xf;
}

static unsigned ev_state(int ev)
{
	return rd32(GSI_EV_CH_E_CNTXT_0(ev)) >> EV_STATE_SHIFT & 0xf;
}

/* gsistart=eedump: READ-ONLY look at the modem's GSI EE (EE1 = our EE0 register block + 0x4000) next to ours:
 * channel and event ring states 0-7 as hex digits (0 not-alloc, 1 allocated, 2 started, 3 stopped, f error),
 * GSI status, error log.  Reading another EE's block could in principle be refused by the XPU (bus error);
 * the run is opt-in for that reason. */
static void ee_dump(void)
{
	static const char hx[] = "0123456789abcdef";
	static char prev[64];
	static unsigned beat;
	char c1[9], e1[9], c0[9], e0[9], sig[64];

	for (int i = 0; i < 8; i++) {
		c1[i] = hx[rd32(GSI_CH_C_CNTXT_0(i) + 0x4000) >> CH_STATE_SHIFT & 0xf];
		e1[i] = hx[rd32(GSI_EV_CH_E_CNTXT_0(i) + 0x4000) >> EV_STATE_SHIFT & 0xf];
		c0[i] = hx[ch_state(i)];
		e0[i] = hx[ev_state(i)];
	}
	c1[8] = e1[8] = c0[8] = e0[8] = 0;
	snprintf(sig, sizeof(sig), "%s %s %s %s %08x", c1, e1, c0, e0, rd32(GSI_BASE + 0x4000 + 0x12200));
	/* log only when something changed (plus a heartbeat every ~10 s): the dump may run for minutes */
	if (!strcmp(sig, prev) && ++beat < 40)
		return;
	beat = 0;
	LOG("eedump EE1 ch[%s] ev[%s] status %08x err %08x | EE0 ch[%s] ev[%s] err %08x%s", c1, e1,
	    rd32(GSI_BASE + 0x4000 + 0x12000), rd32(GSI_BASE + 0x4000 + 0x12200), c0, e0, rd32(GSI_BASE + 0x12200),
	    prev[0] && strcmp(sig, prev) ? "  <- CHANGED" : "");
	if (strcmp(sig, prev)) {
		/* modem EE context words (flags, ring length, ring base lo/hi) of every channel / event ring that exists */
		for (int i = 0; i < 8; i++) {
			if (c1[i] != '0')
				LOG("  EE1 ch %d: %08x %08x %08x %08x", i, rd32(GSI_CH_C_CNTXT_0(i) + 0x4000),
				    rd32(GSI_CH_C_CNTXT_1(i) + 0x4000), rd32(GSI_CH_C_CNTXT_2(i) + 0x4000),
				    rd32(GSI_CH_C_CNTXT_3(i) + 0x4000));
			if (c1[i] != '0')	/* read / write pointers, QOS, scratch */
				LOG("  EE1 ch %d rp/wp %08x/%08x qos %08x scr %08x %08x %08x %08x", i,
				    rd32(GSI_CH_C_CNTXT_0(i) + 0x4010), rd32(GSI_CH_C_CNTXT_0(i) + 0x4018),
				    rd32(GSI_CH_C_QOS(i) + 0x4000), rd32(GSI_CH_C_SCRATCH_0(i) + 0x4000),
				    rd32(GSI_CH_C_SCRATCH_1(i) + 0x4000), rd32(GSI_CH_C_SCRATCH_2(i) + 0x4000),
				    rd32(GSI_CH_C_SCRATCH_3(i) + 0x4000));
			if (e1[i] != '0')
				LOG("  EE1 evt %d: %08x %08x %08x %08x", i, rd32(GSI_EV_CH_E_CNTXT_0(i) + 0x4000),
				    rd32(GSI_EV_CH_E_CNTXT_1(i) + 0x4000), rd32(GSI_EV_CH_E_CNTXT_2(i) + 0x4000),
				    rd32(GSI_EV_CH_E_CNTXT_3(i) + 0x4000));
			if (e1[i] != '0')	/* event ring read / write pointers, moderation, scratch */
				LOG("  EE1 evt %d rp/wp %08x/%08x mod %08x scr %08x %08x", i, rd32(GSI_EV_CH_E_CNTXT_4(i) + 0x4000),
				    rd32(GSI_EV_CH_E_CNTXT_4(i) + 0x4008), rd32(GSI_EV_CH_E_CNTXT_8(i) + 0x4000),
				    rd32(GSI_EV_CH_E_CNTXT_12(i) + 0x4000), rd32(GSI_EV_CH_E_CNTXT_13(i) + 0x4000));
		}
	}
	snprintf(prev, sizeof(prev), "%s", sig);
}

static const char *ch_state_name(unsigned s)
{
	switch (s) {
	case CH_ST_NOT_ALLOC: return "not-alloc";
	case CH_ST_ALLOCATED: return "allocated";
	case CH_ST_STARTED: return "started";
	case CH_ST_STOPPED: return "stopped";
	case CH_ST_STOP_IN_PROC: return "stop-in-proc";
	case CH_ST_FLOW_CTRL: return "flow-controlled";
	case CH_ST_ERROR: return "error";
	default: return "?";
	}
}

/* wait for a channel/event ring command to take effect: poll the state field until it moves
 * away from `from` (0 = it did, -1 = timeout, and both old and new state are logged) */
static int wait_state(uint64_t reg, unsigned shift, unsigned from, unsigned ms, const char *what)
{
	uint64_t t0 = link_ms();

	for (;;) {
		unsigned now = (unsigned) (rd32(reg) >> shift & 0xf);

		if (now != from)
			return 0;
		if (link_ms() - t0 >= ms) {
			LOG("%s stuck in state %s (%x)", what, ch_state_name(now), now);
			return -1;
		}
		delay_ms(1);
	}
}
/* ------------------------------------------------------------------ low (< 4 GiB) memory (p2low=1)
 * Experiment: /dev/pmem gives managed DRAM above 4 GiB only.  /dev/redwood-fastrpc FASTRPC_ALLOC returns
 * RAM < 4 GiB (GPU pool, zeroed); pmem reads/writes reach it with 32-bit aligned offsets/lengths only.
 * One block is taken and bump-allocated; ring/TX buffer allocations use it while lo_on is set. */
#include <fcntl.h>
#include <sys/ioctl.h>
struct lo_alloc { uint64_t size, pa; };
#ifndef FASTRPC_ALLOC
#define FASTRPC_ALLOC _IOWR('f', 2, struct lo_alloc)
#endif
#define LO_SIZE (1024 * 1024)
static int lo_on, lo_fd = -1;
static uint64_t lo_base, lo_next, lo_end;

static int lo_init(void)
{
	struct lo_alloc a = { .size = LO_SIZE };

	lo_fd = open("/dev/redwood-fastrpc", O_RDWR);
	if (lo_fd < 0) {
		LOG("p2low: open /dev/redwood-fastrpc: %s", strerror(errno));
		return -1;
	}
	if (ioctl(lo_fd, FASTRPC_ALLOC, &a) < 0) {
		LOG("p2low: FASTRPC_ALLOC %u: %s", (unsigned) LO_SIZE, strerror(errno));
		return -1;
	}
	lo_base = lo_next = a.pa;
	lo_end = a.pa + LO_SIZE;
	lo_on = 1;
	LOG("p2low: got %u KiB at PA 0x%llx", LO_SIZE / 1024, (unsigned long long) a.pa);
	return 0;
}

static uint64_t ring_mem(uint64_t size, uint64_t align)
{
	uint64_t at;

	if (!lo_on)
		return pm_alloc(size, align);
	at = (lo_next + align - 1) & ~(align - 1);
	if (at + size > lo_end) {
		LOG("p2low: out of low memory (%llu bytes)", (unsigned long long) size);
		return 0;
	}
	lo_next = at + size;
	return at;
}

/* ------------------------------------------------------------------ GSI bring-up */

/* clear a freshly allocated ring (the event rings must start empty for ev_read()) */
static int ring_clear(struct gsi_ring *r)
{
	uint8_t zero[RING_ELEM * 8];

	memset(zero, 0, sizeof(zero));
	for (uint32_t at = 0; at < r->cnt * RING_ELEM; at += sizeof(zero))
		if (pm_wr(r->pa + at, zero, sizeof(zero))) {
			LOG("ring clear failed: %s", strerror(errno));
			return -1;
		}
	return 0;
}

static int ev_alloc_program(int ev, uint32_t cnt)
{
	struct gsi_ring *r = &ctx.ev[ev];
	uint64_t off = GSI_EV_CH_E_CNTXT_0(ev);
	uint32_t st = ev_state(ev);

	if (st != EV_ST_NOT_ALLOC) {
		LOG("evt ring %d not free: state %s (%x)", ev, ch_state_name(st), st);
		return -1;
	}
	r->pa = ring_mem((uint64_t) cnt * RING_ELEM, 4096);
	if (!r->pa) {
		LOG("evt ring %d pm_alloc failed", ev);
		return -1;
	}
	r->cnt = cnt;
	r->rp = 0;
	r->wp = 0;
	if (ring_clear(r))
		return -1;

	wr32(GSI_EV_CH_CMD, EV_CMD_ALLOCATE << 24 | (unsigned) ev);
	if (wait_state(off, EV_STATE_SHIFT, EV_ST_NOT_ALLOC, 100, "evt allocate"))
		return -1;
	st = ev_state(ev);
	if (st != EV_ST_ALLOCATED) {
		LOG("evt ring %d after allocate: state %x", ev, st);
		return -1;
	}
	/* mainline order (gsi_channel_setup_one): ALLOCATE first, then program the context and
	 * ring the doorbell.  Context written while the ring is NOT_ALLOCATED does not stick. */
	wr32(off + 0x00, EV_CHTYPE_GPI | EV_INTYPE | (RING_ELEM << EV_ELEM_SHIFT));
	wr32(off + 0x04, cnt * RING_ELEM & 0xffff);
	wr32(off + 0x08, (uint32_t) r->pa);
	wr32(off + 0x0c, (uint32_t) (r->pa >> 32));
	wr32(off + 0x20, EV_MODT | (1u << EV_MODC_SHIFT));	/* moderation delay + "from channel" */
	wr32(off + 0x24, 0);			/* no MSI write data */
	wr32(off + 0x28, 0);			/* MSI address low */
	wr32(off + 0x2c, 0);			/* MSI address high */
	wr32(off + 0x30, 0);			/* read pointer update (not used) */
	wr32(off + 0x34, 0);

	wr32(GSI_EV_DB(ev), (uint32_t) (r->pa + (uint64_t) (cnt - 1) * RING_ELEM));
	LOG("evt ring %d pa %llx cnt %u ok", ev, (unsigned long long) r->pa, cnt);
	return 0;
}

static int ch_alloc_program(int ch, int ev, uint32_t cnt, int toward_ipa, int command, uint32_t tlv)
{
	struct gsi_ring *r = &ctx.ch[ch];
	uint64_t off = GSI_CH_C_CNTXT_0(ch);
	uint32_t st = ch_state(ch);
	uint32_t qos;

	if (st != CH_ST_NOT_ALLOC) {
		LOG("ch %d not free: state %s (%x)", ch, ch_state_name(st), st);
		return -1;
	}
	r->pa = ring_mem((uint64_t) cnt * RING_ELEM, 4096);
	if (!r->pa) {
		LOG("ch %d pm_alloc failed", ch);
		return -1;
	}
	r->cnt = cnt;
	r->rp = 0;
	r->wp = 0;
	if (ring_clear(r))
		return -1;

	wr32(GSI_CH_CMD, CH_CMD_ALLOCATE << 24 | (unsigned) ch);
	if (wait_state(off, CH_STATE_SHIFT, CH_ST_NOT_ALLOC, 100, "ch allocate"))
		return -1;
	st = ch_state(ch);
	if (st != CH_ST_ALLOCATED) {
		LOG("ch %d after allocate: state %s (%x)", ch, ch_state_name(st), st);
		return -1;
	}
	/* mainline order: ALLOCATE, then gsi_channel_program() (context written before ALLOCATE
	 * is lost, which left START with an empty ring) */
	/* CH_C_CNTXT_0: the CH_EE and CHID fields are left 0 as mainline leaves them (the
	 * ALLOCATE command fills them in for our EE); ERINDEX says which event ring reports. */
	wr32(off + 0x00, CH_CHTYPE_GPI | (toward_ipa ? CH_DIR : 0) |
	       ((uint32_t) ev << CH_ERINDEX_SHIFT) | ((uint32_t) RING_ELEM << CH_ELEM_SHIFT));
	wr32(off + 0x04, cnt * RING_ELEM & CH_LEN_MASK);
	wr32(off + 0x08, (uint32_t) r->pa);
	wr32(off + 0x0c, (uint32_t) (r->pa >> 32));

	/* CH_C_QOS: mainline leaves WRR_WEIGHT 0 for the data channels and uses the field
	 * maximum (7, the lowest weight) for the command channel; max prefetch stays 1 segment
	 * (MAX_PREFETCH bit 8 clear), the doorbell engine is not used from IPA v4.0 on, and the
	 * escape buffer is used on all but the command channel (PREFETCH_MODE = ESCAPE_BUF_ONLY
	 * = 1 on v4.5+). */
	qos = (command ? CH_QOS_WRR_MAX : 0u) << CH_QOS_WRR_SHIFT;
	if (!command)
		qos |= CH_QOS_ESCAPE_ONLY << CH_QOS_PREFETCH_SHIFT;
	wr32(GSI_CH_C_QOS(ch), qos);

	/* GPI scratch (mainline struct gsi_channel_scratch_gpi): words 1 and 2 are reserved,
	 * word 3 = SCRATCH_2 holds max_outstanding_tre in bits 31:16 (transaction size in bytes,
	 * tlv_count * 16 - the TLV FIFO is what one commit can hold), word 4 = SCRATCH_3 holds
	 * outstanding_threshold (2 TREs) in bits 31:16 with the low half preserved. */
	wr32(GSI_CH_C_SCRATCH_0(ch), 0);
	wr32(GSI_CH_C_SCRATCH_1(ch), 0);
	wr32(GSI_CH_C_SCRATCH_2(ch), tlv * RING_ELEM << CH_SCR_MAXTRE_SHIFT);
	{
		uint32_t keep = rd32(GSI_CH_C_SCRATCH_3(ch)) & 0xffffu;

		wr32(GSI_CH_C_SCRATCH_3(ch), keep | (2u * RING_ELEM) << CH_SCR_THRESH_SHIFT);
	}

	LOG("ch %d pa %llx cnt %u evt %d %s ok", ch, (unsigned long long) r->pa, cnt, ev,
	    toward_ipa ? "tx" : "rx");
	return 0;
}

/* release what ch_alloc_program()/ev_alloc_program() took, the way mainline's
 * gsi_channel_teardown_one() does: channel DE_ALLOC straight from ALLOCATED, event ring RESET
 * then DE_ALLOC.  Anything not in ALLOCATED is left alone (and logged). */
static void ch_release(int ch)
{
	unsigned st = ch_state(ch);

	if (st == CH_ST_NOT_ALLOC)
		return;
	if (st != CH_ST_ALLOCATED) {
		LOG("ch %d release: state %s (%x), not releasing", ch, ch_state_name(st), st);
		return;
	}
	wr32(GSI_CH_CMD, CH_CMD_DE_ALLOC << 24 | (unsigned) ch);
	if (wait_state(GSI_CH_C_CNTXT_0(ch), CH_STATE_SHIFT, st, 100, "ch dealloc"))
		return;
	LOG("ch %d released (state %s)", ch, ch_state_name(ch_state(ch)));
}

static void ev_release(int ev)
{
	unsigned st = ev_state(ev);

	if (st == EV_ST_NOT_ALLOC)
		return;
	if (st != EV_ST_ALLOCATED) {
		LOG("evt ring %d release: state %x, not releasing", ev, st);
		return;
	}
	delay_ms(2);
	wr32(GSI_EV_CH_CMD, EV_CMD_RESET << 24 | (unsigned) ev);
	delay_ms(2);
	wr32(GSI_EV_CH_CMD, EV_CMD_DE_ALLOC << 24 | (unsigned) ev);
	if (wait_state(GSI_EV_CH_E_CNTXT_0(ev), EV_STATE_SHIFT, EV_ST_ALLOCATED, 100, "evt dealloc"))
		return;
	LOG("evt ring %d released (state %x)", ev, ev_state(ev));
}

static void gsi_release_all(void)
{
	ch_release(CH_ALT_RX);
	ch_release(CH_ALT_TX);
	ch_release(CH_MDM_RX);
	ch_release(CH_MDM_TX);
	for (int ev = 0; ev < 8; ev++)
		ev_release(ev);
	LOG("released: ch 2 %s, ch 7 %s, evt 0 %x 1 %x 2 %x 3 %x", ch_state_name(ch_state(CH_MDM_TX)),
	    ch_state_name(ch_state(CH_MDM_RX)), ev_state(0), ev_state(1), ev_state(2), ev_state(3));
}

static int ch_start(int ch)
{
	uint32_t st = ch_state(ch);

	if (st != CH_ST_ALLOCATED && st != CH_ST_STOPPED) {
		LOG("ch %d start: bad state %s (%x)", ch, ch_state_name(st), st);
		return -1;
	}
	wr32(GSI_CH_CMD, CH_CMD_START << 24 | (unsigned) ch);
	if (wait_state(GSI_CH_C_CNTXT_0(ch), CH_STATE_SHIFT, st, 100, "ch start")) {
		LOG("ch %d ctx %08x %08x %08x %08x qos %08x scr %08x %08x %08x %08x err %08x", ch,
		    rd32(GSI_CH_C_CNTXT_0(ch)), rd32(GSI_CH_C_CNTXT_1(ch)), rd32(GSI_CH_C_CNTXT_2(ch)),
		    rd32(GSI_CH_C_CNTXT_3(ch)), rd32(GSI_CH_C_QOS(ch)), rd32(GSI_CH_C_SCRATCH_0(ch)),
		    rd32(GSI_CH_C_SCRATCH_1(ch)), rd32(GSI_CH_C_SCRATCH_2(ch)), rd32(GSI_CH_C_SCRATCH_3(ch)),
		    rd32(GSI_BASE + 0x12200));
		LOG("ch %d gsi irq: type %08x src_ch %08x src_ev %08x glob %08x gsi %08x scratch0 %08x status %08x",
		    ch, rd32(GSI_BASE + 0x12080), rd32(GSI_BASE + 0x12090), rd32(GSI_BASE + 0x12094),
		    rd32(GSI_BASE + 0x12100), rd32(GSI_BASE + 0x12118), rd32(GSI_BASE + 0x12400),
		    rd32(GSI_STATUS));
		/* keep watching for 2 s: does START complete late, or never? */
		for (int i = 1; i <= 4; i++) {
			delay_ms(500);
			LOG("ch %d +%d ms: state %s (%x), error log %08x", ch, i * 500,
			    ch_state_name(ch_state(ch)), ch_state(ch), rd32(GSI_BASE + 0x12200));
		}
		return -1;
	}
	st = ch_state(ch);
	if (st != CH_ST_STARTED) {
		LOG("ch %d after start: state %s (%x)", ch, ch_state_name(st), st);
		return -1;
	}
	LOG("ch %d started (state %s)", ch, ch_state_name(st));
	return 0;
}

/* ring a channel doorbell: GSI v4.5 takes the *address* of the first element we have not
 * written (5.15 gsi_channel_doorbell), not an index */
static void ch_doorbell(int ch)
{
	struct gsi_ring *r = &ctx.ch[ch];

	wr32(GSI_CH_DB(ch), (uint32_t) (r->pa + (uint64_t) (r->wp % r->cnt) * RING_ELEM));
}

/* one event ring doorbell: the address of the last element we processed */
static void ev_doorbell(int ev)
{
	struct gsi_ring *r = &ctx.ev[ev];
	uint32_t last = (r->rp - 1) % r->cnt;

	wr32(GSI_EV_DB(ev), (uint32_t) (r->pa + (uint64_t) last * RING_ELEM));
}

/* write one TRE (ring element) at the channel's write position and move it on.  `last` is the
 * IEOT flag: the transaction ends here, so the hardware reports a completion event.  Every
 * transaction we post is a single TRE, so IEOT is always set and CHAIN never used. */
static int ch_post_tre(int ch, uint64_t buf, uint32_t len, int last)
{
	struct gsi_ring *r = &ctx.ch[ch];
	struct gsi_tre tre;
	uint32_t idx = r->wp % r->cnt;

	tre.addr = buf;
	tre.len = (uint16_t) len;
	tre.resv = 0;
	tre.flags = TRE_XFER | (last ? TRE_IEOT : TRE_CHAIN);
	if (pm_wr(r->pa + (uint64_t) idx * RING_ELEM, &tre, sizeof(tre))) {
		LOG("ch %d tre write failed: %s", ch, strerror(errno));
		return -1;
	}
	r->wp++;
	return 0;
}

static int evt_is_free(const struct gsi_evt *e)
{
	const uint8_t *b = (const uint8_t *) e;

	for (unsigned i = 0; i < sizeof(*e); i++)
		if (b[i])
			return 0;
	return 1;
}

/* read the next event on an event ring into *e; 0 = nothing there, 1 = an event.  A slot we
 * have consumed is zeroed, so anything non-zero in it is new (the rings are short and every
 * event has a non-zero code, type or channel id). */
static int ev_read(int ev, struct gsi_evt *e)
{
	struct gsi_ring *r = &ctx.ev[ev];
	uint64_t at = r->pa + (uint64_t) (r->rp % r->cnt) * RING_ELEM;

	if (pm_rd(at, e, sizeof(*e))) {
		LOG("evt ring %d read failed: %s", ev, strerror(errno));
		ctx.rx_err++;
		return -1;
	}
	if (evt_is_free(e))
		return 0;
	/* clear the ring slot from a separate zero element: zeroing *e here wiped the event before the caller saw it */
	struct gsi_evt z;
	memset(&z, 0, sizeof(z));
	if (pm_wr(at, &z, sizeof(z))) {
		LOG("evt ring %d clear failed: %s", ev, strerror(errno));
		return -1;
	}
	r->rp++;
	ev_doorbell(ev);
	return 1;
}

/* step 1: GSI as we find it.  Read only: HW_PARAM_2 tells us how many rings the EE has. */
static int gsi_check(void)
{
	uint32_t hp = rd32(GSI_HW_PARAM_2);
	uint32_t st = rd32(GSI_STATUS);
	unsigned nch = hp >> 3 & 0x1f, nev = hp >> 8 & 0x1f;

	LOG("GSI status %08x hw_param_2 %08x (ch/ee %u, evt/ee %u, iram %u)", st, hp, nch, nev,
	    hp & 7);
	if (!(st & 1)) {
		LOG("GSI not enabled - not touching it");
		return -1;
	}
	if (nch <= CH_MDM_RX || nev <= EV_RX) {
		LOG("EE0 has too few rings (ch %u, evt %u)", nch, nev);
		return -1;
	}
	for (int ch = 0; ch < 8; ch++)
		LOG("  ch %d cntxt0 %08x state %s", ch, rd32(GSI_CH_C_CNTXT_0(ch)),
		    ch_state_name(ch_state(ch)));
	for (int ev = 0; ev < 4; ev++)
		LOG("  evt %d cntxt0 %08x state %s", ev, rd32(GSI_EV_CH_E_CNTXT_0(ev)),
		    ch_state_name(ev_state(ev)));
	return 0;
}
/* ------------------------------------------------------------------ IPA v4.7 endpoint block
 *
 * Offsets and fields here are from mainline drivers/net/ipa/reg/ipa_reg-v4.7.c (checked in both
 * 6.6 and 6.7) and drivers/net/ipa/reg/ipa_reg.h: one 0x70-wide block per endpoint id, from the
 * IPA base.  The endpoint configuration the modem wrote during INIT_DEVICE / INIT_MODEM_DRIVER
 * stays valid for us - endpoints 2, 16 and 7 are the IPA's description of *our* endpoints - so
 * everything here is read and logged, and only changed if one of the IPAGSI_SET_* knobs below is
 * turned on.
 *
 * There is no endpoint -> channel binding register to program.  mainline's ipa_data-v4.7.c pairs
 * each AP endpoint with a fixed GSI channel of EE0 (endpoint 7 <-> channel 5, endpoint 2 <->
 * channel 2, endpoint 16 <-> channel 7) and never writes a binding: the pairing belongs to the
 * IPA/EE, which is why the spec's endpoint table gives those channel ids in the first place.  So
 * taking over the modem's AP channels is exactly a matter of using the same (channel, endpoint)
 * pairs, and ipagsi_endpoints() exists to check that against what the modem programmed and to
 * log the differences.
 */
#define IPA_ENDP_CTRL(ep)	(IPA_BASE + 0x0800 + 0x70 * (ep))
#define IPA_ENDP_CFG(ep)	(IPA_BASE + 0x0808 + 0x70 * (ep))
#define IPA_ENDP_NAT(ep)	(IPA_BASE + 0x080c + 0x70 * (ep))
#define IPA_ENDP_HDR(ep)	(IPA_BASE + 0x0810 + 0x70 * (ep))
#define IPA_ENDP_HDR_EXT(ep)	(IPA_BASE + 0x0814 + 0x70 * (ep))
#define IPA_ENDP_HDR_MASK(ep)	(IPA_BASE + 0x0818 + 0x70 * (ep))
#define IPA_ENDP_MODE(ep)	(IPA_BASE + 0x0820 + 0x70 * (ep))
#define IPA_ENDP_AGGR(ep)	(IPA_BASE + 0x0824 + 0x70 * (ep))
#define IPA_ENDP_HOL_EN(ep)	(IPA_BASE + 0x082c + 0x70 * (ep))
#define IPA_ENDP_HOL_TIMER(ep)	(IPA_BASE + 0x0830 + 0x70 * (ep))
#define IPA_ENDP_DEAGGR(ep)	(IPA_BASE + 0x0834 + 0x70 * (ep))
#define IPA_ENDP_RSRC_GRP(ep)	(IPA_BASE + 0x0838 + 0x70 * (ep))
#define IPA_ENDP_SEQ(ep)	(IPA_BASE + 0x083c + 0x70 * (ep))
#define IPA_ENDP_STATUS(ep)	(IPA_BASE + 0x0840 + 0x70 * (ep))
#define IPA_ENDP_FILT_RT_HSH(ep) (IPA_BASE + 0x085c + 0x70 * (ep))

#define EP_CTRL_SUSPEND		(1u << 0)	/* RX endpoints only */
#define EP_CTRL_DELAY		(1u << 1)	/* TX endpoints only */
#define EP_CFG_FRAG_OFFLOAD	(1u << 0)
#define EP_CFG_CS_OFFLOAD_SHIFT	1		/* 2:1, 0 = off (our choice, and mainline's default) */
#define EP_HDR_LEN_MASK		0x3fu
#define EP_HDR_LEN_MSB_SHIFT	28		/* 29:28: the header length is 8 bits in v4.7+ */
#define EP_MODE_DMA		3u		/* ENDP_MODE 2:0: IPA_BASIC 0, IPA_REPLICA 2, IPA_DMA 3 */
#define EP_MODE_DEST_SHIFT	4		/* DEST_PIPE_INDEX 8:4: the dma_endpoint of the data */
#define EP_AGGR_EN_MASK		0x3u		/* AGGR_EN 1:0 */
#define EP_AGGR_EN_SHIFT	0
#define EP_AGGR_TYPE_SHIFT	2		/* 4:2 (unused here) */
#define EP_AGGR_BYTE_SHIFT	5		/* 10:5 */
#define EP_AGGR_TIME_SHIFT	12		/* 16:12: a Qtime tick count + pulse generator select */
#define EP_AGGR_PKT_SHIFT	17		/* 22:17 */
#define EP_AGGR_SW_EOF		(1u << 23)	/* SW_EOF_ACTIVE: close aggregation on an EOF TRE */
#define EP_AGGR_GRAN		(1u << 27)	/* AGGR_GRAN_SEL: byte limit in bytes, not KB */
#define EP_SEQ_TYPE_MASK	0xffu
#define EP_SEQ_2_PASS_SKIP_LAST_UC 4		/* IPA_SEQ_2_PASS_SKIP_LAST_UC, mainline's ep2 */
#define EP_STATUS_EN		(1u << 0)
#define EP_STATUS_ENDP_SHIFT	1		/* 5:1: endpoint the status message goes to */

#define IPA_LOCAL_PKT_PROC_CNTXT	(IPA_BASE + 0x01e8)
#define IPA_FLAVOR_0			(IPA_BASE + 0x0210)
#define IPA_AGGR_FORCE_CLOSE		(IPA_BASE + 0x01ec)

static void ep_show(int ep)
{
	static const struct { const char *name; uint64_t off; } r[] = {
		{ "ctrl", 0x00 }, { "cfg", 0x08 }, { "nat", 0x0c }, { "hdr", 0x10 },
		{ "hdr_ext", 0x14 }, { "hdr_mask", 0x18 }, { "mode", 0x20 }, { "aggr", 0x24 },
		{ "hol_en", 0x2c }, { "hol_timer", 0x30 }, { "deaggr", 0x34 }, { "rsrc", 0x38 },
		{ "seq", 0x3c }, { "status", 0x40 }, { "hsh", 0x5c },
	};
	uint64_t base = IPA_BASE + 0x0800 + 0x70 * (uint64_t) ep;

	for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); i++)
		LOG("  ep%d %-9s %08x", ep, r[i].name, rd32(base + r[i].off));
}

#if 1	/* was IPAGSI_PHASE >= 2: gsistart=p2late uses it in the default build too */
/* Endpoints are programmed by the modem during IPA QMI INIT_DEVICE / INIT_MODEM_DRIVER (that is
 * how the DPM port {embedded 4, iface 1, rx-ep 2, tx-ep 16} reached them), and mainline's
 * ipa_data-v4.7.c gives the values we would use.  Nothing here is rewritten unless its knob is
 * turned on: ep_rmw() logs "would be old -> new" and leaves the register alone, which is the
 * safest thing while we do not know what the modem has set. */
#define IPAGSI_SET_AGGR		0	/* 1: rewrite endpoint 16's aggregation limits */
#define IPAGSI_SET_SEQ		0	/* 1: rewrite endpoint 2's sequence type */
#define IPAGSI_SET_CTRL		0	/* 1: clear endpoint control (suspend/delay) bits */

static uint32_t ep_rmw(const char *what, uint64_t reg, uint32_t mask, uint32_t val, int allow_change)
{
	uint32_t old = rd32(reg);
	uint32_t next = (old & ~mask) | (val & mask);

	if (next != old && !allow_change) {
		LOG("%s: would be %08x -> %08x, left alone (turn the SET_ knob on if intended)",
		    what, old, next);
		return old;
	}
	if (next != old) {
		wr32(reg, next);
		LOG("%s: %08x -> %08x", what, old, next);
	} else {
		LOG("%s: %08x (unchanged)", what, old);
	}
	return next;
}

/* endpoints 2, 16 (and 7 with tables=1) must already be the modem's setup for our EE: check what
 * is there, warn about anything that will change what we are handed, and write nothing unless a
 * knob says so. */
static int ipagsi_endpoints(void)
{
	for (int ep = 0; ep <= 16; ep++) {
		int ch = ep == EP_MDM_TX ? CH_MDM_TX : ep == EP_MDM_RX ? CH_MDM_RX :
			 ep == EP_CMDTX ? CH_CMDTX : -1;

		if (ch < 0 || (ep == EP_CMDTX && !ctx.tables))
			continue;
		ep_show(ep);
		if (ep == EP_MDM_TX)
			ep_rmw("ep2 seq", IPA_ENDP_SEQ(ep), EP_SEQ_TYPE_MASK,
			       EP_SEQ_2_PASS_SKIP_LAST_UC, IPAGSI_SET_SEQ);
		if (ep == EP_MDM_RX) {
			uint32_t ag = rd32(IPA_ENDP_AGGR(ep));

			/* mainline's ipa_qtime_val() turns aggr_time_limit into a Qtime tick count
			 * plus a pulse-generator selector, and the byte limit's units depend on
			 * AGGR_GRAN_SEL; neither is worked out here, so only the byte/packet/eof
			 * fields below are what the SET_AGGR knob would touch, and they are there
			 * to be read, not written (see the open items in README.ipagsi.md). */
			if (ag & EP_AGGR_EN_MASK) {
				unsigned bytes = (ag >> EP_AGGR_BYTE_SHIFT) & 0x3f;

				LOG("ep16 aggregation: byte_limit %u%s, pkt_limit %u, time_limit"
				    " %02x (raw %08x)", bytes,
				    ag & EP_AGGR_GRAN ? "" : " (units of 1 KB?)",
				    (ag >> EP_AGGR_PKT_SHIFT) & 0x3f,
				    (ag >> EP_AGGR_TIME_SHIFT) & 0x1f, ag);
				if ((bytes << (ag & EP_AGGR_GRAN ? 0 : 10)) != RX_BUF_SZ)
					LOG("ep16: aggregation byte limit does not match our %d"
					    " byte rx buffers: each completion may hold less",
					    RX_BUF_SZ);
			} else {
				LOG("ep16: aggregation off: one ip packet per completion");
			}
			ep_rmw("ep16 aggr", IPA_ENDP_AGGR(ep),
			       0x3u << EP_AGGR_EN_SHIFT | 0x3fu << EP_AGGR_BYTE_SHIFT |
			       0x3fu << EP_AGGR_PKT_SHIFT | EP_AGGR_SW_EOF,
			       1u << EP_AGGR_EN_SHIFT | (RX_BUF_SZ / 1024) << EP_AGGR_BYTE_SHIFT |
			       16u << EP_AGGR_PKT_SHIFT | EP_AGGR_SW_EOF, IPAGSI_SET_AGGR);
		}
		if (ep == EP_CMDTX) {
			uint32_t md = rd32(IPA_ENDP_MODE(ep));

			/* the command endpoint is expected to be in DMA mode toward endpoint 9 */
			if ((md & 7u) != EP_MODE_DMA)
				LOG("ep7 mode %08x: not dma mode (%x)?", md, EP_MODE_DMA);
			else if (((md >> EP_MODE_DEST_SHIFT) & 0x1f) != EP_LAN_RX)
				LOG("ep7 dma destination ep %u, not %d",
				    (md >> EP_MODE_DEST_SHIFT) & 0x1f, EP_LAN_RX);
		}
		if (IPAGSI_SET_CTRL)
			ep_rmw("ctrl", IPA_ENDP_CTRL(ep), EP_CTRL_SUSPEND | EP_CTRL_DELAY,
			       0, 1);
		else if (rd32(IPA_ENDP_CTRL(ep)) & (EP_CTRL_SUSPEND | EP_CTRL_DELAY))
			LOG("ep%d ctrl %08x: suspended/delayed: ipagsi_ep data will not flow",
			    ep, rd32(IPA_ENDP_CTRL(ep)));
	}
	return 0;
}

/* step 5 in the spec: the IPA tables live in the modem's local memory at the addresses ipaqmi.c's
 * ipa47_mem[] describes, so header/system/filter/route init means copying them to DDR and driving
 * channel 5 with immediate commands (that is what ipacmd=1 would be for).  Not implemented - the
 * modem's own INIT_MODEM_DRIVER setup already left those tables in place, so this only logs, and
 * channel 5 and event ring 0 stay allocated but unused.  See open item 4 in README.ipagsi.md. */
static void __attribute__((unused)) ipagsi_tables(void)
{
	if (!ctx.tables)
		return;
	LOG("tables: the channel %d immediate commands (system / filter / route init) are not"
	    " implemented: using the modem's tables as they are", CH_CMDTX);
}
#endif

/* what the IPA looks like now, read only: the endpoint blocks we plan to use plus the global and
 * resource-group registers around them.  Nothing here writes. */
static int ipagsi_dump_ipa(void)
{
	LOG("ipa flavor %08x local_pkt_proc_cntxt %08x", rd32(IPA_FLAVOR_0),
	    rd32(IPA_LOCAL_PKT_PROC_CNTXT));
	for (int ep = 0; ep <= 16; ep++)
		if (ep == EP_MDM_TX || ep == EP_CMDTX || ep == EP_MDM_RX)
			ep_show(ep);
	/* the resource groups (mainline's ipa_data-v4.7.c has one src and one dst group, both id
	 * 0: 0x400 and 0x500, X 5:0 / X_MAX 13:8 / Y 21:16 / Y_MAX 29:24) and the per-endpoint
	 * filter/router hash config of 0x85c, the two blocks that tell us what the modem's table
	 * setup looks like from the IPA side */
	LOG("src rsrc grp0 %08x  dst rsrc grp0 %08x", rd32(IPA_BASE + 0x400), rd32(IPA_BASE + 0x500));
	for (int ep = 0; ep <= 16; ep++)
		if (ep == EP_MDM_TX || ep == EP_MDM_RX || ep == EP_CMDTX)
			LOG("  ep%d filt/route hsh cfg %08x", ep, rd32(IPA_ENDP_FILT_RT_HSH(ep)));
	return 0;
}
/* ------------------------------------------------------------------ data plane
 *
 * The utun takes one packet per read/write, prefixed with its address family in network order
 * (ipnet.c does the same for the WLAN's utun).  The family comes from the IP version nibble.
 */

static uint8_t bounce[RX_BUF_SZ];
static uint8_t tx_frame[TX_CAP + QMAP_HDR];
static uint8_t utun_pkt[4 + 2048];		/* AF prefix + one IP packet (MTU 1500 + room) */

/* the AF prefix for a packet, 0 = not IP we can hand to the utun */
static uint32_t utun_af(const uint8_t *p, size_t n)
{
	switch (n > 0 ? p[0] >> 4 : 0) {
	case 4:
		return n >= 20 ? htonl(AF_INET) : 0;
	case 6:
		return n >= 40 ? htonl(AF_INET6) : 0;
	default:
		return 0;
	}
}

/* an IP packet coming from the modem, already out of its QMAP frame */
static void rx_to_utun(void *unused, uint8_t mux, const uint8_t *p, size_t n)
{
	uint32_t af = utun_af(p, n);
	ssize_t got;

	if (mux != 1) {		/* mux 0 is the QMAP command channel (already counted by
				 * qmap_deagg), anything else is not this call's mux id */
		return;
	}
	if (!af || n + 4 > sizeof(utun_pkt)) {
		ctx.rx_err++;
		if (ctx.rx_err < 8)
			LOG("rx: not ip (ver %u) or %zu bytes too long", n ? p[0] >> 4 : 0, n);
		return;
	}
	if (ctx.utun_fd < 0)
		return;
	memcpy(utun_pkt, &af, 4);
	memcpy(utun_pkt + 4, p, n);
	got = write(ctx.utun_fd, utun_pkt, n + 4);
	if (got < 0) {
		ctx.rx_err++;
		if (ctx.rx_err < 8)
			LOG("utun write: %s", strerror(errno));
		return;
	}
	ctx.rx_pkts++;
}

/* post (or re-post) one RX buffer on channel 7 */
static int rx_post_one(unsigned idx)
{
	if (!ctx.rx_buf_pa[idx]) {
		ctx.rx_buf_pa[idx] = pm_alloc(RX_BUF_SZ, RX_BUF_SZ);
		if (!ctx.rx_buf_pa[idx]) {
			LOG("rx buffer %u pm_alloc failed", idx);
			return -1;
		}
	}
	if (ch_post_tre(CH_MDM_RX, ctx.rx_buf_pa[idx], RX_BUF_SZ, 1))
		return -1;
	return 0;
}

static int rx_prepost(void)
{
	for (unsigned i = 0; i < RX_BUFS; i++)
		if (rx_post_one(i))
			return -1;
	ctx.rx_wp = RX_BUFS;
	ch_doorbell(CH_MDM_RX);
	LOG("%u rx buffers (%d bytes) posted", RX_BUFS, RX_BUF_SZ);
	return 0;
}

/* one completion event of channel 7 (own ring in the old layout, the shared ring 2 in gsistart=p2late):
 * hand the frame to the utun and re-post the buffer.  Returns 1 if the buffer could not be re-posted. */
static unsigned long long rx_bytes;
static int rx_event(const struct gsi_evt *e)
{
	if (e->type != EVT_CBCM && e->type != EVT_XFER) {
		LOG("rx evt: type %02x code %02x ch %u len %u", e->type, e->code, e->chid, e->len);
		ctx.rx_err++;
		return 0;
	}
	if (e->type == EVT_CBCM && e->code != EVT_CODE_DATA && e->code != EVT_CODE_THROW)
		LOG("rx evt: unexpected code %02x len %u", e->code, e->len);
	if (e->type == EVT_XFER && e->code != 0x01 && e->code != 0x02 && e->code != 0x04)
		LOG("rx evt: xfer code %02x len %u", e->code, e->len);
	unsigned idx = ctx.rx_done % RX_BUFS;

	ctx.rx_done++;
	if (e->len > RX_BUF_SZ) {
		LOG("rx len %u over buffer", e->len);
		ctx.rx_err++;
	} else if (e->len) {
		rx_bytes += e->len;
		if (pm_rd(ctx.rx_buf_pa[idx], bounce, e->len))
			LOG("rx buffer read failed: %s", strerror(errno));
		else
			qmap_deagg(bounce, e->len, rx_to_utun, NULL, &ctx.rx_st);
	}
	if (rx_post_one(idx))
		return 1;
	ch_doorbell(CH_MDM_RX);
	return 0;
}

/* take everything the modem has finished on channel 7 and hand it to the utun.  Completions
 * come back in the order the TREs were written, so the next buffer is just the next index. */
static void rx_drain(void)
{
	struct gsi_evt e;
	int seen = 0;

	for (;;) {
		int r = ev_read(EV_RX, &e);

		if (r <= 0) {
			if (r < 0)
				ctx.rx_err++;
			break;
		}
		seen++;
		if (rx_event(&e))
			break;
	}
	if (seen > 12)
		LOG("rx drain looped %d events", seen);
}

/* one utun packet to a QMAP frame on channel 2.  TX buffers form a ring: we never post more
 * than TX_BUFS at a time, and their events release them in order. */
#define TX_BUFS		32

static uint64_t tx_buf_pa[TX_BUFS];
static uint32_t tx_wp, tx_done;

static int tx_drain(void)
{
	struct gsi_evt e;

	while (ev_read(EV_TX, &e) == 1) {
		if (e.type != EVT_CBCM || e.chid != CH_MDM_TX)
			LOG("tx evt: type %02x code %02x ch %u len %u", e.type, e.code, e.chid, e.len);
		tx_done++;
	}
	return 0;
}

static int tx_send(const uint8_t *ip, size_t len)
{
	size_t n = qmap_frame(tx_frame, sizeof(tx_frame), ip, len, 1);
	uint32_t idx = tx_wp % TX_BUFS;

	if (!n) {
		ctx.tx_st.bad++;
		return -1;
	}
	if (!tx_buf_pa[idx]) {
		tx_buf_pa[idx] = ring_mem(TX_CAP, 4096);
		if (!tx_buf_pa[idx]) {
			LOG("tx buffer pm_alloc failed");
			return -1;
		}
	}
	if (pm_wr(tx_buf_pa[idx], tx_frame, lo_on ? (n + 3) & ~(size_t) 3 : n)) {
		LOG("tx buffer write failed: %s", strerror(errno));
		return -1;
	}
	if (ch_post_tre(CH_MDM_TX, tx_buf_pa[idx], (uint32_t) n, 1))
		return -1;
	tx_wp++;
	ch_doorbell(CH_MDM_TX);
	ctx.tx_pkts++;
	return 0;
}

/* utun -> the IPA.  One read = one whole packet with its 4-byte AF prefix.  Only IPv4 goes
 * out: the WDA data format for this call is IPv4 (WDS's 0x20 was sent with PDP type 1), and
 * the modem has no IPv6 endpoint set up. */
static void tx_pump(void)
{
	static uint8_t pkt[2048];

	if (ctx.utun_fd < 0)
		return;
	while (tx_wp - tx_done < TX_BUFS) {
		ssize_t n = read(ctx.utun_fd, pkt, sizeof(pkt));
		uint32_t af;

		if (n <= 0) {
			if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && ctx.tx_err++ < 8)
				LOG("utun read: %s", strerror(errno));
			return;
		}
		if ((size_t) n < 4 + 20) {
			ctx.tx_st.bad++;
			continue;
		}
		memcpy(&af, pkt, 4);
		if (ntohl(af) != AF_INET) {
			ctx.tx_other++;
			continue;
		}
		if (tx_send(pkt + 4, (size_t) n - 4))
			return;
	}
}

/* ------------------------------------------------------------------ utun */

static int utun_up(void)
{
	unsigned mtu = ctx.call.mtu ? ctx.call.mtu : 1500;

	ctx.utun_fd = ipnet_utun_open(ctx.utun_name, sizeof(ctx.utun_name));
	if (ctx.utun_fd < 0) {
		LOG("utun open failed");
		return -1;
	}
	if (ipnet_utun_set(ctx.utun_name, ctx.call.ip, ctx.call.gw, ctx.call.mask, mtu)) {
		LOG("utun %s configure failed", ctx.utun_name);
		close(ctx.utun_fd);
		ctx.utun_fd = -1;
		return -1;
	}
	char a_ip[16], a_gw[16], a_mask[16];	/* inet_ntoa() shares one buffer: three in one LOG printed the last */
	inet_ntop(AF_INET, &ctx.call.ip, a_ip, sizeof(a_ip));
	inet_ntop(AF_INET, &ctx.call.gw, a_gw, sizeof(a_gw));
	inet_ntop(AF_INET, &ctx.call.mask, a_mask, sizeof(a_mask));
	LOG("utun %s up: ip %s gw %s mask %s mtu %u", ctx.utun_name, a_ip, a_gw, a_mask, mtu);
	return 0;
}

/* ------------------------------------------------------------------ start, stop, poll
 *
 * One setup attempt per wland run: ipagsi_stop() only closes the polling and the utun (ipagsi.h:
 * the GSI rings, channels and IPA endpoints are left as they are), so a second ipagsi_start() in
 * the same process would program rings over the ones it already allocated.  Refuse it and say why.
 */

static int ever_ran;		/* setup has run once, successfully or not */
static int phase0;		/* ipaphase=0 in cell.on: utun-only plumbing mode, no GSI/IPA access at all */
static unsigned long long p0_bytes;
static unsigned p0_v4, p0_v6, p0_bad;

/* ipaphase=0 in cell.on picks the utun-only mode at run time (the ws-n-ipa-plumb test, 2026-11-06);
 * anything else runs the compiled IPAGSI_PHASE */
static int read_phase0(void)
{
	char b[512] = "";
	FILE *f = fopen(CELL_ON, "r");
	if (f) {
		size_t k = fread(b, 1, sizeof(b) - 1, f);
		b[k] = 0;
		fclose(f);
	}
	return strstr(b, "ipaphase=0") != NULL;
}

/* gsistart=<mode> in cell.on (phase 1 only) splits the GSI experiment, because START of channel 7
 * has stalled and the modem has asserted in every run so far:
 *   alloc   - allocate, never START (does the modem object to the allocation alone?)
 *   tx      - START channel 2 only        rx - START channel 7 only
 *   rxfirst - START 7 then 2              (absent: 2 then 7, the old behaviour)
 * The 2026-11-06 gsistart=alloc run showed the modem asserts ~12 s after the ALLOCATEs alone, so
 * these split the allocation (nothing is ever STARTed in them):
 *   none    - read-only GSI/IPA access, allocate nothing (control)
 *   evonly  - event rings 2 and 3 only         ch2 / ch7 - one event ring + one channel only
 * 2026-11-06 b64b047: none, evonly, ch2, ch7 ran clean, alloc (ch2+ch7, ev2+ev3) crashed the modem 9 s
 * after setup again, so the PAIR is the trigger.  These probe the pair (still no START):
 *   rev      - allocate in reverse order (ev3, ch7, then ev2, ch2)
 *   onering  - both channels on event ring 2 (event ring 3 never allocated)
 *   free     - allocate both, then release everything at once (DE_ALLOC)
 *   freelate - allocate both, release everything from ipagsi_poll 6 s later (before the ~9 s assert)
 *   evother  - ch2 on event ring 0, ch7 on event ring 1 (not the 2/3 of mainline's v4.7 table)
 *   drop2 / drop7 - allocate both, then release only that channel and its event ring at once
 * f8e42d9 onering ran clean (78 s): ev2+ch2+ch7 fine, ev2+ev3+ch2(ev2)+ch7(ev3) crashes. Either two
 * distinct rings IN USE, or just FOUR contexts allocated.  Told apart by:
 *   onering3 - ev2 + ev3 allocated, ch2 AND ch7 both on ev2 (ev3 unused): four allocations, one mapping
 *   swap     - ch2 on ev3, ch7 on ev2 (both rings in use, mapping swapped vs. mainline's table)
 * 5e71b26: onering3 clean, swap crashed (+8.8 s): the trigger is two channels reporting to two DIFFERENT
 * event rings; two channels on ONE ring were clean in every run (onering, onering3).  So:
 *   onestart - ev2 only, ch2[2] and ch7[2], then START ch2 and ch7 (tx first), park with no ring traffic.
 *   45d6f8c: onestart: both STARTs reach "started" on the shared ring (the old ch 7 stall is gone), but the
 *   modem asserts 9.3 s after up.  So START upsets it too.  Which one?  (shared ring, nothing else changed)
 * ead4c88: c2start, c7start clean; onetx crashed (+8.9 s).  Matrix: one channel (alloc or START) clean; two
 * channels crash with two rings, or with one ring once one is STARTed; two channels on one ring, not started, clean.
 * Our channel ids 2 and 7 are the SAME ids the modem EE uses for its own channels in mainline's ipa_data-v4.7.c
 * (modem ch 0, 2, 7), and the crash needs both of them live.  Test whether the ids matter:
 *   ids34    - ev2 only, channels 3 (tx) and 4 (rx) on it instead of 2 and 7, START both (the `onestart` shape).
 *              Clean here while `onestart` crashed => the modem objects to AP channels 2+7 specifically.
 *   onetx    - ev2, ch2[2], ch7[2] allocated, START ch2 only
 *   onerx    - same allocation, START ch7 only
 *   c2start  - ev2 + ch2 only (ch7 never allocated), START ch2
 *   c7start  - ev3 + ch7 only (ch2 never allocated), START ch7
 *              Does a shared event ring also let both channels START?  (Mainline uses one ring per
 *              channel; a shared ring needs the chid field of the event to demultiplex.)
 * Any mode but the default stops after the starts and parks in state "up" without touching the
 * rings (ipagsi_poll returns early), so the call stays held and the modem can be observed. */
enum { GS_BOTH, GS_ALLOC, GS_TX, GS_RX, GS_RXFIRST, GS_NONE, GS_EVONLY, GS_CH2, GS_CH7,
       GS_REV, GS_ONERING, GS_FREE, GS_FREELATE, GS_EVOTHER, GS_DROP2, GS_DROP7, GS_ONERING3, GS_SWAP, GS_ONESTART,
       GS_ONETX, GS_ONERX, GS_C2START, GS_C7START, GS_IDS34, GS_TLVMIN, GS_LATE, GS_TLVBIG, GS_EV4, GS_CH3, GS_EV01, GS_EV4HI, GS_EEDUMP, GS_C2JOIN, GS_C2JOINT, GS_P2LATE };
static int gsitest;
static uint64_t dump_at;		/* gsistart=eedump: next EE1 register dump */
static int dump_n, dump_keep;
static int join_pending;		/* gsistart=c2join: 2nd channel (ch 7 rx) still to be added */
static uint64_t join_force;		/* gsistart=c2joint: join at this time even if the modem never cycled (0 = off) */
static uint64_t join_seen;		/* when EE1 ch 7 was first seen started */
static uint64_t free_at;		/* gsistart=freelate: when ipagsi_poll releases the rings */

static int read_eedump_flag(void)
{
	char b[512] = "";
	FILE *f = fopen(CELL_ON, "r");
	if (f) {
		size_t k = fread(b, 1, sizeof(b) - 1, f);
		b[k] = 0;
		fclose(f);
	}
	return strstr(b, "eedump=1") != NULL;
}

static int read_gsistart(void)
{
	char b[512] = "";
	FILE *f = fopen(CELL_ON, "r");
	if (f) {
		size_t k = fread(b, 1, sizeof(b) - 1, f);
		b[k] = 0;
		fclose(f);
	}
	if (strstr(b, "gsistart=alloc"))
		return GS_ALLOC;
	if (strstr(b, "gsistart=none"))
		return GS_NONE;
	if (strstr(b, "gsistart=tlvmin"))
		return GS_TLVMIN;
	if (strstr(b, "gsistart=eedump"))
		return GS_EEDUMP;
	if (strstr(b, "gsistart=ev4hi"))
		return GS_EV4HI;
	if (strstr(b, "gsistart=ev01"))
		return GS_EV01;
	if (strstr(b, "gsistart=ev4"))
		return GS_EV4;
	if (strstr(b, "gsistart=ch3"))
		return GS_CH3;
	if (strstr(b, "gsistart=tlvbig"))
		return GS_TLVBIG;
	if (strstr(b, "gsistart=late"))
		return GS_LATE;
	if (strstr(b, "gsistart=ids34"))
		return GS_IDS34;
	if (strstr(b, "gsistart=onetx"))
		return GS_ONETX;
	if (strstr(b, "gsistart=onerx"))
		return GS_ONERX;
	if (strstr(b, "gsistart=p2late"))
		return GS_P2LATE;
	if (strstr(b, "gsistart=c2joint"))
		return GS_C2JOINT;
	if (strstr(b, "gsistart=c2join"))
		return GS_C2JOIN;
	if (strstr(b, "gsistart=c2start"))
		return GS_C2START;
	if (strstr(b, "gsistart=c7start"))
		return GS_C7START;
	if (strstr(b, "gsistart=onestart"))
		return GS_ONESTART;
	if (strstr(b, "gsistart=onering3"))
		return GS_ONERING3;
	if (strstr(b, "gsistart=swap"))
		return GS_SWAP;
	if (strstr(b, "gsistart=evother"))
		return GS_EVOTHER;
	if (strstr(b, "gsistart=drop2"))
		return GS_DROP2;
	if (strstr(b, "gsistart=drop7"))
		return GS_DROP7;
	if (strstr(b, "gsistart=freelate"))
		return GS_FREELATE;
	if (strstr(b, "gsistart=free"))
		return GS_FREE;
	if (strstr(b, "gsistart=rev"))
		return GS_REV;
	if (strstr(b, "gsistart=onering"))
		return GS_ONERING;
	if (strstr(b, "gsistart=evonly"))
		return GS_EVONLY;
	if (strstr(b, "gsistart=ch2"))
		return GS_CH2;
	if (strstr(b, "gsistart=ch7"))
		return GS_CH7;
	if (strstr(b, "gsistart=rxfirst"))
		return GS_RXFIRST;
	if (strstr(b, "gsistart=tx"))
		return GS_TX;
	if (strstr(b, "gsistart=rx"))
		return GS_RX;
	return GS_BOTH;
}

/* phase 0: drain the utun so the stack doesn't queue up; count and drop */
static void p0_drain(void)
{
	static uint8_t pkt[2048];
	for (int i = 0; i < 64 && ctx.utun_fd >= 0; i++) {
		ssize_t n = read(ctx.utun_fd, pkt, sizeof(pkt));
		uint32_t af;
		if (n < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				ctx.tx_err++;
			return;
		}
		if (n <= 4)
			return;
		memcpy(&af, pkt, 4);
		af = ntohl(af);
		if (af == AF_INET)
			p0_v4++;
		else if (af == AF_INET6)
			p0_v6++;
		else {
			p0_bad++;
			continue;
		}
		ctx.tx_pkts++;
		p0_bytes += (unsigned long long)(n - 4);
	}
}
static int setup_step;		/* the setup-N we are in (0 before setup-1) */
static char stbuf[16];		/* the state string that carries the step number */

static void set_state(const char *s)
{
	if (!strncmp(s, "setup-", 6))
		setup_step = atoi(s + 6);
	if (ctx.state != s && strcmp(ctx.state, s))
		LOG("state %s -> %s", ctx.state, s);
	ctx.state = s;
}

static void fail_state(void)
{
	snprintf(stbuf, sizeof(stbuf), "failed-%d", setup_step);
	set_state(stbuf);
}

/* ------------------------------------------------------------------ gsistart=p2late
 * A real data path that respects the modem's limit (BRIEF-ipagsi-phase2-latejoin-2026-11-07.md):
 *  1. event ring 2 (128 entries, shared) + TX channel 2, started: 3 units, the c2start shape
 *  2. wait until the modem has finished its own channel 4-7 start (EE1 ch 7 started for 5 s), or p2join=<s>
 *     (default 0 = forever: a join before the modem's cycle crashes it at that cycle, c2joint-r3)
 *  3. join: RX channel 7 on ring 2, started; endpoints checked (read only, see ipagsi_endpoints); RX buffers
 *     posted; utun opened; TX from the utun
 * Words in cell.on: p2tx=1 (TX channel only, never join: first proof that the modem takes a packet),
 * p2join=<s>, p2ep=before|after (when ipagsi_endpoints() runs, default after the join).
 * One reader for ring 2 tells TX and RX events apart by the channel id.  No channel 5, no tables. */
static int p2_stage;			/* 0 off, 1 waiting for the modem, 2 running */
static int p2_tx_only, p2_ep_before, p2_rec_done, p2_chdefer, p2_irqonly;
static long p2_rec_s = -1;
static int p2_cmd;			/* p2cmd=1: command channel test (below) */
static int p2_cmd_setup(void);
static void p2_irq_setup(void);
static int p2_tx_recreate(void);
static unsigned p2_join_s;		/* 0 = wait forever (default) */
static uint64_t p2_t0, p2_seen, p2_stat;
static unsigned p2_txev, p2_rxev, p2_unk;

static int cell_word_int(const char *key, int def)
{
	char b[512] = "";
	FILE *f = fopen(CELL_ON, "r");
	char *at;

	if (f) {
		size_t k = fread(b, 1, sizeof(b) - 1, f);
		b[k] = 0;
		fclose(f);
	}
	at = strstr(b, key);
	if (!at)
		return def;
	return atoi(at + strlen(key));
}

/* key=<number> (decimal, or 0x hex) from cell.on; def when absent */
static long cell_word_num(const char *key, long def)
{
	char b[512] = "";
	FILE *f = fopen(CELL_ON, "r");
	char *at;

	if (f) {
		size_t k = fread(b, 1, sizeof(b) - 1, f);
		b[k] = 0;
		fclose(f);
	}
	at = strstr(b, key);
	return at ? strtol(at + strlen(key), NULL, 0) : def;
}

static int cell_word_has(const char *w)
{
	char b[512] = "";
	FILE *f = fopen(CELL_ON, "r");

	if (f) {
		size_t k = fread(b, 1, sizeof(b) - 1, f);
		b[k] = 0;
		fclose(f);
	}
	return strstr(b, w) != NULL;
}

/* TX posted but not completed: what do the GSI channel/event ring and the first TRE + buffer look like? */
/* one-shot dump of the IPA global block and the GSI global block (non-zero lines only) */
static void p2_regdump(const char *what, uint64_t base, uint32_t from, uint32_t to)
{
	for (uint32_t o = from; o < to; o += 32) {
		uint32_t w[8], any = 0;

		for (int i = 0; i < 8; i++) {
			w[i] = rd32(base + o + 4 * i);
			any |= w[i];
		}
		if (any)
			LOG("%s +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", what, o, w[0], w[1], w[2], w[3], w[4], w[5],
			    w[6], w[7]);
	}
}

static void p2_diag(const char *why)
{
	struct gsi_tre t;
	uint8_t b[16] = { 0 };
	uint64_t cb = GSI_CH_C_CNTXT_0(CH_MDM_TX), eb = GSI_EV_CH_E_CNTXT_0(EV_TX);
	static int dumped;

	if (dumped < 2 && ctx.ch[CH_MDM_TX].wp) {
		dumped++;	/* after the first post, and once more later (compare: IPA state registers change?) */
		p2_regdump("p2 ipa", IPA_BASE, 0, 0x240);
		p2_regdump("p2 gsi", GSI_BASE, 0, 0x200);
		if (dumped == 1) {	/* full channel/event contexts: modem's EE1 (working) next to ours (EE0) */
			char nm[24];

			for (int i = 0; i < 8; i++) {
				snprintf(nm, sizeof(nm), "p2 e1ch%d", i);
				p2_regdump(nm, GSI_CH_C_CNTXT_0(i) + 0x4000, 0, 0x80);
				snprintf(nm, sizeof(nm), "p2 e1ev%d", i);
				p2_regdump(nm, GSI_EV_CH_E_CNTXT_0(i) + 0x4000, 0, 0x80);
			}
			/* wider look (non-zero lines only): GSI debug block, rest of the IPA global block, ep2's block tail */
			p2_regdump("p2 gdbg", GSI_BASE, 0x5000, 0x5800);
			p2_regdump("p2 ipa2", IPA_BASE, 0x240, 0x800);
			p2_regdump("p2 ipaep2", IPA_BASE, 0x8e0, 0x960);
			p2_regdump("p2 e0ch2", GSI_CH_C_CNTXT_0(CH_MDM_TX), 0, 0x80);
			p2_regdump("p2 e0ev2", GSI_EV_CH_E_CNTXT_0(EV_TX), 0, 0x80);
		}
	}

	LOG("p2 diag (%s): ch2 state %s wp %u (db addr %llx) ctx %08x %08x %08x %08x", why, ch_state_name(ch_state(CH_MDM_TX)),
	    ctx.ch[CH_MDM_TX].wp, (unsigned long long) (ctx.ch[CH_MDM_TX].pa +
	    (uint64_t) (ctx.ch[CH_MDM_TX].wp % ctx.ch[CH_MDM_TX].cnt) * RING_ELEM), rd32(cb), rd32(cb + 4), rd32(cb + 8),
	    rd32(cb + 12));
	LOG("p2 diag (%s): ch2 rp/wp words +10 %08x +14 %08x +18 %08x +1c %08x +20 %08x +24 %08x", why, rd32(cb + 0x10),
	    rd32(cb + 0x14), rd32(cb + 0x18), rd32(cb + 0x1c), rd32(cb + 0x20), rd32(cb + 0x24));
	LOG("p2 diag (%s): ch2 qos %08x scratch %08x %08x %08x %08x", why, rd32(GSI_CH_C_QOS(CH_MDM_TX)),
	    rd32(GSI_CH_C_SCRATCH_0(CH_MDM_TX)), rd32(GSI_CH_C_SCRATCH_1(CH_MDM_TX)), rd32(GSI_CH_C_SCRATCH_2(CH_MDM_TX)),
	    rd32(GSI_CH_C_SCRATCH_3(CH_MDM_TX)));
	LOG("p2 diag (%s): ev2 state %x rp %u ctx %08x %08x %08x %08x", why, ev_state(EV_TX), ctx.ev[EV_TX].rp, rd32(eb),
	    rd32(eb + 4), rd32(eb + 8), rd32(eb + 12));
	LOG("p2 diag (%s): ev2 +10 %08x +14 %08x +18 %08x +1c %08x +20 %08x +24 %08x", why, rd32(eb + 0x10), rd32(eb + 0x14),
	    rd32(eb + 0x18), rd32(eb + 0x1c), rd32(eb + 0x20), rd32(eb + 0x24));
	LOG("p2 diag (%s): ev2 +28 %08x +2c %08x +30 %08x err %08x gsi-irq %08x %08x %08x", why, rd32(eb + 0x28), rd32(eb + 0x2c),
	    rd32(eb + 0x30), rd32(GSI_BASE + 0x12200), rd32(GSI_BASE + 0x12080), rd32(GSI_BASE + 0x12090),
	    rd32(GSI_BASE + 0x12094));
	if (!pm_rd(ctx.ch[CH_MDM_TX].pa, &t, sizeof(t)))
		LOG("p2 diag (%s): TRE[0] addr %llx len %u flags %08x", why, (unsigned long long) t.addr, t.len, t.flags);
	if (tx_buf_pa[0] && !pm_rd(tx_buf_pa[0], b, sizeof(b)))
		LOG("p2 diag (%s): buf0 %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x", why, b[0], b[1], b[2],
		    b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
	ipagsi_dump_ipa();
}

/* Program one AP endpoint the way mainline's ipa_endpoint_program() does for ipa_data-v4.7.c, with checksum
 * offload OFF (our frames carry the 4-byte QMAP header only, as the WDA data format asked for).  The first
 * p2late runs showed ep2/ep16/ep7 at their reset values: the modem does NOT program the AP's endpoints, so
 * nothing moved.  ep2 = AP_MODEM_TX (toward the IPA, QMAP, status to endpoint 14 = MODEM_AP_RX, 2-pass seq),
 * ep16 = AP_MODEM_RX (QMAP header filled in by the IPA, generic aggregation 6 KB / 500 us / close on EOF).
 * p2prog=0 skips it; p2status=0 leaves out the status messages of ep2. */
#define EP_MODEM_AP_RX	14
static void ep_program(int ep, int status)
{
	uint32_t cfg = 0, nat = 0, hdr, ext, mmask = 0, mode = 0, aggr = 0, deaggr = 0, rsrc = 0, seq = 0, st = 0;
	uint32_t rb_hdr, rb_ext, rb_aggr, rb_seq, rb_st, rb_mask;

	if (ep == EP_MDM_TX) {
		hdr = 4 | 1u << 6;		/* HDR_LEN 4, OFST_METADATA_VALID (offset 0) */
		ext = 1;			/* HDR_ENDIANNESS: big endian */
		seq = EP_SEQ_2_PASS_SKIP_LAST_UC;
		if (status)
			st = EP_STATUS_EN | EP_MODEM_AP_RX << EP_STATUS_ENDP_SHIFT;
		if (cell_word_int("p2loop=", 0)) {
			/* p2loop=1 (Claude): RX-side self test without the modem.  ep2 in DMA mode with ep16 as its destination
			 * (as mainline's ep7 -> ep9): every TX packet is copied by the IPA straight to AP_MODEM_RX, so it must
			 * show up on ch7 if ch7/ep16 work.  Seq type DMA (0) as for ep7, no status messages.  Test only. */
			mode = EP_MODE_DMA | (uint32_t) EP_MDM_RX << EP_MODE_DEST_SHIFT;
			seq = 0;
			st = 0;
			LOG("p2: p2loop=1: ep2 DMA -> ep%d (mode %08x), no modem in the path", EP_MDM_RX, mode);
		}
	} else {
		hdr = 4 | 1u << 6 | 1u << 7 | 1u << 19 | 2u << 20;	/* len 4, metadata valid at offset 1 (mux id), pkt size valid at offset 2 */
		ext = 1 | 1u << 1 | 1u << 3;	/* big endian, TOTAL_LEN_OR_PAD_VALID, PAYLOAD_LEN_INC_PADDING (pad align 0) */
		mmask = 0xff000000u;		/* cpu_to_be32(0xff) */
		aggr = 1 | 4u << EP_AGGR_TYPE_SHIFT | 6u << EP_AGGR_BYTE_SHIFT | 5u << EP_AGGR_TIME_SHIFT | EP_AGGR_SW_EOF;
		if (cell_word_int("p2noaggr=", 0)) {
			aggr = 0;	/* p2noaggr=1 (Claude): AGGR_EN = bypass, one packet per RX buffer (test) */
			LOG("p2: p2noaggr=1: ep%d aggregation off", ep);
		}
	}
	wr32(IPA_ENDP_CFG(ep), cfg);
	if (ep == EP_MDM_TX)
		wr32(IPA_ENDP_NAT(ep), nat);
	wr32(IPA_ENDP_HDR(ep), hdr);
	wr32(IPA_ENDP_HDR_EXT(ep), ext);
	if (ep != EP_MDM_TX)
		wr32(IPA_ENDP_HDR_MASK(ep), mmask);
	if (ep == EP_MDM_TX) {
		wr32(IPA_ENDP_MODE(ep), mode);
		wr32(IPA_ENDP_DEAGGR(ep), deaggr);
		wr32(IPA_ENDP_SEQ(ep), seq);
	}
	wr32(IPA_ENDP_AGGR(ep), aggr);
	if (ep != EP_MDM_TX)
		wr32(IPA_ENDP_HOL_EN(ep), 0);
	wr32(IPA_ENDP_RSRC_GRP(ep), rsrc);
	wr32(IPA_ENDP_STATUS(ep), st);
	rb_hdr = rd32(IPA_ENDP_HDR(ep));
	rb_ext = rd32(IPA_ENDP_HDR_EXT(ep));
	rb_mask = rd32(IPA_ENDP_HDR_MASK(ep));
	rb_aggr = rd32(IPA_ENDP_AGGR(ep));
	rb_seq = rd32(IPA_ENDP_SEQ(ep));
	rb_st = rd32(IPA_ENDP_STATUS(ep));
	LOG("p2: ep%d programmed: hdr %08x/%08x ext %08x/%08x mask %08x/%08x aggr %08x/%08x seq %08x/%08x status %08x/%08x"
	    " (wrote/read back)%s", ep, hdr, rb_hdr, ext, rb_ext, mmask, rb_mask, aggr, rb_aggr, seq, rb_seq, st, rb_st,
	    (rb_hdr != hdr || rb_ext != ext || rb_aggr != aggr || (ep == EP_MDM_TX && rb_seq != seq) || rb_st != st) ?
	    "  <- READBACK DIFFERS" : "");
}

static int p2_prog = 1, p2_status = 1;

static int p2_setup(void)
{
	p2_tx_only = cell_word_int("p2tx=", 0);
	p2_rec_s = cell_word_num("p2rec=", -1);
	p2_rec_done = 0;
	p2_join_s = (unsigned) cell_word_int("p2join=", 0);
	p2_ep_before = cell_word_has("p2ep=before");
	p2_prog = cell_word_int("p2prog=", 1);
	p2_status = cell_word_int("p2status=", 1);
	if (read_eedump_flag()) {
		dump_n = -1;	/* until reboot; logs only on change */
		dump_at = link_ms();
	}
	if (cell_word_int("p2low=", 0) && lo_init())
		return -1;
	{
		/* IPA ROUTE register (default route pipe / fragment pipe / retain header): mainline ipa_route_config() writes it
		 * for the AP; ipagsi never did.  Always logged; p2route=<ep> (decimal, e.g. 16) writes default+frag pipe = ep. */
		long rt = cell_word_num("p2route=", -1);

		LOG("p2: IPA ROUTE reg %08x (def pipe %u frag pipe %u retain %u dis %u)", rd32(IPA_BASE + 0x48),
		    rd32(IPA_BASE + 0x48) >> 1 & 31, rd32(IPA_BASE + 0x48) >> 17 & 31, rd32(IPA_BASE + 0x48) >> 24 & 1,
		    rd32(IPA_BASE + 0x48) & 1);
		if (rt >= 0) {
			/* p2routedis=1 (Claude): also set ROUTE_DIS (bit 0), so every packet goes to the default pipe without a
			 * route-table lookup.  A test of the RX side alone: with def pipe 16, anything the IPA routes (our own TX,
			 * the modem's downlink) should land on our RX ring if ch7/ep16 work.  Global register: test only. */
			uint32_t dis = cell_word_int("p2routedis=", 0) ? 1u : 0u;

			wr32(IPA_BASE + 0x48, (uint32_t) rt << 1 | (uint32_t) rt << 17 | 1u << 24 | dis);
			LOG("p2: p2route=%ld%s: ROUTE reg -> %08x", rt, dis ? " p2routedis=1" : "", rd32(IPA_BASE + 0x48));
		}
	}
	p2_chdefer = 0;
	p2_irqonly = 0;
	if (cell_word_int("p2irq=", 0) == 5) {
		/* p2irq=5: ONLY the irq config, no GSI command at all (no ev/ch alloc), no join: does the config alone make the
		 * modem's start blip?  Watch EE1 with eedump=1. */
		p2_irq_setup();
		p2_irqonly = 1;
		p2_stage = 1;
		p2_t0 = link_ms();
		p2_seen = 0;
		p2_stat = link_ms();
		set_state("p2-wait");
		LOG("p2: p2irq=5: irq config only, no GSI command, no join");
		ctx.active = 1;
		return 0;
	}
	if (cell_word_int("p2irq=", 0) == 1)	/* p2irq=2: later, at the join (after the modem's start cycle) */
		p2_irq_setup();
	else if (cell_word_int("p2irq=", 0) == 4) {
		/* p2irq=4: irq config before the first EE0 command (what makes ch2 serviced), but ev2 + ch2 are only ALLOCATED
		 * now; ep2 programming and ch2 START wait for the join, so the modem's start sequence sees one fewer live AP unit */
		p2_irq_setup();
		p2_chdefer = !p2_tx_only;
	}
	p2_cmd = cell_word_int("p2cmd=", 0);
	if (p2_cmd)
		return p2_cmd_setup();
	set_state("setup-2");
	if (ev_alloc_program(EV_TX, 128) || ch_alloc_program(CH_MDM_TX, EV_TX, TX_TRE_CNT, 1, 0, tlv_tx))
		return -1;
	{
		long q = cell_word_num("p2qos=", -1);	/* experiment: override ch 2's QOS register (prefetch mode etc.) */

		if (q >= 0) {
			LOG("p2: ch2 QOS %08x -> %08lx (p2qos=)", rd32(GSI_CH_C_QOS(CH_MDM_TX)), q);
			wr32(GSI_CH_C_QOS(CH_MDM_TX), (uint32_t) q);
		}
	}
	{
		/* experiments (all before ch_start): modem-like event ring moderation / channel scratch words */
		long v = cell_word_num("p2evmod=", -1);

		if (v >= 0) {
			LOG("p2: ev2 CNTXT_8 %08x -> %08lx (p2evmod=)", rd32(GSI_EV_CH_E_CNTXT_8(EV_TX)), v);
			wr32(GSI_EV_CH_E_CNTXT_8(EV_TX), (uint32_t) v);
		}
		for (int i = 0; i < 4; i++) {
			char w[16];

			snprintf(w, sizeof(w), "p2scr%d=", i);
			v = cell_word_num(w, -1);
			if (v >= 0) {
				LOG("p2: ch2 SCRATCH_%d %08x -> %08lx (%s)", i, rd32(GSI_CH_C_SCRATCH_0(CH_MDM_TX) + 4 * i), v, w);
				wr32(GSI_CH_C_SCRATCH_0(CH_MDM_TX) + 4 * i, (uint32_t) v);
			}
		}
	}
	set_state("setup-3");
	if (p2_chdefer) {
		LOG("p2: p2irq=4: ev2 + ch2 allocated only (ch2 %s); ep2 + START at the join", ch_state_name(ch_state(CH_MDM_TX)));
	} else {
		if (p2_prog) {
			ep_program(EP_MDM_TX, p2_status);	/* before the channel is started, as mainline */
			if (p2_ep_before && !p2_tx_only)
				ep_program(EP_MDM_RX, 0);
		}
		if (ch_start(CH_MDM_TX))
			return -1;
	}
	LOG("p2: tx ch up (ev2 + ch2 %s: 3 units); tx_only %d join timeout %u s endpoints %s",
	    p2_chdefer ? "allocated" : "started", p2_tx_only, p2_join_s, p2_ep_before ? "before" : "after");
	if (p2_ep_before || p2_tx_only)
		ipagsi_endpoints();	/* read only */
	p2_t0 = link_ms();
	p2_seen = 0;
	p2_stat = link_ms();
	if (p2_tx_only) {
		if (utun_up())
			return -1;
		p2_stage = 2;
		set_state("up");
		LOG("p2: tx-only running, utun %s", ctx.utun_name);
	} else {
		p2_stage = 1;
		set_state("p2-wait");
		LOG("p2: waiting for modem");
	}
	ctx.active = 1;
	return 0;
}

static void p2_drain(void)
{
	struct gsi_evt e;

	while (ev_read(EV_TX, &e) == 1) {
		if (e.chid == CH_MDM_TX) {
			if (e.type != EVT_CBCM)
				LOG("p2 tx evt: type %02x code %02x len %u", e.type, e.code, e.len);
			tx_done++;
			p2_txev++;
		} else if (e.chid == CH_MDM_RX) {
			p2_rxev++;
			if (rx_event(&e))
				break;
		} else {
			if (p2_unk++ < 8)
				LOG("p2: event for unknown ch %u type %02x code %02x len %u", e.chid, e.type, e.code, e.len);
		}
	}
}

/* p2cmd=1: command-channel test.  ev2 + ch5 (AP_COMMAND_TX, ep7) only (3 units, as safe as p2tx), one immediate
 * command (IP_PACKET_INIT, dest ep2: harmless, only affects the next data packet on ch5) and a look at whether
 * ch5's RP moves / an event arrives.  If ch5 completes, the GSI->IPA TX path works and ch2's stall is specific to
 * that pipe (routing/pipe config); if ch5 stalls too, the GSI or IPA core is not processing our channels at all. */
static uint64_t p2_cmd_buf, p2_cmd_t;
static unsigned p2_cmd_n;

static void p2_cmd_dump(const char *why)
{
	uint64_t cb = GSI_CH_C_CNTXT_0(CH_CMDTX), eb = GSI_EV_CH_E_CNTXT_0(EV_TX);

	LOG("p2cmd (%s): ch5 state %s ctx %08x %08x %08x %08x rp %08x/%08x wp %08x/%08x qos %08x", why,
	    ch_state_name(ch_state(CH_CMDTX)), rd32(cb), rd32(cb + 4), rd32(cb + 8), rd32(cb + 12), rd32(cb + 0x10),
	    rd32(cb + 0x14), rd32(cb + 0x18), rd32(cb + 0x1c), rd32(GSI_CH_C_QOS(CH_CMDTX)));
	LOG("p2cmd (%s): ch5 scratch %08x %08x %08x %08x | ev2 state %x rp %08x wp %08x err %08x", why,
	    rd32(GSI_CH_C_SCRATCH_0(CH_CMDTX)), rd32(GSI_CH_C_SCRATCH_1(CH_CMDTX)), rd32(GSI_CH_C_SCRATCH_2(CH_CMDTX)),
	    rd32(GSI_CH_C_SCRATCH_3(CH_CMDTX)), ev_state(EV_TX), rd32(eb + 0x18), rd32(eb + 0x20), rd32(GSI_BASE + 0x12200));
}

static void p2_cmd_post(void)
{
	struct gsi_ring *r = &ctx.ch[CH_CMDTX];
	struct gsi_tre tre;
	uint32_t idx = r->wp % r->cnt;

	tre.addr = p2_cmd_buf;
	tre.len = 0x10;			/* IPA_CMD_IP_PACKET_INIT */
	tre.resv = 0;
	tre.flags = TRE_IMMD_CMD | TRE_IEOT;
	if (pm_wr(r->pa + (uint64_t) idx * RING_ELEM, &tre, sizeof(tre))) {
		LOG("p2cmd: tre write failed: %s", strerror(errno));
		return;
	}
	r->wp++;
	ch_doorbell(CH_CMDTX);
	LOG("p2cmd: posted command %u (IP_PACKET_INIT dest ep2), buf %llx, wp %u", r->wp, (unsigned long long) p2_cmd_buf,
	    r->wp);
}

/* p2irq=1: what mainline's gsi_irq_setup() + gsi_irq_enable() write for the AP EE before any channel exists
 * (we have never written any of it: GSI_CFG etc. are the firmware's).  EE0 registers only.  Polled: no handler. */
static void p2_irq_setup(void)
{
	static const struct { const char *n; uint32_t off; } r[] = {
		{ "INTSET", 0x12180 }, { "TYPE_IRQ_MSK", 0x12088 }, { "SRC_CH_IRQ_MSK", 0x12098 },
		{ "SRC_EV_CH_IRQ_MSK", 0x1209c }, { "GLOB_IRQ_EN", 0x12108 }, { "SRC_IEOB_IRQ_MSK", 0x120b8 },
		{ "GSI_IRQ_EN", 0x12120 }, { "GLOB_IRQ_STTS", 0x12100 }, { "GSI_IRQ_STTS", 0x12118 },
		{ "TYPE_IRQ", 0x12080 }, { "SRC_CH_IRQ", 0x12090 }, { "SRC_EV_CH_IRQ", 0x12094 }, { "SRC_IEOB_IRQ", 0x120b0 },
		{ "IEE_CH_MSK", 0xc020 }, { "IEE_EV_MSK", 0xc024 },
	};
	char line[520];
	int n = 0;

	for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); i++)
		n += snprintf(line + n, sizeof(line) - n, " %s %08x", r[i].n, rd32(GSI_BASE + r[i].off));
	LOG("p2irq: before:%s", line);
	/* p2irqm=<mask> selects which groups are written (default 0x1f = all, as before):
	 * bit0 INTSET=1, bit1 clear TYPE/SRC_CH/SRC_EV/IEOB masks, bit2 GLOB_IRQ_EN=1, bit3 TYPE_IRQ_MSK=GLOB_EE|GENERAL, bit4 GSI_IRQ_EN=0xe, bit5 inter-EE masks=0 (default off: 0x1f) */
	uint32_t m = (uint32_t) cell_word_int("p2irqm=", 0x1f);

	LOG("p2irq: groups %02x", m);
	if (cell_word_int("p2ack=", 0)) {
		/* acknowledge whatever is pending for EE0 (a polled client never ran an irq handler: the GSI may be
		 * waiting for these to be cleared before it services our channels).  Status regs read, CLR regs written. */
		uint32_t ch = rd32(GSI_BASE + 0x12090), evc = rd32(GSI_BASE + 0x12094), ieob = rd32(GSI_BASE + 0x120b0);
		uint32_t glob = rd32(GSI_BASE + 0x12100), gen = rd32(GSI_BASE + 0x12118);

		LOG("p2ack: pending ch %08x ev %08x ieob %08x glob %08x gsi %08x", ch, evc, ieob, glob, gen);
		wr32(GSI_BASE + 0x120a0, ch);		/* SRC_CH_IRQ_CLR */
		wr32(GSI_BASE + 0x120a4, evc);		/* SRC_EV_CH_IRQ_CLR */
		wr32(GSI_BASE + 0x120c0, ieob);		/* SRC_IEOB_IRQ_CLR */
		wr32(GSI_BASE + 0x12110, glob);		/* GLOB_IRQ_CLR */
		wr32(GSI_BASE + 0x12128, gen);		/* GSI_IRQ_CLR */
		LOG("p2ack: after ch %08x ev %08x ieob %08x glob %08x gsi %08x type %08x", rd32(GSI_BASE + 0x12090),
		    rd32(GSI_BASE + 0x12094), rd32(GSI_BASE + 0x120b0), rd32(GSI_BASE + 0x12100), rd32(GSI_BASE + 0x12118),
		    rd32(GSI_BASE + 0x12080));
	}
	if (m & 1)
		wr32(GSI_BASE + 0x12180, 1);		/* INTSET: IRQ (not MSI) */
	if (m & 2) {
		wr32(GSI_BASE + 0x12088, 0);		/* TYPE_IRQ_MSK: all types off */
		wr32(GSI_BASE + 0x12098, 0);		/* SRC_CH_IRQ_MSK */
		wr32(GSI_BASE + 0x1209c, 0);		/* SRC_EV_CH_IRQ_MSK */
		wr32(GSI_BASE + 0x120b8, 0);		/* SRC_IEOB_IRQ_MSK */
	}
	if (m & 4) {
		wr32(GSI_BASE + 0x12108, 0);		/* GLOB_IRQ_EN */
		wr32(GSI_BASE + 0x12108, 1);		/* GLOB_IRQ_EN: ERROR_INT */
	}
	if (m & 16) {
		wr32(GSI_BASE + 0x12120, 0);		/* GSI_IRQ_EN */
		wr32(GSI_BASE + 0x12120, 0xe);		/* BUS_ERROR | CMD_FIFO_OVRFLOW | MCS_STACK_OVRFLOW */
	}
	if (m & 8)
		wr32(GSI_BASE + 0x12088, 1u << 2 | 1u << 6);	/* TYPE_IRQ_MSK: GLOB_EE + GENERAL */
	if (m & 32) {
		/* p2irqm bit5: inter-EE source masks (the other EEs' channel/event irqs towards EE0), as HakiOS gsi_interrupts_off() */
		wr32(GSI_BASE + 0xc020, 0);		/* INTER_EE_SRC_CH_IRQ_MSK (EE0) */
		wr32(GSI_BASE + 0xc024, 0);		/* INTER_EE_SRC_EV_CH_IRQ_MSK (EE0) */
	}
	n = 0;
	for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); i++)
		n += snprintf(line + n, sizeof(line) - n, " %s %08x", r[i].n, rd32(GSI_BASE + r[i].off));
	LOG("p2irq: after: %s", line);
}

/* p2irq=3: at the join, do the irq setup, then tear ch2 + ev2 down (STOP, RESET, DE_ALLOC) and set them up again, so
 * the GSI sees the irq config when the channel is allocated/started (p2irq=1 does that at setup and ch2 is serviced,
 * but then the modem's start blips; p2irq=2 holds the modem start but ch2 is not serviced).  3 units before and after. */
static int p2_tx_recreate(void)
{
	uint64_t t0;

	p2_irq_setup();
	tx_wp = tx_done = 0;
	LOG("p2: recreate ch2/ev2: ch2 %s ev2 %x", ch_state_name(ch_state(CH_MDM_TX)), ev_state(EV_TX));
	/* mainline (gsi_channel_stop_retry) re-issues STOP while the channel is in stop-in-proc: up to 10 times, 1 ms apart */
	int maxtry = cell_word_int("p2stopn=", 1);

	for (int tries = 0; tries < maxtry && ch_state(CH_MDM_TX) != CH_ST_STOPPED; tries++) {
		wr32(GSI_CH_CMD, CH_CMD_STOP << 24 | CH_MDM_TX);
		for (t0 = link_ms(); ch_state(CH_MDM_TX) != CH_ST_STOPPED && link_ms() - t0 < 20;)
			delay_ms(1);
		if (tries && ch_state(CH_MDM_TX) == CH_ST_STOPPED)
			LOG("p2: recreate: stop done after %d retries", tries);
	}
	if (ch_state(CH_MDM_TX) != CH_ST_STOPPED) {
		LOG("p2: recreate: ch2 stop failed, state %s", ch_state_name(ch_state(CH_MDM_TX)));
		return -1;
	}
	wr32(GSI_CH_CMD, CH_CMD_RESET << 24 | CH_MDM_TX);
	for (t0 = link_ms(); ch_state(CH_MDM_TX) != CH_ST_ALLOCATED && link_ms() - t0 < 300;)
		delay_ms(1);
	if (ch_state(CH_MDM_TX) != CH_ST_ALLOCATED) {
		LOG("p2: recreate: ch2 reset failed, state %s", ch_state_name(ch_state(CH_MDM_TX)));
		return -1;
	}
	ch_release(CH_MDM_TX);
	ev_release(EV_TX);
	if (ch_state(CH_MDM_TX) != CH_ST_NOT_ALLOC || ev_state(EV_TX) != EV_ST_NOT_ALLOC) {
		LOG("p2: recreate: release incomplete (ch2 %s, ev2 %x)", ch_state_name(ch_state(CH_MDM_TX)), ev_state(EV_TX));
		return -1;
	}
	if (ev_alloc_program(EV_TX, 128) || ch_alloc_program(CH_MDM_TX, EV_TX, TX_TRE_CNT, 1, 0, tlv_tx))
		return -1;
	if (p2_prog)
		ep_program(EP_MDM_TX, p2_status);
	if (ch_start(CH_MDM_TX))
		return -1;
	LOG("p2: recreate done: ch2 %s ev2 %x", ch_state_name(ch_state(CH_MDM_TX)), ev_state(EV_TX));
	return 0;
}

static int p2_cmd_setup(void)
{
	uint8_t pl[8] = { EP_MDM_TX, 0, 0, 0, 0, 0, 0, 0 };

	set_state("setup-2");
	if (ev_alloc_program(EV_TX, 128) || ch_alloc_program(CH_CMDTX, EV_TX, CMD_TRE_CNT, 1, 1, TLV_CMDTX))
		return -1;
	{
		long q = cell_word_num("p2qos=", -1);	/* p2qos= also applies to ch5 here (mainline gives it WRR 7 and prefetch mode 0) */

		if (q >= 0) {
			LOG("p2cmd: ch5 QOS %08x -> %08lx (p2qos=)", rd32(GSI_CH_C_QOS(CH_CMDTX)), q);
			wr32(GSI_CH_C_QOS(CH_CMDTX), (uint32_t) q);
		}
	}
	/* mainline ep7 (AP_COMMAND_TX): DMA mode to AP_LAN_RX (ep 9), seq type DMA, resource group 0 */
	wr32(IPA_ENDP_MODE(EP_CMDTX), 3 | 9u << 4);
	wr32(IPA_ENDP_SEQ(EP_CMDTX), 0);
	LOG("p2cmd: ep7 mode %08x seq %08x", rd32(IPA_ENDP_MODE(EP_CMDTX)), rd32(IPA_ENDP_SEQ(EP_CMDTX)));
	if (ch_start(CH_CMDTX))
		return -1;
	p2_cmd_buf = ring_mem(64, 64);
	if (!p2_cmd_buf || pm_wr(p2_cmd_buf, pl, sizeof(pl))) {
		LOG("p2cmd: payload buffer failed");
		return -1;
	}
	LOG("p2cmd: ev2 + ch5 started (3 units)");
	p2_cmd_dump("before");
	p2_cmd_post();
	p2_cmd_t = link_ms();
	p2_cmd_n = 0;
	p2_stage = 2;
	set_state("up");
	ctx.active = 1;		/* ipagsi_poll() returns early without it: p2_cmd_poll() would never run */
	return 0;
}

static void p2_cmd_poll(void)
{
	struct gsi_evt e;
	int n;

	while ((n = ev_read(EV_TX, &e)) == 1) {
		uint32_t w[4];

		memcpy(w, &e, sizeof(w));

		LOG("p2cmd: EVENT %08x %08x %08x %08x (code %02x type %02x chid %u)", w[0], w[1], w[2], w[3], e.code, e.type,
		    e.chid);
	}
	if (p2_cmd_n < 12 && link_ms() >= p2_cmd_t + 2000u) {
		p2_cmd_t = link_ms();
		p2_cmd_n++;
		p2_cmd_dump("poll");
		if (p2_cmd_n == 3 || p2_cmd_n == 7)
			p2_cmd_post();		/* ring again: a missed first doorbell would show here */
	}
}

/* p2ackloop=1: a polled client never runs an irq handler, so with the EE0 irq config in place the status bits stay
 * asserted.  Acknowledge them on every poll (and log the first few non-zero sets) to see whether that stops the modem's
 * channel start from blipping. */
static void p2_ackloop(void)
{
	static int on = -1, logged;
	uint32_t ch, evc, ieob, glob, gen;

	if (on < 0)
		on = cell_word_int("p2ackloop=", 0);
	if (!on || !(rd32(GSI_BASE + 0x12180) & 1))
		return;
	ch = rd32(GSI_BASE + 0x12090);
	evc = rd32(GSI_BASE + 0x12094);
	ieob = rd32(GSI_BASE + 0x120b0);
	glob = rd32(GSI_BASE + 0x12100);
	gen = rd32(GSI_BASE + 0x12118);
	if (!(ch | evc | ieob | glob | gen))
		return;
	if (logged < 12) {
		logged++;
		LOG("p2ackloop: ch %08x ev %08x ieob %08x glob %08x gsi %08x", ch, evc, ieob, glob, gen);
	}
	if (ch)
		wr32(GSI_BASE + 0x120a0, ch);
	if (evc)
		wr32(GSI_BASE + 0x120a4, evc);
	if (ieob)
		wr32(GSI_BASE + 0x120c0, ieob);
	if (glob)
		wr32(GSI_BASE + 0x12110, glob);
	if (gen)
		wr32(GSI_BASE + 0x12128, gen);
}

static void p2_poll(void)
{
	p2_ackloop();
	if (p2_cmd) {
		p2_cmd_poll();
		return;
	}
	if (dump_n && link_ms() >= dump_at) {
		if (dump_n > 0)
			dump_n--;
		dump_at = link_ms() + 250;
		ee_dump();
	}
	if (p2_tx_only && p2_rec_s >= 0 && !p2_rec_done && link_ms() >= p2_t0 + (uint64_t) p2_rec_s * 1000) {
		/* p2rec=<s> (with p2tx=1): irq setup + re-create ch2/ev2 once, s seconds after setup, no modem cycle needed */
		p2_rec_done = 1;
		LOG("p2: p2rec: recreating ch2/ev2 now (send the UDP after this line)");
		if (p2_tx_recreate())
			LOG("p2: p2rec: recreate FAILED");
	}
	if (p2_irqonly)
		return;
	if (p2_stage == 1) {
		int go = 0;

		if ((rd32(GSI_CH_C_CNTXT_0(7) + 0x4000) >> CH_STATE_SHIFT & 0xf) != 2)
			p2_seen = 0;
		else if (!p2_seen)
			p2_seen = link_ms();
		else if (link_ms() >= p2_seen + 5000)
			go = 1;
		if (!go && p2_join_s && link_ms() >= p2_t0 + (uint64_t) p2_join_s * 1000)
			go = 2;
		if (!go) {
			if (link_ms() - p2_stat >= 30000) {
				p2_stat = link_ms();
				LOG("p2: waiting for modem: %llu s, EE1 ch 7 state %u", (unsigned long long) (link_ms() - p2_t0) / 1000,
				    (unsigned) (rd32(GSI_CH_C_CNTXT_0(7) + 0x4000) >> CH_STATE_SHIFT & 0xf));
			}
			return;
		}
		LOG("p2: join (%s)", go == 1 ? "cycle" : "timeout");
		if (cell_word_int("p2irq=", 0) == 2)
			p2_irq_setup();
		else if (cell_word_int("p2irq=", 0) == 3 && p2_tx_recreate())
			goto failed;
		if (p2_chdefer) {
			if (p2_prog)
				ep_program(EP_MDM_TX, p2_status);
			if (ch_start(CH_MDM_TX))
				goto failed;
			LOG("p2: p2irq=4: ch2 started at the join");
		}
		if (ch_alloc_program(CH_MDM_RX, EV_TX, RX_TRE_CNT, 0, 0, tlv_rx))
			goto failed;
		if (p2_prog && !p2_ep_before)
			ep_program(EP_MDM_RX, 0);	/* before the RX channel is started */
		if (ch_start(CH_MDM_RX))
			goto failed;
		if (!p2_ep_before)
			ipagsi_endpoints();
		LOG("p2: endpoints done");
		if (rx_prepost())
			goto failed;
		LOG("p2: rx posted %u", RX_BUFS);
		if (utun_up())
			goto failed;
		p2_stage = 2;
		set_state("up");
		LOG("up (p2late, join %s)", go == 1 ? "cycle" : "timeout");
		return;
	failed:
		LOG("p2: join FAILED");
		p2_stage = 0;
		ctx.active = 0;
		fail_state();
		return;
	}
	if (p2_stage != 2)
		return;
	p2_drain();
	tx_pump();
	{
		static unsigned seen_wp, diag_n;
		static uint64_t diag_at;

		if (tx_wp != seen_wp && tx_wp <= 3) {	/* the first three posts */
			seen_wp = tx_wp;
			p2_diag("after post");
		} else if (tx_wp > tx_done && diag_n < 4 && link_ms() >= diag_at + 20000) {
			diag_at = link_ms();
			diag_n++;
			p2_diag("stalled");
		}
	}
	if (link_ms() - p2_stat >= 10000) {
		p2_stat = link_ms();
		LOG("p2: tx posted %u completed %u (ev %u) | rx events %u bytes %llu pkts %u frames %u | unknown %u"
		    " | errors rx %u tx %u bad %u other-af %u", tx_wp, tx_done, p2_txev, p2_rxev, rx_bytes, ctx.rx_pkts,
		    ctx.rx_st.pkts, p2_unk, ctx.rx_err, ctx.tx_err, ctx.tx_st.bad, ctx.tx_other);
	}
}

/* The modem starts its own GSI channels 4-7 at an unpredictable time (+3..+113 s after us, sometimes not
 * within 400 s) and asserts if the AP holds >= 4 GSI contexts at that moment (c2start/ch3/c2join runs).  The
 * normal path therefore WAITS (state wait-modem, call held) until EE1 ch 7 has been 'started' for 5 s, or
 * DEFER_TIMEOUT_MS after we were asked, and only then runs the whole GSI setup (ipagsi_setup_rest).
 * cell.on word nodefer=1 sets up at once (old behaviour). */
#define DEFER_TIMEOUT_MS	0	/* 0 = wait for the modem forever: c2joint-r3 showed a timeout join (5 units) crashes at the modem's later cycle */
static int defer_pending, defer_go;
static uint64_t defer_t0, defer_seen;
static void ipagsi_setup_rest(void);

static int read_nodefer_flag(void)
{
	char b[512] = "";
	FILE *f = fopen(CELL_ON, "r");
	if (f) {
		size_t k = fread(b, 1, sizeof(b) - 1, f);
		b[k] = 0;
		fclose(f);
	}
	return strstr(b, "nodefer=1") != NULL;
}

void ipagsi_start(const struct ipagsi_call *c, int tables)
{
	if (ctx.active) {
		LOG("start while already %s", ctx.state);
		return;
	}
	if (ever_ran) {
		LOG("not starting: setup ran earlier in this wland run (%s) and nothing here resets"
		    " the GSI rings or the IPA: restart wland to try again", ctx.state);
		return;
	}
	ever_ran = 1;
	ctx.call = *c;
	ctx.tables = tables;
	ctx.rx_wp = ctx.rx_done = 0;
	tx_wp = tx_done = 0;
	ctx.rx_st = ctx.tx_st = qmap_zero;
	ctx.rx_pkts = ctx.tx_pkts = ctx.rx_err = ctx.tx_err = ctx.tx_other = 0;
	ctx.stat_ms = link_ms();
	phase0 = read_phase0();
	gsitest = GS_BOTH;
	free_at = 0;
	dump_n = 0;
	p0_bytes = 0;
	p0_v4 = p0_v6 = p0_bad = 0;
	if (phase0) {
		LOG("ipaphase=0: utun only, no GSI/IPA access (plumbing mode)");
		set_state("setup-4");
		if (utun_up()) {
			fail_state();
			return;
		}
		set_state("up");
		ctx.active = 1;
		LOG("up (phase 0, packets sent to %s are counted and dropped)", ctx.utun_name);
		return;
	}
	set_state("setup-1");

	if (gsi_check()) {
		fail_state();
		return;
	}
	wr32(GSI_BASE + 0x12200, 0);	/* ERROR_LOG: mainline zeroes it in gsi_setup(); it powers up stale */
	{
#if IPAGSI_PHASE < 2
		int plain = read_gsistart() == GS_BOTH;
#else
		int plain = 1;
#endif
		if (plain && !defer_go && !read_nodefer_flag()) {
			defer_pending = 1;
			defer_t0 = link_ms();
			defer_seen = 0;
			set_state("wait-modem");
			ctx.active = 1;
			LOG("waiting for the modem's own GSI start-up (EE1 ch 7 started for 5 s) before any GSI setup");
			return;
		}
	}
	ipagsi_setup_rest();
}

static void ipagsi_setup_rest(void)
{
#if IPAGSI_PHASE < 2
	gsitest = read_gsistart();
	tlv_tx = TLV_MDM_TX;
	tlv_rx = TLV_MDM_RX;
	if (gsitest == GS_TLVMIN) {
		/* onestart shape, but minimal TLV FIFO sizes: tests a shared-TLV-memory budget */
		tlv_tx = tlv_rx = 2;
		gsitest = GS_ONESTART;
		LOG("gsistart=tlvmin: onestart with tlv_count 2/2");
	} else if (gsitest == GS_TLVBIG) {
		/* the other side of tlvmin: ONE channel (c2start shape) with a big TLV FIFO (30 vs mainline 16).
		 * Crash => the modem objects to the TLV FIFO budget used, not to having two channels. */
		tlv_tx = 30;
		gsitest = GS_C2START;
		LOG("gsistart=tlvbig: c2start with tlv_count 30");
	} else if (gsitest == GS_C2JOINT) {
		/* c2join, but if the modem has not cycled 120 s after we are up, add ch 7 rx anyway: tests a LATER
		 * modem cycle against the 5-unit state (the risk c2join cannot cover). */
		gsitest = GS_C2START;
		join_pending = 1;
		join_force = link_ms() + 120000;
		LOG("gsistart=c2joint: c2start now, ch 7 rx added after the modem's ch 7 started for 5 s OR after 120 s");
	} else if (gsitest == GS_C2JOIN) {
		/* c2start shape (survives the modem's ch4-7 start), then, once the modem has been seen with its
		 * ch 7 started for 5 s, add the RX channel on the same ring and start it (5 units, but after the
		 * modem's sequence).  Crash => the modem objects to the count at any time, not just during its start. */
		gsitest = GS_C2START;
		join_pending = 1;
		LOG("gsistart=c2join: c2start now, ch 7 rx added after the modem's ch 7 has been started for 5 s");
	} else if (gsitest == GS_LATE) {
		/* onestart shape, but only after 45 s: every crash so far lands at ~157-158 s of uptime,
		 * whatever time we came up at, i.e. the modem's own IPA init */
		gsitest = GS_ONESTART;
		LOG("gsistart=late: waiting 45 s before touching GSI channels");
		for (int i = 0; i < 45; i++)
			delay_ms(1000);
	}
	if (gsitest == GS_P2LATE) {
		LOG("gsistart=p2late: shared-ring staged data path");
		if (p2_setup())
			goto failed;
		return;
	}
#endif
	{
		int gm = gsitest == GS_C2START ? GS_CH2 : gsitest == GS_C7START ? GS_CH7 :
			 (gsitest == GS_ONETX || gsitest == GS_ONERX) ? GS_ONESTART :
			 (gsitest == GS_IDS34 || gsitest == GS_EV4 || gsitest == GS_CH3 || gsitest == GS_EV01 ||
			  gsitest == GS_EV4HI || gsitest == GS_EEDUMP) ? GS_NONE : gsitest;
		int ev_tx = gm != GS_NONE && gm != GS_CH7;
		int ev_rx = gm != GS_NONE && gm != GS_CH2 && gm != GS_ONERING &&
			     gm != GS_ONESTART;	/* onering3 keeps ring 3 */
		int ch_tx = gm != GS_NONE && gm != GS_EVONLY && gm != GS_CH7;
		int ch_rx = gm != GS_NONE && gm != GS_EVONLY && gm != GS_CH2;
		int rev = gm == GS_REV;
		int tx_ring = gm == GS_EVOTHER ? 0 : EV_TX;	/* event ring ids that get allocated */
		int rx_ring = gm == GS_EVOTHER ? 1 : EV_RX;
		int tx_ev = gm == GS_SWAP ? rx_ring : tx_ring;	/* the ring each channel reports to */
		int rx_ev = (gm == GS_ONERING || gm == GS_ONERING3 || gm == GS_SWAP ||
		     gm == GS_ONESTART) ?
			    tx_ring : rx_ring;

		set_state("setup-2");
		if (ctx.tables && gm == GS_BOTH && ev_alloc_program(EV_CMD, EV_CNT))
			goto failed;
		if (rev && ev_rx && ev_alloc_program(rx_ring, EV_CNT))
			goto failed;
		if (ev_tx && ev_alloc_program(tx_ring, EV_CNT))
			goto failed;
		if (!rev && ev_rx && ev_alloc_program(rx_ring, EV_CNT))
			goto failed;

		set_state("setup-3");
		if (ctx.tables && gm == GS_BOTH &&
		    ch_alloc_program(CH_CMDTX, EV_CMD, CMD_TRE_CNT, 1, 1, TLV_CMDTX))
			goto failed;
		if (rev && ch_rx && ch_alloc_program(CH_MDM_RX, rx_ev, RX_TRE_CNT, 0, 0, tlv_rx))
			goto failed;
		if (ch_tx && ch_alloc_program(CH_MDM_TX, tx_ev, TX_TRE_CNT, 1, 0, tlv_tx))
			goto failed;
		if (!rev && ch_rx && ch_alloc_program(CH_MDM_RX, rx_ev, RX_TRE_CNT, 0, 0, tlv_rx))
			goto failed;
	}

	/* Resource-count model fitted to every result so far: units = event rings + channels + STARTed channels;
	 * <= 3 units clean, >= 4 crash (r1c1s0=2, r1c1s1=3, r1c2s0=3, r2c0=2 clean; r2c2s0=4, r1c2s1=4 crash).
	 *   ev4: four event rings (0-3), NO channels (4 units): crash => the count of rings alone counts, no channel needed.
	 *   ch3: ev2 plus channels 2, 3, 7 on it, none started (4 units): crash => a third channel is enough. */
	if (gsitest == GS_EV4) {
		set_state("setup-2");
		for (int e = 0; e < 4; e++)
			if (ev_alloc_program(e, EV_CNT))
				goto failed;
	} else if (gsitest == GS_EV01 || gsitest == GS_EV4HI) {
		/* ring ids vs ring count: ev01 = two rings with low ids (0,1); ev4hi = four rings with HIGH ids (4-7).
		 * ev (2,3) was clean for 150+ s, ev (0-3) crashed. ev4hi clean => it is the low ring ids (the modem
		 * uses ring 0 or 1 of the shared pool), not the count; ev01 crash points at the same. */
		set_state("setup-2");
		for (int e = gsitest == GS_EV01 ? 0 : 4; e < (gsitest == GS_EV01 ? 2 : 8); e++)
			if (ev_alloc_program(e, EV_CNT))
				goto failed;
	} else if (gsitest == GS_CH3 || gsitest == GS_EEDUMP) {
		set_state("setup-2");
		if (ev_alloc_program(EV_TX, EV_CNT))
			goto failed;
		set_state("setup-3");
		if (ch_alloc_program(CH_MDM_TX, EV_TX, TX_TRE_CNT, 1, 0, TLV_MDM_TX) ||
		    ch_alloc_program(CH_ALT_TX, EV_TX, TX_TRE_CNT, 1, 0, TLV_MDM_TX) ||
		    ch_alloc_program(CH_MDM_RX, EV_TX, RX_TRE_CNT, 0, 0, TLV_MDM_RX))
			goto failed;
	}
	if (gsitest == GS_IDS34) {
		set_state("setup-2");
		if (ev_alloc_program(EV_TX, EV_CNT))
			goto failed;
		set_state("setup-3");
		if (ch_alloc_program(CH_ALT_TX, EV_TX, TX_TRE_CNT, 1, 0, TLV_MDM_TX) ||
		    ch_alloc_program(CH_ALT_RX, EV_TX, RX_TRE_CNT, 0, 0, TLV_MDM_RX))
			goto failed;
	}

	set_state("setup-4");
	if (utun_up())
		goto failed;

#if IPAGSI_PHASE >= 2
	set_state("setup-5");
	ipagsi_dump_ipa();
	ipagsi_tables();		/* channel 5 immediate commands, if tables=1 */
	set_state("setup-6");
	if (ipagsi_endpoints())		/* IPA endpoints 2 and 16 -> our channels */
		goto failed;
	set_state("setup-7");
	if (ch_start(CH_CMDTX) || ch_start(CH_MDM_TX) || ch_start(CH_MDM_RX))
		goto failed;
#else
	/* phase 1: start the channels so we can see whether GSI accepts the rings; the IPA is
	 * still pointed at the modem's channels, so no packets move */
	set_state("setup-5");
	ipagsi_dump_ipa();
	/* gsitest was read (and late/tlvmin mapped to onestart) at setup-1: do not re-read it here */
	if (gsitest != GS_BOTH) {
		static const char *const nm[] = { "both", "alloc", "tx", "rx", "rxfirst", "none", "evonly",
						  "ch2", "ch7", "rev", "onering", "free", "freelate", "evother", "drop2",
						  "drop7", "onering3", "swap", "onestart", "onetx", "onerx", "c2start",
						  "c7start", "ids34", "tlvmin", "late", "tlvbig", "ev4", "ch3", "ev01", "ev4hi", "eedump", "c2join", "c2joint", "p2late" };
	_Static_assert(sizeof(nm) / sizeof(nm[0]) == GS_P2LATE + 1, "nm[] out of sync with enum");

		LOG("gsistart=%s: GSI test mode", nm[gsitest]);
		if (gsitest == GS_EEDUMP || read_eedump_flag()) {
			dump_n = -1;	/* until reboot; logs only on change */
			dump_at = link_ms();
		}
		if (gsitest == GS_RXFIRST) {
			if (ch_start(CH_MDM_RX) || ch_start(CH_MDM_TX))
				goto failed;
		} else if (gsitest == GS_TX) {
			if (ch_start(CH_MDM_TX))
				goto failed;
		} else if (gsitest == GS_RX) {
			if (ch_start(CH_MDM_RX))
				goto failed;
		} else if (gsitest == GS_ONESTART) {
			if (ch_start(CH_MDM_TX) || ch_start(CH_MDM_RX))
				goto failed;
		} else if (gsitest == GS_IDS34) {
			if (ch_start(CH_ALT_TX) || ch_start(CH_ALT_RX))
				goto failed;
		} else if (gsitest == GS_ONETX || gsitest == GS_C2START) {
			if (ch_start(CH_MDM_TX))
				goto failed;
		} else if (gsitest == GS_ONERX || gsitest == GS_C7START) {
			if (ch_start(CH_MDM_RX))
				goto failed;
		}
		if (gsitest == GS_FREE)
			gsi_release_all();
		else if (gsitest == GS_FREELATE)
			free_at = link_ms() + 6000;
		else if (gsitest == GS_DROP2) {
			ch_release(CH_MDM_TX);
			ev_release(EV_TX);
		} else if (gsitest == GS_DROP7) {
			ch_release(CH_MDM_RX);
			ev_release(EV_RX);
		}
		LOG("gsi test: ch 2 %s, ch 7 %s; parked (no ring traffic)",
		    ch_state_name(ch_state(CH_MDM_TX)), ch_state_name(ch_state(CH_MDM_RX)));
		set_state("up");
		ctx.active = 1;
		return;
	}
	if (ch_start(CH_MDM_TX) || ch_start(CH_MDM_RX))
		goto failed;
#endif

	set_state("setup-8");
	if (rx_prepost())
		goto failed;

	set_state("up");
	ctx.active = 1;
	LOG("up (phase %d, tables %d)", IPAGSI_PHASE, ctx.tables);
	return;
failed:
	fail_state();		/* cell.on's x1 hold ends with ipagsi_running() = 0: the call stops */
	if (ctx.utun_fd >= 0) {
		close(ctx.utun_fd);
		ctx.utun_fd = -1;
	}
	return;
}

void ipagsi_stop(const char *why)
{
	if (!ctx.active)		/* cell.c calls this unconditionally when its options change */
		return;
	LOG("stopping (%s)", why);
	ctx.active = 0;
	dump_keep = dump_n != 0;	/* eedump=1: keep logging EE1/EE0 channel states (read only) after a call stop */
	if (ctx.utun_fd >= 0) {
		close(ctx.utun_fd);
		ctx.utun_fd = -1;
	}
	set_state("stopped");
}

int ipagsi_running(void)
{
	return ctx.active;
}

const char *ipagsi_state(void)
{
	return ctx.state;
}

void ipagsi_poll(void)
{
	if (!ctx.active) {
		if (dump_keep && link_ms() >= dump_at) {
			dump_at = link_ms() + 250;
			ee_dump();
		}
		return;
	}
	if (gsitest == GS_P2LATE) {
		p2_poll();
		return;
	}
	if (defer_pending) {
		int go = 0;
		uint32_t st = rd32(GSI_CH_C_CNTXT_0(7) + 0x4000) >> CH_STATE_SHIFT & 0xf;

		if (st != 2)
			defer_seen = 0;
		else if (!defer_seen)
			defer_seen = link_ms();
		else if (link_ms() >= defer_seen + 5000)
			go = 1;
		if (!go && DEFER_TIMEOUT_MS && link_ms() >= defer_t0 + DEFER_TIMEOUT_MS)
			go = 2;
		if (!go) {
			static uint64_t last;
			if (link_ms() - last >= 30000) {
				last = link_ms();
				LOG("wait-modem: %llu s, EE1 ch 7 state %u", (unsigned long long) (link_ms() - defer_t0) / 1000, st);
			}
			return;
		}
		LOG("wait-modem over: %s (EE1 ch 7 state %u); GSI setup now", go == 1 ?
		    "modem ch 7 started for 5 s" : "timeout, no modem start-up seen", st);
		defer_pending = 0;
		defer_go = 1;
		ctx.active = 0;
		ipagsi_setup_rest();
		return;
	}
	if (gsitest) {
		if (dump_n && link_ms() >= dump_at) {
			if (dump_n > 0)
				dump_n--;
			dump_at = link_ms() + 250;
			ee_dump();
		}
		if (join_pending) {
			int go = 0;
			if ((rd32(GSI_CH_C_CNTXT_0(7) + 0x4000) >> CH_STATE_SHIFT & 0xf) != 2)
				join_seen = 0;
			else if (!join_seen)
				join_seen = link_ms();
			else if (link_ms() >= join_seen + 5000)
				go = 1;
			if (join_force && link_ms() >= join_force)
				go = 2;
			if (go) {
				join_pending = 0;
				LOG("c2join: %s, adding ch 7 rx + start", go == 1 ? "modem ch 7 started for 5 s" :
				    "timeout, modem has not cycled (or not finished)");
				if (ch_alloc_program(CH_MDM_RX, EV_TX, RX_TRE_CNT, 0, 0, tlv_rx) || ch_start(CH_MDM_RX))
					LOG("c2join: ch 7 add/start FAILED");
				else
					LOG("c2join: ch 7 started (state %d)", ch_state(CH_MDM_RX));
			}
		}
		if (free_at && link_ms() >= free_at) {
			free_at = 0;
			gsi_release_all();
		}
		return;
	}
	if (phase0) {
		p0_drain();
		if (link_ms() - ctx.stat_ms >= 10000) {
			ctx.stat_ms = link_ms();
			LOG("phase 0 %s: tx %u pkts %llu B (v4 %u v6 %u bad-af %u) read-err %u", ctx.utun_name, ctx.tx_pkts,
			    p0_bytes, p0_v4, p0_v6, p0_bad, ctx.tx_err);
		}
		return;
	}
	rx_drain();
	tx_drain();
	tx_pump();
	if (link_ms() - ctx.stat_ms >= 10000) {
		ctx.stat_ms = link_ms();
		LOG("rx pkts %u frames %u cmd %u bad %u err %u | tx pkts %u bad %u other-af %u"
		    " | ch7 wp %u done %u | ch2 wp %u done %u | evt rx rp %u tx rp %u", ctx.rx_pkts, ctx.rx_st.pkts,
		    ctx.rx_st.cmds, ctx.rx_st.bad, ctx.rx_err, ctx.tx_pkts, ctx.tx_st.bad, ctx.tx_other,
		    ctx.ch[CH_MDM_RX].wp, ctx.rx_done, tx_wp, tx_done, ctx.ev[EV_RX].rp, ctx.ev[EV_TX].rp);
	}
}
