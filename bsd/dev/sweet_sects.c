/*
 * redfin (Pixel 5) touch: Samsung sec_ts (S6SY79x) on GENI SPI QUPv3 wrapper 0 SE0.
 * Call once after the scheduler is running; all work runs on one thread.
 *
 * Sources: lito-redfin-touch.dtsi (spi@880000/touchscreen@0), lito-qupv3.dtsi,
 * pinctrl-lito.c, gcc-lito.c, and Google's driver drivers/input/touchscreen/
 * sec_touch/sec_ts.c in the redbull kernel. Structure and the GENI FIFO code
 * follow flame's sweet_fts.c.
 *
 * The IC keeps its firmware in flash. Commands sent: SW_RESET (only if no
 * boot-complete event arrives), CLEAR_EVENT_STACK, SET_TOUCHFUNCTION, SENSE_ON.
 * Nothing is written to the IC's flash. Reads: events, ID, panel info, status.
 *
 * Bus framing (SPI, sec_ts_write / sec_ts_read_internal):
 *   write: AA, (n+1)>>8, (n+1), 00, 00, reg, data[n], sum    -> padded to 4 bytes
 *   read:  AA, 00, 01, n>>8, n, reg, sum (TX only, CS released), wait,
 *          then RX only: AA .. reg(5) .. [7 header bytes] data[n] sum
 * Each half is its own GENI command (TX_ONLY / RX_ONLY), so CS drops between
 * them as with Linux's two spi_sync calls. Events are read one at a time
 * (READ_ONE_EVENT), which keeps every transfer inside one FIFO fill.
 *
 * Boot-args: sectouch=1 on (default off until it has run on a phone),
 * sectouchdelay=N start after N s, sectouchlog=1 log every event,
 * sectouchirq=0 poll instead of taking the attention interrupt.
 *
 * 0945: one statistics line every 5 s while touching (always on, no change
 * to what is sent), and sectouchx=BITS (default 0 = the path as before):
 *   0x1 HID timestamp = the attention interrupt's time (Linux: sec_ts_isr
 *       ktime_get, one input frame per IRQ), not the time of dispatch; a
 *       finger's moves read in one drain go out once, at the newest position
 *   0x2 queued events in one READ_ALL_EVENT (0x61) transfer when the first
 *       event says >= 3 more wait (left_event, as sec_ts_read_event)
 *   0x4 read-only probe at start: log SET_TOUCHFUNCTION (0x30) and
 *       SET_REPORT_RATE (0x34) as the IC returns them
 *   0x8 touch thread at priority 92 instead of the kernel default 81
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern int sweet_rpmh_vreg_enable(const char *res, unsigned mv);
extern void sweet_hid_touch(int finger, int down, int x, int y);
extern void sweet_hid_touch_at(int finger, int down, int x, int y, uint64_t stamp);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter,
    integer_t priority, thread_t *new_thread);
void sweet_sects_init(void);

#define ST_SE_PA        0x00880000u     /* spi@880000, QUPv3 wrap0 SE0 */
#define ST_GCC_PA       0x00100000u     /* qcom,gcc@100000 */
#define ST_WEST_PA      0x0f500000u     /* TLMM 0xf000000 + WEST (pinctrl-lito.c) */
#define ST_EAST_PA      0x0f900000u     /* TLMM 0xf000000 + EAST */
#define ST_GCC_VOTE     0x52008u        /* APCS_CLOCK_BRANCH_ENA_VOTE_1 */
/* wrap0 m_ahb (6), s_ahb (7), core (8), core_2x (9), s0 (10) */
#define ST_GCC_BITS     (0x1fu << 6)
#define ST_RCG          0x17010u        /* gcc_qupv3_wrap0_s0_clk_src */
#define ST_GPIO_CFG(n)  ((uint32_t)(n) * 0x1000u)
#define ST_GPIO_IO(n)   (ST_GPIO_CFG(n) + 4u)
#define ST_PIN_SPI0     42              /* EAST 42-45 = qup00 (function 1) */
#define ST_PIN_SWITCH   35              /* EAST; 0 = AP owns the bus, 1 = SLPI */
#define ST_PIN_RESET    8               /* WEST; DT default output high */
#define ST_PIN_IRQ      9               /* WEST; level low */
#define ST_MAX          10u             /* MAX_SUPPORT_TOUCH_COUNT */
#define ST_PANEL_W      1080            /* SweetHID takes panel pixels */
#define ST_PANEL_H      2340

#define SE_STATUS       0x040u
#define SE_MCLK         0x048u
#define SE_FIFO_OFF     0x064u
#define SE_FW           0x068u
#define SE_CLK_SEL      0x07cu
#define SE_SPI_CPHA     0x224u
#define SE_SPI_LOOP     0x22cu
#define SE_SPI_CPOL     0x230u
#define SE_SPI_INV      0x24cu
#define SE_SPI_DEMUX    0x250u
#define SE_DMA          0x258u
#define SE_SPI_TRANS    0x25cu
#define SE_SPI_WORD     0x268u
#define SE_TX_LEN       0x26cu
#define SE_RX_LEN       0x270u
#define SE_SPI_DELAY    0x278u
#define SE_CMD          0x600u
#define SE_CTRL         0x604u
#define SE_IRQ          0x610u
#define SE_IRQ_EN       0x614u
#define SE_IRQ_CLR      0x618u
#define SE_TX           0x700u
#define SE_RX           0x780u
#define SE_TX_COUNT     0x800u
#define SE_RX_COUNT     0x804u
#define SE_TX_WM        0x80cu
#define SE_HW_TX        0xe24u
#define SE_HW_RX        0xe28u
#define SE_DONE         (1u << 0)
#define SE_RX_WM_IRQ    (1u << 26)
#define SE_RX_LAST      (1u << 27)
#define SE_TX_WM_IRQ    (1u << 30)
#define SE_ERRORS       (0x3eu | (3u << 28))
#define SE_EVENTS       (SE_DONE | SE_ERRORS | SE_RX_WM_IRQ | SE_RX_LAST | SE_TX_WM_IRQ)
#define SPI_TX_ONLY     1u
#define SPI_RX_ONLY     2u
#define SE_PACK0        0x0007f8feu
#define SE_PACK1        0x000ffefeu

