/*
 * redfin (Pixel 5): NFC phase P0, a read-only probe of the ST54J NFC controller. Patch 1115.
 *
 * Hardware (Android FDT st21nfc@08, see logs/research/nfc-plan-2026-11-15.md): I2C QUPv3 SE7 i2c@984000
 * (the rt5514 mic codec's bus: bsd/dev/sweet_mic.c sweet_se7_xfer(), one lock), address 0x08; VEN/reset
 * TLMM gpio12 (output, high = on); IRQ gpio34 (input, level high = a packet is waiting; pull-up per Android's
 * nfc_int_active); PIDLE gpio69 (input); CLK_REQ gpio58 per clkreq-gpios or gpio31 per the nfc_clk_req
 * pinctrl group (unresolved). TLMM tiles from pinctrl-lito.c (TLMM 0x0f000000; REG_SIZE 0x1000 per pin):
 *   gpio6/7 WEST 0x0f506000/0x0f507000, gpio12 EAST 0x0f90c000, gpio31 EAST 0x0f91f000,
 *   gpio34 EAST 0x0f922000, gpio35 EAST 0x0f923000, gpio58 WEST 0x0f53a000, gpio69 EAST 0x0f945000.
 *
 * 1117: a runtime switch, no boot-arg needed. By default nothing at all is touched at boot (no pin read, no bus,
 * no thread). Writing kern.sweet_nfc_probe=1 on the running phone runs the probe below once, in a kernel thread
 * (again on each later write of 1; EBUSY while one runs; EPERM once state is "loader", latched until reboot).
 * The SE7 bus is brought up lazily by the first probe if the mic driver has not done it yet
 * (sweet_se7_init_nfc(): a failed bring-up from here is not latched, so the mic can still try it later).
 * The boot-arg sweetnfc=1 still works as an alternative: it starts the same probe 1 s after sweet_nfc_init().
 * The probe, from a kernel thread, never on the boot path:
 *   1. log the TLMM config of gpio6/7/12/31/34/35/58/69 (read only)
 *   2. VEN high, then Android st21nfc's PULSE_RESET (20 ms low, 10 ms high, 20 ms low, high)
 *   3. wait up to 100 ms for IRQ high (gpio34 polled), read the packet the chip sends: 3-byte header with
 *      ST's 0x7E idle-byte handling (i2clayer.cc), payload length in byte 2. If no CORE_RESET_NTF came,
 *      CORE_RESET_CMD once and wait again.
 *   4. loader mode ([2]==0x39 && [3]==0xA1, or 0x41/0xA2): VEN low, state "loader", nothing more is ever
 *      sent until reboot (kern.sweet_nfc_probe refuses too)
 *   5. router mode ([1]==0 && [3]==1, ST hal_fd.cc; or reason 2 after our CORE_RESET_CMD): CORE_INIT, read
 *      the response; if rejected, PROP_NFC_MODE_SET ON once and CORE_INIT once more
 *   6. log nci/manuf/hw/fw/loader (hal_fd.cc offsets: hw [8], fw [10..13], loader [14..16], cust [31..32])
 *   7. VEN low
 *
 * HARD RULES (not switchable): the only bytes ever written to 0x08 are the three constant packets in
 * nfc_tx_ok[] (sweet_se7_xfer() checks every write to 0x08 against it, whoever the caller). No host or user
 * path to the chip. No pin is driven but gpio12. spi@980000 (Titan M / eSE) and gpio26/35/55/67 are never
 * touched (gpio35 is only read). No firmware download, no configuration writes.
 *
 * Sysctls: kern.sweet_nfc_state (off/probing/router/loader/error:<why>), kern.sweet_nfc_ver (decoded
 * versions), kern.sweet_nfc_probe (reads the number of probes run; write 1: probe now; refused after loader mode).
 * Log lines: "SWEET NFC: ...".
 *
 * 1119 (P1): AppleStockholmControl + com.apple.uart.stockholm + the NCI gateway, behind kern.sweet_nfc_p1 (default 0);
 * see the P1 section at the end of this file. sweet_nfc_tx_allowed() also lets a host packet through to 0x08, but
 * only in gateway mode 1 with a chip session and only if the section-4 filter forwards it unchanged.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/thread_call.h>               /* 1140: deferred persona NTF */
#include <kern/task.h>                      /* 1141: current_task() */
#include <vm/vm_shared_region.h>            /* 1141: nfcd's dyld shared region (slide) */
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern kern_return_t thread_terminate(thread_t thread);
extern bool sweet_se7_init_nfc(void);
extern int sweet_se7_xfer(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr);
void sweet_nfc_init(void);
bool sweet_nfc_tx_allowed(const uint8_t *tx, unsigned nt);
int sweet_nfc_uc_call(uint32_t sel, uint32_t nin, uint64_t v);         /* 1119: SweetNFC.cpp's user client */
void sweet_nfc_uc_event(int open, uint32_t type);
extern int SweetNFCPublish(bool on, const char *why);                  /* 1119: iokit/Kernel/arm/SweetNFC.cpp */
static bool gw_bus_ok(const uint8_t *tx, unsigned nt);
static void nfc_p1_boot_thread(void *arg, wait_result_t wr);

#define SAY(...)                printf("SWEET NFC: " __VA_ARGS__)

#define NFC_ADDR                0x08
#define NFC_CHUNK               64u             /* sweet_mic.c SE7_RX_MAX */
#define NFC_WAIT_MS             100u
#define TLMM_PA                 0x0f000000u
#define TILE_WEST               0x00500000u
#define TILE_EAST               0x00900000u
#define PIN_VEN                 12u

/* ---- the only bytes the chip may ever receive ---------------------------------------------------------------- */

static const uint8_t TX_CORE_RESET[] = { 0x20, 0x00, 0x01, 0x00 };             /* reset, keep configuration */
static const uint8_t TX_CORE_INIT[] = { 0x20, 0x01, 0x02, 0x00, 0x00 };        /* NCI 2.0 CORE_INIT */
static const uint8_t TX_MODE_ON[] = { 0x2f, 0x02, 0x02, 0x02, 0x01 };          /* ST PROP_NFC_MODE_SET ON */
static const struct { const uint8_t *b; unsigned n; } nfc_tx_ok[] = {
	{ TX_CORE_RESET, sizeof(TX_CORE_RESET) },
	{ TX_CORE_INIT, sizeof(TX_CORE_INIT) },
	{ TX_MODE_ON, sizeof(TX_MODE_ON) },
};

static volatile bool nfc_loader;                /* latched: loader mode seen, nothing is sent until reboot */
static volatile bool nfc_tx_open;               /* only while our probe runs */
static unsigned nfc_refused;

bool
sweet_nfc_tx_allowed(const uint8_t *tx, unsigned nt)
{
	if (nfc_loader || tx == NULL) {
		nfc_refused++;
		return false;
	}
	for (unsigned i = 0; nfc_tx_open && i < sizeof(nfc_tx_ok) / sizeof(nfc_tx_ok[0]); i++) {
		if (nt == nfc_tx_ok[i].n && memcmp(tx, nfc_tx_ok[i].b, nt) == 0) {
			return true;
		}
	}
	if (gw_bus_ok(tx, nt)) {        /* 1119: a filtered host packet, gateway mode 1 session only */
		return true;
	}
	nfc_refused++;
	return false;
}

/* ---- state ---------------------------------------------------------------------------------------------------- */

static LCK_GRP_DECLARE(nfc_lock_group, "sweet-nfc");
static LCK_MTX_DECLARE(nfc_lock, &nfc_lock_group);
static uint32_t nfc_bootarg;
static char nfc_state[48] = "off";
static char nfc_ver[160] = "";
static volatile bool nfc_busy;
static uint32_t nfc_probes;

static void
set_state(const char *s)
{
	lck_mtx_lock(&nfc_lock);
	strlcpy(nfc_state, s, sizeof(nfc_state));
	lck_mtx_unlock(&nfc_lock);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint32_t
mrd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);

	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static void
mwr(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile ("dsb osh" ::: "memory");
}

/* ---- pins: read only, except gpio12 (VEN) ---------------------------------------------------------------------- */

static struct {
	unsigned gpio;
	uint32_t tile;
	const char *what;
	vm_offset_t va;
} pins[] = {
	{ 12, TILE_EAST, "VEN", 0 },
	{ 34, TILE_EAST, "IRQ", 0 },
	{ 69, TILE_EAST, "PIDLE", 0 },
	{ 58, TILE_WEST, "CLKREQ(clkreq-gpios)", 0 },
	{ 31, TILE_EAST, "CLKREQ?(nfc_clk_req group)", 0 },
	{ 35, TILE_EAST, "nfc_enable 2nd pin (read only)", 0 },
	{ 6, TILE_WEST, "SE7 SDA", 0 },
	{ 7, TILE_WEST, "SE7 SCL", 0 },
};
#define P_VEN   0
#define P_IRQ   1
#define P_PIDLE 2
#define P_CLK58 3
#define P_CLK31 4
#define NPINS   (sizeof(pins) / sizeof(pins[0]))

static bool
pins_map(void)
{
	for (unsigned i = 0; i < NPINS; i++) {
		if (pins[i].va == 0) {
			pins[i].va = ml_io_map(TLMM_PA + pins[i].tile + 0x1000u * pins[i].gpio, 0x1000);
		}
		if (pins[i].va == 0) {
			return false;
		}
	}
	return true;
}

static void
pins_log(const char *when)
{
	for (unsigned i = 0; i < NPINS; i++) {
		uint32_t cfg = mrd(pins[i].va, 0), io = mrd(pins[i].va, 4);

		SAY("%s: gpio%u %s @%#x cfg %#x (func %u pull %u drive %umA %s) io %#x (in %u out %u)\n", when,
		    pins[i].gpio, pins[i].what, TLMM_PA + pins[i].tile + 0x1000u * pins[i].gpio, cfg, (cfg >> 2) & 0xfu,
		    cfg & 3u, 2u * (((cfg >> 6) & 7u) + 1u), (cfg & (1u << 9)) ? "OUT" : "in", io, io & 1u, (io >> 1) & 1u);
	}
}

static unsigned
pin_in(unsigned p)
{
	return mrd(pins[p].va, 4) & 1u;
}

static void
lines_log(const char *when)
{
	SAY("%s: VEN out %u, IRQ %u, PIDLE %u, CLKREQ gpio58 %u gpio31 %u\n", when, (mrd(pins[P_VEN].va, 4) >> 1) & 1u,
	    pin_in(P_IRQ), pin_in(P_PIDLE), pin_in(P_CLK58), pin_in(P_CLK31));
}

/* The only pin write in this file: gpio12 as a GPIO output (2 mA; pull-up while on as nfc_enable_active, none
 * while off as nfc_enable_suspend), the level in the IO register's bit 1 first. */
static void
ven(bool high)
{
	vm_offset_t va = pins[P_VEN].va;

	if (pins[P_VEN].gpio != PIN_VEN || va == 0) {
		return;
	}
	mwr(va, 4, high ? 2u : 0u);
	mwr(va, 0, (mrd(va, 0) & ~0x3ffu) | (high ? 3u : 0u) | (1u << 9));
}

/* ---- NCI packets ------------------------------------------------------------------------------------------------ */

static unsigned rx_pkts, rx_idle, rx_errs;

static void
hex(char *out, size_t cap, const uint8_t *b, unsigned n)
{
	size_t at = 0;

	out[0] = 0;
	for (unsigned i = 0; i < n && at + 4 < cap; i++) {
		at += (size_t)snprintf(out + at, cap - at, i ? " %02x" : "%02x", b[i]);
	}
}

static int
nfc_rd(uint8_t *b, unsigned n)
{
	for (unsigned at = 0; at < n;) {
		unsigned k = n - at > NFC_CHUNK ? NFC_CHUNK : n - at;
		int err = sweet_se7_xfer(NFC_ADDR, NULL, 0, b + at, k);

		if (err) {
			return err;
		}
		at += k;
	}
	return 0;
}

static int
nfc_wr(const uint8_t *b, unsigned n, const char *what)
{
	char h[24];
	int err;

	if (!sweet_nfc_tx_allowed(b, n)) {
		SAY("write of %s REFUSED (not on the allow-list or after loader mode)\n", what);
		return EPERM;
	}
	hex(h, sizeof(h), b, n);
	err = sweet_se7_xfer(NFC_ADDR, b, n, NULL, 0);
	SAY("-> %s [%s] %s (%d)\n", what, h, err ? "FAILED" : "sent", err);
	return err;
}

/* One packet (header + payload, at most 258 bytes); IRQ polled, every wait bounded by wait_ms. */
static int
nfc_read_pkt(uint8_t *p, unsigned *len, unsigned wait_ms)
{
	unsigned ms = 0;
	int err = ETIMEDOUT;

	for (;;) {
		while (!pin_in(P_IRQ)) {
			if (ms >= wait_ms) {
				return err;
			}
			sleep_ms(1);
			ms++;
		}
		err = nfc_rd(p, 3);
		if (!err) {
			bool ok = false;

			/* ST i2clayer.cc: idle bytes 0x7E before the header */
			if (p[0] != 0x7e && p[1] != 0x7e) {
				ok = true;
			} else if (p[1] != 0x7e) {
				p[0] = p[1];
				p[1] = p[2];
				ok = (err = nfc_rd(p + 2, 1)) == 0;
			} else if (p[2] != 0x7e) {
				p[0] = p[2];
				ok = (err = nfc_rd(p + 1, 2)) == 0;
			} else {
				rx_idle++;
				err = ETIMEDOUT;
			}
			if (ok) {
				err = p[2] ? nfc_rd(p + 3, p[2]) : 0;
				if (!err) {
					*len = 3u + p[2];
					rx_pkts++;
					return 0;
				}
			}
		}
		if (err && err != ETIMEDOUT) {
			rx_errs++;
		}
		if (ms >= wait_ms) {
			return err;
		}
		sleep_ms(1);
		ms++;
	}
}

/* Read packets until one starts b0 b1 (others are logged), at most 8 packets, each wait bounded. */
static int
nfc_wait_for(uint8_t b0, uint8_t b1, uint8_t *p, unsigned *len, unsigned wait_ms)
{
	char h[3 * 48];

	for (unsigned i = 0; i < 8; i++) {
		int err = nfc_read_pkt(p, len, wait_ms);

		if (err) {
			return err;
		}
		hex(h, sizeof(h), p, *len > 46 ? 46 : *len);
		SAY("<- %u bytes [%s%s]\n", *len, h, *len > 46 ? " ..." : "");
		if (p[0] == b0 && p[1] == b1) {
			return 0;
		}
	}
	return EPROTO;
}

/* ---- the probe -------------------------------------------------------------------------------------------------- */

static uint8_t ntf[3 + 255], pkt[3 + 255];

static void
nfc_probe(void)
{
	unsigned n = 0, m = 0;
	int err;
	char why[48], mhex[3 * 40], init[24] = "none";
	bool mode_on = false;

	set_state("probing");
	nfc_probes++;
	rx_pkts = rx_idle = rx_errs = 0;
	if (!pins_map()) {
		set_state("error:pin_map");
		return;
	}
	pins_log("pins");
	lines_log("before");
	if (!sweet_se7_init_nfc()) {     /* 1117: lazy; brings SE7 up if the mic driver has not */
		set_state("error:i2c_bus");
		SAY("SE7 I2C bus not up: nothing sent\n");
		return;
	}
	nfc_tx_open = true;
	ven(true);
	sleep_ms(10);
	lines_log("VEN high");
	/* st21nfc PULSE_RESET */
	ven(false);
	sleep_ms(20);
	ven(true);
	sleep_ms(10);
	ven(false);
	sleep_ms(20);
	ven(true);
	lines_log("after reset pulse");

	err = nfc_wait_for(0x60, 0x00, ntf, &n, NFC_WAIT_MS);
	if (err) {
		SAY("no CORE_RESET_NTF after the pulse (%d; packets %u idle %u errors %u): CORE_RESET_CMD once\n",
		    err, rx_pkts, rx_idle, rx_errs);
		if (nfc_wr(TX_CORE_RESET, sizeof(TX_CORE_RESET), "CORE_RESET_CMD") == 0) {
			err = nfc_wait_for(0x60, 0x00, ntf, &n, NFC_WAIT_MS);
		}
	}
	if (err) {
		snprintf(why, sizeof(why), "error:no_reset_ntf_%d", err);
		SAY("chip silent: %s (packets %u idle %u errors %u, refused %u)\n", why, rx_pkts, rx_idle, rx_errs, nfc_refused);
		lines_log("silent");
		goto off;
	}
	lines_log("NTF read");
	/* loader mode first: nothing else is sent, ever (until reboot) */
	if ((ntf[2] == 0x39 && ntf[3] == 0xa1) || (ntf[2] == 0x41 && ntf[3] == 0xa2)) {
		nfc_loader = true;
		nfc_tx_open = false;
		ven(false);
		hex(mhex, sizeof(mhex), ntf, n > 38 ? 38 : n);
		SAY("CORE_RESET_NTF [%s] = LOADER MODE (%#x/%#x) - STOPPED, VEN low, nothing more is sent until reboot\n",
		    mhex, ntf[2], ntf[3]);
		set_state("loader");
		return;
	}
	if (n < 8 || ntf[3] == 0x00 || ntf[3] >= 0x80) {
		snprintf(why, sizeof(why), "error:ntf_len%u_reason%02x", n, ntf[3]);
		SAY("CORE_RESET_NTF not router mode: %s; nothing more sent\n", why);
		goto off;
	}
	/* router mode (reason 1 after the pulse; 2 after CORE_RESET_CMD) */
	strlcpy(why, "router", sizeof(why));
	err = nfc_wr(TX_CORE_INIT, sizeof(TX_CORE_INIT), "CORE_INIT_CMD");
	err = err ? err : nfc_wait_for(0x40, 0x01, pkt, &m, NFC_WAIT_MS);
	if (err || m < 4 || pkt[3] != 0) {
		SAY("CORE_INIT %s (%d, status %#x): PROP_NFC_MODE_SET ON once, then CORE_INIT again\n",
		    err ? "unanswered" : "rejected", err, m >= 4 ? pkt[3] : 0xff);
		mode_on = true;
		if (nfc_wr(TX_MODE_ON, sizeof(TX_MODE_ON), "PROP_NFC_MODE_SET_ON") == 0 &&
		    nfc_wait_for(0x4f, 0x02, pkt, &m, NFC_WAIT_MS) == 0) {
			SAY("mode-set response status %#x\n", m >= 4 ? pkt[3] : 0xff);
		}
		err = nfc_wr(TX_CORE_INIT, sizeof(TX_CORE_INIT), "CORE_INIT_CMD (2nd)");
		err = err ? err : nfc_wait_for(0x40, 0x01, pkt, &m, NFC_WAIT_MS);
	}
	if (err) {
		snprintf(init, sizeof(init), "none:%d", err);
	} else if (m < 4 || pkt[3] != 0) {
		snprintf(init, sizeof(init), "rej:%#x", m >= 4 ? pkt[3] : 0xff);
	} else {
		strlcpy(init, "ok", sizeof(init));
		hex(mhex, sizeof(mhex), pkt + 3, m - 3 > 38 ? 38 : m - 3);
		SAY("CORE_INIT_RSP payload [%s] (features %02x %02x %02x %02x)\n", mhex, m > 4 ? pkt[4] : 0, m > 5 ? pkt[5] : 0,
		    m > 6 ? pkt[6] : 0, m > 7 ? pkt[7] : 0);
	}
off:
	nfc_tx_open = false;
	ven(false);
	lines_log("VEN low");
	if (n >= 8 && strcmp(why, "router") == 0) {
		unsigned ml = ntf[7] <= n - 8 ? ntf[7] : n - 8;
		uint32_t fw = n > 13 ? (uint32_t)ntf[10] << 24 | (uint32_t)ntf[11] << 16 | (uint32_t)ntf[12] << 8 | ntf[13] : 0;
		uint32_t ld = n > 16 ? (uint32_t)ntf[14] << 16 | (uint32_t)ntf[15] << 8 | ntf[16] : 0;
		uint32_t cust = n > 32 ? (uint32_t)ntf[31] << 8 | ntf[32] : 0;

		hex(mhex, sizeof(mhex), ntf + 8, ml > 38 ? 38 : ml);
		SAY("CORE_RESET_NTF reason=%#x cfg=%#x nci=0x%02x manuf=0x%02x hw=0x%02x fw=0x%08x loader=0x%06x cust=0x%04x "
		    "init=%s modeon=%u manuf_info(%u)=[%s]\n", ntf[3], ntf[4], ntf[5], ntf[6], n > 8 ? ntf[8] : 0, fw, ld, cust,
		    init, mode_on, ntf[7], mhex);
		lck_mtx_lock(&nfc_lock);
		snprintf(nfc_ver, sizeof(nfc_ver), "nci=0x%02x manuf=0x%02x hw=0x%02x fw=0x%08x loader=0x%06x cust=0x%04x init=%s modeon=%u",
		    ntf[5], ntf[6], n > 8 ? ntf[8] : 0, fw, ld, cust, init, mode_on);
		lck_mtx_unlock(&nfc_lock);
	}
	set_state(why);
	SAY("probe %u done: %s (packets %u idle %u errors %u, refused writes %u)\n", nfc_probes, why, rx_pkts, rx_idle,
	    rx_errs, nfc_refused);
}

