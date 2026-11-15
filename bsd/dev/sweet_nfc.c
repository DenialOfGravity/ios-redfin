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
 * Boot-arg sweetnfc (default 0): 0 = nothing at all is touched (no pin read, no bus, no thread).
 * sweetnfc=1, from a kernel thread 1 s after sweet_nfc_init(), never on the boot path:
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
 * versions), kern.sweet_nfc_probe (write 1: probe again; only with sweetnfc=1 and not after loader mode).
 * Log lines: "SWEET NFC: ...".
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

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern kern_return_t thread_terminate(thread_t thread);
extern bool sweet_se7_init(void);
extern int sweet_se7_xfer(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr);
void sweet_nfc_init(void);
bool sweet_nfc_tx_allowed(const uint8_t *tx, unsigned nt);

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
	if (nfc_loader || !nfc_tx_open || tx == NULL) {
		nfc_refused++;
		return false;
	}
	for (unsigned i = 0; i < sizeof(nfc_tx_ok) / sizeof(nfc_tx_ok[0]); i++) {
		if (nt == nfc_tx_ok[i].n && memcmp(tx, nfc_tx_ok[i].b, nt) == 0) {
			return true;
		}
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
	if (!sweet_se7_init()) {
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

	if (nfc_bootarg != 1 || nfc_loader) {
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
    "redfin 1115: write 1 to probe the NFC controller again (sweetnfc=1 only; never after loader mode)");

void
sweet_nfc_init(void)
{
	if (!PE_parse_boot_argn("sweetnfc", &nfc_bootarg, sizeof(nfc_bootarg)) || nfc_bootarg == 0) {
		nfc_bootarg = 0;
		return;                 /* nothing touched */
	}
	if (nfc_bootarg != 1) {
		SAY("sweetnfc=%u is not a P0 value (1): off\n", nfc_bootarg);
		return;
	}
	SAY("sweetnfc=1: read-only probe of the ST54J at SE7 0x08 in 1 s\n");
	(void)nfc_start(true);
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