/* sec_ts.h */
#define SEC_SYNC                0xaau
#define SEC_RD_HDR              7u
#define SEC_CMD_SENSE_ON        0x10u
#define SEC_CMD_SW_RESET        0x12u
#define SEC_READ_PANEL_INFO     0x23u
#define SEC_CMD_SET_TOUCHFUNC   0x30u
#define SEC_READ_ID             0x52u
#define SEC_READ_BOOT_STATUS    0x55u
#define SEC_READ_ONE_EVENT      0x60u
#define SEC_READ_ALL_EVENT      0x61u
#define SEC_CMD_SET_REPORT_RATE 0x34u
#define SEC_CMD_CLEAR_EVENTS    0x62u
#define SEC_READ_TS_STATUS      0xafu
#define SEC_STATUS_APP_MODE     0x20u
#define SEC_ACK_BOOT_COMPLETE   0x00u
#define SEC_EVT_COORD           0u
#define SEC_EVT_STATUS          1u
#define SEC_STYPE_INFO          2u
#define SEC_STYPE_VENDOR_INFO   7u
#define SEC_ACT_PRESS           1u
#define SEC_ACT_MOVE            2u
#define SEC_ACT_RELEASE         3u
#define SEC_TTYPE_PALM          5u
#define SEC_TTYPE_GRIP          10u
/* SEC_TS_DEFAULT_ENABLE_BIT_SETFUNC: touch | palm | wet */
#define SEC_SETFUNC_DEFAULT     ((1u << 0) | (1u << 5) | (1u << 6))

static struct {
	vm_offset_t se, gcc, west, east;
	bool started, dead;
	int log;
	unsigned fifo_words;
	uint32_t active;
	int max_x, max_y;
	int x[ST_MAX], y[ST_MAX];
} st;
static int st_sleep_chan;

static uint32_t
st_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
st_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
st_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&st_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static bool
st_wait_clear(vm_offset_t base, uint32_t off, uint32_t mask)
{
	for (unsigned i = 0; i < 5000; ++i) {
		if (!(st_rd(base, off) & mask)) {
			return true;
		}
		delay(1);
	}
	return false;
}

/* TLMM: [1:0] pull, [5:2] function, [8:6] drive (mA/2 - 1), [9] output. */
static void
st_gpio(vm_offset_t tile, unsigned pin, unsigned mux, unsigned pull, unsigned drive,
    bool out, bool high)
{
	uint32_t off = ST_GPIO_CFG(pin);
	if (out) {
		st_wr(tile, ST_GPIO_IO(pin),
		    (st_rd(tile, ST_GPIO_IO(pin)) & ~2u) | (high ? 2u : 0u));
	}
	uint32_t cfg = st_rd(tile, off) & ~0x3ffu;
	cfg |= (mux << 2) | pull | (drive << 6) | (out ? 0x200u : 0u);
	st_wr(tile, off, cfg);
}

static bool
st_irq_asserted(void)
{
	return !(st_rd(st.west, ST_GPIO_IO(ST_PIN_IRQ)) & 1u);
}

/*
 * 0911: the attention line as an interrupt. TLMM GPIO 9 (level low) feeds the
 * TLMM summary interrupt, GIC SPI 208 = INTID 240 (live DT pinctrl@f000000
 * interrupts = <0 0xd0 4>). Per-pin INTR_CFG (+0x8): bit 0 enable, 1 polarity
 * (0 = low), 3:2 detection (0 = level), 4 raw status, 7:5 target (3 = apps);
 * INTR_STATUS (+0xc) bit 0, cleared by writing 0 (pinctrl-lito.c PINGROUP).
 * The handler masks the pin and wakes the thread, which drains the events and
 * unmasks it. If the line is seen low without an interrupt arriving (summary
 * not delivered, e.g. routed elsewhere), or the summary storms for another
 * pin, the driver goes back to 8 ms polling. sectouchirq=0 keeps polling.
 */
#define ST_IRQ_INTID            240u
#define ST_INTR_CFG(n)          (ST_GPIO_CFG(n) + 0x8u)
#define ST_INTR_STATUS(n)       (ST_GPIO_CFG(n) + 0xcu)
#define ST_INTR_CFG_ON          ((3u << 5) | (1u << 4))        /* apps, raw status, level low; | 1 enables */
extern bool sweet_spi_attach(uint32_t intid, bool edge, void (*fn)(void *arg), void *arg);
extern void sweet_spi_mask(uint32_t intid, bool masked);
extern bool sweet_fpc_tlmm_irq(void);  /* 1190: bsd/dev/sweet_fpc.c */
static volatile bool st_irq_on;
static volatile uint32_t st_irqs, st_irq_others;
static volatile uint64_t st_irq_t;      /* 0945: mach time of the last attention interrupt */

static void
st_irq(__unused void *arg)
{
	if (st_rd(st.west, ST_INTR_STATUS(ST_PIN_IRQ)) & 1u) {
		st_wr(st.west, ST_INTR_CFG(ST_PIN_IRQ), ST_INTR_CFG_ON); /* masked until the thread has read */
		st_wr(st.west, ST_INTR_STATUS(ST_PIN_IRQ), 0);
		st_irq_t = mach_absolute_time();
		st_irqs++;
		thread_wakeup(&st_sleep_chan);
	} else if (sweet_fpc_tlmm_irq()) {
		/* 1190: the fingerprint sensor's gpio112 shares the TLMM summary interrupt */
	} else if (++st_irq_others > 1000) {
		sweet_spi_mask(ST_IRQ_INTID, true);     /* another pin holds the summary line */
		st_irq_on = false;
		thread_wakeup(&st_sleep_chan);
	}
}