static void
nfc_thread(void *parameter, __unused wait_result_t result)
{
	if (parameter != NULL) {
		sleep_ms(1000);         /* (at boot: off the boot path, after the other drivers' init) */
	}
	nfc_probe();
	nfc_busy = false;
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

static int
nfc_start(bool at_boot)
{
	thread_t thread;

	if (nfc_loader) {               /* (1117: no longer needs sweetnfc=1; loader mode stays latched) */
		return EPERM;
	}
	if (__atomic_exchange_n(&nfc_busy, true, __ATOMIC_ACQ_REL)) {
		return EBUSY;
	}
	set_state("probing");
	if (kernel_thread_start(nfc_thread, at_boot ? (void *)1 : NULL, &thread) != KERN_SUCCESS) {
		nfc_busy = false;
		set_state("error:no_thread");
		return ENOMEM;
	}
	thread_deallocate(thread);
	return 0;
}

/* ---- sysctls ---------------------------------------------------------------------------------------------------- */

static int
sysctl_nfc_str SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg2)
	char b[160];

	lck_mtx_lock(&nfc_lock);
	strlcpy(b, arg1 == (void *)1 ? nfc_ver : nfc_state, sizeof(b));
	lck_mtx_unlock(&nfc_lock);
	return SYSCTL_OUT(req, b, strlen(b) + 1);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_nfc_state, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, (void *)0, 0,
    sysctl_nfc_str, "A", "redfin 1115: NFC P0 probe state (off/probing/router/loader/error:...)");
SYSCTL_PROC(_kern, OID_AUTO, sweet_nfc_ver, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, (void *)1, 0,
    sysctl_nfc_str, "A", "redfin 1115: ST54J versions from CORE_RESET_NTF");

static int
sysctl_nfc_probe SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, (int)nfc_probes, sizeof(value), &value, &changed);

	if (error || !changed) {
		return error;
	}
	return value == 1 ? nfc_start(false) : EINVAL;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_nfc_probe, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_nfc_probe, "I",
    "redfin 1117: write 1 to run the read-only NFC probe now (no boot-arg needed; never after loader mode)");

void
sweet_nfc_init(void)
{
	thread_t th;

	if (!PE_parse_boot_argn("sweetnfc", &nfc_bootarg, sizeof(nfc_bootarg)) || nfc_bootarg == 0) {
		nfc_bootarg = 0;
		return;                 /* nothing touched (1117: kern.sweet_nfc_probe=1 probes on demand) */
	}
	if (nfc_bootarg & 2) {          /* 1119: the P1 surface 2 s after init (as kern.sweet_nfc_p1=1) */
		SAY("sweetnfc bit 2: AppleStockholmControl + com.apple.uart.stockholm in 2 s\n");
		if (kernel_thread_start(nfc_p1_boot_thread, NULL, &th) == KERN_SUCCESS) {
			thread_deallocate(th);
		}
	}
	if (!(nfc_bootarg & 1)) {
		return;
	}
	SAY("sweetnfc=1: read-only probe of the ST54J at SE7 0x08 in 1 s\n");
	(void)nfc_start(true);
}

/* ==== 1119: NFC P1 - the kernel interface nfcd expects (plan section 5 P1; logs/research/nfc-plan-2026-11-15.md) ====
 *
 * Switch kern.sweet_nfc_p1 (default 0 = nothing below is published; the kernel behaves as 1117/1118). Writing 1
 * publishes, from the writer's thread:
 *   - IOKit: a "stockholm" nub under the platform expert and, on it, AppleStockholmControl with IONameMatched =
 *     "nfc,primary,gpio" and IOUserClientClass AppleStockholmControlUserClient (iokit/Kernel/arm/SweetNFC.cpp).
 *     libNFC_HAL's lookup is IOServiceNameMatching("AppleStockholmControl") + IOPropertyMatch {IONameMatched:
 *     "nfc,primary,gpio"} (sub_1f5de1650), so no ADT node is needed. No AppleStockholmSPMI: interface type 1 (UART).
 *   - the kernel control com.apple.uart.stockholm (SOCK_STREAM, unprivileged: nfcd runs as mobile), one client.
 * Writing 0 takes both away again. 1125: also while nfcd holds them: the IOService goes at once (nfcd's user client
 * is terminated with it), the control refuses new connects and answers nothing, and is deregistered as soon as the
 * client closes (no more EBUSY). Boot-arg sweetnfc bit 2 (2 or 3) publishes
 * 2 s after sweet_nfc_init instead (bit 1 is still the P0 probe at boot).
 *
 * Gateway kern.sweet_nfc_gw:
 *   0 (default): nothing reaches the chip. Every host packet is logged (hex, rate-limited). CORE_RESET_CMD gets a
 *     local NCI 1.0 CORE_RESET_RSP and (kern.sweet_nfc_id bit 0, default on) a CORE_RESET_NTF carrying an NXP
 *     identity (manufacturer 0x04, info kern.sweet_nfc_id_info = hw/rom/fw-major/fw-minor, default 42 02 01 6A:
 *     PN80V = hw 0x42, ROM 2, PN552_FW_02_01_6A; 1125: was 51 = "Unknown platform" for libNFC_Comet).
 *     kern.sweet_nfc_id bit 1 (default off) also answers CORE_INIT_CMD locally with an NCI 1.0 CORE_INIT_RSP
 *     carrying the same identity. Every other command gets STATUS_REJECTED; data is dropped.
 *     1125: kern.sweet_nfc_id bit 2 (default off) = the PN80V persona (persona_packet() below): the whole libNFC_Comet
 *     init sequence is answered locally, nothing reaches the chip (still mode 0), no CORE_RESET_NTF (NCI 1.0).
 *   1 (for later; off): host CORE_RESET powers and resets the chip (VEN pulse, ST CORE_RESET_NTF, the driver's own
 *     CORE_INIT and PROP_NFC_MODE_SET), answers the host locally as in mode 0, then forwards the section-4 whitelist
 *     (filtered) and sends the chip's packets up (no NCI 2.0 -> 1.0 rewriting yet: P2). The SE7 FIFO code sends at
 *     most 8 bytes per write, so longer host commands are refused (reason bus_len) until a FIFO-refill TX exists.
 *
 * HARD RULES (all modes, no switch): gw_filter() below, and the same filter again at the bus in
 * sweet_nfc_tx_allowed() (a host packet reaches 0x08 only if the filter forwards it byte-for-byte unchanged):
 *   no host packet with first byte 0x2F (GID 0x0F) ever; only MT=CMD (unsegmented) or MT=DATA from the host
 *   (download frames are invalid or data off-session); AppleStockholmControl DownloadRequest=1 refused; NFCEE_*:
 *   DISCOVER answered "0 NFCEEs" locally, everything else refused; CORE_CONN_CREATE to an NFCEE refused;
 *   RF_SET/GET_LISTEN_MODE_ROUTING refused; data on conn 1 (HCI) refused, data only on conn 0 while an RF interface
 *   is active; CORE_SET_CONFIG passes ids 0x00-0x7F only (others stripped, logged); CORE_GET_CONFIG standard ids
 *   only; RF_DISCOVER and RF_DISCOVER_MAP lose every listen entry; anything not on the list is refused; loader mode
 *   latches (nothing from the host is accepted until reboot). The only pin driven is gpio12 (VEN): spi@980000,
 *   gpio26/35/55/67 are never touched by this file.
 *
 * Counters: kern.sweet_nfc_host_pkts, kern.sweet_nfc_refused ("bus=N prop2f=N ..."). Log lines "SWEET NFC:".
 */

enum {
	RF_PROP, RF_INVALID, RF_DOWNLOAD, RF_NFCEE, RF_HCI, RF_LROUTING, RF_CONN_NFCEE, RF_CFG, RF_LISTEN, RF_UNLISTED,
	RF_LOADER, RF_MODE0, RF_DATA, RF_BUSLEN, RF_CHIP, RF_N
};
static const char *const rf_names[RF_N] = {
	"prop2f", "invalid", "download", "nfcee", "hci", "listen_routing", "conn_nfcee", "config_ids", "listen_modes",
	"unlisted", "loader", "mode0", "data", "bus_len", "chip"
};
enum { V_FWD, V_LOCAL, V_REJ };

static LCK_MTX_DECLARE(gw_lock, &nfc_lock_group);
static LCK_MTX_DECLARE(p1_lock, &nfc_lock_group);
static kern_ctl_ref gw_ctl;
static uint32_t nfc_p1, nfc_gw_mode;
static uint32_t nfc_id_bits = 1, nfc_id_info = 0x4202016au;   /* 1125: hw 0x42 = PN80V (was 0x51) */
static uint32_t nfc_fwif = 0x0015;     /* 1125/1127: 2F 19 answer; MwFwCompatibilityChkProcess wants 0x15 for hw 0x42 */
static uint32_t nfc_fwif_fmt;          /* 1127: kern.sweet_nfc_fwif_fmt, persona variants (see persona_packet) */
static volatile bool gw_dereg_pending; /* 1125: withdrawn while a client was connected: deregister at its close */
static uint32_t gw_conns;
static struct { uint16_t op; uint16_t n; } persona_seen[32];   /* 1125: (gid << 8 | oid, count) answered by the persona */
static uint32_t persona_said;
static uint32_t gw_refused[RF_N];
static uint32_t gw_host_pkts, gw_host_bytes, gw_up_pkts, gw_up_fail, gw_said, gw_opt_said, gw_sel_said, gw_chip_said;
static volatile bool gw_session, gw_rx_running;
/* 1146: NXP HDLL download/bootloader capture (kern.sweet_nfc_fwif_fmt 0x40000, with the 0x4000 eSE group).
 * DownloadRequest(1) is accepted and latches gw_dl_mode; the HDLL frames nfcd then writes are logged and
 * NOTHING is forwarded to the chip (persona runs in gateway mode 0; gw_bus_ok()/sweet_nfc_tx_allowed() still
 * block all forwarding) and nothing is answered (capture only; nfcd times out on the FW-version read). */
static volatile bool gw_dl_mode;
static unsigned gw_dl_n;
static uint32_t gw_dl_frames;
static uint8_t gw_dl_buf[2 + 512 + 2];  /* [LEN_hi LEN_lo | payload | CRC16] */
static struct {
	bool connected, rf_active;
	uint32_t unit, crc, on_demand;
	char who[32];
	unsigned asm_n;
	uint8_t asm_buf[3 + 255];
	uint8_t termios[72];
	bool pend, crc_rx, crc_out;     /* 1127: packet held for a CRC16 trailer; CRC seen this session; reply gets CRC */
	unsigned pend_n, crc_k;
	uint16_t pend_crc;
	uint8_t pend_buf[3 + 255];
	bool chip_crc;                  /* 1132: SET_CONFIG A0 BF = 01 answered: every later reply carries a CRC16 */
} gw;

static bool
hdr_ok(uint8_t b0)
{
	return (b0 >> 5) <= 1;          /* MT 0 = data, 1 = command: all a host may send */
}

/*
 * The section-4 filter. p/n: one whole host packet. V_FWD: o/on = the bytes the chip may get (maybe stripped);
 * V_LOCAL: the gateway answers itself (CORE_RESET, CORE_INIT, NFCEE_DISCOVER, nothing left after stripping);
 * V_REJ: refused. *why: the refuse/strip reason or -1.
 */
static int
gw_filter(const uint8_t *p, unsigned n, uint8_t *o, unsigned *on, int *why)
{
	unsigned mt, gid, oid, i, k, at;

	*why = -1;
	*on = 0;
	if (n < 3 || n != 3u + p[2] || !hdr_ok(p[0])) {
		*why = RF_INVALID;
		return V_REJ;
	}
	mt = p[0] >> 5;
	if (mt == 0) {                  /* data: conn id in the low nibble, byte 1 RFU */
		unsigned conn = p[0] & 0x0fu;
		if ((p[0] & 0x10u) || p[1] != 0) {
			*why = RF_INVALID;
			return V_REJ;
		}
		if (conn == 1) {
			*why = RF_HCI;
			return V_REJ;
		}
		if (conn != 0 || !gw.rf_active) {
			*why = RF_DATA;
			return V_REJ;
		}
		memcpy(o, p, n);
		*on = n;
		return V_FWD;
	}
	gid = p[0] & 0x0fu;
	oid = p[1] & 0x3fu;
	if ((p[0] & 0x10u) || (p[1] & 0xc0u)) {    /* segmented command or RFU bits: not NCI as nfcd writes it */
		*why = RF_INVALID;
		return V_REJ;
	}
	if (gid == 0x0f) {
		*why = RF_PROP;
		return V_REJ;
	}
	switch (gid << 8 | oid) {
	case 0x000:                     /* CORE_RESET */
	case 0x001:                     /* CORE_INIT */
		return V_LOCAL;
	case 0x002:                     /* CORE_SET_CONFIG: standard ids 0x00-0x7F only */
		if (n < 4) {
			*why = RF_INVALID;
			return V_REJ;
		}
		memcpy(o, p, 4);
		at = 4;
		k = 0;
		for (i = 0, *on = 4; i < p[3]; i++) {
			if (at + 2 > n || at + 2u + p[at + 1] > n) {
				*why = RF_INVALID;
				return V_REJ;
			}
			if (p[at] < 0x80) {
				memcpy(o + *on, p + at, 2u + p[at + 1]);
				*on += 2u + p[at + 1];
				k++;
			} else {
				*why = RF_CFG;
			}
			at += 2u + p[at + 1];
		}
		if (at != n) {
			*why = RF_INVALID;
			return V_REJ;
		}
		o[2] = (uint8_t)(*on - 3);
		o[3] = (uint8_t)k;
		return k ? V_FWD : V_LOCAL;
	case 0x003:                     /* CORE_GET_CONFIG: standard ids only */
		if (n < 4 || n != 4u + p[3]) {
			*why = RF_INVALID;
			return V_REJ;
		}
		for (i = 0; i < p[3]; i++) {
			if (p[4 + i] >= 0x80) {
				*why = RF_CFG;
				return V_REJ;
			}
		}
		break;
	case 0x004:                     /* CORE_CONN_CREATE: loopback (1) or remote NFC endpoint (2) only */
		if (n < 4) {
			*why = RF_INVALID;
			return V_REJ;
		}
		if (p[3] == 0x03) {
			*why = RF_CONN_NFCEE;
			return V_REJ;
		}
		if (p[3] != 0x01 && p[3] != 0x02) {
			*why = RF_UNLISTED;
			return V_REJ;
		}
		break;
	case 0x005:                     /* CORE_CONN_CLOSE */
		if (n != 4 || p[3] == 0 || p[3] == 1) {
			*why = n != 4 ? RF_INVALID : RF_HCI;
			return V_REJ;
		}
		break;
	case 0x100:                     /* RF_DISCOVER_MAP: entries (protocol, mode, interface); poll bit (1) only */
		if (n < 4 || n != 4u + 3u * p[3]) {
			*why = RF_INVALID;
			return V_REJ;
		}
		memcpy(o, p, 4);
		for (i = 0, k = 0, *on = 4; i < p[3]; i++) {
			const uint8_t *e = p + 4 + 3 * i;
			if (e[1] & 0x02u) {
				*why = RF_LISTEN;
			}
			if (e[1] & 0x01u) {
				o[*on] = e[0];
				o[*on + 1] = 0x01;
				o[*on + 2] = e[2];
				*on += 3;
				k++;
			}
		}
		o[2] = (uint8_t)(*on - 3);
		o[3] = (uint8_t)k;
		return k ? V_FWD : V_LOCAL;
	case 0x101:                     /* RF_SET_LISTEN_MODE_ROUTING */
	case 0x102:                     /* RF_GET_LISTEN_MODE_ROUTING */
		*why = RF_LROUTING;
		return V_REJ;
	case 0x103:                     /* RF_DISCOVER: entries (tech & mode, frequency); poll modes (< 0x80) only */
		if (n < 4 || n != 4u + 2u * p[3]) {
			*why = RF_INVALID;
			return V_REJ;
		}
		memcpy(o, p, 4);
		for (i = 0, k = 0, *on = 4; i < p[3]; i++) {
			const uint8_t *e = p + 4 + 2 * i;
			if (e[0] >= 0x80) {
				*why = RF_LISTEN;
				continue;
			}
			o[*on] = e[0];
			o[*on + 1] = e[1];
			*on += 2;
			k++;
		}
		if (k == 0) {
			*why = RF_LISTEN;
			*on = 0;
			return V_REJ;
		}
		o[2] = (uint8_t)(*on - 3);
		o[3] = (uint8_t)k;
		return V_FWD;
	case 0x104:                     /* RF_DISCOVER_SELECT */
	case 0x106:                     /* RF_DEACTIVATE */
	case 0x108:                     /* RF_T3T_POLLING */
	case 0x110:                     /* RF_ISO_DEP_NAK_PRESENCE (NCI 2.0) */
		break;
	case 0x200:                     /* NFCEE_DISCOVER: "0 NFCEEs", locally */
		*why = RF_NFCEE;
		return V_LOCAL;
	default:
		*why = gid == 0x02 ? RF_NFCEE : RF_UNLISTED;
		return V_REJ;
	}
	memcpy(o, p, n);
	*on = n;
	return V_FWD;
}

/* Bus-level check (sweet_nfc_tx_allowed): a host packet only if the filter forwards it unchanged. */
static bool
gw_bus_ok(const uint8_t *tx, unsigned nt)
{
	uint8_t o[3 + 255];
	unsigned on = 0;
	int why;

	if (!gw_session || nfc_gw_mode != 1 || nt < 3 || nt > sizeof(o)) {
		return false;
	}
	return gw_filter(tx, nt, o, &on, &why) == V_FWD && on == nt && memcmp(o, tx, nt) == 0;
}

static void
gw_hexlog(const char *dir, const uint8_t *b, unsigned n, const char *note)
{
	char h[3 * 60 + 4];

	hex(h, sizeof(h), b, n > 60 ? 60 : n);
	SAY("%s %u [%s%s]%s%s\n", dir, n, h, n > 60 ? " .." : "", note ? " " : "", note ? note : "");
}

/* append a packet to the reply buffer (sent up after gw_lock is dropped) */
static void
out_put(uint8_t *out, unsigned *no, unsigned cap, const uint8_t *b, unsigned n)
{
	if (*no + n <= cap) {
		memcpy(out + *no, b, n);
		*no += n;
	}
}

static void
out_status(uint8_t *out, unsigned *no, unsigned cap, uint8_t gid, uint8_t oid, uint8_t status)
{
	const uint8_t r[4] = { (uint8_t)(0x40u | gid), oid, 1, status };

	out_put(out, no, cap, r, sizeof(r));
}

/* the identity: hw, rom, fw major, fw minor (kern.sweet_nfc_id_info, default 42 02 01 6A) */
static void
id_bytes(uint8_t *b)
{
	uint32_t v = nfc_id_info;

	b[0] = (uint8_t)(v >> 24);
	b[1] = (uint8_t)(v >> 16);
	b[2] = (uint8_t)(v >> 8);
	b[3] = (uint8_t)v;
}

