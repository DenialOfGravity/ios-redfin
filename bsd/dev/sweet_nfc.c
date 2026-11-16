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
			r[len++] = 0x00;
			r[len++] = 0x00;
			what = "CORE_SET_CONFIG: OK (not applied)";
			break;
		case 0x0003: {                  /* CORE_GET_CONFIG: each id with one zero byte */
			unsigned cnt = n > 3 ? p[3] : 0, i = 4, k = 0;

			r[len++] = 0x00;
			r[len++] = 0x00;        /* count, filled below */
			for (; k < cnt && i < n && len + 4 < sizeof(r); k++) {
				if (p[i] >= 0xa0 && i + 1 < n) {        /* NXP extended (2-byte) tag */
					r[len++] = p[i++];
				}
				r[len++] = p[i++];
				r[len++] = 0x01;
				r[len++] = 0x00;
			}
			r[4] = (uint8_t)k;
			what = "CORE_GET_CONFIG: each id = 1 zero byte";
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
			r[len++] = 0x00;
			what = "NFCEE_DISCOVER: OK, 0 NFCEEs (end of the init sequence)";
			break;
		default:
			return false;
		}
	}
	r[2] = (uint8_t)(len - 3);
	out_put(out, no, cap, r, len);
	persona_note((uint16_t)((gid << 8) | oid));
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
		local_reset(p, n, out, no, cap);
		gw.rf_active = false;
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
	strlcpy(gw.who, who, sizeof(gw.who));
	if (++gw_conns <= 3) {
		gw_said = 0;            /* 1125: full packet log for each of the first three sessions */
		persona_said = 0;       /* 1127: and the persona lines */
	}
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
	lck_mtx_unlock(&gw_lock);
	mbuf_freem(m);
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
			ven(v != 0);
			what = v ? "VEN (gpio12) high" : "VEN (gpio12) low";
		}
	} else if (sel == 1) {
		if (v == 1) {
			r = 1;
			gw_refused[RF_DOWNLOAD]++;
			what = "REFUSED: no download mode on this port";
		} else {
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
SYSCTL_UINT(_kern, OID_AUTO, sweet_nfc_fwif_fmt, CTLFLAG_RW | CTLFLAG_LOCKED, &nfc_fwif_fmt, 0,
    "redfin 1127: persona variants: bits 0-1 reply CRC16 (0 mirror the host, 1 never, 2 always once seen/IO_STOCKHOLM_CRC); "
    "4 = 1125 answers to 2F xx; 8 = GetFwType type 1");

static int
sysctl_nfc_persona SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char b[32 * 12 + 8];
	size_t o = 0;

	b[0] = 0;
	for (unsigned i = 0; i < 32 && persona_seen[i].n != 0 && o + 12 < sizeof(b); i++) {
		o += (size_t)snprintf(b + o, sizeof(b) - o, "%s%04x:%u", i ? " " : "", persona_seen[i].op, persona_seen[i].n);
	}
	return SYSCTL_OUT(req, b, strlen(b) + 1);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_nfc_persona, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, sysctl_nfc_persona,
    "A", "redfin 1125: commands answered by the PN80V persona, gid<<8|oid:count (0200 = NFCEE_DISCOVER = end of init)");
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