/* Unmask the attention interrupt and sleep until it fires or ms pass. */
static void
st_irq_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&st_sleep_chan, THREAD_UNINT, deadline);
	st_wr(st.west, ST_INTR_CFG(ST_PIN_IRQ), ST_INTR_CFG_ON | 1u);  /* after assert_wait: no lost wakeup */
	thread_block(THREAD_CONTINUE_NULL);
}

static bool
st_clocks(void)
{
	st_wr(st.gcc, ST_GCC_VOTE, st_rd(st.gcc, ST_GCC_VOTE) | ST_GCC_BITS);
	uint32_t cmd = st_rd(st.gcc, ST_RCG), cfg = st_rd(st.gcc, ST_RCG + 4u);
	uint32_t dfs = st_rd(st.gcc, ST_RCG + 0x14u);
	printf("SWEET SECTS: gcc vote=%#x rcg=%#x cfg=%#x dfs=%#x\n",
	    st_rd(st.gcc, ST_GCC_VOTE), cmd, cfg, dfs);
	if (dfs & 1u) {
		/* CLK_SEL 0 picks DFS level 0; it must be XO (19.2 MHz) undivided. */
		cfg = st_rd(st.gcc, ST_RCG + 0x1cu);
		printf("SWEET SECTS: DFS level0 cfg=%#x\n", cfg);
		if ((cfg & 0x371fu) != 1u) {
			printf("SWEET SECTS: DFS level 0 is not XO/1; stopping\n");
			return false;
		}
	} else {
		cfg = (cfg & ~(0x371fu | (1u << 20))) | 1u;
		st_wr(st.gcc, ST_RCG + 4u, cfg);
		st_wr(st.gcc, ST_RCG, st_rd(st.gcc, ST_RCG) | 1u);
		if (!st_wait_clear(st.gcc, ST_RCG, 1u)) {
			printf("SWEET SECTS: RCG update timeout\n");
			return false;
		}
	}
	/* Branch halt bits: wrap0 m_ahb, s_ahb, s0 (gcc-lito.c halt_reg). */
	const uint32_t branches[] = {0x17004, 0x17008, 0x1700c};
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); ++i) {
		uint32_t cbcr = st_rd(st.gcc, branches[i]);
		printf("SWEET SECTS: GCC branch %#x = %#x\n", branches[i], cbcr);
		if (cbcr & 2u) {
			continue;       /* hardware clock gating: reads halted while idle */
		}
		if (!st_wait_clear(st.gcc, branches[i], 1u << 31)) {
			printf("SWEET SECTS: GCC branch %#x halted (%#x)\n", branches[i],
			    st_rd(st.gcc, branches[i]));
			return false;
		}
	}
	return true;
}

static bool
st_se_init(void)
{
	uint32_t fw = st_rd(st.se, SE_FW);
	uint32_t tx = st_rd(st.se, SE_HW_TX), rx = st_rd(st.se, SE_HW_RX);
	printf("SWEET SECTS: se0 fw=%#x proto=%u status=%#x fifo-off=%#x tx=%#x rx=%#x\n",
	    fw, (fw >> 8) & 0xffu, st_rd(st.se, SE_STATUS), st_rd(st.se, SE_FIFO_OFF), tx, rx);
	/* Protocol 1 = SPI; 32-bit FIFO words. */
	if (((fw >> 8) & 0xffu) != 1u || (st_rd(st.se, SE_FIFO_OFF) & 1u) ||
	    ((tx >> 24) & 0x3fu) != 32u || ((rx >> 24) & 0x3fu) != 32u ||
	    ((tx >> 16) & 0x3fu) < 4u || ((rx >> 16) & 0x3fu) < 4u) {
		printf("SWEET SECTS: need XBL/ABL-loaded SPI firmware and 32-bit FIFOs; stopping\n");
		return false;
	}
	if (st_rd(st.se, SE_STATUS) & 0x1001u) {
		printf("SWEET SECTS: SE0 already busy; stopping\n");
		return false;
	}
	st.fifo_words = (tx >> 16) & 0x3fu;
	if (((rx >> 16) & 0x3fu) < st.fifo_words) {
		st.fifo_words = (rx >> 16) & 0x3fu;
	}
	/* geni_se_init + FIFO mode, polled (no GSI, no IRQ routing). */
	st_wr(st.se, 0xe18, 0);
	st_wr(st.se, 0xe1c, 0);
	st_wr(st.se, 0x644, 0);
	st_wr(st.se, SE_IRQ_CLR, 0xffffffffu);
	st_wr(st.se, 0x648, 0xffffffffu);
	st_wr(st.se, SE_DMA, 0);
	st_wr(st.se, 0x28, st_rd(st.se, 0x28) | 0x7fu);
	st_wr(st.se, 0xe30, st_rd(st.se, 0xe30) | 0x0fu);
	st_wr(st.se, 0x24, 0x7f);
	st_wr(st.se, 0x20, 1);
	/* SPI mode 3 (DT spi-cpol, spi-cpha), CS0, no loopback; 19.2 MHz / 4 = 4.8 MHz (DT max 10). */
	st_wr(st.se, SE_SPI_LOOP, st_rd(st.se, SE_SPI_LOOP) & ~3u);
	st_wr(st.se, SE_SPI_DEMUX, 0);
	st_wr(st.se, SE_SPI_CPHA, st_rd(st.se, SE_SPI_CPHA) | 1u);
	st_wr(st.se, SE_SPI_CPOL, st_rd(st.se, SE_SPI_CPOL) | 4u);
	st_wr(st.se, SE_SPI_INV, 0);
	st_wr(st.se, SE_CLK_SEL, 0);
	st_wr(st.se, SE_MCLK, (4u << 4) | 1u);
	st_wr(st.se, SE_SPI_DELAY, 0);
	st_wr(st.se, SE_SPI_WORD, (st_rd(st.se, SE_SPI_WORD) & ~0x3ffu) | (8u - 4u));
	st_wr(st.se, SE_SPI_TRANS, st_rd(st.se, SE_SPI_TRANS) & ~1u); /* no CS toggle per word */
	st_wr(st.se, 0x254, 0);
	st_wr(st.se, 0x260, SE_PACK0);
	st_wr(st.se, 0x264, SE_PACK1);
	st_wr(st.se, 0x284, SE_PACK0);
	st_wr(st.se, 0x288, SE_PACK1);
	st_wr(st.se, SE_TX_WM, 0);
	st_wr(st.se, 0x810, 0);
	st_wr(st.se, 0x814, ((rx >> 16) & 0x3fu) - 2u);
	st_wr(st.se, SE_IRQ_EN, SE_EVENTS);
	if ((st_rd(st.se, SE_TX_COUNT) & 0xfffffffu) ||
	    (st_rd(st.se, SE_RX_COUNT) & 0x1ffffffu)) {
		printf("SWEET SECTS: inherited FIFO data; stopping\n");
		return false;
	}
	return true;
}