static void
local_reset(const uint8_t *p, unsigned n, uint8_t *out, unsigned *no, unsigned cap)
{
	uint8_t cfg = (n >= 4 && p[3]) ? 1 : 0;
	const uint8_t rsp[6] = { 0x40, 0x00, 0x03, 0x00, 0x10, cfg };   /* NCI 1.0: status, version 1.0, config status */
	uint8_t nt[12] = { 0x60, 0x00, 0x09, 0x02, cfg, 0x10, 0x04, 0x04 };    /* reason: CORE_RESET_CMD; NXP; 4 info bytes */

	id_bytes(nt + 8);
	out_put(out, no, cap, rsp, sizeof(rsp));
	bool ntf = (nfc_id_bits & 1) && !(nfc_id_bits & 4);

	if (ntf) {
		out_put(out, no, cap, nt, sizeof(nt));
	}
	SAY("gw: CORE_RESET_CMD (%s form, type %u) answered locally: RSP NCI 1.0%s, id %02x %02x %02x %02x\n",
	    n == 4 ? "1-byte" : "odd", cfg, ntf ? " + NTF" : (nfc_id_bits & 4) ? " (persona: no NTF)" : "", nt[8], nt[9],
	    nt[10], nt[11]);
}

static void
local_init(const uint8_t *p, unsigned n, uint8_t *out, unsigned *no, unsigned cap)
{
	/* NCI 1.0 CORE_INIT_RSP: status, features(4), RF interfaces (n + list), max logical conns, max routing table
	 * size(2), max control payload, max large-parameter size(2), manufacturer id, manufacturer info(4) */
	uint8_t r[3 + 24] = { 0x40, 0x01, 0,
		              0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x01, 0x02, 0x03, 0x01, 0x00, 0x00, 0xff, 0x00, 0x00, 0x04 };
	unsigned len = 19;

	(void)p;
	id_bytes(r + len);
	len += 4;
	r[2] = (uint8_t)(len - 3);
	out_put(out, no, cap, r, len);
	SAY("gw: CORE_INIT_CMD (%s, %u payload bytes) answered locally: NCI 1.0 CORE_INIT_RSP, manuf 0x04 id %02x %02x %02x %02x\n",
	    n == 3 ? "NCI 1.0 form" : n == 5 ? "NCI 2.0 form" : "odd form", n - 3, r[len - 4], r[len - 3], r[len - 2], r[len - 1]);
}

/*
 * 1125: the PN80V persona (kern.sweet_nfc_id bit 2, gateway mode 0 only; nothing here touches the chip).
 * From the 16.7 dyld cache, libNFC_Comet (NXP phLibNfc/phNciNfc) init sequence gphLibNfc_InitializeSequence
 * @0x2125326f8: Initialize (CORE_RESET + CORE_INIT) -> ConfigHsuBr -> ReApplyHsuBr -> CrcConfig -> ReInitialize ->
 * MwFwCompatibilityChk -> EnablePropExtn -> GetFwType -> DisableStandbyMode -> GetEepromParams -> InitSetConfig ->
 * SetEepromParams -> SetLsntModeRtng -> InitSetMapping -> ReInitialize -> EnablePropExtn -> SwpIntfControl ->
 * GetNfceeIntfCfg -> EnableNfceeDisc -> InitializeComplete. The P1 run (k10-1116) died at the first CORE_INIT
 * (mode 0 rejected it), after which nfcd asked for DownloadRequest(1).
 *   - phNciNfc_ProcessInitRsp (0x1f51698d8): NCI 1.0 layout, payload >= 17, status 0, 1..8 RF interfaces, then
 *     manufacturer id + 4 info bytes hw, rom, fw major, fw minor. Only hw 06-08/0d/1d/42/a2-a4/b1/b2/ba/d0-d2 are
 *     known platforms (phLibNfc_GetPlatformInfo 0x1f5159764); 0x51 is "Unknown platform". hw 0x42 = model 5 =
 *     "PN80v" (libnfrestore table @0x202d973d0); ROM 2 selects PN552_FW_02_01_6A_rev125914 (production; AF is mfg).
 *   - phLibNfc_MwFwCompatibilityChk (0x1f513949c): for hw 0x42 (flag +0xafb set by phLibNfc_GetNfccFeatures
 *     0x1f517ba00) FW minor 0xa0..0xdf skips the check; 0x6A sends 2F 03 (phNciNfc_GetFwInterfaceVersionCmd) and
 *     phLibNfc_MwFwCompatibilityChkProcess (0x1f51781ec) wants interface version 0x0015 (big endian in the RSP,
 *     status 0, 3 bytes), else "FW IS TOO OLD/NEW" (0x80/0x81 -> firmware update).
 *   - every other NXP proprietary (2F xx) command gets STATUS_OK and no data; CORE_GET_CONFIG answers each id with
 *     one zero byte; SET_CONFIG, RF_DISCOVER_MAP, listen routing (local only, no route exists anywhere), RF_DISCOVER
 *     (nothing is ever found: the field stays off) and RF_DEACTIVATE are acknowledged; NFCEE_DISCOVER = 0 NFCEEs.
 *   - not answered (refused as before): NFCEE_MODE_SET/POWER, CORE_CONN_CREATE, RF_GET_LISTEN_MODE_ROUTING,
 *     RF_DISCOVER_SELECT, data. DownloadRequest(1) is still refused.
 * 1127: 1125 took 2F 03 for GetFwInterfaceVersion. It is not: the NXP command builders load the header {MT 1, GID 0x0F}
 * from 0x1f51e3160 and store the OID at [sp,#8]: 2F 00 SendStandby (0x1f513d304), 2F 02 EnableExtn (0x1f51ce31c),
 * 2F 03 ConfigHsuBr (0x1f51302c0; the host's 2F 03 08 .. is the HSU baud-rate set, init step 2 after CORE_INIT),
 * 2F 06 GetFwType (0x1f51475c4), 2F 19 GetFwInterfaceVersion (0x1f5137d38). Response checks:
 *   2F 03 phNciNfc_ConfigHsuBrCmdRsp 0x1f516a154: payload length must be exactly 1 and status 0. 1125's 3-byte answer
 *     gave "Configure HSU baud rate failed!" -> power cycle -> DownloadRequest(1) (phone run p16).
 *   2F 19 phNciNfc_GetFwInterfaceVersionRsp 0x1f5169f34: length exactly 3, status 0 (status 1 or 5 = "OLD FW" 0x80);
 *     bytes 1,2 -> ctx+0x1120, compared after ldrh + rev16 (big endian) with 0x15 for hw 0x42
 *     (MwFwCompatibilityChkProcess 0x1f51781ec).
 *   2F 02 phNciNfc_EnableExtnRsp 0x1f51ce41c: length exactly 5, status 0; bytes 1..4 -> ctx+0xb30 (big endian word,
 *     phLibNfc_EnableExtnProcess 0x1f5178430). We send the identity bytes.
 *   2F 06 phNciNfc_GetFwTypeRsp 0x1f51476c4: length 2 (status 0 + type) or >= 22 (longer info form). We send 2.
 *   2F 00 phNciNfc_ProcessStandbyRsp 0x1f516a054: length 1, status 0 (0xF1 = express/VAS busy).
 * Init table gphLibNfc_InitializeSequence 0x2125326f8 (pairs cmd/process): Initialize, ConfigHsuBr (2F 03),
 * ReApplyHsuBr (2F 03 again), CrcConfig, ReInitialize, MwFwCompatibilityChk (2F 19), EnablePropExtn (2F 02),
 * GetFwType (2F 06), DisableStandbyMode (2F 00), GetEepromParams, InitSetConfig, ... NFCEE discovery, complete.
 * CORE_RESET_NTF: not needed (p16: the NCI 1.0 RSP alone moved the stack on to CORE_INIT and the CORE_INIT_RSP was
 * accepted, it went on to ConfigHsuBr).
 * CRC: CrcConfig (0x1f5139bd8) may turn on the stack's CRC (CRC-16/CCITT-FALSE: init 0xFFFF, poly 0x1021, MSB first,
 * appended big endian, phNciNfc_CalcCrc16 0x1f51da7ac / CoreUtilsAppendCrc 0x1f5139054); CrcConfigProcess
 * 0x1f5177f90 sets the check flag (ctx+0x146c) and maybe IO_STOCKHOLM_CRC through phTmlNfc_IoCtl 0x46/0x47.
 * In persona mode a host packet followed in the same write by its valid CRC16 has the trailer taken off and
 * logged. kern.sweet_nfc_fwif_fmt (default 0) picks the variant without a rebuild:
 *   bits 0-1  reply CRC16: 0 mirror (a reply gets a CRC iff the command it answers had one), 1 never, 2 on every
 *             reply once the host sent one CRC frame or set IO_STOCKHOLM_CRC this session
 *   bit 2 (4) 1125 answers to 2F xx (2F 03 = 3-byte interface version, other 2F = status only), for A/B
 *   bit 3 (8) GetFwType (2F 06) type byte 0x01 instead of 0x00
 */
/*
 * 1128: NXP config space for the persona. phone run n12 (k12): CORE_GET_CONFIG [A0 0F] got the generic one zero
 * byte, nfcd power-cycled the chip and asked DownloadRequest(1). libNFC_Comet (16.7 dyld cache):
 *   - phLibNfc_GetEepromParams (init step 9, 0x1f51393cc) asks GET_CONFIG with only A0 0F (+4 bit 0x1000 sys-config
 *     group, +0x244 bit 8). The TLV store phNciNfc_ProcessRfParams/StorePropParams (0x1f51d4608; A0 0F at 0x1f51d4e7c)
 *     wants length exactly 0x40 (64): else the TLV is an error, phLibNfc_GetEepromParamsProc (0x1f51787c0) logs
 *     "EEPROM params read failed" (+0x245 bit 0 clear), returns 0xFF, init fails -> recovery = power cycle + firmware
 *     download. So it was our length, not a value.
 *   - A0 0F = the MW "EEPROM user area" (64 bytes): bytes 0-7 copied to ctx+0xd68: wCurrMwVer (LE16), wCurrFwVer (LE16),
 *     bConfig (LE32); the rest is not read. GetEepromParamsProc: bConfig 0xC3C3A5A5 -> "Skipping set config",
 *     phLibNfc_SkipSequenceSeq(+5): steps 10-14 (InitSetConfig, SetEepromParams, SetLsntModeRtng, InitSetMapping,
 *     ReInitialize) are skipped, next is 15. bConfig 0 or 0xA5A5C3C3 -> "Firmware updated, continue with NFCC
 *     configurations" (ctx+0xbda = 1), any other value -> "Unexpected MW Log Area Value still forcing MW config";
 *     both go on with step 10. The versions are only logged. phLibNfc_SetEepromParams (step 11, 0x1f5178e84) writes
 *     SET_CONFIG A0 0F 40 {34 01, ctx+0xb2c LE16, A5 A5 C3 C3, 56 x 00}: a configured chip reads back C3C3A5A5.
 *     Default here: {34 01, fw minor, fw major, A5 A5 C3 C3, 00..} = "already configured" (skip path).
 *   - other A0 ids the store checks: 12/22/23/24/E9 length 1; E8/EF/F0 length 0x45; EA/EB/1E length 8; 17 <= 0x1F;
 *     18 <= 0xD6; 02 04 07 08 13 19 D5 EC D4 ED A2 F2 44 37 F5 06 68 ... read byte 0. An A0 id it does not know is an
 *     error. GET_CONFIG RSP status 0 needs the RSP count == the number of ids asked (phNciNfc_GetConfigRsp 0x1f51cfe74).
 *   - step 16 SwpIntfControl (0x1f51bd958): SET_CONFIG A0 EC, A0 ED (+A0 D4 if ctx+0xafa) from ctx+0x1d0; its Proc
 *     ignores the answer. Step 17 GetNfceeIntfCfg (0x1f5135c94): GET_CONFIG A0 EC, A0 ED (+A0 D4); Proc 0x1f5179324
 *     needs the SWP group present (+4 bit 0x2000); value 0 = that SWP off (ctx+0x145/0x146 = 2). Step 18
 *     EnableNfceeDisc (0x1f5179494): all of 0x145..0x148 == 2 -> no NFCEE_DISCOVER at all, straight to step 19
 *     InitializeComplete. Default A0 EC = A0 ED = A0 D4 = 00 (no SE, like the 0-NFCEE answer).
 *   - SET_CONFIG RSP (0x1f51cf350) needs >= 2 bytes, status 0; listen routing (21 01) and RF_DISCOVER_MAP (21 00) RSP
 *     1 byte status 0: already answered so.
 * kern.sweet_nfc_cfg_set (string, rw): "A00F=<hex>[,A0EC=<hex>...]" sets the value (and length) returned for that id;
 * "A0EC=" drops one override, "clear" drops all. Read: the overrides, "id:len:first bytes". 2-byte ids are A0xx/A1xx,
 * a standard id is written 00xx. kern.sweet_nfc_fwif_fmt bit 4 (0x10): SET_CONFIG of A0xx stored as an override (EEPROM
 * mirror; default off), bit 5 (0x20): the 1125-1127 GET_CONFIG answer (each id = one zero byte), for A/B.
 * SET_CONFIG is never forwarded in persona mode (mode 0: nothing reaches the chip).
 */
/*
 * 1132: the post-init CORE_RESET. phone run v14 (k14): after InitializeComplete nfcd set the NCI CRC check
 * (phLibNfc_Ioctl_PropSetCrcChkConfig 0x1f5143194 -> phLibNfc_SM_IoctlCrcChkConfigTrans 0x1f513e808: ctx+0x146c = 1,
 * prop config {A0 BF, 01} -> 2F 03, 2F 02, SET_CONFIG A0 BF = 01). phLibNfc_IoctlSetPropRfCfgComplete ("All Rf
 * configs are set", 0x1f5154d08) then sets the NCI check flag (phNciNfc_UpdateCrcChkFlag 0x1f51da5fc, nci+0x16c1 =
 * ctx+0x146c = 1) and starts the ReInit sequence 0x212532618: ReInitialize (CORE_RESET 20 00 01 00 + CORE_INIT),
 * CrcConfig, ReInitialize, EnablePropExtn, ReConfigHsuBr, ReApplyHsuBr, ChipScopeLogCfg x2 (SET_CONFIG),
 * GetNfceeIntfCfg, ReInitDisableNfceeDisc, EnableNfceeDisc, ReInitializeComplete. With the check flag on,
 * phNciNfc_CoreRecvChkPktType (0x1f513cb24) runs phNciNfc_CoreUtilsVerifyCrc (0x1f513f12c) on every received packet:
 * CRC-16/CCITT-FALSE over header+payload, 2 trailer bytes big endian after the payload (not in the NCI length);
 * a packet without it fails ("Incorrect CRC", phNciNfc_InvokeCrcErrNtf) and is dropped. Our CORE_RESET_RSP had no
 * trailer -> dropped -> phNciNfc_SendReset (0x1f513b48c) 2000 ms timeout -> one resend -> failure -> Power 0,
 * DownloadRequest(0), init again (8 rounds -> DownloadRequest(1)). The RSP itself was right: phNciNfc_ProcessResetRsp
 * (0x1f51697dc) needs payload length 3, status 0 (NCI 1.x: no CORE_RESET_NTF is waited for, as in init step 0); it
 * copies the check flag to the append flag (nci+0x16c0 = +0x16c1), so the host's own packets carry a CRC16 from then
 * on (1127 takes those trailers off). So: after the RSP to SET_CONFIG A0 BF = 01 (sent plain: the host turns its
 * check on only after it), every reply gets a CRC16 trailer (gw.chip_crc) until A0 BF = 00, VEN low (Power 0) or a
 * new connect. kern.sweet_nfc_fwif_fmt bit 6 (0x40): ignore A0 BF (k14 behaviour); bit 7 (0x80): keep the CRC state
 * over Power 0 (EEPROM-like). Nothing is forwarded to the chip.
 */
#define CFG_N   16
#define CFG_MAX 72
static struct { uint16_t id; uint8_t len; uint8_t val[CFG_MAX]; } nfc_cfg[CFG_N];   /* 1128: overrides; gw_lock */
static unsigned nfc_cfg_n;
static int persona_st = -1;             /* 1128: last init-table step index seen (-1 = none this session) */
static int gw_crc_next;                 /* 1132: 1/2 = chip CRC on/off after the reply now being built; gw_lock */
/*
 * 1134: why the ReInit after A0 BF = 01 ends in CORE_RESET + Power 0 (phone run v15, k15). Every reply of the
 * ReInit (0x212532618) was taken (CRCs right, no timeout); the 2F 03 ReApplyHsuBr and ChipScope SET_CONFIG steps are
 * skipped by host-side conditions, not by a reply. GetNfceeIntfCfgProc (0x1f5179324) turned our A0 ED = 00 into
 * ctx+0x146 = 2 (eSE off); with 0x145..0x148 all 2, ReInitializeComplete (0x1f51432dc) runs the static feature
 * config sequence 0x2125325a8 (SetStandbyMode = our 2F 00 01 01, NfceeDisable x4 and FlushSramData send nothing),
 * then phLibNfc_staticFeatureCfgComplete (0x1f5141880) -> VerifySEStatus (0x1f5141990). During the first init the
 * init callback (ctx+0x16e8) is still set and 0x146 = 2 passes; after it (ReInit) the test is
 * (0x146 != 1) == (ctx+0x1d0 bit 1), and nfcd's SE config has bit 1 set (it is what SwpIntfControl writes as
 * SET_CONFIG A0 ED = 01: the eSE on SWP2): an eSE that is expected but not connected -> 0xFF -> "NFCC Re-Init
 * failed" -> DeInitialize (CORE_RESET 0.3 ms later) + Power 0 -> init again (9 rounds) -> nfrestore DownloadRequest(1).
 * ctx+0x146 = 1 is set only by the SE notification handler (phLibNfc_SENtfHandler 0x1f51332ec) for a connected eSE
 * NFCEE (id 0xC0), so a persona without an eSE cannot pass the ReInit. Variants (kern.sweet_nfc_fwif_fmt):
 *   bit 8 (0x100) virtual eSE: GET_CONFIG A0 EC/ED/D4 answers the value the host set (SwpIntfControl) and
 *                 NFCEE_DISCOVER (enable) reports 1 NFCEE for A0 ED = 01: NTF 62 00 {C0, 00 enabled, 1 protocol
 *                 00 APDU, 0 TLVs}; NFCEE_MODE_SET (22 01) answered OK (NCI 1.0: no NTF). Nothing reaches the chip.
 *   bit 9 (0x200) the same, only while the chip CRC is on (the ReInit after A0 BF = 01; the first init unchanged).
 * Default 0 = k15 behaviour. Each ReInit step gets a klog line `persona: 1134: ReInit #N step S <name>`.
 */
static int persona_ri = -1;             /* 1134: -1 no ReInit; 0..10 ReInit step; 50 static cfg seen; 60 DeInit; gw_lock */
static unsigned persona_ri_hsu, persona_ri_n, ri_said;
static uint8_t swp_set[3], swp_have;    /* 1134: last SET_CONFIG value of A0EC/A0ED/A0D4 (bit i of swp_have = valid) */

static int
swp_idx(uint16_t id)
{
	return id == 0xa0ec ? 0 : id == 0xa0ed ? 1 : id == 0xa0d4 ? 2 : -1;
}

/* 1134: the virtual eSE is on (kern.sweet_nfc_fwif_fmt 0x100, or 0x200 while the chip CRC is on) */
static bool
vese_on(void)
{
	return (nfc_fwif_fmt & 0x100) || ((nfc_fwif_fmt & 0x200) && gw.chip_crc);
}