static void
st_abort(void)
{
	st_wr(st.se, SE_CTRL, 2);
	bool done = false;
	for (unsigned i = 0; i < 5000; ++i) {
		if (st_rd(st.se, SE_IRQ) & (1u << 5)) {
			done = true;
			break;
		}
		delay(1);
	}
	for (unsigned i = 0; i < 64 && (st_rd(st.se, SE_RX_COUNT) & 0x1ffffffu); ++i) {
		(void)st_rd(st.se, SE_RX);
	}
	st_wr(st.se, SE_IRQ_CLR, 0xffffffffu);
	if (!done || !st_wait_clear(st.se, SE_STATUS, 1u)) {
		st.dead = true;
		printf("SWEET SECTS: abort failed status=%#x irq=%#x; stopping\n",
		    st_rd(st.se, SE_STATUS), st_rd(st.se, SE_IRQ));
	}
}

/*
 * One half-duplex GENI command: TX_ONLY sends buf[0..len), RX_ONLY clocks
 * len bytes into buf. CS is asserted for the command and released after it.
 * Bounded by one FIFO fill (len <= fifo_words * 4).
 */
static int
st_xfer(unsigned op, uint8_t *buf, unsigned len)
{
	unsigned words = (len + 3u) / 4u, sent = 0, got = 0;
	bool tx = op == SPI_TX_ONLY;
	if (st.dead || len == 0 || words > st.fifo_words) {
		return EIO;
	}
	if (!st_wait_clear(st.se, SE_STATUS, 1u)) {
		st.dead = true;
		return EBUSY;
	}
	st_wr(st.se, SE_IRQ_CLR, 0xffffffffu);
	st_wr(st.se, SE_TX_LEN, tx ? len : 0);
	st_wr(st.se, SE_RX_LEN, tx ? 0 : len);
	st_wr(st.se, SE_TX_WM, tx ? 1 : 0);
	st_wr(st.se, SE_CMD, op << 27);
	for (unsigned i = 0; i < 20000; ++i) {
		uint32_t irq = st_rd(st.se, SE_IRQ);
		if (irq & SE_ERRORS) {
			printf("SWEET SECTS: xfer error op=%u irq=%#x\n", op, irq);
			break;
		}
		if (tx && !sent && (irq & SE_TX_WM_IRQ)) {
			for (unsigned w = 0; w < words; ++w) {
				uint32_t word = 0;
				for (unsigned j = 0; j < 4 && w * 4u + j < len; ++j) {
					word |= (uint32_t)buf[w * 4u + j] << (8u * j);
				}
				st_wr(st.se, SE_TX, word);
			}
			sent = len;
			st_wr(st.se, SE_TX_WM, 0);
		}
		if (!tx) {
			uint32_t count = st_rd(st.se, SE_RX_COUNT) & 0x1ffffffu;
			while (count-- && got < len) {
				uint32_t word = st_rd(st.se, SE_RX);
				for (unsigned j = 0; j < 4 && got < len; ++j) {
					buf[got++] = (uint8_t)(word >> (8u * j));
				}
			}
		}
		if (irq & SE_DONE) {
			st_wr(st.se, SE_IRQ_CLR, irq);
			if (tx ? sent == len : got == len) {
				return 0;
			}
			if (!tx) {
				continue; /* final RX words may land after DONE */
			}
			break;
		}
		st_wr(st.se, SE_IRQ_CLR, irq & (SE_RX_WM_IRQ | SE_RX_LAST | SE_TX_WM_IRQ));
		delay(1);
	}
	printf("SWEET SECTS: xfer op=%u len=%u sent=%u got=%u irq=%#x status=%#x\n", op, len,
	    sent, got, st_rd(st.se, SE_IRQ), st_rd(st.se, SE_STATUS));
	st_abort();
	return EIO;
}

static uint8_t
st_sum(const uint8_t *p, unsigned n)
{
	uint8_t s = 0;
	while (n--) {
		s += *p++;
	}
	return s;
}

/* sec_ts_write over SPI: header, register, data, checksum, zero-padded to 4. */
static int
st_write(uint8_t reg, const uint8_t *data, unsigned n)
{
	uint8_t buf[32] = {0};
	if (6u + n + 1u + 3u > sizeof(buf)) {
		return EINVAL;
	}
	buf[0] = SEC_SYNC;
	buf[1] = (uint8_t)((n + 1u) >> 8);
	buf[2] = (uint8_t)(n + 1u);
	buf[5] = reg;
	for (unsigned i = 0; i < n; ++i) {
		buf[6 + i] = data[i];
	}
	unsigned len = 6u + n;
	buf[len] = st_sum(buf, len);
	len = (len + 1u + 3u) & ~3u;
	int err = st_xfer(SPI_TX_ONLY, buf, len);
	delay(100);
	return err;
}

/* sec_ts_read_internal over SPI (len <= io_burstmax path), with its checks. */
static int
st_read(uint8_t reg, uint8_t *out, unsigned n)
{
	uint8_t hdr[8] = {SEC_SYNC, 0x00, 0x01, (uint8_t)(n >> 8), (uint8_t)n, reg, 0, 0};
	uint8_t rx[64];
	unsigned rlen = (SEC_RD_HDR + n + 1u + 3u) & ~3u;
	if (rlen > sizeof(rx)) {
		return EINVAL;
	}
	hdr[6] = st_sum(hdr, 6);
	for (unsigned attempt = 0; attempt < 3; ++attempt) {
		int err = st_xfer(SPI_TX_ONLY, hdr, sizeof(hdr));
		if (err) {
			return err;
		}
		delay(reg == SEC_READ_ALL_EVENT ? 500 : 100);     /* sec_ts_spi_delay */
		memset(rx, 0, sizeof(rx));
		err = st_xfer(SPI_RX_ONLY, rx, rlen);
		if (err) {
			return err;
		}
		if (rx[0] == SEC_SYNC && rx[5] == reg &&
		    st_sum(rx, SEC_RD_HDR + n) == rx[SEC_RD_HDR + n]) {
			memcpy(out, rx + SEC_RD_HDR, n);
			return 0;
		}
		if (st.log || attempt == 2) {
			printf("SWEET SECTS: read %#x bad frame %02x %02x %02x %02x %02x %02x %02x\n",
			    reg, rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6]);
		}
		delay(1000);
	}
	return EIO;
}

static void
st_log_event(const char *what, const uint8_t ev[8])
{
	printf("SWEET SECTS: %s %02x %02x %02x %02x %02x %02x %02x %02x\n", what,
	    ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], ev[6], ev[7]);
}

/* sec_ts_wait_for_ready: an INFO or VENDOR_INFO status event carrying ack. */
static int
st_wait_ready(uint8_t ack, unsigned tries)
{
	uint8_t ev[8];
	for (unsigned i = 0; i < tries && !st.dead; ++i) {
		if (!st_read(SEC_READ_ONE_EVENT, ev, sizeof(ev)) && ev[0]) {
			st_log_event("boot", ev);
			unsigned stype = (ev[0] >> 2) & 0xfu;
			if ((stype == SEC_STYPE_INFO || stype == SEC_STYPE_VENDOR_INFO) && ev[1] == ack) {
				return 0;
			}
		}
		st_wait(20);
	}
	return ETIMEDOUT;
}

/*
 * 0945 part A: measurement (always on). Sample time = the attention
 * interrupt that started the drain (the drain start when polled). Per 5 s
 * window: events read, coordinate moves, moves of a finger beyond its first
 * in one drain (batched), dt between a finger's samples in consecutive
 * drains, dt > 20 ms where the finger moved >= 8 px (stalls), interrupt ->
 * sweet_hid_touch latency, interrupt -> thread running (wake), events and
 * time per drain.
 */
#define STX_IRQ_STAMP   0x1u
#define STX_READ_ALL    0x2u
#define STX_PROBE       0x4u
#define STX_PRIO        0x8u
#define ST_STAT_N       1024u
#define ST_GAP_US       20000u
#define ST_GAP_PX       8
static uint32_t st_x;
static struct {
	uint64_t t;             /* this drain's sample time */
	bool irq;               /* started by an interrupt */
	uint32_t seen;          /* fingers with a sample in this drain */
	uint32_t pend;          /* sectouchx & 1: moves held to the end of the drain */
	int px[ST_MAX], py[ST_MAX];
} std_;
static struct {
	uint64_t start;
	uint32_t events, moves, batched, gaps, drains, drain_ev_max, drain_us_max;
	uint64_t drain_ev_sum, dt_sum;
	uint32_t n_dt, n_lat, n_wake, n_rate;
	uint32_t dt[ST_STAT_N], lat[ST_STAT_N], wake[ST_STAT_N];
	uint64_t last_t[ST_MAX];
	int last_x[ST_MAX], last_y[ST_MAX];
} sts;

static uint32_t
st_us(uint64_t abs)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(abs, &ns);
	return ns / 1000u > 0xffffffffull ? 0xffffffffu : (uint32_t)(ns / 1000u);
}

static void
st_put(uint32_t *a, uint32_t *n, uint32_t v)
{
	if (*n < ST_STAT_N) {
		a[(*n)++] = v;
	}
}

static uint32_t
st_pct(uint32_t *a, uint32_t n, unsigned pct)
{
	if (n == 0) {
		return 0;
	}
	for (uint32_t i = 1; i < n; ++i) {      /* insertion sort, n <= 1024, once per 5 s */
		uint32_t v = a[i], j = i;
		while (j > 0 && a[j - 1] > v) {
			a[j] = a[j - 1];
			--j;
		}
		a[j] = v;
	}
	return a[(n - 1u) * pct / 100u];
}

#define ST_MS(v) (v) / 1000u, ((v) % 1000u) / 100u