/*
 * 1136: virtual HCI network (kern.sweet_nfc_fwif_fmt 0x400, only together with 0x100/0x200, i.e. vese_on()).
 * libNFC_Comet starts the eSE child sequence only from phLibNfc_HciInitComplete, which needs the HCI network NFCEE
 * (ID 0x01); without it InitializeComplete/ReInitializeComplete wait ~9 s (watchdog 1 s x 9) and the init fails
 * (logs/research/nfc-ese-2026-11-16.md, Q1-Q3). With 0x400:
 *   NFCEE_DISCOVER enable: RSP 42 00 02 00 02, NTF 62 00 05 01 00 01 01 00 (HCI network, enabled, HCI access),
 *     NTF 60 06 03 01 01 01 (CORE_CONN_CREDITS, static conn 1), then the 1134 NTF for the eSE C0.
 *   NFCEE_MODE_SET 01: 42 01 01 00 (NCI 1.0, no NTF), as 1134 does for C0.
 *   HCP on static conn 1 (01 00 LL ..): answered here. Admin pipe (0x81): ANY_OPEN_PIPE, ANY_SET_PARAMETER (WHITELIST),
 *     ADM_CLEAR_ALL_PIPE and other admin commands -> 01 00 02 81 80 (ANY_OK); ANY_GET_PARAMETER SESSION_IDENTITY ->
 *     ANY_OK + 8 fixed bytes; commands on other pipes -> 8p 81 (ANY_E_NOK); events/responses get no reply.
 *     Every host data packet is followed by the credit NTF 60 06 03 01 01 01.
 *   CORE_CONN_CREATE: logged; answered 40 04 04 00 ff 01 02 only with bit 0x800, never for destination type 03 (NFCEE).
 * Everything is generated in persona_packet(), which runs only in gateway mode 0 (nothing is forwarded there).
 * gw_filter() is unchanged: it still refuses conn-1 data (RF_HCI) and NFCEE_* commands for every sysctl value, and
 * sweet_nfc_tx_allowed() re-checks at the bus. No 2F packet, no pin. Klog lines: "SWEET NFC: nfc-hci: persona: 1136: HCI".
 */
static unsigned hci_said;
static const uint8_t hci_session_id[8] = { 0x52, 0x46, 0x4e, 0x30, 0x00, 0x00, 0x00, 0x01 };     /* "RFN0" 00 00 00 01 */
static const uint8_t hci_credit_ntf[6] = { 0x60, 0x06, 0x03, 0x01, 0x01, 0x01 };     /* conn 1, +1 credit */
/*
 * 1137: phone run k17n: OpenLogConn sends CORE_CONN_CREATE 20 04 06 03 01 01 02 01 01 (destination NFCEE, TLV 01 =
 * NFCEE 01 + protocol 01 HCI access), and 1136 refused it, so the HCI init never started. With 0x800 (and 0x400 +
 * vese_on()) exactly that packet, the virtual HCI network, is answered here: 40 04 04 00 ff 01 02 (status, max payload,
 * credits, conn ID 02), and conn 02 becomes the HCI connection: its HCP data gets the same local responder as static
 * conn 1, credit NTF 60 06 03 01 02 01, and CORE_CONN_CLOSE 02 -> 40 05 01 00. Any other destination or NFCEE ID keeps
 * the refusal. gw_filter() still refuses data on any conn but 0 (RF_DATA/RF_HCI) and every NFCEE destination
 * (RF_CONN_NFCEE), so nothing of this reaches the chip. Cleared on CORE_RESET and at session start/stop.
 */
static uint8_t hci_conn;                /* 1137: dynamic conn ID of the virtual HCI network, 0 = none; gw_lock */
/*
 * 1138 (logs/research/nfc-ese-2026-11-16.md, follow-up "bit 0x1000"): libNFC_Comet waits for the NXP System End of
 * Operation NTF 6F 20 {NFCEE, status} (phNciNfc_ProcessSysEoOpNtf 0x1f514a1a0): after an NFCEE_MODE_SET_RSP for any
 * NFCEE but 01 it runs a 2000 ms set-mode timer that fails the mode set without it, and ModeSetHciNwk waits 4 s for
 * the eSE's EoOP when none was seen. kern.sweet_nfc_fwif_fmt 0x1000 (only with 0x400 + vese_on()): 6F 20 02 C0 00 after
 * the NFCEE_DISCOVER enable packets and right after every NFCEE_MODE_SET_RSP for C0 (mode 00/01); never for MODE_SET 01.
 * 0x2000: GET_CONFIG A0EB (eSE session ID) returns the last value the host SET for A0EB (persona-local only).
 * Host-bound packets only, through out_put (CRC trailer as for every reply); nothing reaches the chip.
 */
static const uint8_t hci_eoop_ntf[5] = { 0x6f, 0x20, 0x02, 0xc0, 0x00 };     /* EoOP: NFCEE C0, status 00 ready */
static uint8_t a0eb_set[16];            /* 1138: last SET_CONFIG A0EB value from the host; gw_lock */
static unsigned a0eb_len;               /* 0 = none seen */

static bool
vhci_on(void)
{
	return (nfc_fwif_fmt & 0x400) && vese_on();
}

/*
 * 1139 (logs/research/nfc-ese-2026-11-16.md, "Follow-up 2"): the eSE power cycle (ClearEseSessionId -> "eSE ReInit
 * Sequence" 0x212535298) starts with NFCEE_MODE_SET C0 00. A disable is completed by the NXP NTF 6F 18 ("IceFall Power
 * OFF", phNciNfc_ProcessModeSetDisableNtf 0x1f513750c), not by EoOP 6F 20 (dropped for a disable: ctx+0xddc = 0).
 * kern.sweet_nfc_fwif_fmt 0x4000 (only with 0x400 + 0x1000 + vese_on()): MODE_SET C0 00 -> 42 01 01 00 + 6F 18 01 00
 * (0x8000: 6F 18 02 C0 00 instead), no 6F 20; MODE_SET C0 01 -> 42 01 01 00 + 6F 20 02 C0 00 (1138); NFCEE_DISCOVER
 * disable -> 42 00 02 00 00; repeated NFCEE_DISCOVER enable / CORE_CONN_CREATE answered as the first time (conn 2
 * re-tracked, never "already open"); GET_CONFIG A0EB = the last host-set value (as 0x2000), so ff*8 verifies on the
 * second pass. Host-bound only via out_put; nothing reaches the chip.
 */
static bool
ese_pc_on(void)
{
	return vhci_on() && (nfc_fwif_fmt & 0x1000) && (nfc_fwif_fmt & 0x4000);
}

/*
 * 1140 (research doc "Follow-up 3"): phNciNfc_Nfcee_ModeSet stores its pending callback (ctx+0x260/0xba8) only after
 * the 22 01 write returns; the persona's NTF in the same read as the RSP is handled before that and takes the generic
 * path, so the 2 s set-mode timer fails the mode set. kern.sweet_nfc_fwif_fmt 0x10000 (with 0x4000 etc.): MODE_SET C0
 * 00/01 -> 42 01 01 00 alone; the 6F 18 (disable; 0x8000 form) / 6F 20 02 C0 00 (enable) follows as a separate packet
 * after kern.sweet_nfc_ntf_delay_ms (default 100, clamped to 0..1500) from a thread_call. 0x20000 (diagnostic): after
 * the disable RSP send nothing. The queued NTF is dropped on CORE_RESET, session open and close. The thread_call takes
 * gw_lock (a mutex; never a spinlock), builds the packet with the CRC16 trailer if the reply it belongs to had one,
 * drops the lock and sends with ctl_enqueuedata like gw_ctl_send. Host-bound only; nothing reaches the chip.
 */
static uint32_t nfc_ntf_delay_ms = 100;         /* kern.sweet_nfc_ntf_delay_ms */
static thread_call_t ntf_call;                  /* allocated on first use (gw_lock is a mutex: may block) */
static uint8_t ntf_q[8];
static unsigned ntf_qn;                         /* 0 = nothing queued; gw_lock */
static bool ntf_q_crc;
static uint32_t ntf_q_unit, ntf_q_gen, ntf_q_ms;
static uint64_t ntf_q_t0;

static uint64_t
ntf_now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

/* gw_lock held */
static void
ntf_cancel(const char *why)
{
	if (ntf_qn != 0) {
		SAY("nfc-hci: persona: 1140: HCI deferred NTF %02x %02x dropped (%s) t=%llu us\n", ntf_q[0], ntf_q[1], why,
		    (unsigned long long)ntf_now_us());
	}
	ntf_qn = 0;
	ntf_q_gen++;
	if (ntf_call != NULL) {
		(void)thread_call_cancel(ntf_call);
	}
}

static uint16_t nci_crc16(const uint8_t *b, unsigned n);

static void
ntf_fire(__unused thread_call_param_t p0, thread_call_param_t p1)
{
	uint8_t b[sizeof(ntf_q) + 2];
	unsigned n;
	uint32_t unit;
	uint64_t t = ntf_now_us(), t0;
	bool crc;

	lck_mtx_lock(&gw_lock);
	n = ntf_qn;
	if (n == 0 || (uint32_t)(uintptr_t)p1 != ntf_q_gen) {     /* stale call: a newer NTF is queued or none */
		lck_mtx_unlock(&gw_lock);
		return;
	}
	if (!gw.connected || gw.unit != ntf_q_unit) {
		ntf_qn = 0;
		lck_mtx_unlock(&gw_lock);
		return;
	}
	memcpy(b, ntf_q, n);
	crc = ntf_q_crc;
	if (crc) {
		uint16_t c = nci_crc16(b, n);

		b[n] = (uint8_t)(c >> 8);
		b[n + 1] = (uint8_t)c;
	}
	unit = gw.unit;
	t0 = ntf_q_t0;
	ntf_qn = 0;
	lck_mtx_unlock(&gw_lock);
	if (gw_ctl != NULL && ctl_enqueuedata(gw_ctl, unit, b, n + (crc ? 2 : 0), 0) == 0) {
		gw_up_pkts++;
		SAY("nfc-hci: persona: 1140: HCI <persona deferred %u [%02x %02x %02x %02x%s]%s sent t=%llu us (+%llu us after the RSP, delay %u ms)\n",
		    n, b[0], b[1], b[2], b[3], n > 4 ? " .." : "", crc ? " +CRC16" : "", (unsigned long long)t,
		    (unsigned long long)(t - t0), ntf_q_ms);
	} else {
		gw_up_fail++;
		SAY("nfc-hci: persona: 1140: HCI <persona deferred %02x %02x send FAILED t=%llu us\n", b[0], b[1],
		    (unsigned long long)t);
	}
}

/* gw_lock held: queue one NTF to go up as its own packet after kern.sweet_nfc_ntf_delay_ms */
static void
ntf_queue(const uint8_t *b, unsigned n)
{
	uint64_t dl;
	uint32_t ms = nfc_ntf_delay_ms > 1500 ? 1500 : nfc_ntf_delay_ms;

	if (n > sizeof(ntf_q)) {
		return;
	}
	if (ntf_call == NULL) {
		ntf_call = thread_call_allocate(ntf_fire, NULL);
		if (ntf_call == NULL) {
			SAY("nfc-hci: persona: 1140: HCI thread_call_allocate failed: NTF %02x %02x not sent\n", b[0], b[1]);
			return;
		}
	}
	ntf_cancel("replaced");
	memcpy(ntf_q, b, n);
	ntf_qn = n;
	ntf_q_crc = gw.crc_out;
	ntf_q_unit = gw.unit;
	ntf_q_ms = ms;
	ntf_q_t0 = ntf_now_us();
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &dl);
	(void)thread_call_enter1_delayed(ntf_call, (thread_call_param_t)(uintptr_t)ntf_q_gen, dl);
	SAY("nfc-hci: persona: 1140: HCI deferred NTF [%02x %02x %02x %02x%s] queued t=%llu us, delay %u ms%s\n", b[0], b[1],
	    b[2], b[3], n > 4 ? " .." : "", (unsigned long long)ntf_q_t0, ms, ntf_q_crc ? " +CRC16" : "");
}

/*
 * 1141 (research doc "Follow-up 4"): libNFC_Comet prints a middleware message only if table[component] >= level;
 * the two per-component tables (16 x int) are libNFC_Comet's __DATA.__common 0x2125364ac and __DATA_DIRTY.__common
 * 0x214bea2a0 (unslid dyld-cache addresses), filled once by phLogNfc_ConfigureMwLogging with {2,2,2,1,...}, so the
 * level-4 lines (mode-set RSP/timer/6F 18 handler/notify) never print. kern.sweet_nfc_mwlog = N (default 0 = off):
 * on the first CORE_RESET of a session, on nfcd's own write thread (gw_ctl_send, current_task() = nfcd, checked by
 * name), copyout 16 x N (clamped 1..4) to both tables + nfcd's shared-region slide. Slide = the slid start of the
 * cache (vm_shared_region_start_address) - the unslid address of the cache's first mapping (dyld_cache_header
 * mapping[0], read with copyin). Both are writable data pages (copy-on-write: only nfcd's own copy changes). A failed
 * copyout is logged and never retried this boot (no vm_protect). Nothing goes to the chip.
 */
static uint32_t nfc_mwlog;              /* kern.sweet_nfc_mwlog */
static bool mwlog_done, mwlog_pending;  /* per session; gw_lock */
static bool mwlog_failed;               /* sticky for the boot: no retry after a failure */

static void
mwlog_run(void)
{
	static const uint64_t tgt[2] = { 0x2125364acULL, 0x214bea2a0ULL };
	uint32_t lv = nfc_mwlog > 4 ? 4 : nfc_mwlog < 1 ? 1 : nfc_mwlog, v[16];
	char who[32] = "?";
	task_t task = current_task();
	vm_shared_region_t sr;
	mach_vm_offset_t start = 0;
	struct { char magic[16]; uint32_t moff, mcnt; } hdr;
	struct { uint64_t addr, size, foff; uint32_t maxp, initp; } m;
	uint64_t slide;
	kern_return_t kr;
	int fails = 0;

	proc_selfname(who, sizeof(who));
	if (strncmp(who, "nfcd", sizeof(who)) != 0) {
		SAY("nfc-hci: 1141: mwlog: CORE_RESET written by %s (pid %d), not nfcd: skipped\n", who, proc_selfpid());
		return;
	}
	sr = vm_shared_region_get(task);
	if (sr == NULL) {
		SAY("nfc-hci: 1141: mwlog: nfcd (pid %d) has no shared region: not done, no retry\n", proc_selfpid());
		mwlog_failed = true;
		return;
	}
	kr = vm_shared_region_start_address(sr, &start, task);
	vm_shared_region_deallocate(sr);                /* drop the reference vm_shared_region_get took */
	if (kr != KERN_SUCCESS) {
		SAY("nfc-hci: 1141: mwlog: vm_shared_region_start_address -> %d: not done, no retry\n", kr);
		mwlog_failed = true;
		return;
	}
	if (copyin((user_addr_t)start, &hdr, sizeof(hdr)) != 0 || strncmp(hdr.magic, "dyld_v1", 7) != 0 ||
	    hdr.mcnt == 0 || copyin((user_addr_t)(start + hdr.moff), &m, sizeof(m)) != 0) {
		SAY("nfc-hci: 1141: mwlog: dyld cache header at %#llx not readable/valid: not done, no retry\n",
		    (unsigned long long)start);
		mwlog_failed = true;
		return;
	}
	slide = start - m.addr;
	SAY("nfc-hci: 1141: mwlog: nfcd pid %d, cache at %#llx (unslid %#llx, %u mappings), slide %#llx, level %u\n",
	    proc_selfpid(), (unsigned long long)start, (unsigned long long)m.addr, hdr.mcnt, (unsigned long long)slide, lv);
	for (unsigned i = 0; i < 16; i++) {
		v[i] = lv;
	}
	for (unsigned t = 0; t < 2; t++) {
		const char *where = "not in the main cache file's mappings (subcache)";
		int e;

		for (unsigned i = 0; i < hdr.mcnt && i < 16; i++) {
			if (copyin((user_addr_t)(start + hdr.moff + 32u * i), &m, sizeof(m)) != 0) {
				break;
			}
			if (tgt[t] >= m.addr && tgt[t] + sizeof(v) <= m.addr + m.size) {
				where = (m.initp & 2) ? "in a writable mapping" : "in a READ-ONLY mapping";
				break;
			}
		}
		e = copyout(v, (user_addr_t)(tgt[t] + slide), sizeof(v));
		SAY("nfc-hci: 1141: mwlog: table %#llx -> %#llx (%s): copyout 16 x %u -> %d%s\n", (unsigned long long)tgt[t],
		    (unsigned long long)(tgt[t] + slide), where, lv, e, e ? " FAILED (no retry, no vm_protect)" : " ok");
		if (e != 0) {
			fails++;
		}
	}
	if (fails) {
		mwlog_failed = true;
	}
}

static void
hci_log(const char *dir, const uint8_t *b, unsigned n, const char *note)
{
	char h[3 * 40 + 4];

	if (hci_said >= 1000) {
		return;
	}
	hci_said++;
	hex(h, sizeof(h), b, n > 40 ? 40 : n);
	SAY("nfc-hci: persona: 1136: HCI %s %u [%s%s] %s%s\n", dir, n, h, n > 40 ? " .." : "", note,
	    gw.crc_out ? " +CRC16" : "");
}

/* 1136: one host data packet on static conn 1 (p[0] == 0x01): local HCP answer + credit NTF. gw_lock held. */
static bool
persona_hci_data(const uint8_t *p, unsigned n, uint8_t *out, unsigned *no, unsigned cap)
{
	uint8_t r[3 + 2 + 8];
	unsigned rl = 0;
	const char *what;

	uint8_t cid = (uint8_t)(p[0] & 0x0f);
	const uint8_t cr[6] = { 0x60, 0x06, 0x03, 0x01, cid, 0x01 };    /* 1137: credit for the conn it came on */

	hci_log("host>", p, n, cid == 1 ? "data, static conn 1" : "data, HCI conn (CORE_CONN_CREATE, 1137)");
	if (n < 5 || n != 3u + p[2] || p[1] != 0) {
		what = "short/malformed HCP: no reply";
	} else if (!(p[3] & 0x80)) {
		what = "fragmented HCP (CB = 0): no reply";
	} else {
		uint8_t pipe = (uint8_t)(p[3] & 0x7f), type = (uint8_t)(p[4] >> 6), ins = (uint8_t)(p[4] & 0x3f);

		r[0] = cid;
		r[1] = 0x00;
		r[3] = (uint8_t)(0x80 | pipe);
		if (type != 0) {
			what = type == 1 ? "HCP event: no reply" : "HCP response: no reply";
		} else if (pipe != 0x01) {
			r[4] = 0x81;    /* response, ANY_E_NOK */
			rl = 5;
			what = "command on a non-admin pipe: ANY_E_NOK";
		} else {
			r[4] = 0x80;    /* response, ANY_OK */
			rl = 5;
			switch (ins) {
			case 0x01:
				what = (n > 5 && p[5] == 0x03) ? "admin ANY_SET_PARAMETER WHITELIST: ANY_OK" :
				    "admin ANY_SET_PARAMETER (other register): ANY_OK";
				break;
			case 0x02:
				if (n > 5 && p[5] == 0x01) {
					memcpy(r + 5, hci_session_id, sizeof(hci_session_id));
					rl = 5 + sizeof(hci_session_id);
					what = "admin ANY_GET_PARAMETER SESSION_IDENTITY: ANY_OK + 8 fixed bytes";
				} else {
					what = "admin ANY_GET_PARAMETER (other register): ANY_OK, no data";
				}
				break;
			case 0x03:
				what = "admin ANY_OPEN_PIPE: ANY_OK";
				break;
			case 0x04:
				what = "admin ANY_CLOSE_PIPE: ANY_OK";
				break;
			case 0x10:
				what = "admin 0x10 (research doc: CLEAR_ALL_PIPE; ETSI 102 622: ADM_CREATE_PIPE): ANY_OK";
				break;
			case 0x14:
				what = "admin ADM_CLEAR_ALL_PIPE (0x14): ANY_OK";
				break;
			default:
				what = "admin other command: ANY_OK";
				break;
			}
		}
	}
	if (rl) {
		r[2] = (uint8_t)(rl - 3);
		out_put(out, no, cap, r, rl);
		hci_log("<persona", r, rl, what);
	} else {
		hci_log("(no reply)", p, n, what);
	}
	out_put(out, no, cap, cr, sizeof(cr));
	hci_log("<persona", cr, sizeof(cr), cid == 1 ? "CORE_CONN_CREDITS_NTF conn 1 +1" : "CORE_CONN_CREDITS_NTF HCI conn +1");
	return true;
}

static unsigned
cfg_default(uint16_t id, uint8_t *v)
{
	switch (id) {
	case 0xa00f:                    /* EEPROM user area: MW 0x0134, FW = identity minor/major, bConfig 0xC3C3A5A5 */
		memset(v, 0, 64);
		v[0] = 0x34;
		v[1] = 0x01;
		v[2] = (uint8_t)nfc_id_info;
		v[3] = (uint8_t)(nfc_id_info >> 8);
		v[4] = 0xa5;
		v[5] = 0xa5;
		v[6] = 0xc3;
		v[7] = 0xc3;
		return 64;
	case 0xa0e8:
	case 0xa0ef:
	case 0xa0f0:
		memset(v, 0, 0x45);
		return 0x45;
	case 0xa0ea:
	case 0xa0eb:
	case 0xa01e:
		memset(v, 0, 8);
		return 8;
	default:
		v[0] = 0;
		return 1;
	}
}

/* value for a GET_CONFIG id: override, else the default. Returns the length. */
static unsigned
cfg_get(uint16_t id, uint8_t *v, bool *ovr)
{
	for (unsigned i = 0; i < nfc_cfg_n; i++) {
		if (nfc_cfg[i].id == id) {
			memcpy(v, nfc_cfg[i].val, nfc_cfg[i].len);
			*ovr = true;
			return nfc_cfg[i].len;
		}
	}
	*ovr = false;
	if (vese_on() && swp_idx(id) >= 0 && (swp_have >> swp_idx(id) & 1)) {        /* 1134 */
		v[0] = swp_set[swp_idx(id)];
		return 1;
	}
	if (id == 0xa0eb && a0eb_len != 0 && (((nfc_fwif_fmt & 0x2000) && vhci_on()) || ese_pc_on())) {   /* 1138/1139 */
		memcpy(v, a0eb_set, a0eb_len);
		return a0eb_len;
	}
	return cfg_default(id, v);
}

/* set (len > 0) or drop (len == 0) an override; false if the table is full */
static bool
cfg_put(uint16_t id, const uint8_t *v, unsigned len)
{
	unsigned i;

	for (i = 0; i < nfc_cfg_n && nfc_cfg[i].id != id; i++) {
	}
	if (len == 0) {
		if (i < nfc_cfg_n) {
			nfc_cfg[i] = nfc_cfg[--nfc_cfg_n];
		}
		return true;
	}
	if (i == nfc_cfg_n) {
		if (nfc_cfg_n == CFG_N) {
			return false;
		}
		nfc_cfg_n++;
	}
	nfc_cfg[i].id = id;
	nfc_cfg[i].len = (uint8_t)(len > CFG_MAX ? CFG_MAX : len);
	memcpy(nfc_cfg[i].val, v, nfc_cfg[i].len);
	return true;
}

static bool
cfg_wide(uint8_t b)
{
	return (b & 0xfeu) == 0xa0u;    /* NXP 2-byte tag, as phNciNfc_TlvUtilsAddTlv / StorePropParams test it */
}

/* does the GET/SET_CONFIG payload (p+4.., count p[3]) name only ids from list? any of them? set: TLVs */
static void
cfg_ids_scan(const uint8_t *p, unsigned n, bool set, const uint16_t *list, unsigned ln, bool *any, bool *only,
    unsigned *cnt)
{
	unsigned i = 4, k;

	*any = false;
	*only = true;
	*cnt = 0;
	for (k = 0; n > 3 && k < p[3] && i < n; k++) {
		uint16_t id = p[i];
		bool hit = false;

		if (cfg_wide(p[i]) && i + 1 < n) {
			id = (uint16_t)(p[i] << 8 | p[i + 1]);
			i++;
		}
		i++;
		if (set) {
			if (i >= n) {
				break;
			}
			i += 1u + p[i];
		}
		for (unsigned j = 0; j < ln; j++) {
			hit |= list[j] == id;
		}
		*any |= hit;
		*only &= hit;
		(*cnt)++;
	}
	if (*cnt == 0) {
		*only = false;
	}
}

static const uint16_t cfg_eeprom_ids[] = { 0xa00f };
static const uint16_t cfg_swp_ids[] = { 0xa0ec, 0xa0ed, 0xa0d4 };

/* 1128: init-table step (gphLibNfc_InitializeSequence 0x2125326f8) a persona command belongs to, or -1 */
static int
persona_step_of(const uint8_t *p, unsigned n, const char **name)
{
	uint16_t op = (uint16_t)((p[0] & 0x0fu) << 8 | (p[1] & 0x3fu));
	int st = persona_st;
	bool any, only;
	unsigned cnt;

	*name = NULL;
	switch (op) {
	case 0x000:
	case 0x001:
		*name = op ? "CORE_INIT" : "CORE_RESET";
		return op == 0x001 && st >= 0 ? st : st < 0 || st >= 15 ? 0 : st < 9 ? 4 : 14;
	case 0xf03:
		*name = st == 1 ? "ReApplyHsuBr (2F 03)" : "ConfigHsuBr (2F 03)";
		return st == 1 ? 2 : 1;
	case 0xf19:
		*name = "MwFwCompatibilityChk (2F 19)";
		return 5;
	case 0xf02:
		*name = "EnablePropExtn (2F 02)";
		return st >= 9 ? 15 : 6;
	case 0xf06:
		*name = "GetFwType (2F 06)";
		return 7;
	case 0xf00:
		*name = st >= 9 ? "SetPowerMode (2F 00, after init)" : "DisableStandbyMode (2F 00)";
		return st >= 9 ? -1 : 8;
	case 0x003:
		cfg_ids_scan(p, n, false, cfg_eeprom_ids, 1, &any, &only, &cnt);
		if (any) {
			*name = "GetEepromParams (GET_CONFIG A0 0F)";
			return 9;
		}
		cfg_ids_scan(p, n, false, cfg_swp_ids, 3, &any, &only, &cnt);
		if (only) {
			*name = "GetNfceeIntfCfg (GET_CONFIG A0 EC/ED/D4)";
			return 17;
		}
		*name = "GET_CONFIG (not an init-table step)";
		return -1;
	case 0x002:
		cfg_ids_scan(p, n, true, cfg_eeprom_ids, 1, &any, &only, &cnt);
		if (only) {
			*name = "SetEepromParams (SET_CONFIG A0 0F)";
			return 11;
		}
		cfg_ids_scan(p, n, true, cfg_swp_ids, 3, &any, &only, &cnt);
		if (only) {
			*name = "SwpIntfControl (SET_CONFIG A0 EC/ED/D4)";
			return 16;
		}
		if (st >= 9 && st < 10) {
			*name = "InitSetConfig (SET_CONFIG)";
			return 10;
		}
		*name = "SET_CONFIG (not an init-table step)";
		return -1;
	case 0x101:
		*name = "SetLsntModeRtng (RF_SET_LISTEN_MODE_ROUTING)";
		return st >= 10 && st < 12 ? 12 : -1;
	case 0x100:
		*name = "InitSetMapping (RF_DISCOVER_MAP)";
		return st >= 10 && st < 13 ? 13 : -1;
	case 0x200:
		*name = "EnableNfceeDisc (NFCEE_DISCOVER)";
		return 18;
	default:
		return -1;
	}
}

/* 1128: one klog line naming the init step, with what the stack does next where it is known. gw_lock held. */
/* 1138: plain NCI name of a host command (klog labels after the ReInit's NFCEE_DISCOVER) */
static const char *
nci_op_name(uint16_t op)
{
	switch (op) {
	case 0x000: return "CORE_RESET";
	case 0x001: return "CORE_INIT";
	case 0x002: return "CORE_SET_CONFIG";
	case 0x003: return "CORE_GET_CONFIG";
	case 0x004: return "CORE_CONN_CREATE";
	case 0x005: return "CORE_CONN_CLOSE";
	case 0x100: return "RF_DISCOVER_MAP";
	case 0x101: return "RF_SET_LISTEN_MODE_ROUTING";
	case 0x102: return "RF_GET_LISTEN_MODE_ROUTING";
	case 0x103: return "RF_DISCOVER";
	case 0x106: return "RF_DEACTIVATE";
	case 0x200: return "NFCEE_DISCOVER";
	case 0x201: return "NFCEE_MODE_SET";
	case 0xf00: return "NXP 2F 00 (standby/power mode)";
	case 0xf02: return "NXP 2F 02 (EnablePropExtn)";
	case 0xf03: return "NXP 2F 03 (HSU baud rate)";
	case 0xf06: return "NXP 2F 06 (GetFwType)";
	case 0xf19: return "NXP 2F 19 (GetFwInterfaceVersion)";
	default: return "NCI command";
	}
}

/* 1134: the ReInit sequence 0x212532618 (after SET_CONFIG A0 BF = 01): one klog line per step. True = handled. */
/* 1138: after the ReInit's NFCEE_DISCOVER enable (step 10) the HCI init / eSE child sequences run, not the ReInit
 * table: those ops are labelled "step 11 post-discover (HCI/eSE seq)" + NCI name + bytes, no table step names. */
static bool
persona_ri_say(const uint8_t *p, unsigned n)
{
	uint16_t op = (uint16_t)((p[0] & 0x0fu) << 8 | (p[1] & 0x3fu));
	const char *name = NULL;
	char extra[200];
	int st = persona_ri;
	bool say;

	extra[0] = 0;
	if (op == 0x000) {
		if (persona_ri >= 0) {
			st = 60;
			name = "DeInitialize (CORE_RESET): the stack gave up on the ReInit";
			snprintf(extra, sizeof(extra), " after step %d%s", persona_ri, persona_ri == 50 ?
			    " (static feature cfg -> VerifySEStatus failed: eSE expected, ctx+0x146 != 1)" :
			    persona_ri == 11 ? " (post-discover HCI/eSE seq, 1138)" : "");
		} else if (gw.chip_crc) {
			st = 0;
			persona_ri_hsu = 0;
			persona_ri_n++;
			name = "ReInitialize (CORE_RESET, chip CRC on)";
		} else {
			return false;
		}
	} else if (persona_ri < 0 || persona_ri >= 60) {
		return false;
	} else if (persona_ri == 10 || persona_ri == 11) {      /* 1138: post-discover, label by opcode */
		char h[3 * 16 + 4];

		hex(h, sizeof(h), p, n > 16 ? 16 : n);
		st = 11;
		name = "post-discover (HCI/eSE seq):";
		snprintf(extra, sizeof(extra), " %s [%s%s]", nci_op_name(op), h, n > 16 ? " .." : "");
	} else if (op == 0x001) {
		st = 2;
		name = "ReInitialize (CORE_INIT; step 1 CrcConfig is host-only)";
	} else if (op == 0xf02) {
		st = 3;
		name = "EnablePropExtn (2F 02)";
	} else if (op == 0xf03) {
		st = persona_ri_hsu++ ? 5 : 4;
		name = st == 4 ? "ReConfigHsuBr (2F 03)" : "ReApplyHsuBr (2F 03)";
	} else if (op == 0x002) {
		st = st < 6 ? 6 : 7;
		name = "SetChipScopeLogCfg (SET_CONFIG)";
	} else if (op == 0x003) {
		uint8_t a[CFG_MAX], b[CFG_MAX];
		bool o;
		bool want = (swp_have & 2) && swp_set[1] != 0;

		(void)cfg_get(0xa0ec, a, &o);
		(void)cfg_get(0xa0ed, b, &o);
		st = 8;
		name = "GetNfceeIntfCfg (GET_CONFIG A0 EC/ED)";
		snprintf(extra, sizeof(extra), ": A0EC %02x A0ED %02x (host set A0ED %s) -> %s", a[0], b[0],
		    (swp_have & 2) ? (swp_set[1] ? "01" : "00") : "?",
		    b[0] != 0 ? "eSE pending (0x146 = 3): NFCEE discovery next" :
		    want ? "eSE off (0x146 = 2) but expected: ReInit will fail at VerifySEStatus (fwif_fmt 0x100/0x200 = virtual eSE)" :
		    "eSE off, not expected: static feature cfg next");
	} else if (op == 0x200) {
		bool en = n > 3 && p[3] != 0;

		st = en ? 10 : 9;
		name = en ? "EnableNfceeDisc (NFCEE_DISCOVER enable)" : "ReInitDisableNfceeDisc (NFCEE_DISCOVER disable)";
		if (en) {
			snprintf(extra, sizeof(extra), ": %s", vese_on() && (swp_have & 2) && swp_set[1] ?
			    "virtual eSE C0 reported (connected)" : "0 NFCEEs");
		}
	} else if (op == 0x201) {
		name = "NFCEE_MODE_SET";
	} else if (op == 0xf00) {
		st = 50;
		name = "ReInitializeComplete -> static feature cfg: SetStandbyMode (2F 00), then VerifySEStatus";
	} else {
		name = "not a ReInit-table command";
	}
	say = ri_said < 200;
	if (say) {
		ri_said++;
		SAY("persona: 1134: ReInit #%u step %d %s%s\n", persona_ri_n, st, name, extra);
	}
	persona_ri = st;
	return true;
}

static void
persona_step_say(const uint8_t *p, unsigned n)
{
	const char *name;
	int st;
	char extra[96];

	if (persona_ri_say(p, n)) {     /* 1134 */
		return;
	}
	st = persona_step_of(p, n, &name);
	extra[0] = 0;
	if (st < 0) {
		if (name && persona_said < 400) {
			SAY("persona: %s (after step %d)\n", name, persona_st);
		}
		return;
	}
	if (st == 9) {
		uint8_t v[CFG_MAX];
		bool ovr;
		unsigned l = cfg_get(0xa00f, v, &ovr);
		uint32_t bc = l >= 8 ? (uint32_t)v[4] | (uint32_t)v[5] << 8 | (uint32_t)v[6] << 16 | (uint32_t)v[7] << 24 : 0;

		snprintf(extra, sizeof(extra), ": A0 0F len %u%s bConfig %#x -> %s", l, ovr ? " (override)" : "", bc,
		    l != 64 ? "LENGTH NOT 64: EEPROM read fails" : bc == 0xc3c3a5a5u ? "skip set config, next step 15" :
		    "full MW config, next step 10");
	} else if (st == 17) {
		uint8_t a[CFG_MAX], b[CFG_MAX], c[CFG_MAX];
		bool o;

		(void)cfg_get(0xa0ec, a, &o);
		(void)cfg_get(0xa0ed, b, &o);
		(void)cfg_get(0xa0d4, c, &o);
		snprintf(extra, sizeof(extra), ": A0EC %02x A0ED %02x A0D4 %02x -> %s", a[0], b[0], c[0],
		    (a[0] | b[0] | c[0]) == 0 ? "SWP off: no NFCEE_DISCOVER, next step 19 InitializeComplete" :
		    "NFCEE_DISCOVER (step 18) next");
	} else if (st == 18) {
		snprintf(extra, sizeof(extra), "%s", vese_on() && (swp_have & 2) && swp_set[1] && n > 3 && p[3] ?
		    ": 1134 virtual eSE C0 reported, the SE handler completes init" : ": 0 NFCEEs, next step 19 InitializeComplete");
	}
	persona_st = st;
	if (persona_said < 400) {
		SAY("persona: init step %d %s%s\n", st, name, extra);
	}
}

/* 1128: SET_CONFIG in persona mode: list the ids; bit 0x10 of kern.sweet_nfc_fwif_fmt keeps A0xx values as overrides */
static void
persona_set_config(const uint8_t *p, unsigned n)
{
	char ids[96];
	size_t o = 0;
	unsigned i = 4, k, kept = 0;

	ids[0] = 0;
	for (k = 0; n > 3 && k < p[3] && i + 1 < n; k++) {
		uint16_t id = p[i];
		unsigned l;

		if (cfg_wide(p[i])) {
			id = (uint16_t)(p[i] << 8 | p[i + 1]);
			i++;
		}
		i++;
		if (i >= n) {
			break;
		}
		l = p[i++];
		if (i + l > n) {
			break;
		}
		if (o + 12 < sizeof(ids)) {
			o += (size_t)snprintf(ids + o, sizeof(ids) - o, "%s%04x/%u", k ? " " : "", id, l);
		}
		if (id >= 0xa000 && (nfc_fwif_fmt & 0x10) && l > 0 && cfg_put(id, p + i, l)) {
			kept++;
		}
		if (id == 0xa0eb && l >= 1 && l <= sizeof(a0eb_set)) {        /* 1138: kept for fwif_fmt 0x2000 */
			memcpy(a0eb_set, p + i, l);
			a0eb_len = l;
		}
		if (swp_idx(id) >= 0 && l >= 1) {       /* 1134: what SwpIntfControl asked for (the host's SE config) */
			swp_set[swp_idx(id)] = p[i];
			swp_have |= (uint8_t)(1u << swp_idx(id));
		}
		if (id == 0xa0bf && l >= 1 && !(nfc_fwif_fmt & 0x40)) {
			/* 1132: CRC check config. The RSP to this SET_CONFIG goes out without CRC (the host sets its check
			 * flag only at "All Rf configs are set", 0x1f5154d5c); from the next reply on (the CORE_RESET_RSP of
			 * the ReInit sequence 0x212532618) every reply gets the CRC16 trailer. */
			bool on = p[i] != 0;

			if (on != gw.chip_crc || persona_said < 400) {
				SAY("persona: 1132: SET_CONFIG A0 BF = %02x: chip CRC16 %s from the next reply (was %s)\n", p[i],
				    on ? "ON" : "off", gw.chip_crc ? "on" : "off");
			}
			gw_crc_next = on ? 1 : 2;
		}
		if (id == 0xa00f && l >= 8 && persona_said < 400) {
			SAY("persona: SET_CONFIG A0 0F (EEPROM user area) MW %02x%02x FW %02x%02x bConfig %02x%02x%02x%02x%s\n",
			    p[i + 1], p[i], p[i + 3], p[i + 2], p[i + 7], p[i + 6], p[i + 5], p[i + 4],
			    (nfc_fwif_fmt & 0x10) ? " (kept)" : " (not kept, fwif_fmt bit 0x10 off)");
		}
		i += l;
	}
	if (persona_said < 400) {
		SAY("persona: SET_CONFIG %u ids [%s] acknowledged locally, not forwarded%s\n", k, ids,
		    kept ? " (A0xx kept as overrides)" : "");
	}
}

static void
persona_note(uint16_t op)
{
	unsigned i;

	for (i = 0; i < 32 && persona_seen[i].n != 0; i++) {
		if (persona_seen[i].op == op) {
			persona_seen[i].n++;
			return;
		}
	}
	if (i < 32) {
		persona_seen[i].op = op;
		persona_seen[i].n = 1;
	}
}