static void
st_stat_report(uint64_t now)
{
	uint64_t win;
	if (!sts.start) {
		return;
	}
	nanoseconds_to_absolutetime(5ull * NSEC_PER_SEC, &win);
	if (now - sts.start < win) {
		return;
	}
	uint32_t d50 = st_pct(sts.dt, sts.n_dt, 50), d95 = st_pct(sts.dt, sts.n_dt, 95);
	uint32_t dmax = sts.n_dt ? sts.dt[sts.n_dt - 1] : 0;
	uint32_t l50 = st_pct(sts.lat, sts.n_lat, 50), l95 = st_pct(sts.lat, sts.n_lat, 95);
	uint32_t w50 = st_pct(sts.wake, sts.n_wake, 50), w95 = st_pct(sts.wake, sts.n_wake, 95);
	uint32_t rate = sts.dt_sum ? (uint32_t)((uint64_t)sts.n_rate * 1000000u / sts.dt_sum) : 0;
	uint32_t avg10 = sts.drains ? (uint32_t)(sts.drain_ev_sum * 10u / sts.drains) : 0;
	printf("SWEET SECTS: 5s: events %u (moves %u, batched %u) rate %u/s | dt p50/p95/max %u.%u/%u.%u/%u.%u ms"
	    " | gaps>20ms %u | irq->dispatch p50/p95 %u/%u us | wake p50/p95 %u/%u us"
	    " | per drain avg %u.%u max %u (%u us max) | x %#x\n",
	    sts.events, sts.moves, sts.batched, rate, ST_MS(d50), ST_MS(d95), ST_MS(dmax),
	    sts.gaps, l50, l95, w50, w95, avg10 / 10u, avg10 % 10u, sts.drain_ev_max,
	    sts.drain_us_max, st_x);
	sts.start = 0;
	sts.events = sts.moves = sts.batched = sts.gaps = sts.drains = 0;
	sts.drain_ev_max = sts.drain_us_max = 0;
	sts.drain_ev_sum = sts.dt_sum = 0;
	sts.n_dt = sts.n_lat = sts.n_wake = sts.n_rate = 0;
}

/* A coordinate sample of finger id (press or move) at panel x, y. */
static void
st_stat_move(unsigned id, int x, int y, bool press)
{
	uint32_t bit = 1u << id;
	sts.moves++;
	if (std_.seen & bit) {
		sts.batched++;          /* more than one sample of this finger in one drain */
	} else {
		std_.seen |= bit;
		if (!press && sts.last_t[id]) {
			uint32_t dt = st_us(std_.t - sts.last_t[id]);
			int d = (x > sts.last_x[id] ? x - sts.last_x[id] : sts.last_x[id] - x) +
			    (y > sts.last_y[id] ? y - sts.last_y[id] : sts.last_y[id] - y);
			st_put(sts.dt, &sts.n_dt, dt);
			if (dt < 100000u) {
				sts.dt_sum += dt;
				sts.n_rate++;
			}
			if (dt > ST_GAP_US && d >= ST_GAP_PX) {
				sts.gaps++;
			}
		}
		sts.last_t[id] = std_.t;
	}
	sts.last_x[id] = x;
	sts.last_y[id] = y;
}

/* To SweetHID; sectouchx & 1 stamps it with the sample (interrupt) time. */
static void
st_emit(unsigned id, int down, int x, int y)
{
	if (down && std_.irq) {
		st_put(sts.lat, &sts.n_lat, st_us(mach_absolute_time() - std_.t));
	}
	if (st_x & STX_IRQ_STAMP) {
		sweet_hid_touch_at((int)id, down, x, y, std_.t);
	} else {
		sweet_hid_touch((int)id, down, x, y);
	}
}

/* sectouchx & 1: the held moves, one per finger, at the end of a drain. */
static void
st_flush(void)
{
	for (unsigned id = 0; std_.pend && id < ST_MAX; ++id) {
		if (std_.pend & (1u << id)) {
			std_.pend &= ~(1u << id);
			st_emit(id, 1, std_.px[id], std_.py[id]);
		}
	}
}

static void
st_handle(const uint8_t ev[8])
{
	unsigned eid = ev[0] & 3u;
	if (st.log) {
		st_log_event("event", ev);
	}
	if (eid != SEC_EVT_COORD) {
		if (eid == SEC_EVT_STATUS && ((ev[0] >> 2) & 0xfu) && !st.log) {
			st_log_event("status", ev);
		}
		return;
	}
	/* struct sec_ts_event_coordinate (little-endian bitfields) */
	unsigned tid = (ev[0] >> 2) & 0xfu, action = ev[0] >> 6;
	int x = (ev[1] << 4) | (ev[3] >> 4);
	int y = (ev[2] << 4) | (ev[3] & 0x0f);
	unsigned ttype = ((ev[6] >> 6) << 2) | (ev[7] >> 6);
	if (tid == 0 || tid > ST_MAX) {
		return;
	}
	unsigned id = tid - 1u;
	/* IC coordinates -> panel pixels (the IC reports its own resolution). */
	x = st.max_x > 0 ? x * ST_PANEL_W / (st.max_x + 1) : x;
	y = st.max_y > 0 ? y * ST_PANEL_H / (st.max_y + 1) : y;
	bool palm = ttype == SEC_TTYPE_PALM || ttype == SEC_TTYPE_GRIP;
	if ((action == SEC_ACT_PRESS || action == SEC_ACT_MOVE) && !palm) {
		bool press = !(st.active & (1u << id));
		st.active |= 1u << id;
		st.x[id] = x;
		st.y[id] = y;
		st_stat_move(id, x, y, press);
		if ((st_x & STX_IRQ_STAMP) && !press) {
			std_.pend |= 1u << id;  /* newest position wins; sent at the end of the drain */
			std_.px[id] = x;
			std_.py[id] = y;
		} else {
			st_emit(id, 1, x, y);
		}
	} else if (action == SEC_ACT_RELEASE || palm) {
		if (st.active & (1u << id)) {
			st.active &= ~(1u << id);
			if (std_.pend & (1u << id)) {
				std_.pend &= ~(1u << id);
				st_emit(id, 1, std_.px[id], std_.py[id]);
			}
			st_emit(id, 0, st.x[id], st.y[id]);
		}
		sts.last_t[id] = 0;
	}
}