static bool
persona_packet(const uint8_t *p, unsigned n, uint8_t *out, unsigned *no, unsigned cap)
{
	uint8_t gid = (uint8_t)(p[0] & 0x0fu), oid = (uint8_t)(p[1] & 0x3fu);
	uint8_t r[3 + 255];
	unsigned len = 3;
	const char *what = NULL;

	if (n >= 3 && (p[0] == 0x01 || (hci_conn != 0 && p[0] == hci_conn)) && vhci_on()) {
		return persona_hci_data(p, n, out, no, cap);    /* 1136: HCP on static conn 1, local only */
	}
	if ((p[0] >> 5) != 1 || (p[0] & 0x10) != 0 || n < 3) {
		return false;                   /* commands only, unsegmented */
	}
	r[0] = (uint8_t)(0x40u | gid);
	r[1] = oid;
	if (gid == 0x0f) {
		r[len++] = 0x00;
		if (nfc_fwif_fmt & 4) {         /* 1127: the 1125 answers, for A/B */
			if (oid == 0x03) {
				r[len++] = (uint8_t)(nfc_fwif >> 8);
				r[len++] = (uint8_t)nfc_fwif;
				what = "(fmt 4, 1125) 2F 03 as interface version";
			} else {
				what = "(fmt 4, 1125) NXP proprietary: OK, no data";
			}
		} else {
			switch (oid) {
			case 0x00:
				what = "NXP SetStandby/power mode (2F 00): OK";
				break;
			case 0x02:
				id_bytes(r + len);
				len += 4;
				what = "NXP EnablePropExtn (2F 02): OK + 4 info bytes (identity)";
				break;
			case 0x03:
				what = "NXP ConfigHsuBr (2F 03): OK, 1-byte status (baud rate taken, nothing changes)";
				break;
			case 0x06:
				r[len++] = (nfc_fwif_fmt & 8) ? 0x01 : 0x00;
				what = "NXP GetFwType (2F 06): OK + type";
				break;
			case 0x19:
				r[len++] = (uint8_t)(nfc_fwif >> 8);
				r[len++] = (uint8_t)nfc_fwif;
				what = "NXP GetFwInterfaceVersion (2F 19): OK + version (big endian)";
				break;
			default:
				what = "NXP proprietary: OK, no data";
				break;
			}
		}
	} else {
		switch ((gid << 8) | oid) {
		case 0x0001: {                  /* CORE_INIT_RSP, NCI 1.0 */
			static const uint8_t body[] = {
				0x00,                                   /* status */
				0x03, 0x1e, 0x03, 0x00,                 /* features: disc. freq/cfg, routing, power states */
				0x05, 0x00, 0x01, 0x02, 0x03, 0x80,     /* RF interfaces: EE direct, frame, ISO-DEP, NFC-DEP, NXP MIFARE */
				0x01,                                   /* max logical connections */
				0x00, 0x02,                             /* max routing table size (LE) */
				0xff,                                   /* max control packet payload */
				0x00, 0x01,                             /* max size for large parameters (LE) */
				0x04,                                   /* manufacturer: NXP */
			};
			memcpy(r + len, body, sizeof(body));
			len += sizeof(body);
			id_bytes(r + len);
			len += 4;
			what = "CORE_INIT_RSP NCI 1.0, NXP PN80V identity";
			break;
		}
		case 0x0002:                    /* CORE_SET_CONFIG: OK, 0 invalid ids (nothing is written anywhere) */
			persona_set_config(p, n);       /* 1128 */
			r[len++] = 0x00;
			r[len++] = 0x00;
			what = "CORE_SET_CONFIG: OK (not applied)";
			break;
		case 0x0003: {                  /* CORE_GET_CONFIG: 1128 each id with its value (table/override) */
			unsigned cnt = n > 3 ? p[3] : 0, i = 4, k = 0;
			bool legacy = (nfc_fwif_fmt & 0x20) != 0;

			r[len++] = 0x00;
			r[len++] = 0x00;        /* count, filled below */
			for (; k < cnt && i < n; k++) {
				uint8_t v[CFG_MAX];
				uint16_t id = p[i];
				bool ovr = false;
				unsigned vl, il = 1;

				if ((legacy ? p[i] >= 0xa0 : cfg_wide(p[i])) && i + 1 < n) {    /* NXP extended (2-byte) tag */
					id = (uint16_t)(p[i] << 8 | p[i + 1]);
					il = 2;
				}
				if (legacy) {
					v[0] = 0;
					vl = 1;
				} else {
					vl = cfg_get(id, v, &ovr);
				}
				if (len + il + 1 + vl > sizeof(r) || len + il + 1 + vl > 3 + 255) {
					break;  /* does not fit: the count stays short and the stack sees a failed GET */
				}
				r[len++] = p[i++];
				if (il == 2) {
					r[len++] = p[i++];
				}
				r[len++] = (uint8_t)vl;
				memcpy(r + len, v, vl);
				len += vl;
			}
			r[4] = (uint8_t)k;
			what = legacy ? "CORE_GET_CONFIG: each id = 1 zero byte (fmt 0x20, 1127)" :
			    "CORE_GET_CONFIG: each id = table value (kern.sweet_nfc_cfg_set overrides)";
			break;
		}
		case 0x0100:
			r[len++] = 0x00;
			what = "RF_DISCOVER_MAP: OK";
			break;
		case 0x0101:
			r[len++] = 0x00;
			what = "RF_SET_LISTEN_MODE_ROUTING: OK (local only, nothing routed)";
			break;
		case 0x0103:
			r[len++] = 0x00;
			what = "RF_DISCOVER: OK (field stays off, nothing will be found)";
			break;
		case 0x0106: {
			const uint8_t nt[5] = { 0x61, 0x06, 0x02, (uint8_t)(n > 3 ? p[3] : 0), 0x00 };

			r[len++] = 0x00;
			r[2] = (uint8_t)(len - 3);
			out_put(out, no, cap, r, len);
			out_put(out, no, cap, nt, sizeof(nt));
			persona_note(0x0106);
			if (persona_said++ < 400) {
				SAY("persona: RF_DEACTIVATE: OK + NTF (type %u)\n", nt[3]);
			}
			return true;
		}
		case 0x0200:
			r[len++] = 0x00;
			if (vese_on() && (swp_have & 2) && swp_set[1] != 0) {  /* 1134: virtual eSE on SWP2 */
				bool en = n > 3 && p[3] != 0;
				bool hci = en && (nfc_fwif_fmt & 0x400);       /* 1136: + virtual HCI network */
				const uint8_t nt[8] = { 0x62, 0x00, 0x05, 0xc0, 0x00, 0x01, 0x00, 0x00 };
				static const uint8_t hn[8] = { 0x62, 0x00, 0x05, 0x01, 0x00, 0x01, 0x01, 0x00 };

				r[len++] = en ? (hci ? 0x02 : 0x01) : 0x00;
				r[2] = (uint8_t)(len - 3);
				out_put(out, no, cap, r, len);
				if (hci) {
					out_put(out, no, cap, hn, sizeof(hn));
					out_put(out, no, cap, hci_credit_ntf, sizeof(hci_credit_ntf));
				}
				if (en) {
					out_put(out, no, cap, nt, sizeof(nt));
				}
				if (hci && (nfc_fwif_fmt & 0x1000)) {  /* 1138: eSE EoOP, so ModeSetHciNwk skips its 4 s wait */
					out_put(out, no, cap, hci_eoop_ntf, sizeof(hci_eoop_ntf));
				}
				persona_note(0x0200);
				persona_step_say(p, n);
				if (!en && ese_pc_on()) {
					hci_log("host>", p, n, "NFCEE_DISCOVER disable (eSE ReInit seq)");
					hci_log("<persona", r, len, "NFCEE_DISCOVER_RSP OK, 0 NFCEEs (1139)");
				}
				if (hci) {
					hci_log("host>", p, n, "NFCEE_DISCOVER enable");
					hci_log("<persona", r, len, "NFCEE_DISCOVER_RSP OK, 2 NFCEEs");
					hci_log("<persona", hn, sizeof(hn), "NFCEE_DISCOVER_NTF HCI network 01, enabled, HCI access");
					hci_log("<persona", hci_credit_ntf, sizeof(hci_credit_ntf), "CORE_CONN_CREDITS_NTF conn 1 +1");
					hci_log("<persona", nt, sizeof(nt), "NFCEE_DISCOVER_NTF eSE C0 (1134)");
					if (nfc_fwif_fmt & 0x1000) {
						hci_log("<persona", hci_eoop_ntf, sizeof(hci_eoop_ntf),
						    "NXP SYS_EoOP_NTF (6F 20) C0 status 00 ready (1138)");
					}
				} else if (ri_said < 200) {
					ri_said++;
					SAY("persona: 1134: NFCEE_DISCOVER %s: OK, %s%s\n", en ? "enable" : "disable",
					    en ? "1 NFCEE + NTF [62 00 05 c0 00 01 00 00] (virtual eSE C0, enabled, APDU)" : "0 NFCEEs",
					    gw.crc_out ? " +CRC16" : "");
				}
				return true;
			}
			r[len++] = 0x00;
			what = "NFCEE_DISCOVER: OK, 0 NFCEEs (end of the init sequence)";
			break;
		case 0x0201:
			if (!vese_on()) {
				return false;
			}
			r[len++] = 0x00;        /* 1134: NFCEE_MODE_SET for the virtual eSE: OK (NCI 1.0, no NTF) */
			what = "1134: NFCEE_MODE_SET (virtual eSE): OK";
			if (vhci_on() && n > 3 && p[3] == 0x01) {       /* 1136 */
				what = "1136: NFCEE_MODE_SET (HCI network 01): OK";
				r[2] = (uint8_t)(len - 3);
				hci_log("host>", p, n, "NFCEE_MODE_SET HCI network 01");
				hci_log("<persona", r, len, "NFCEE_MODE_SET_RSP OK (NCI 1.0, no NTF)");
			}
			if (ese_pc_on() && n > 4 && p[3] == 0xc0 && (p[4] == 0x00 || p[4] == 0x01) &&
			    ((nfc_fwif_fmt & 0x10000) || (p[4] == 0x00 && (nfc_fwif_fmt & 0x20000)))) {
				/* 1140: RSP alone; the NTF later as its own packet (0x10000), or none after a disable (0x20000) */
				static const uint8_t d1[4] = { 0x6f, 0x18, 0x01, 0x00 };
				static const uint8_t d2[5] = { 0x6f, 0x18, 0x02, 0xc0, 0x00 };
				bool alt = (nfc_fwif_fmt & 0x8000) != 0;

				r[2] = (uint8_t)(len - 3);
				out_put(out, no, cap, r, len);
				persona_note(0x0201);
				persona_step_say(p, n);
				hci_log("host>", p, n, p[4] ? "NFCEE_MODE_SET C0 enable" : "NFCEE_MODE_SET C0 disable");
				hci_log("<persona", r, len, "NFCEE_MODE_SET_RSP OK, alone (1140)");
				if (p[4] == 0x00 && (nfc_fwif_fmt & 0x20000)) {
					SAY("nfc-hci: persona: 1140: HCI diagnostic 0x20000: no NTF after the disable RSP t=%llu us\n",
					    (unsigned long long)ntf_now_us());
				} else if (p[4] == 0x00) {
					ntf_queue(alt ? d2 : d1, alt ? sizeof(d2) : sizeof(d1));
				} else {
					ntf_queue(hci_eoop_ntf, sizeof(hci_eoop_ntf));
				}
				return true;
			}
			if (ese_pc_on() && n > 4 && (p[3] == 0xc0 || p[3] == 0x01) && p[4] == 0x00) {
				/* 1139/1144/1145: disable of any persona NFCEE (eSE C0 or HCI network 01): RSP, then 6F 18 00 (IceFall
				 * Power OFF, zero-length payload) completes the pending mode set; no 6F 20. 1145 generalizes the
				 * id==C0 guard to mode byte==0x00 so the eSE teardown's NFCEE 01 disable (22 01 02 01 00) gets the
				 * same 6F 18 00 the length validator needs (NFCEE 01's pending flag is set by phNciNfc_NfceeModeSetSequence) */
				static const uint8_t off1[3] = { 0x6f, 0x18, 0x00 };   /* 1144: zero-length payload; phNciNfc_CoreUtilsValRspLen needs the length byte 0 */
				static const uint8_t off2[5] = { 0x6f, 0x18, 0x02, 0xc0, 0x00 };
				bool alt = (nfc_fwif_fmt & 0x8000) != 0;

				r[2] = (uint8_t)(len - 3);
				out_put(out, no, cap, r, len);
				out_put(out, no, cap, alt ? off2 : off1, alt ? sizeof(off2) : sizeof(off1));
				persona_note(0x0201);
				persona_step_say(p, n);
				hci_log("host>", p, n, p[3] == 0x01 ? "NFCEE_MODE_SET 01 (HCI network) disable" : "NFCEE_MODE_SET C0 disable");
				hci_log("<persona", r, len, "NFCEE_MODE_SET_RSP OK");
				hci_log("<persona", alt ? off2 : off1, alt ? sizeof(off2) : sizeof(off1),
				    alt ? "NXP 6F 18 (IceFall Power OFF) C0 status 00, fwif_fmt 0x8000 form (1139)" :
				    "NXP 6F 18 00 (IceFall Power OFF, zero-length) disable complete (1144)");
				return true;
			}
			if (vhci_on() && (nfc_fwif_fmt & 0x1000) && n > 4 && p[3] == 0xc0 && (p[4] == 0x00 || p[4] == 0x01)) {
				/* 1138: RSP, then the eSE EoOP at once: completes the pending mode set (2 s timer) */
				r[2] = (uint8_t)(len - 3);
				out_put(out, no, cap, r, len);
				out_put(out, no, cap, hci_eoop_ntf, sizeof(hci_eoop_ntf));
				persona_note(0x0201);
				persona_step_say(p, n);
				hci_log("host>", p, n, p[4] ? "NFCEE_MODE_SET C0 enable" : "NFCEE_MODE_SET C0 disable");
				hci_log("<persona", r, len, "NFCEE_MODE_SET_RSP OK");
				hci_log("<persona", hci_eoop_ntf, sizeof(hci_eoop_ntf),
				    "NXP SYS_EoOP_NTF (6F 20) C0 status 00 ready: mode set complete (1138)");
				return true;
			}
			break;
		case 0x0004: {                  /* 1136: CORE_CONN_CREATE logged; 1137: answered only for the virtual NFCEE 01 */
			/* exactly: destination 03 (NFCEE), 1 parameter, TLV 01 len 2 = NFCEE 01, protocol 01 (HCI access) */
			bool hci_ee = n == 9 && p[2] == 6 && p[3] == 0x03 && p[4] == 0x01 && p[5] == 0x01 && p[6] == 0x02 &&
			    p[7] == 0x01 && p[8] == 0x01;

			if (!vhci_on()) {
				return false;
			}
			if (!(nfc_fwif_fmt & 0x800) || !hci_ee) {
				hci_log("host>", p, n, !(nfc_fwif_fmt & 0x800) ?
				    "CORE_CONN_CREATE: not answered (fwif_fmt 0x800 off; refused as before)" :
				    "CORE_CONN_CREATE: not the virtual HCI network (NFCEE 01, HCI access): refused as before");
				return false;
			}
			if (hci_conn != 0) {    /* 1139: a repeat (eSE ReInit seq) is answered as the first, conn re-tracked */
				hci_log("host>", p, n, "CORE_CONN_CREATE again: answered as the first time, conn 2 re-tracked (1139)");
			}
			hci_conn = 0x02;        /* the only dynamic conn the persona ever has */
			r[len++] = 0x00;        /* status OK */
			r[len++] = 0xff;        /* max data packet payload */
			r[len++] = 0x01;        /* initial credits */
			r[len++] = hci_conn;    /* conn ID */
			r[2] = (uint8_t)(len - 3);
			hci_log("host>", p, n, "CORE_CONN_CREATE NFCEE 01 (virtual HCI network), HCI access");
			hci_log("<persona", r, len, "CORE_CONN_CREATE_RSP OK, 255, 1 credit, conn 2 = HCI conn (1137)");
			what = "1137: CORE_CONN_CREATE (virtual HCI network 01): OK, conn 2";
			break;
		}
		case 0x0005:                    /* 1137: CORE_CONN_CLOSE of the HCI conn, locally */
			if (!vhci_on() || hci_conn == 0 || n != 4 || p[3] != hci_conn) {
				return false;
			}
			r[len++] = 0x00;
			r[2] = (uint8_t)(len - 3);
			hci_log("host>", p, n, "CORE_CONN_CLOSE HCI conn");
			hci_log("<persona", r, len, "CORE_CONN_CLOSE_RSP OK (1137)");
			hci_conn = 0;
			what = "1137: CORE_CONN_CLOSE (HCI conn): OK";
			break;
		default:
			return false;
		}
	}
	r[2] = (uint8_t)(len - 3);
	out_put(out, no, cap, r, len);
	persona_note((uint16_t)((gid << 8) | oid));
	persona_step_say(p, n);                 /* 1128 */
	if (persona_said++ < 400) {
		char h[3 * 40 + 4];

		hex(h, sizeof(h), r, len > 40 ? 40 : len);
		SAY("persona: %02x %02x -> %s: [%s%s]%s\n", p[0], p[1], what, h, len > 40 ? " .." : "",
		    gw.crc_out ? " +CRC16" : "");
	}
	return true;
}

/* 1127: CRC-16/CCITT-FALSE as libNFC_Comet's phNciNfc_CalcCrc16 (init 0xFFFF, poly 0x1021, MSB first) */
static uint16_t
nci_crc16(const uint8_t *b, unsigned n)
{
	uint16_t c = 0xffff;

	for (unsigned i = 0; i < n; i++) {
		c ^= (uint16_t)((uint16_t)b[i] << 8);
		for (int k = 0; k < 8; k++) {
			c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
		}
	}
	return c;
}

static void
gw_refuse(int why)
{
	if (why >= 0 && why < RF_N) {
		gw_refused[why]++;
	}
}

static void gw_session_stop(const char *why);
static bool persona_on(void);
static int gw_chip_tx(const uint8_t *b, unsigned n);

/* one host packet; gw_lock held. *want_up: mode 1 asks for a chip bring-up after the lock is dropped */
static void
gw_host_packet(const uint8_t *p, unsigned n, uint8_t *out, unsigned *no, unsigned cap, bool *want_up)
{
	uint8_t fwd[3 + 255];
	unsigned fn = 0;
	int why, v;
	bool cmd = (p[0] >> 5) == 1;
	uint8_t gid = (uint8_t)(p[0] & 0x0fu), oid = (uint8_t)(p[1] & 0x3fu);
	bool say;

	gw_host_pkts++;
	say = gw_said < 300 || (gw_host_pkts % 50) == 0;
	if (nfc_loader) {
		gw_refuse(RF_LOADER);
		if (say) {
			gw_said++;
			gw_hexlog("host>", p, n, "REFUSED: loader mode latched");
		}
		if (cmd) {
			out_status(out, no, cap, gid, oid, 0x01);
		}
		return;
	}
	v = gw_filter(p, n, fwd, &fn, &why);
	gw_refuse(why);
	if (say) {
		gw_said++;
		gw_hexlog("host>", p, n, why >= 0 ? rf_names[why] : NULL);
	}
	if (v == V_LOCAL && cmd && gid == 0 && oid == 0) {
		if (persona_on()) {
			persona_step_say(p, n);         /* 1128 */
		}
		local_reset(p, n, out, no, cap);
		gw.rf_active = false;
		hci_conn = 0;                   /* 1137 */
		ntf_cancel("CORE_RESET");       /* 1140 */
		if (nfc_mwlog != 0 && !mwlog_done && !mwlog_failed) {  /* 1141: once per session, after the lock is dropped */
			mwlog_done = true;
			mwlog_pending = true;
		}
		if (nfc_gw_mode == 1) {
			*want_up = true;
		}
		return;
	}
	if (nfc_gw_mode != 1) {         /* mode 0: nothing reaches the chip */
		if ((nfc_id_bits & 4) && persona_packet(p, n, out, no, cap)) {
			return;                 /* 1125: answered by the PN80V persona */
		}
		if (v == V_LOCAL && gid == 0 && oid == 1 && (nfc_id_bits & 2)) {
			local_init(p, n, out, no, cap);
			return;
		}
		if (v != V_REJ) {
			gw_refuse(RF_MODE0);
		}
		if (cmd) {
			out_status(out, no, cap, gid, oid, 0x01);
		}
		return;
	}
	if (v == V_LOCAL) {
		if (gid == 0 && oid == 1) {
			local_init(p, n, out, no, cap);
		} else if (gid == 2) {
			const uint8_t r[5] = { 0x42, 0x00, 0x02, 0x00, 0x00 };  /* NFCEE_DISCOVER_RSP: OK, 0 NFCEEs */
			out_put(out, no, cap, r, sizeof(r));
		} else {
			out_status(out, no, cap, gid, oid, 0x00);     /* nothing left after stripping: OK */
			if (gid == 0 && oid == 2) {
				out[*no - 2] = 2;       /* CORE_SET_CONFIG_RSP: status OK, 0 invalid ids */
				const uint8_t z = 0;
				out_put(out, no, cap, &z, 1);
			}
		}
		return;
	}
	if (v == V_REJ) {
		if (cmd) {
			out_status(out, no, cap, gid, oid, 0x01);
		}
		return;
	}
	if (!gw_session) {
		gw_refuse(RF_CHIP);
	} else if (fn > 8) {
		gw_refuse(RF_BUSLEN);
	} else if (gw_chip_tx(fwd, fn) == 0) {
		return;
	} else {
		gw_refuse(RF_CHIP);
	}
	if (cmd) {
		out_status(out, no, cap, gid, oid, 0x01);
	}
}

/* ---- mode 1: the chip session ---------------------------------------------------------------------------------- */

static int
gw_chip_tx(const uint8_t *b, unsigned n)
{
	int err = sweet_se7_xfer(NFC_ADDR, b, n, NULL, 0);     /* the bus checks gw_bus_ok() again */

	if (gw_chip_said < 200) {
		gw_chip_said++;
		gw_hexlog("->chip", b, n, err ? "FAILED" : NULL);
	}
	return err;
}

static void
gw_up(const uint8_t *b, unsigned n)
{
	uint32_t unit;

	lck_mtx_lock(&gw_lock);
	unit = gw.connected ? gw.unit : 0;
	lck_mtx_unlock(&gw_lock);
	if (unit == 0 || gw_ctl == NULL || n == 0) {
		return;
	}
	if (ctl_enqueuedata(gw_ctl, unit, (void *)(uintptr_t)b, n, 0) == 0) {
		gw_up_pkts++;
	} else {
		gw_up_fail++;
	}
}

static void
gw_chip_packet(const uint8_t *p, unsigned n)
{
	const char *note = NULL;
	bool up = true;

	if ((p[0] == 0x61 && p[1] == 0x05)) {
		gw.rf_active = true;
	} else if ((p[0] == 0x61 || p[0] == 0x41) && p[1] == 0x06) {
		gw.rf_active = false;
	}
	if ((p[0] & 0x0fu) == 0x0f && (p[0] >> 5) != 0) {
		up = false;
		note = "ST proprietary: kept";
	} else if ((p[0] == 0x40 || p[0] == 0x60) && p[1] == 0x00) {
		up = false;
		note = "reset: driver-owned, kept";
	} else if (p[0] == 0x40 && p[1] == 0x01) {
		up = false;
		note = "init: driver-owned, kept";
	}
	if (gw_chip_said < 200) {
		gw_chip_said++;
		gw_hexlog("chip>", p, n, note);
	}
	if (up) {
		gw_up(p, n);
	}
}

static void
gw_rx_thread(__unused void *arg, __unused wait_result_t wr)
{
	static uint8_t p[3 + 255];
	unsigned n;

	while (gw_session) {
		if (!pin_in(P_IRQ)) {
			sleep_ms(2);
			continue;
		}
		if (nfc_read_pkt(p, &n, 0) != 0) {
			sleep_ms(2);
			continue;
		}
		if (p[0] == 0x60 && p[1] == 0x00 && n >= 4 &&
		    ((p[2] == 0x39 && p[3] == 0xa1) || (p[2] == 0x41 && p[3] == 0xa2))) {
			nfc_loader = true;
			gw_session = false;
			ven(false);
			gw_hexlog("chip>", p, n, "LOADER MODE - STOPPED, VEN low, latched until reboot");
			set_state("loader");
			break;
		}
		gw_chip_packet(p, n);
	}
	gw_rx_running = false;
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

/* VEN pulse, ST CORE_RESET_NTF, the driver's CORE_INIT (+ MODE_SET ON once if rejected), then the RX thread */
static int
gw_session_start(void)
{
	unsigned n = 0, m = 0;
	int err;
	thread_t th;

	if (nfc_loader) {
		return EPERM;
	}
	if (gw_session) {
		gw_session_stop("host reset");
	}
	if (__atomic_exchange_n(&nfc_busy, true, __ATOMIC_ACQ_REL)) {
		SAY("gw: chip busy (P0 probe running): no session\n");
		return EBUSY;
	}
	if (!pins_map() || !sweet_se7_init_nfc()) {
		nfc_busy = false;
		SAY("gw: pins or SE7 bus not available: no session\n");
		return ENXIO;
	}
	nfc_tx_open = true;
	ven(true);
	sleep_ms(10);
	ven(false);
	sleep_ms(20);
	ven(true);
	sleep_ms(10);
	ven(false);
	sleep_ms(20);
	ven(true);
	err = nfc_wait_for(0x60, 0x00, ntf, &n, NFC_WAIT_MS);
	if (err == 0 && ((ntf[2] == 0x39 && ntf[3] == 0xa1) || (ntf[2] == 0x41 && ntf[3] == 0xa2))) {
		nfc_loader = true;
		nfc_tx_open = false;
		ven(false);
		set_state("loader");
		SAY("gw: chip in LOADER MODE - STOPPED, VEN low, nothing more is sent until reboot\n");
		nfc_busy = false;
		return EPERM;
	}
	if (err == 0) {
		err = nfc_wr(TX_CORE_INIT, sizeof(TX_CORE_INIT), "CORE_INIT_CMD");
		err = err ? err : nfc_wait_for(0x40, 0x01, pkt, &m, NFC_WAIT_MS);
		if (err || m < 4 || pkt[3] != 0) {
			if (nfc_wr(TX_MODE_ON, sizeof(TX_MODE_ON), "PROP_NFC_MODE_SET_ON") == 0) {
				(void)nfc_wait_for(0x4f, 0x02, pkt, &m, NFC_WAIT_MS);
			}
			err = nfc_wr(TX_CORE_INIT, sizeof(TX_CORE_INIT), "CORE_INIT_CMD (2nd)");
			err = err ? err : nfc_wait_for(0x40, 0x01, pkt, &m, NFC_WAIT_MS);
			if (!err && (m < 4 || pkt[3] != 0)) {
				err = EPROTO;
			}
		}
	}
	nfc_tx_open = false;
	if (err) {
		ven(false);
		nfc_busy = false;
		set_state("gw:chip_error");
		SAY("gw: chip bring-up failed (%d): VEN low, no session\n", err);
		return err;
	}
	gw.rf_active = false;
	gw_session = true;
	gw_rx_running = true;
	if (kernel_thread_start(gw_rx_thread, NULL, &th) != KERN_SUCCESS) {
		gw_session = false;
		gw_rx_running = false;
		ven(false);
		nfc_busy = false;
		return ENOMEM;
	}
	thread_deallocate(th);
	set_state("gw:session");
	SAY("gw: chip session up (mode 1): filtered host packets now reach the chip\n");
	return 0;
}

static void
gw_session_stop(const char *why)
{
	if (!gw_session && !gw_rx_running) {
		return;
	}
	gw_session = false;
	for (unsigned i = 0; gw_rx_running && i < 500; i++) {
		sleep_ms(1);
	}
	ven(false);
	gw.rf_active = false;
	nfc_busy = false;
	if (!nfc_loader) {
		set_state("off");
	}
	if (gw_dl_mode) {               /* 1146: a chip reset ends the download capture */
		gw_dl_mode = false;
		gw_dl_n = 0;
	}
	SAY("gw: chip session stopped (%s): VEN low\n", why);
}

/* ---- the kernel control com.apple.uart.stockholm ---------------------------------------------------------------- */

/* 1125: the control cannot be deregistered from inside its own disconnect callback: a short thread does it */
static void
gw_dereg_thread(__unused void *arg, __unused wait_result_t wr)
{
	sleep_ms(20);
	lck_mtx_lock(&p1_lock);
	if (gw_dereg_pending && nfc_p1 == 0 && gw_ctl != NULL && !gw.connected) {
		errno_t e = ctl_deregister(gw_ctl);

		if (e == 0) {
			gw_ctl = NULL;
			gw_dereg_pending = false;
		}
		SAY("kernel control com.apple.uart.stockholm deregistered after the client closed -> %d\n", e);
	}
	lck_mtx_unlock(&p1_lock);
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

static errno_t
gw_ctl_connect(__unused kern_ctl_ref ref, struct sockaddr_ctl *sac, void **unitinfo)
{
	char who[32] = "?";

	proc_selfname(who, sizeof(who));
	*unitinfo = NULL;
	if (nfc_p1 == 0) {
		SAY("com.apple.uart.stockholm: connect by %s (pid %d) refused: P1 withdrawn\n", who, proc_selfpid());
		return ECONNREFUSED;
	}
	lck_mtx_lock(&gw_lock);
	if (gw.connected) {
		lck_mtx_unlock(&gw_lock);
		SAY("com.apple.uart.stockholm: second connect by %s (pid %d) refused (EBUSY), %s holds it\n", who,
		    proc_selfpid(), gw.who);
		return EBUSY;
	}
	gw.connected = true;
	gw.unit = sac->sc_unit;
	gw.asm_n = 0;
	gw.rf_active = false;
	gw.crc = gw.on_demand = 0;
	gw.pend = gw.crc_rx = gw.crc_out = false;       /* 1127 */
	hci_conn = 0;                                   /* 1137 */
	ntf_cancel("session open");                     /* 1140 */
	mwlog_done = mwlog_pending = false;             /* 1141 */
	gw.chip_crc = false;                            /* 1132 */
	gw_crc_next = 0;
	strlcpy(gw.who, who, sizeof(gw.who));
	if (++gw_conns <= 3) {
		gw_said = 0;            /* 1125: full packet log for each of the first three sessions */
		persona_said = 0;       /* 1127: and the persona lines */
	}
	persona_st = -1;                /* 1128: init steps counted per session */
	persona_ri = -1;                /* 1134 */
	persona_ri_n = ri_said = 0;
	swp_have = 0;
	lck_mtx_unlock(&gw_lock);
	SAY("com.apple.uart.stockholm: OPEN by %s (pid %d), unit %u, gateway mode %u\n", who, proc_selfpid(), sac->sc_unit,
	    nfc_gw_mode);
	return 0;
}

static errno_t
gw_ctl_disconnect(__unused kern_ctl_ref ref, u_int32_t unit, __unused void *unitinfo)
{
	char who[32];

	lck_mtx_lock(&gw_lock);
	if (gw.unit == unit) {
		gw.connected = false;
		gw.unit = 0;
	}
	ntf_cancel("session close");    /* 1140 */
	strlcpy(who, gw.who, sizeof(who));
	lck_mtx_unlock(&gw_lock);
	SAY("com.apple.uart.stockholm: CLOSE (%s, unit %u): host packets %u (%u bytes), sent up %u (failed %u)\n", who, unit,
	    gw_host_pkts, gw_host_bytes, gw_up_pkts, gw_up_fail);
	gw_session_stop("client closed");
	if (gw_dereg_pending) {
		thread_t th;

		if (kernel_thread_start(gw_dereg_thread, NULL, &th) == KERN_SUCCESS) {
			thread_deallocate(th);
		}
	}
	return 0;
}

/* 1127: the persona is on (mode 0, kern.sweet_nfc_id bit 2): host packets go through gw_pend_* */
static bool
persona_on(void)
{
	return (nfc_id_bits & 4) && nfc_gw_mode != 1;
}

/* 1127: answer the held packet; had_crc: its CRC16 trailer came and matched. gw_lock held. */
static void
gw_pend_flush(bool had_crc, uint8_t *out, unsigned *no, unsigned cap, bool *want_up)
{
	unsigned first = *no, mode = nfc_fwif_fmt & 3;

	gw.pend = false;
	gw.crc_k = 0;
	if (had_crc && !gw.crc_rx) {
		gw.crc_rx = true;
		SAY("1127: host packets now carry a CRC16 trailer (first on %02x %02x, crc %04x): taken off; reply CRC mode %u\n",
		    gw.pend_buf[0], gw.pend_buf[1], gw.pend_crc, mode);
	}
	gw.crc_out = mode == 0 ? had_crc : mode == 2 ? (gw.crc_rx || gw.crc != 0) : false;
	if (gw.chip_crc && !(nfc_fwif_fmt & 0x40)) {    /* 1132: the "chip" turned its CRC on (A0 BF) */
		gw.crc_out = true;
	}
	gw_host_packet(gw.pend_buf, gw.pend_n, out, no, cap, want_up);
	if (gw.crc_out && *no > first) {
		uint8_t t[1024];
		unsigned k = first, tn = 0, pk = 0;

		while (k + 3 <= *no) {
			unsigned pl = 3u + out[k + 2];
			uint16_t c;

			if (k + pl > *no || tn + pl + 2 > sizeof(t)) {
				break;
			}
			memcpy(t + tn, out + k, pl);
			c = nci_crc16(out + k, pl);
			t[tn + pl] = (uint8_t)(c >> 8);
			t[tn + pl + 1] = (uint8_t)c;
			tn += pl + 2;
			k += pl;
			pk++;
		}
		if (k == *no && first + tn <= cap) {
			memcpy(out + first, t, tn);
			*no = first + tn;
			if (persona_said < 400) {
				SAY("1127: +CRC16 on %u reply packet(s)\n", pk);
			}
		}
	}
	gw.crc_out = false;
	if (gw_crc_next != 0) {                         /* 1132: A0 BF takes effect after its own RSP */
		gw.chip_crc = gw_crc_next == 1;
		gw_crc_next = 0;
	}
}

/* 1127: a whole host packet in gw.asm_buf: hold it for a CRC16 trailer (persona) or answer it now */
static void
gw_packet_done(unsigned need, uint8_t *out, unsigned *no, unsigned cap, bool *want_up)
{
	if (!persona_on()) {
		gw_host_packet(gw.asm_buf, need, out, no, cap, want_up);
		return;
	}
	if (gw.pend) {
		gw_pend_flush(false, out, no, cap, want_up);
	}
	memcpy(gw.pend_buf, gw.asm_buf, need);
	gw.pend_n = need;
	gw.pend_crc = nci_crc16(gw.asm_buf, need);
	gw.crc_k = 0;
	gw.pend = true;
}

/* 1146: log the NXP HDLL frames nfcd writes while in download mode. Framing: [LEN_hi LEN_lo | payload | CRC16],
 * LEN = payload length, whole frame = 2 + LEN + 2. Each complete frame is one "nfc-hci" klog line; bytes left at
 * the end of a write are flushed as a partial line so no byte nfcd sends is lost even if our LEN read is off.
 * Nothing here reaches the chip and nothing is answered. */
static void
gw_dl_capture(mbuf_t m)
{
	for (mbuf_t b = m; b != NULL; b = mbuf_next(b)) {
		const uint8_t *d = mbuf_data(b);
		size_t len = mbuf_len(b), i = 0;

		while (i < len) {
			if (gw_dl_n >= sizeof(gw_dl_buf)) {
				gw_hexlog("nfc-hci", gw_dl_buf, gw_dl_n, "dnld host> overflow, resync");
				gw_dl_n = 0;
			}
			gw_dl_buf[gw_dl_n++] = d[i++];
			if (gw_dl_n >= 2) {
				unsigned plen = ((unsigned)gw_dl_buf[0] << 8) | gw_dl_buf[1];
				unsigned need = 2u + plen + 2u;

				if (need > sizeof(gw_dl_buf)) {
					gw_hexlog("nfc-hci", gw_dl_buf, gw_dl_n, "dnld host> bad LEN, resync");
					gw_dl_n = 0;
					continue;
				}
				if (gw_dl_n == need) {
					gw_dl_frames++;
					gw_hexlog("nfc-hci", gw_dl_buf, gw_dl_n, "dnld host> HDLL frame (not forwarded, no reply)");
					gw_dl_n = 0;
				}
			}
		}
	}
	if (gw_dl_n > 0) {
		gw_hexlog("nfc-hci", gw_dl_buf, gw_dl_n, "dnld host> write end, LEN not satisfied (partial)");
		gw_dl_n = 0;
	}
}

static errno_t
gw_ctl_send(__unused kern_ctl_ref ref, u_int32_t unit, __unused void *unitinfo, mbuf_t m, __unused int flags)
{
	static uint8_t out[1024];       /* replies, sent after gw_lock is dropped; one sender at a time (gw_lock) */
	uint8_t copy[1024];
	unsigned no = 0;
	bool want_up = false;

	if (nfc_p1 == 0) {              /* 1125: withdrawn, waiting for the client to close */
		mbuf_freem(m);
		return 0;
	}
	if (gw_dl_mode) {               /* 1146: download capture: log HDLL frames, forward nothing, answer nothing */
		gw_dl_capture(m);
		mbuf_freem(m);
		return 0;
	}
	lck_mtx_lock(&gw_lock);
	for (mbuf_t b = m; b != NULL; b = mbuf_next(b)) {
		const uint8_t *d = mbuf_data(b);
		size_t len = mbuf_len(b), i = 0;

		gw_host_bytes += (uint32_t)len;
		while (i < len) {
			if (gw.pend && gw.asm_n == 0) {         /* 1127: a CRC16 trailer for the held packet? */
				uint8_t want = gw.crc_k == 0 ? (uint8_t)(gw.pend_crc >> 8) : (uint8_t)gw.pend_crc;

				if (d[i] == want) {
					i++;
					if (++gw.crc_k == 2) {
						gw_pend_flush(true, out, &no, sizeof(out), &want_up);
					}
					continue;
				}
				bool half = gw.crc_k == 1;

				gw_pend_flush(false, out, &no, sizeof(out), &want_up);
				if (half) {                     /* that byte was the next packet's first, not CRC */
					gw.asm_buf[gw.asm_n++] = (uint8_t)(gw.pend_crc >> 8);
				}
			}
			if (gw.asm_n == 0 && gw.crc && len - i == 2) {
				SAY("host> 2 trailing bytes [%02x %02x] after a packet with CRC on: taken as CRC16, dropped\n", d[i],
				    d[i + 1]);
				break;
			}
			if (gw.asm_n == 0 && !hdr_ok(d[i])) {
				gw_refuse(RF_INVALID);
				gw_hexlog("host>", d + i, (unsigned)(len - i), "INVALID first byte (not NCI cmd/data): rest of chunk dropped");
				break;
			}
			if (gw.asm_n < 3) {
				gw.asm_buf[gw.asm_n++] = d[i++];
				continue;
			}
			unsigned need = 3u + gw.asm_buf[2], take = need - gw.asm_n;
			if (take > len - i) {
				take = (unsigned)(len - i);
			}
			memcpy(gw.asm_buf + gw.asm_n, d + i, take);
			gw.asm_n += take;
			i += take;
			if (gw.asm_n == need) {
				gw_packet_done(need, out, &no, sizeof(out), &want_up);
				gw.asm_n = 0;
			}
		}
		/* a header with a zero-length payload completes at the third byte */
		if (gw.asm_n == 3 && gw.asm_buf[2] == 0) {
			gw_packet_done(3, out, &no, sizeof(out), &want_up);
			gw.asm_n = 0;
		}
	}
	if (gw.pend) {                  /* 1127: the write ended without a (whole) CRC16 trailer */
		bool half = gw.crc_k == 1;

		gw_pend_flush(false, out, &no, sizeof(out), &want_up);
		if (half) {
			gw.asm_buf[gw.asm_n++] = (uint8_t)(gw.pend_crc >> 8);
		}
	}
	memcpy(copy, out, no);
	bool mwl = mwlog_pending;                       /* 1141 */
	mwlog_pending = false;
	lck_mtx_unlock(&gw_lock);
	mbuf_freem(m);
	if (mwl) {
		mwlog_run();                            /* nfcd's own thread, no lock held (copyin/copyout may fault) */
	}
	if (want_up) {
		(void)gw_session_start();
	}
	if (no) {
		if (gw_said < 300) {
			gw_hexlog("host<", copy, no, "(local)");
		}
		if (ctl_enqueuedata(gw_ctl, unit, copy, no, 0) == 0) {
			gw_up_pkts++;
		} else {
			gw_up_fail++;
		}
	}
	return 0;
}

static const char *
opt_name(uint32_t opt)
{
	switch (opt) {
	case 0x80047301u: return "IO_STOCKHOLM_CRC";
	case 0x80047302u: return "IO_STOCKHOLM_ON_DEMAND_CLIENT";
	case 0x40407303u: return "IO_STOCKHOLM_SPMIERRORS";
	case 0x40487413u: return "TIOCGETA";
	case 0x80487414u: return "TIOCSETA";
	case 0x80047410u: return "TIOCFLUSH";
	case 0x4004746au: return "TIOCMGET";
	default: return "unknown";
	}
}

static errno_t
gw_ctl_setopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t len)
{
	uint32_t o = (uint32_t)opt, v = 0;
	uint64_t cflag = 0, ispeed = 0, ospeed = 0;

	if (data != NULL && len >= 4) {
		memcpy(&v, data, 4);
	}
	lck_mtx_lock(&gw_lock);
	switch (o) {
	case 0x80047301u:
		gw.crc = v;
		break;
	case 0x80047302u:
		gw.on_demand = v;
		break;
	case 0x80487414u:
		if (data != NULL && len <= sizeof(gw.termios)) {
			memcpy(gw.termios, data, len);
		}
		if (len >= 72) {
			memcpy(&cflag, (uint8_t *)data + 16, 8);
			memcpy(&ispeed, (uint8_t *)data + 56, 8);
			memcpy(&ospeed, (uint8_t *)data + 64, 8);
		}
		break;
	case 0x80047410u:
		gw.asm_n = 0;           /* flush: drop a half-assembled host packet */
		break;
	default:
		break;
	}
	lck_mtx_unlock(&gw_lock);
	if (gw_opt_said++ < 200) {
		if (o == 0x80487414u) {
			SAY("setopt %#x %s (%zu bytes): c_cflag %#llx ispeed %llu ospeed %llu\n", o, opt_name(o), len, cflag, ispeed, ospeed);
		} else {
			SAY("setopt %#x %s (%zu bytes) = %u%s\n", o, opt_name(o), len, v,
			    o == 0x80047301u && v ? " (CRC ON: framing unknown; 2-byte trailers are logged and dropped)" : "");
		}
	}
	return 0;
}

static errno_t
gw_ctl_getopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t *len)
{
	uint32_t o = (uint32_t)opt;
	size_t want = len ? *len : 0;

	if (data != NULL && len != NULL) {
		bzero(data, *len);
		if (o == 0x40487413u) {                         /* TIOCGETA */
			lck_mtx_lock(&gw_lock);
			size_t k = *len < sizeof(gw.termios) ? *len : sizeof(gw.termios);
			memcpy(data, gw.termios, k);
			lck_mtx_unlock(&gw_lock);
			*len = k;
		} else if (o == 0x4004746au && *len >= 4) {     /* TIOCMGET: CTS asserted */
			const uint32_t lines = 0x20u;           /* TIOCM_CTS */
			memcpy(data, &lines, 4);
			*len = 4;
		} else if (o == 0x40407303u) {                  /* IO_STOCKHOLM_SPMIERRORS: 64 bytes of zeros */
			*len = *len < 64 ? *len : 64;
		}
	}
	if (gw_opt_said++ < 200) {
		SAY("getopt %#x %s (%zu bytes asked, %zu given)\n", o, opt_name(o), want, len ? *len : 0);
	}
	return 0;
}