static void
st_release_all(void)
{
	st_flush();
	for (unsigned id = 0; id < ST_MAX; ++id) {
		if (st.active & (1u << id)) {
			sweet_hid_touch((int)id, 0, st.x[id], st.y[id]);
		}
	}
	st.active = 0;
}

static void
st_thread(void *arg, wait_result_t result)
{
	unsigned delay_s = 20;
	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("sectouchdelay", &delay_s, sizeof(delay_s));
	printf("SWEET SECTS: starting in %u s\n", delay_s);
	st_wait(delay_s * 1000u);
	st.gcc = ml_io_map(ST_GCC_PA, 0x60000);
	st.west = ml_io_map(ST_WEST_PA, ST_GPIO_CFG(ST_PIN_IRQ + 1));
	st.east = ml_io_map(ST_EAST_PA, ST_GPIO_CFG(ST_PIN_SPI0 + 4));
	st.se = ml_io_map(ST_SE_PA, 0x4000);
	if (!st.gcc || !st.west || !st.east || !st.se) {
		printf("SWEET SECTS: MMIO mapping failed\n");
		goto out;
	}
	printf("SWEET SECTS: TLMM cfg/io spi0=%#x/%#x irq=%#x/%#x switch=%#x/%#x reset=%#x/%#x\n",
	    st_rd(st.east, ST_GPIO_CFG(ST_PIN_SPI0)), st_rd(st.east, ST_GPIO_IO(ST_PIN_SPI0)),
	    st_rd(st.west, ST_GPIO_CFG(ST_PIN_IRQ)), st_rd(st.west, ST_GPIO_IO(ST_PIN_IRQ)),
	    st_rd(st.east, ST_GPIO_CFG(ST_PIN_SWITCH)), st_rd(st.east, ST_GPIO_IO(ST_PIN_SWITCH)),
	    st_rd(st.west, ST_GPIO_CFG(ST_PIN_RESET)), st_rd(st.west, ST_GPIO_IO(ST_PIN_RESET)));
	/* DT pinctrl default: reset out high, irq in, bus switch out low (AP). */
	st_gpio(st.west, ST_PIN_RESET, 0, 0, 0, true, true);
	st_gpio(st.west, ST_PIN_IRQ, 0, 0, 0, false, false);
	st_gpio(st.east, ST_PIN_SWITCH, 0, 0, 0, true, false);
	/* AVDD: pm8150_l17 at 3.304 V (Linux keeps it always-on; vote it anyway). */
	int err = sweet_rpmh_vreg_enable("ldoa17", 3304);
	if (err) {
		printf("SWEET SECTS: L17A vote failed %d (continuing: ABL may have left it on)\n", err);
	}
	st_wait(70);
	if (!st_clocks()) {
		goto out;
	}
	for (unsigned pin = ST_PIN_SPI0; pin < ST_PIN_SPI0 + 4; ++pin) {
		st_gpio(st.east, pin, 1, 0, 2, false, false); /* qup00, 6 mA, no pull */
	}
	if (!st_se_init()) {
		goto out;
	}
	st_wait(10);
	/* sec_ts_probe: wait for BOOT_COMPLETE; if ABL already took it, soft reset. */
	err = st_wait_ready(SEC_ACK_BOOT_COMPLETE, 15);
	if (err && !st.dead) {
		err = st_write(SEC_CMD_SW_RESET, NULL, 0);
		printf("SWEET SECTS: no boot event; SW_RESET err=%d\n", err);
		st_wait(30);
		err = st_wait_ready(SEC_ACK_BOOT_COMPLETE, 100);
	}
	printf("SWEET SECTS: ready err=%d irq-pin=%u\n", err,
	    st_rd(st.west, ST_GPIO_IO(ST_PIN_IRQ)) & 1u);
	uint8_t id[3] = {0}, info[11] = {0}, boot = 0, status[4] = {0};
	int ierr = st_read(SEC_READ_ID, id, sizeof(id));
	printf("SWEET SECTS: id err=%d %02x %02x %02x\n", ierr, id[0], id[1], id[2]);
	if (ierr || st.dead) {
		goto out;
	}
	if (!st_read(SEC_READ_PANEL_INFO, info, sizeof(info))) {
		st.max_x = ((info[0] << 8) | info[1]) - 1;
		st.max_y = ((info[2] << 8) | info[3]) - 1;
	}
	(void)st_read(SEC_READ_BOOT_STATUS, &boot, 1);
	(void)st_read(SEC_READ_TS_STATUS, status, sizeof(status));
	printf("SWEET SECTS: res %dx%d tx %u rx %u boot %#x status %02x %02x %02x %02x\n",
	    st.max_x + 1, st.max_y + 1, info[8], info[9], boot, status[0], status[1],
	    status[2], status[3]);
	if (boot != SEC_STATUS_APP_MODE) {
		printf("SWEET SECTS: IC not in app mode (firmware needs Linux's updater); stopping\n");
		goto out;
	}
	(void)st_write(SEC_CMD_CLEAR_EVENTS, NULL, 0);
	const uint8_t func[2] = {SEC_SETFUNC_DEFAULT & 0xffu, SEC_SETFUNC_DEFAULT >> 8};
	err = st_write(SEC_CMD_SET_TOUCHFUNC, func, sizeof(func));
	if (!err) {
		err = st_write(SEC_CMD_SENSE_ON, NULL, 0);
	}
	if (err) {
		printf("SWEET SECTS: sense on err=%d\n", err);
		goto out;
	}
	if (st_x & STX_PROBE) {
		uint8_t fn[2] = {0}, rr[1] = {0};
		int e1 = st_read(SEC_CMD_SET_TOUCHFUNC, fn, sizeof(fn));
		int e2 = st_read(SEC_CMD_SET_REPORT_RATE, rr, sizeof(rr));
		printf("SWEET SECTS: probe touchfunction err=%d %02x %02x report-rate err=%d %02x\n",
		    e1, fn[0], fn[1], e2, rr[0]);
	}
	unsigned all_max = (st.fifo_words * 4u - SEC_RD_HDR - 1u) / 8u;
	if (all_max > 7u) {
		all_max = 7u;           /* st_read's 64-byte frame */
	}
	int irq = 1;
	(void)PE_parse_boot_argn("sectouchirq", &irq, sizeof(irq));
	if (irq) {
		st_wr(st.west, ST_INTR_CFG(ST_PIN_IRQ), ST_INTR_CFG_ON);
		st_wr(st.west, ST_INTR_STATUS(ST_PIN_IRQ), 0);
		st_irq_on = sweet_spi_attach(ST_IRQ_INTID, false, st_irq, NULL);
	}
	printf("SWEET SECTS: sense on; %s\n", st_irq_on ? "interrupt (TLMM gpio9, INTID 240)" : "polling every 8 ms");
	unsigned errors = 0, events = 0, ticks = 0, missed = 0;
	uint32_t seen = 0;
	while (!st.dead) {
		uint8_t ev[8];
		/* The IRQ line (level low) says events are queued; when polling also
		 * read every ~100 ms in case the line does not work as assumed. */
		bool poll = st_irq_on || (++ticks % 12u) == 0;
		uint64_t t0 = mach_absolute_time(), irq_t = st_irq_t;
		std_.irq = st_irq_on && st_irqs != seen && irq_t && irq_t <= t0;
		std_.t = std_.irq ? irq_t : t0;
		std_.seen = 0;
		unsigned drained = 0;
		for (unsigned n = 0; n < 32 && (poll || st_irq_asserted()) && !st.dead; ++n) {
			poll = false;
			if (st_read(SEC_READ_ONE_EVENT, ev, sizeof(ev))) {
				if ((errors++ % 100u) == 0) {
					printf("SWEET SECTS: read error (%u)\n", errors);
				}
				break;
			}
			if (ev[0] == 0) {
				break;  /* event buffer empty */
			}
			if (events++ < 32 && !st.log) {
				st_log_event("first", ev);
			}
			if (!sts.start) {
				sts.start = t0;
			}
			if (drained++ == 0 && std_.irq) {
				st_put(sts.wake, &sts.n_wake, st_us(t0 - irq_t));
			}
			sts.events++;
			st_handle(ev);
			/* sectouchx & 2: the rest of the queue in one transfer (sec_ts_read_event). */
			unsigned left = ev[7] & 0x3fu;
			if ((st_x & STX_READ_ALL) && left >= 3u && all_max >= 3u) {
				uint8_t all[7 * 8];
				unsigned k = left < all_max ? left : all_max;
				if (st_read(SEC_READ_ALL_EVENT, all, k * 8u) == 0) {
					for (unsigned i = 0; i < k && all[i * 8u]; ++i) {
						sts.events++;
						drained++;
						st_handle(&all[i * 8u]);
					}
				}
			}
		}
		st_flush();
		if (drained) {
			uint32_t us = st_us(mach_absolute_time() - t0);
			sts.drains++;
			sts.drain_ev_sum += drained;
			sts.drain_ev_max = drained > sts.drain_ev_max ? drained : sts.drain_ev_max;
			sts.drain_us_max = us > sts.drain_us_max ? us : sts.drain_us_max;
		}
		st_stat_report(mach_absolute_time());
		if (!st_irq_on) {
			if (irq && st_irq_others > 1000 && st_irq_others < 2000) {
				st_irq_others = 2000;
				printf("SWEET SECTS: summary interrupt held by another pin (%u ours): polling\n", st_irqs);
			}
			st_wait(8);
			continue;
		}
		seen = st_irqs;
		st_irq_wait(500);
		/* Timed out with the line low and no interrupt: it is not reaching us. */
		if (st_irq_on && st_irqs == seen && st_irq_asserted() && ++missed >= 3) {
			sweet_spi_mask(ST_IRQ_INTID, true);
			st_irq_on = false;
			printf("SWEET SECTS: attention low but no interrupt (%u taken): polling every 8 ms\n", st_irqs);
		}
		if (st_irqs != seen) {
			missed = 0;
		}
	}
	if (st_irq_on) {
		sweet_spi_mask(ST_IRQ_INTID, true);
		st_wr(st.west, ST_INTR_CFG(ST_PIN_IRQ), ST_INTR_CFG_ON);
	}
	st_release_all();
out:
	printf("SWEET SECTS: thread exits\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
sweet_sects_init(void)
{
	int enabled = 0;
	thread_t thread;
	if (st.started) {
		return;
	}
	st.started = true;
	(void)PE_parse_boot_argn("sectouch", &enabled, sizeof(enabled));
	(void)PE_parse_boot_argn("sectouchlog", &st.log, sizeof(st.log));
	(void)PE_parse_boot_argn("sectouchx", &st_x, sizeof(st_x));
	if (!enabled) {
		printf("SWEET SECTS: disabled (sectouch=1 enables the redfin sec_ts driver)\n");
		return;
	}
	printf("SWEET SECTS: sectouchx %#x\n", st_x);
	kern_return_t kr = (st_x & STX_PRIO) ?
	    kernel_thread_start_priority(st_thread, NULL, 92, &thread) :
	    kernel_thread_start(st_thread, NULL, &thread);
	if (kr == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("SWEET SECTS: kernel_thread_start failed %d\n", kr);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