static void
gw_ctl_rcvd(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, __unused int flags)
{
}

static errno_t
gw_ctl_register(void)
{
	struct kern_ctl_reg reg;
	uint64_t cflag = 0x4b00, speed = 115200;        /* CS8 | CREAD | CLOCAL until nfcd sets its own */
	errno_t e;

	bzero(&reg, sizeof(reg));
	strlcpy(reg.ctl_name, "com.apple.uart.stockholm", sizeof(reg.ctl_name));
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM | CTL_FLAG_REG_EXTENDED;
	reg.ctl_sendsize = 16 * 1024;
	reg.ctl_recvsize = 16 * 1024;
	reg.ctl_connect = gw_ctl_connect;
	reg.ctl_disconnect = gw_ctl_disconnect;
	reg.ctl_send = gw_ctl_send;
	reg.ctl_setopt = gw_ctl_setopt;
	reg.ctl_getopt = gw_ctl_getopt;
	reg.ctl_rcvd = gw_ctl_rcvd;
	bzero(gw.termios, sizeof(gw.termios));
	memcpy(gw.termios + 16, &cflag, 8);
	memcpy(gw.termios + 56, &speed, 8);
	memcpy(gw.termios + 64, &speed, 8);
	e = ctl_register(&reg, &gw_ctl);
	SAY("kernel control com.apple.uart.stockholm -> %d (id %u)\n", e, reg.ctl_id);
	if (e) {
		gw_ctl = NULL;
	}
	return e;
}

/* ---- AppleStockholmControlUserClient (SweetNFC.cpp) calls ------------------------------------------------------ */

/* 0 ok, 1 not permitted, 2 unsupported, 3 bad argument */
int
sweet_nfc_uc_call(uint32_t sel, uint32_t nin, uint64_t v)
{
	static const char *const names[9] = { "Power", "DownloadRequest", "PMUStandbyPower", "DeviceWake", "(4)", "VBAT",
		                              "ValidateSPMIConfig", "ConfigureHammerfestSPMI", "SPMISlaveReset" };
	int r = 0;
	const char *what = "";
	char who[32];

	proc_selfname(who, sizeof(who));
	if (sel > 8 || ((sel <= 3 || sel == 5 || sel == 8) && nin < 1)) {
		r = 3;
	} else if (sel == 0) {
		if (v && nfc_loader) {
			r = 1;
			what = "loader mode latched: VEN stays low";
		} else if (nfc_busy && !gw_session) {
			what = "P0 probe running: VEN left to the probe";
		} else if (!pins_map()) {
			r = 2;
			what = "pins not mapped";
		} else {
			if (!v && gw_session) {
				gw_session_stop("Power 0");
			}
			if (!v && (gw.chip_crc || gw_crc_next) && !(nfc_fwif_fmt & 0x80)) {
				gw.chip_crc = false;    /* 1132: VEN low = the chip forgets A0 BF (bit 0x80 keeps it) */
				gw_crc_next = 0;
				SAY("persona: 1132: Power 0: chip CRC16 off\n");
			}
			if (!v && persona_ri >= 0) {
				SAY("persona: 1134: Power 0 after ReInit #%u step %d\n", persona_ri_n, persona_ri);
				persona_ri = -1;        /* 1134 */
			}
			ven(v != 0);
			what = v ? "VEN (gpio12) high" : "VEN (gpio12) low";
		}
	} else if (sel == 1) {
		if (v == 1) {
			if ((nfc_fwif_fmt & 0x40000) && (nfc_fwif_fmt & 0x4000)) {
				/* 1146: accept DownloadRequest and capture the NXP HDLL frames nfcd writes; nothing reaches
				 * the chip and nothing is answered (gateway mode 0; forwarding stays blocked). */
				gw_dl_mode = true;
				gw_dl_n = 0;
				gw_dl_frames = 0;
				what = "download mode latched (fwif_fmt 0x40000): capturing HDLL frames, nothing forwarded";
			} else {
				r = 1;
				gw_refused[RF_DOWNLOAD]++;
				what = "REFUSED: no download mode on this port";
			}
		} else {
			if (gw_dl_mode) {
				gw_dl_mode = false;
				gw_dl_n = 0;
				SAY("nfc-hci: persona: 1146: download mode off after %u frame(s) captured\n", gw_dl_frames);
			}
			what = "download request off: accepted";
		}
	} else if (sel == 2 || sel == 3 || sel == 5) {
		what = "accepted, no-op";
	} else {
		r = 2;
		what = sel == 4 ? "unsupported" : "SPMI only: unsupported";
	}
	if (gw_sel_said++ < 200 || sel == 1) {
		SAY("AppleStockholmControlUserClient: %s sel %u %s(%llu) [%u in] -> %s%s\n", who, sel, sel <= 8 ? names[sel] : "?",
		    v, nin, r == 0 ? "ok" : r == 1 ? "kIOReturnNotPermitted" : r == 2 ? "kIOReturnUnsupported" : "kIOReturnBadArgument",
		    *what ? ": " : "");
		if (*what) {
			SAY("  ... %s\n", what);
		}
	}
	return r;
}

void
sweet_nfc_uc_event(int open, uint32_t type)
{
	char who[32];

	proc_selfname(who, sizeof(who));
	SAY("AppleStockholmControlUserClient: %s by %s (pid %d), type %u\n", open ? "OPEN" : "CLOSE", who, proc_selfpid(),
	    type);
	if (!open && gw_dl_mode) {      /* 1146: client gone: end download capture */
		gw_dl_mode = false;
		gw_dl_n = 0;
	}
	if (!open && !gw_session && !(nfc_busy) && pins_map()) {
		ven(false);             /* the client is gone: chip off */
	}
}

/* ---- the P1 switch ------------------------------------------------------------------------------------------------ */

static int
nfc_p1_set(uint32_t v, const char *why)
{
	int e = 0;

	lck_mtx_lock(&p1_lock);
	if (v == nfc_p1) {
		lck_mtx_unlock(&p1_lock);
		return 0;
	}
	if (v == 1) {
		e = SweetNFCPublish(true, why);
		if (e == 0 || e == EALREADY) {
			gw_dereg_pending = false;
			e = gw_ctl != NULL ? 0 : gw_ctl_register();     /* 1125: still registered after a withdraw */
			if (e) {
				(void)SweetNFCPublish(false, "control registration failed");
			}
		}
		if (e == 0) {
			nfc_p1 = 1;
		}
	} else {
		/* 1125: no EBUSY. The IOService goes first (terminating nfcd's user client), the control is closed for new
		 * clients and answers nothing; with a client still connected it is deregistered when that client closes. */
		nfc_p1 = 0;
		(void)SweetNFCPublish(false, why);
		gw_session_stop(why);
		if (gw.connected) {
			gw_dereg_pending = true;
		} else if (gw_ctl != NULL) {
			e = ctl_deregister(gw_ctl);
			if (e == 0) {
				gw_ctl = NULL;
			} else {
				gw_dereg_pending = true;        /* raced with a connect: done at its close */
				e = 0;
			}
		}
		if (!nfc_busy && pins_map()) {
			ven(false);
		}
	}
	lck_mtx_unlock(&p1_lock);
	SAY("P1 %s (%s) -> %d\n", v ? "publish" : "withdraw", why, e);
	return e;
}

static void
nfc_p1_boot_thread(__unused void *arg, __unused wait_result_t wr)
{
	sleep_ms(2000);
	for (unsigned i = 0; i < 30 && nfc_p1_set(1, "boot-arg sweetnfc bit 2") == ENXIO; i++) {
		sleep_ms(1000);         /* no platform expert yet */
	}
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

static int
sysctl_nfc_p1 SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, (int)nfc_p1, sizeof(value), &value, &changed);

	if (error || !changed) {
		return error;
	}
	if (value != 0 && value != 1) {
		return EINVAL;
	}
	return nfc_p1_set((uint32_t)value, value ? "kern.sweet_nfc_p1=1" : "kern.sweet_nfc_p1=0");
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_nfc_p1, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_nfc_p1, "I",
    "redfin 1119: 1 = publish AppleStockholmControl + com.apple.uart.stockholm (default 0: nothing published)");

static int
sysctl_nfc_gw SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, (int)nfc_gw_mode, sizeof(value), &value, &changed);

	if (error || !changed) {
		return error;
	}
	if (value != 0 && value != 1) {
		return EINVAL;
	}
	nfc_gw_mode = (uint32_t)value;
	if (value == 0) {
		gw_session_stop("kern.sweet_nfc_gw=0");
	}
	SAY("gateway mode %d%s\n", value, value ? " (pass-through of the filtered whitelist from the next host CORE_RESET)" :
	    " (log + reject, nothing reaches the chip)");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_nfc_gw, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_nfc_gw, "I",
    "redfin 1119: NFC gateway mode (0 = log and reject, nothing to the chip; 1 = filtered pass-through)");
SYSCTL_UINT(_kern, OID_AUTO, sweet_nfc_id, CTLFLAG_RW | CTLFLAG_LOCKED, &nfc_id_bits, 0,
    "redfin 1119/1125: bit 0 = CORE_RESET_NTF after the local RSP (default 1); bit 1 = local CORE_INIT_RSP in mode 0; "
    "bit 2 = PN80V persona in mode 0 (whole NXP init sequence answered locally, no NTF; default off)");
SYSCTL_UINT(_kern, OID_AUTO, sweet_nfc_id_info, CTLFLAG_RW | CTLFLAG_LOCKED, &nfc_id_info, 0,
    "redfin 1119/1125: identity bytes hw.rom.fwmajor.fwminor (default 0x4202016a = PN80V, ROM 2, PN552 FW 02.01.6A)");
SYSCTL_UINT(_kern, OID_AUTO, sweet_nfc_fwif, CTLFLAG_RW | CTLFLAG_LOCKED, &nfc_fwif, 0,
    "redfin 1125/1127: persona answer to NXP 2F 19 GetFwInterfaceVersion, big endian (default 0x15, what hw 0x42 needs)");
SYSCTL_UINT(_kern, OID_AUTO, sweet_nfc_mwlog, CTLFLAG_RW | CTLFLAG_LOCKED, &nfc_mwlog, 0,
    "redfin 1141: N > 0 = raise nfcd's libNFC_Comet middleware log tables to N (1..4) at the first CORE_RESET of a session");
SYSCTL_UINT(_kern, OID_AUTO, sweet_nfc_ntf_delay_ms, CTLFLAG_RW | CTLFLAG_LOCKED, &nfc_ntf_delay_ms, 0,
    "redfin 1140: delay of the persona's deferred NTF after NFCEE_MODE_SET C0 (fwif_fmt 0x10000), ms, 0..1500 (default 100)");
SYSCTL_UINT(_kern, OID_AUTO, sweet_nfc_fwif_fmt, CTLFLAG_RW | CTLFLAG_LOCKED, &nfc_fwif_fmt, 0,
    "redfin 1127: persona variants: bits 0-1 reply CRC16 (0 mirror the host, 1 never, 2 always once seen/IO_STOCKHOLM_CRC); "
    "4 = 1125 answers to 2F xx; 8 = GetFwType type 1; 1128: 0x10 = SET_CONFIG A0xx kept as GET values (EEPROM mirror), "
    "0x20 = 1127 GET_CONFIG answer (each id one zero byte); 1132: 0x40 = ignore SET_CONFIG A0 BF (no chip CRC), "
    "0x80 = chip CRC state kept over Power 0; 1134: 0x100/0x200 = virtual eSE; 1136: 0x400 = + virtual HCI network, "
    "0x800 = 1137: also answer CORE_CONN_CREATE for the virtual NFCEE 01 (HCI conn 2); 1138: 0x1000 = eSE EoOP NTF "
    "6F 20 C0 after NFCEE_DISCOVER and MODE_SET C0, 0x2000 = GET_CONFIG A0EB returns the host-set value; 1139: 0x4000 = "
    "MODE_SET C0/01 00 -> 6F 18 00 (1144; 1145: also for NFCEE 01; 0x8000: 6F 18 02 C0 00), no 6F 20, + A0EB mirror; 1140: 0x10000 = that NTF "
    "deferred by kern.sweet_nfc_ntf_delay_ms as its own packet, 0x20000 = no NTF after the disable RSP; 1146: 0x40000 "
    "(with 0x4000) = accept DownloadRequest and log the NXP HDLL frames nfcd writes (nfc-hci), forwarding/answering nothing");

static int
cfg_hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/*
 * 1128: kern.sweet_nfc_cfg_set: "A00F=3401..,A0EC=00" sets what the persona's CORE_GET_CONFIG returns for an id
 * (value and length, at most 72 bytes), "A0EC=" drops it, "clear" drops all. Read: "id:len:first 8 bytes ..".
 * Ids are 4 hex digits (A0xx/A1xx NXP, 00xx standard). Spaces, ':' and '.' inside a value are ignored.
 */
static int
sysctl_nfc_cfg_set SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char b[CFG_N * 30 + 8], in[400];
	size_t o = 0;
	int err;

	b[0] = 0;
	lck_mtx_lock(&gw_lock);
	for (unsigned i = 0; i < nfc_cfg_n && o + 30 < sizeof(b); i++) {
		char h[3 * 8 + 4];

		hex(h, sizeof(h), nfc_cfg[i].val, nfc_cfg[i].len > 8 ? 8 : nfc_cfg[i].len);
		o += (size_t)snprintf(b + o, sizeof(b) - o, "%s%04X:%u:%s%s", i ? "," : "", nfc_cfg[i].id, nfc_cfg[i].len, h,
		    nfc_cfg[i].len > 8 ? ".." : "");
	}
	lck_mtx_unlock(&gw_lock);
	err = SYSCTL_OUT(req, b, strlen(b) + 1);
	if (err || !req->newptr) {
		return err;
	}
	if (req->newlen >= sizeof(in)) {
		return EINVAL;
	}
	err = SYSCTL_IN(req, in, req->newlen);
	if (err) {
		return err;
	}
	in[req->newlen] = 0;
	if (strncmp(in, "clear", 5) == 0) {
		lck_mtx_lock(&gw_lock);
		nfc_cfg_n = 0;
		lck_mtx_unlock(&gw_lock);
		SAY("1128: kern.sweet_nfc_cfg_set: all GET_CONFIG overrides dropped\n");
		return 0;
	}
	for (char *c = in; *c;) {
		uint8_t v[CFG_MAX];
		unsigned id = 0, vl = 0, d;
		int x;

		while (*c == ',' || *c == ' ' || *c == '\n') {
			c++;
		}
		if (!*c) {
			break;
		}
		for (d = 0; d < 4; d++, c++) {
			if ((x = cfg_hexval(*c)) < 0) {
				return EINVAL;
			}
			id = id << 4 | (unsigned)x;
		}
		if (*c++ != '=') {
			return EINVAL;
		}
		for (d = 0; *c && *c != ','; c++) {
			if (*c == ' ' || *c == ':' || *c == '.') {
				continue;
			}
			if ((x = cfg_hexval(*c)) < 0 || vl >= CFG_MAX) {
				return EINVAL;
			}
			if (d++ & 1) {
				v[vl - 1] = (uint8_t)(v[vl - 1] << 4 | (unsigned)x);
			} else {
				v[vl++] = (uint8_t)x;
			}
		}
		if (d & 1) {
			return EINVAL;  /* odd number of hex digits */
		}
		lck_mtx_lock(&gw_lock);
		bool ok = cfg_put((uint16_t)id, v, vl);
		lck_mtx_unlock(&gw_lock);
		if (!ok) {
			return ENOSPC;
		}
		SAY("1128: kern.sweet_nfc_cfg_set: GET_CONFIG %04X -> %s %u byte(s)\n", id, vl ? "override" : "default", vl);
	}
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_nfc_cfg_set, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_nfc_cfg_set,
    "A", "redfin 1128: persona GET_CONFIG values: \"A00F=<hex>,A0EC=<hex>\" set, \"A0EC=\" drop, \"clear\"; read = overrides");

static int
sysctl_nfc_persona SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char b[32 * 12 + 32];
	size_t o = 0;

	b[0] = 0;
	for (unsigned i = 0; i < 32 && persona_seen[i].n != 0 && o + 12 < sizeof(b); i++) {
		o += (size_t)snprintf(b + o, sizeof(b) - o, "%s%04x:%u", i ? " " : "", persona_seen[i].op, persona_seen[i].n);
	}
	if (o + 24 < sizeof(b)) {       /* 1132: the chip CRC state (SET_CONFIG A0 BF) */
		snprintf(b + o, sizeof(b) - o, " | crc %s", gw.chip_crc ? "on" : "off");
	}
	return SYSCTL_OUT(req, b, strlen(b) + 1);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_nfc_persona, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, sysctl_nfc_persona,
    "A", "redfin 1125: commands answered by the PN80V persona, gid<<8|oid:count (0200 = NFCEE_DISCOVER = end of init); 1132: | crc on/off");
SYSCTL_UINT(_kern, OID_AUTO, sweet_nfc_host_pkts, CTLFLAG_RD | CTLFLAG_LOCKED, &gw_host_pkts, 0,
    "redfin 1119: NCI packets received from the host on com.apple.uart.stockholm");

static int
sysctl_nfc_refused SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char b[400];
	size_t o = 0;

	o += (size_t)snprintf(b, sizeof(b), "bus=%u", nfc_refused);
	for (unsigned i = 0; i < RF_N && o < sizeof(b); i++) {
		o += (size_t)snprintf(b + o, sizeof(b) - o, " %s=%u", rf_names[i], gw_refused[i]);
	}
	return SYSCTL_OUT(req, b, strlen(b) + 1);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_nfc_refused, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, sysctl_nfc_refused, "A",
    "redfin 1119: refused / stripped host traffic by reason (bus = writes the SE7 bus check refused)");
#endif /* ARM64_BOARD_CONFIG_SWEET */
