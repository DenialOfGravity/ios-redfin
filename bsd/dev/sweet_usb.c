/*
 * SWEET: USB bring-up, step 1 - read-only state probe.
 *
 * The phone's USB is a Synopsys DWC3 (USB 2.0 only, maximum-speed
 * high-speed) at 0x0a600000 behind Qualcomm's dwc-usb3-msm wrapper, with a
 * QUSB2 PHY at 0x088e2000; its power domain is the USB30_PRIM GDSC and its
 * clocks come from GCC (offsets from Linux gcc-sdmmagpie.c). ABL uses this
 * controller for fastboot; this probe only reports what state it is left
 * in after `fastboot boot`. It never writes a register, and it touches the
 * DWC3 / PHY register blocks only when the GCC status registers say their
 * power domain and clocks are on (an access to a gated block would hang).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <kern/locks.h>
#include <sys/kpi_mbuf.h>
#include <sys/kpi_socket.h>
#include <sys/sockio.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/if_ether.h>
#include <net/ether_if_module.h>
#include <net/kpi_interface.h>
#include <net/kpi_protocol.h>
#include <netinet/in.h>
#include <net/if_media.h>
#include <net/dlil.h>
#include <sys/kern_event.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
void sweet_usb_probe(void);
static void sweet_usb_gadget_start(vm_offset_t dwc);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);

#define GCC_PA                  0x00100000
#define GCC_SIZE                0x00090000
#define GCC_USB30_PRIM_GDSCR    0x0f004         /* bit 31: power on */
#define CBCR_CLK_OFF            (1u << 31)

static const struct {
	const char *name;
	uint32_t    off;
} usb_cbcr[] = {
	{ "master", 0x0f010 },
	{ "sleep", 0x0f014 },
	{ "utmi", 0x0f018 },
	{ "cfg-noc-axi", 0x0502c },
	{ "aggre-axi", 0x8201c },
	{ "ahb2phy", 0x6a004 },
	{ "clkref", 0x8c010 },
};

#define DWC3_PA                 0x0a600000
#define DWC3_SIZE               0x0000d000
#define QUSB2_PA                0x088e2000
#define QUSB2_SIZE              0x00000400

static uint32_t
usb_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

void
sweet_usb_probe(void)
{
	int enabled = 1;
	uint32_t gdscr, cbcr[sizeof(usb_cbcr) / sizeof(usb_cbcr[0])];
	bool clocks_on = true;

	/* sweetusb=0 off, 1 (default) report state only, 2 also start the USB device */
	PE_parse_boot_argn("sweetusb", &enabled, sizeof(enabled));
	if (!enabled) {
		return;
	}
	vm_offset_t gcc = ml_io_map(GCC_PA, GCC_SIZE);
	if (gcc == 0) {
		printf("SWEET USB: GCC map failed\n");
		return;
	}
	gdscr = usb_rd(gcc, GCC_USB30_PRIM_GDSCR);
	printf("SWEET USB: GDSC usb30_prim 0x%08x (%s)\n", gdscr, (gdscr & (1u << 31)) ? "on" : "off");
	for (unsigned i = 0; i < sizeof(usb_cbcr) / sizeof(usb_cbcr[0]); i++) {
		cbcr[i] = usb_rd(gcc, usb_cbcr[i].off);
		printf("SWEET USB: clk %-12s CBCR 0x%08x (%s)\n", usb_cbcr[i].name, cbcr[i],
		    (cbcr[i] & CBCR_CLK_OFF) ? "off" : "on");
	}
	/* DWC3 registers need the GDSC, the master clock and the config-NoC path. */
	clocks_on = (gdscr & (1u << 31)) && !(cbcr[0] & CBCR_CLK_OFF) && !(cbcr[3] & CBCR_CLK_OFF);
	if (!clocks_on) {
		printf("SWEET USB: controller unpowered or unclocked; DWC3 not read\n");
		return;
	}
	vm_offset_t dwc = ml_io_map(DWC3_PA, DWC3_SIZE);
	if (dwc == 0) {
		printf("SWEET USB: DWC3 map failed\n");
		return;
	}
	printf("SWEET USB: DWC3 GSNPSID 0x%08x GCTL 0x%08x GSTS 0x%08x GUSB2PHYCFG 0x%08x\n",
	    usb_rd(dwc, 0xc120), usb_rd(dwc, 0xc110), usb_rd(dwc, 0xc118), usb_rd(dwc, 0xc200));
	printf("SWEET USB: DWC3 DCFG 0x%08x DCTL 0x%08x DEVTEN 0x%08x DSTS 0x%08x DALEPENA 0x%08x\n",
	    usb_rd(dwc, 0xc700), usb_rd(dwc, 0xc704), usb_rd(dwc, 0xc708), usb_rd(dwc, 0xc70c),
	    usb_rd(dwc, 0xc720));
	printf("SWEET USB: DWC3 GEVNTADR 0x%08x%08x GEVNTSIZ 0x%08x GEVNTCOUNT 0x%08x GHWPARAMS0 0x%08x\n",
	    usb_rd(dwc, 0xc404), usb_rd(dwc, 0xc400), usb_rd(dwc, 0xc408), usb_rd(dwc, 0xc40c),
	    usb_rd(dwc, 0xc140));
	if (enabled >= 2) {
		sweet_usb_gadget_start(dwc);
	}
	if (cbcr[5] & CBCR_CLK_OFF) {
		printf("SWEET USB: ahb2phy clock off; QUSB2 PHY not read\n");
		return;
	}
	vm_offset_t phy = ml_io_map(QUSB2_PA, QUSB2_SIZE);
	if (phy == 0) {
		return;
	}
	/* DT qcom,qusb-phy-reg-offset order (Linux phy-msm-qusb-v2.c): PORT_TUNE1 0x240,
	 * PLL_COMMON_STATUS_ONE 0x1a0, PWR_CTRL1 0x210, INTR_CTRL 0x230,
	 * PLL_CORE_INPUT_OVERRIDE 0x0a8, TEST1 0x254. */
	printf("SWEET USB: QUSB2 PLL_STATUS 0x%08x PWR_CTRL1 0x%08x PORT_TUNE1 0x%08x TEST1 0x%08x\n",
	    usb_rd(phy, 0x1a0), usb_rd(phy, 0x210), usb_rd(phy, 0x240), usb_rd(phy, 0x254));
}

/*
 * Step 2: USB device mode with only the default control endpoint, enough
 * for the host to enumerate the phone (boot-arg sweetusb=2). ABL leaves
 * the DWC3 powered, clocked, in device mode and halted, with the PHY set
 * up, so only the device controller is restarted: soft reset, an event
 * buffer in our memory, endpoint 0, run. Events are polled from a kernel
 * thread (no interrupt), like the UFS driver. DMA is not cache coherent
 * (DT qcom,gsi-disable-io-coherency): buffers are cleaned/invalidated.
 * Register layout and the control-transfer state machine follow the
 * Synopsys DWC3 databook as used by Linux drivers/usb/dwc3 (gadget.c,
 * ep0.c).
 */
#define GCTL            0xc110
#define GEVNTADRLO      0xc400
#define GEVNTADRHI      0xc404
#define GEVNTSIZ        0xc408
#define GEVNTCOUNT      0xc40c
#define DCFG            0xc700
#define DCTL            0xc704
#define DEVTEN          0xc708
#define DSTS            0xc70c
#define DALEPENA        0xc720
#define DEPCMDPAR2(n)   (0xc800 + 0x10 * (n))
#define DEPCMDPAR1(n)   (0xc804 + 0x10 * (n))
#define DEPCMDPAR0(n)   (0xc808 + 0x10 * (n))
#define DEPCMD(n)       (0xc80c + 0x10 * (n))

/*
 * Qualcomm wrapper registers ("QSCRATCH", core base + 0xf8800). The
 * controller only connects (pull-up on D+) while it sees a valid VBUS
 * session; Linux dwc3-qcom forces that in software when the charger
 * reports a cable (dwc3_qcom_vbus_override_enable).
 */
#define QSCRATCH_PA             0x0a6f8800
#define QSCRATCH_HS_PHY_CTRL    0x10
#define   UTMI_OTG_VBUS_VALID   (1u << 20)
#define   SW_SESSVLD_SEL        (1u << 28)
#define QSCRATCH_SS_PHY_CTRL    0x30
#define   LANE0_PWR_PRESENT     (1u << 24)

#define DCTL_RUN_STOP   (1u << 31)
#define DCTL_CSFTRST    (1u << 30)
#define DSTS_DEVCTRLHLT (1u << 22)

#define DEPCMD_SETEPCONFIG      1
#define DEPCMD_SETTRANSFRESOURCE 2
#define DEPCMD_SETSTALL         4
#define DEPCMD_STARTTRANSFER    6
#define DEPCMD_DEPSTARTCFG      9
#define DEPCMD_CMDACT           (1u << 10)

#define TRB_HWO         (1u << 0)
#define TRB_LST         (1u << 1)
#define TRB_ISP_IMI     (1u << 10)
#define TRB_IOC         (1u << 11)
#define TRBCTL_SETUP    (2u << 4)
#define TRBCTL_STATUS2  (3u << 4)
#define TRBCTL_STATUS3  (4u << 4)
#define TRBCTL_DATA     (5u << 4)

/* DMA area layout */
#define UD_EVT          0x0000          /* event buffer, 4 KiB */
#define UD_EVT_SIZE     0x1000
#define UD_TRB_OUT      0x1000          /* ep0 OUT TRB */
#define UD_TRB_IN       0x1010          /* ep0 IN TRB */
#define UD_TRB_RX       0x1020          /* ep1 OUT (bulk, host -> phone) */
#define UD_TRB_TX       0x1030          /* ep1 IN (bulk, phone -> host) */
#define UD_TRB_INT      0x1050          /* ep2 IN (interrupt, notifications) */
#define UD_SETUP        0x1040          /* 8-byte setup packet */
#define UD_DATA         0x1100          /* ep0 data stage, 512 bytes */
#define UD_NOTIFY       0x1300          /* 16-byte notification */
#define UD_RX           0x2000          /* one received frame */
#define UD_TX           0x2800          /* one frame to send */
#define UD_FRAME_MAX    0x800
#define UD_SIZE         0x4000

/* Physical endpoints: 2n = OUT, 2n+1 = IN */
#define EP_RX           2               /* 0x01 bulk OUT */
#define EP_TX           3               /* 0x81 bulk IN */
#define EP_INT          5               /* 0x82 interrupt IN */
#define TRBCTL_NORMAL   (1u << 4)

#define UD_TXQ          32

static struct {
	vm_offset_t dwc;
	vm_offset_t dma;
	uint64_t    dma_pa;
	uint32_t    evt_pos;
	int         sleep_chan;
	enum { EP0_SETUP, EP0_DATA, EP0_WAIT_STATUS2, EP0_WAIT_STATUS3, EP0_STATUS } state;
	bool        data_in;        /* data stage direction */
	uint32_t    events;
	bool        configured;
	uint8_t     data_alt;       /* data interface alternate setting */
	/* network */
	ifnet_t     ifp;
	lck_mtx_t  *lock;           /* TX ring and TX endpoint */
	mbuf_t      txq[UD_TXQ];
	unsigned    txq_head, txq_count;
	bool        tx_busy;
	uint64_t    rx_frames, tx_frames;
} ud;

static inline void
dwc_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(ud.dwc + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline uint32_t
dwc_rd(uint32_t off)
{
	return usb_rd(ud.dwc, off);
}

static void
ud_sync(uint32_t off, uint32_t len)
{
	flush_dcache64((addr64_t)(ud.dma_pa + off), len, TRUE);
}

static void
ud_sleep_ms(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&ud.sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static int
ud_epcmd(unsigned ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
	dwc_wr(DEPCMDPAR0(ep), p0);
	dwc_wr(DEPCMDPAR1(ep), p1);
	dwc_wr(DEPCMDPAR2(ep), p2);
	dwc_wr(DEPCMD(ep), cmd | DEPCMD_CMDACT);
	for (int i = 0; i < 1000; i++) {
		uint32_t v = dwc_rd(DEPCMD(ep));
		if (!(v & DEPCMD_CMDACT)) {
			uint32_t status = (v >> 12) & 0xf;
			if (status) {
				printf("SWEET USB: ep%u cmd 0x%x status %u\n", ep, cmd & 0xf, status);
				return -1;
			}
			return 0;
		}
		delay(1);
	}
	printf("SWEET USB: ep%u cmd 0x%x timeout\n", ep, cmd & 0xf);
	return -1;
}

/* One TRB (at DMA offset trb) on physical endpoint ep. */
static void
ud_start_trb(unsigned ep, uint32_t trb, uint32_t buf, uint32_t len, uint32_t ctl)
{
	volatile uint32_t *t = (volatile uint32_t *)(ud.dma + trb);
	uint64_t bpa = ud.dma_pa + buf;

	t[0] = (uint32_t)bpa;
	t[1] = (uint32_t)(bpa >> 32);
	t[2] = len;
	t[3] = ctl | TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC;
	ud_sync(trb, 16);
	if (len) {
		ud_sync(buf, len);
	}
	uint64_t tpa = ud.dma_pa + trb;
	ud_epcmd(ep, DEPCMD_STARTTRANSFER, (uint32_t)(tpa >> 32), (uint32_t)tpa, 0);
}

static void
ud_start(unsigned ep, uint32_t buf, uint32_t len, uint32_t ctl)
{
	ud_start_trb(ep, ep ? UD_TRB_IN : UD_TRB_OUT, buf, len, ctl);
}

static void
ud_start_setup(void)
{
	ud.state = EP0_SETUP;
	ud_start(0, UD_SETUP, 8, TRBCTL_SETUP);
}

static void
ud_stall(void)
{
	ud_epcmd(0, DEPCMD_SETSTALL, 0, 0, 0);
	ud_start_setup();
}

/*
 * USB descriptors: a CDC-ECM (Ethernet) function, which Linux (cdc_ether)
 * and macOS drive without extra drivers. VID 0x1209 / PID 0x0001 =
 * pid.codes test ID, not a product.
 */
static const uint8_t ud_dev_desc[18] = {
	18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64,
	0x09, 0x12, 0x01, 0x00, 0x00, 0x01, 1, 2, 3, 1,
};
static const uint8_t ud_cfg_desc[80] = {
	9, 2, 80, 0, 2, 1, 0, 0x80, 250,                /* config: 2 interfaces, 500 mA */
	9, 4, 0, 0, 1, 0x02, 0x06, 0x00, 0,             /* comm interface 0: CDC ECM, 1 endpoint */
	5, 0x24, 0x00, 0x10, 0x01,                      /* header, CDC 1.10 */
	5, 0x24, 0x06, 0, 1,                            /* union: control 0, data 1 */
	13, 0x24, 0x0f, 4, 0, 0, 0, 0, 0xea, 0x05, 0, 0, 0, /* ethernet: MAC string 4, max segment 1514 */
	7, 5, 0x82, 0x03, 16, 0, 9,                     /* ep 0x82 interrupt IN, 16 bytes, 2^(9-1) uframes */
	9, 4, 1, 0, 0, 0x0a, 0x00, 0x00, 0,             /* data interface 1 alt 0: no endpoints */
	9, 4, 1, 1, 2, 0x0a, 0x00, 0x00, 0,             /* data interface 1 alt 1: two bulk endpoints */
	7, 5, 0x81, 0x02, 0x00, 0x02, 0,                /* ep 0x81 bulk IN, 512 */
	7, 5, 0x01, 0x02, 0x00, 0x02, 0,                /* ep 0x01 bulk OUT, 512 */
};
/* The host side's MAC (ECM iMACAddress); the phone's own is ..:02 */
static const char *const ud_strings[] = {
	NULL, "ios-sweet", "Sweet iOS (USB Ethernet)", "0001", "02005E77EE01",
};
static const uint8_t ud_phone_mac[ETHER_ADDR_LEN] = { 0x02, 0x00, 0x5e, 0x77, 0xee, 0x02 };

static uint32_t
ud_string(unsigned idx, uint8_t *out)
{
	if (idx == 0) {
		out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04;   /* en-US */
		return 4;
	}
	if (idx >= sizeof(ud_strings) / sizeof(ud_strings[0])) {
		return 0;
	}
	const char *str = ud_strings[idx];
	uint32_t n = 2;
	for (; *str && n < 126; str++) {
		out[n++] = (uint8_t)*str;
		out[n++] = 0;
	}
	out[0] = (uint8_t)n;
	out[1] = 3;
	return n;
}

static void ud_rx_arm(void);
static void ud_tx_kick_locked(void);
static void ud_link_event(bool up);

/* NETWORK_CONNECTION notification: the host brings its link up on it. */
static void
ud_notify_connected(void)
{
	uint8_t *n = (uint8_t *)(ud.dma + UD_NOTIFY);
	n[0] = 0xa1; n[1] = 0x00;       /* NETWORK_CONNECTION */
	n[2] = 1; n[3] = 0;             /* connected */
	n[4] = 0; n[5] = 0;             /* interface 0 */
	n[6] = 0; n[7] = 0;
	ud_start_trb(EP_INT, UD_TRB_INT, UD_NOTIFY, 8, TRBCTL_NORMAL);
}

/* SET_CONFIGURATION(1): bulk IN/OUT and the interrupt endpoint. */
static void
ud_configure(void)
{
	if (ud.configured) {
		return;
	}
	if (ud_epcmd(0, DEPCMD_DEPSTARTCFG | (2u << 16), 0, 0, 0) ||
	    ud_epcmd(EP_RX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3), (1u << 8) | ((uint32_t)EP_RX << 25), 0) ||
	    ud_epcmd(EP_RX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(EP_TX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3) | (1u << 17), (1u << 8) | ((uint32_t)EP_TX << 25), 0) ||
	    ud_epcmd(EP_TX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(EP_INT, DEPCMD_SETEPCONFIG, (3u << 1) | (16u << 3) | (2u << 17),
	    (1u << 8) | (8u << 16) | ((uint32_t)EP_INT << 25), 0) ||
	    ud_epcmd(EP_INT, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		printf("SWEET USB: data endpoints setup failed\n");
		return;
	}
	dwc_wr(DALEPENA, dwc_rd(DALEPENA) | (1u << EP_RX) | (1u << EP_TX) | (1u << EP_INT));
	ud.configured = true;
	ud_rx_arm();
	printf("SWEET USB: ECM endpoints configured\n");
}

static void
ud_setup(void)
{
	const uint8_t *st = (const uint8_t *)(ud.dma + UD_SETUP);
	uint8_t *data = (uint8_t *)(ud.dma + UD_DATA);
	uint8_t type = st[0], req = st[1];
	uint16_t value = (uint16_t)(st[2] | st[3] << 8);
	uint16_t index = (uint16_t)(st[4] | st[5] << 8);
	uint16_t length = (uint16_t)(st[6] | st[7] << 8);
	uint32_t n = 0;
	bool ok = true;

	printf("SWEET USB: setup %02x %02x %04x %04x len %u\n", type, req, value, index, length);
	ud.data_in = (type & 0x80) != 0;
	if ((type & 0x60) == 0x20) {    /* class request (e.g. SET_ETHERNET_PACKET_FILTER) */
		ok = (length == 0);
	} else if ((type & 0x60) != 0) {
		ok = false;
	} else if (req == 6) {          /* GET_DESCRIPTOR */
		switch (value >> 8) {
		case 1: memcpy(data, ud_dev_desc, sizeof(ud_dev_desc)); n = sizeof(ud_dev_desc); break;
		case 2: memcpy(data, ud_cfg_desc, sizeof(ud_cfg_desc)); n = sizeof(ud_cfg_desc); break;
		case 3: n = ud_string(value & 0xff, data); ok = n != 0; break;
		default: ok = false; break;
		}
	} else if (req == 5) {          /* SET_ADDRESS: takes effect now (DWC3 handles the timing) */
		uint32_t dcfg = dwc_rd(DCFG) & ~(0x7fu << 3);
		dwc_wr(DCFG, dcfg | ((uint32_t)(value & 0x7f) << 3));
		printf("SWEET USB: address %u\n", value & 0x7f);
	} else if (req == 9) {          /* SET_CONFIGURATION */
		if (value == 1) {
			ud_configure();
		}
		printf("SWEET USB: configured (%u)\n", value);
	} else if (req == 8) {          /* GET_CONFIGURATION */
		data[0] = ud.configured ? 1 : 0; n = 1;
	} else if (req == 0) {          /* GET_STATUS */
		data[0] = 0; data[1] = 0; n = 2;
	} else if (req == 11) {         /* SET_INTERFACE */
		if (index == 1) {
			ud.data_alt = (uint8_t)value;
			if (value == 1 && ud.configured) {
				ud_notify_connected();
				ud_link_event(true);
			}
		}
	} else if (req == 10) {         /* GET_INTERFACE */
		data[0] = index == 1 ? ud.data_alt : 0; n = 1;
	} else if (req == 1 || req == 3) { /* CLEAR/SET_FEATURE (halt, remote wakeup) */
	} else {
		ok = false;
	}
	if (!ok) {
		ud_stall();
		return;
	}
	if (length == 0) {
		ud.state = EP0_WAIT_STATUS2;    /* status stage on XferNotReady */
		return;
	}
	ud.state = EP0_DATA;
	if (ud.data_in) {
		if (n > length) {
			n = length;
		}
		ud_start(1, UD_DATA, n, TRBCTL_DATA);
	} else {
		ud_start(0, UD_DATA, 64, TRBCTL_DATA);  /* OUT data is not used here */
	}
}

/* ---- network interface (ue0) ---- */

static void
ud_rx_arm(void)
{
	ud_start_trb(EP_RX, UD_TRB_RX, UD_RX, UD_FRAME_MAX, TRBCTL_NORMAL);
}

static void
ud_rx_done(void)
{
	volatile uint32_t *t = (volatile uint32_t *)(ud.dma + UD_TRB_RX);
	ud_sync(UD_TRB_RX, 16);
	uint32_t len = UD_FRAME_MAX - (t[2] & 0xffffff);
	ud_sync(UD_RX, UD_FRAME_MAX);
	if (ud.ifp != NULL && len >= ETHER_HDR_LEN && len <= ETHER_MAX_LEN) {
		/*
		 * One mbuf with a 2 KiB cluster holds any frame. (mbuf_allocpacket
		 * may return a first mbuf of length 0: stripping the header from
		 * it panicked "Failed mbuf validity check: len -14".)
		 */
		mbuf_t m = NULL;
		if (mbuf_gethdr(MBUF_DONTWAIT, MBUF_TYPE_DATA, &m) == 0 &&
		    mbuf_mclget(MBUF_DONTWAIT, MBUF_TYPE_DATA, &m) == 0 &&
		    (mbuf_flags(m) & MBUF_EXT) && mbuf_maxlen(m) >= len) {
			struct ifnet_stat_increment_param stats = {};
			memcpy(mbuf_data(m), (const void *)(ud.dma + UD_RX), len);
			mbuf_setlen(m, len);
			mbuf_pkthdr_setlen(m, len);
			void *frame_header = mbuf_data(m);
			mbuf_pkthdr_setrcvif(m, ud.ifp);
			mbuf_pkthdr_setheader(m, frame_header);
			mbuf_pkthdr_adjustlen(m, -ETHER_HDR_LEN);
			(void)mbuf_setdata(m, (char *)mbuf_data(m) + ETHER_HDR_LEN, mbuf_len(m) - ETHER_HDR_LEN);
			stats.packets_in = 1;
			stats.bytes_in = len;
			ud.rx_frames++;
			ifnet_input(ud.ifp, m, &stats);
		} else if (m != NULL) {
			mbuf_freem(m);
		}
	}
	ud_rx_arm();
}

/* Start the next queued frame if the IN endpoint is idle. ud.lock held. */
static void
ud_tx_kick_locked(void)
{
	while (!ud.tx_busy && ud.txq_count > 0 && ud.configured) {
		mbuf_t m = ud.txq[ud.txq_head];
		ud.txq_head = (ud.txq_head + 1) % UD_TXQ;
		ud.txq_count--;
		size_t len = mbuf_pkthdr_len(m);
		if (len > ETHER_MAX_LEN - ETHER_CRC_LEN) {
			mbuf_freem(m);
			continue;
		}
		mbuf_copydata(m, 0, len, (void *)(ud.dma + UD_TX));
		mbuf_freem(m);
		/* A frame of a multiple of 512 bytes would need a zero-length
		 * packet to end it; one pad byte does the same (IP ignores it). */
		if ((len % 512) == 0) {
			((uint8_t *)(ud.dma + UD_TX))[len++] = 0;
		}
		ud.tx_busy = true;
		ud.tx_frames++;
		ud_start_trb(EP_TX, UD_TRB_TX, UD_TX, (uint32_t)len, TRBCTL_NORMAL);
	}
}

static errno_t
ud_if_output(ifnet_t ifp, mbuf_t m)
{
	lck_mtx_lock(ud.lock);
	if (ud.txq_count == UD_TXQ || !ud.configured) {
		lck_mtx_unlock(ud.lock);
		mbuf_freem(m);
		ifnet_stat_increment_out(ifp, 0, 0, 1);
		return 0;
	}
	ifnet_stat_increment_out(ifp, 1, mbuf_pkthdr_len(m), 0);
	ud.txq[(ud.txq_head + ud.txq_count) % UD_TXQ] = m;
	ud.txq_count++;
	ud_tx_kick_locked();
	lck_mtx_unlock(ud.lock);
	return 0;
}

/*
 * Link state for configd (KernelEventMonitor reads SIOCGIFMEDIA and the
 * KEV_DL_LINK_ON/OFF events; IPMonitor does not route through a service
 * whose interface has no active link).
 */
static void
ud_link_event(bool up)
{
	if (ud.ifp != NULL) {
		dlil_post_msg(ud.ifp, KEV_DL_SUBCLASS, up ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF, NULL, 0, FALSE);
	}
}

static errno_t
ud_if_ioctl(ifnet_t ifp, unsigned long cmd, void *data)
{
	switch (cmd) {
	case SIOCGIFMEDIA32:
	case SIOCGIFMEDIA64: {
		struct ifmediareq *ifmr = (struct ifmediareq *)data;
		user_addr_t list = (cmd == SIOCGIFMEDIA64) ?
		    ((struct ifmediareq64 *)ifmr)->ifmu_ulist :
		    CAST_USER_ADDR_T(((struct ifmediareq32 *)ifmr)->ifmu_ulist);
		int media = IFM_ETHER | IFM_AUTO;
		ifmr->ifm_current = media;
		ifmr->ifm_active = ud.configured ? (IFM_ETHER | IFM_1000_T | IFM_FDX) : media;
		ifmr->ifm_mask = 0;
		ifmr->ifm_status = IFM_AVALID | (ud.configured ? IFM_ACTIVE : 0);
		if (list != USER_ADDR_NULL && ifmr->ifm_count > 0) {
			ifmr->ifm_count = 1;
			return copyout(&media, list, sizeof(media));
		}
		ifmr->ifm_count = 1;
		return 0;
	}
	case SIOCSIFADDR:
	case SIOCAIFADDR:
		ifnet_set_flags(ifp, IFF_UP, IFF_UP);
		return 0;
	case SIOCSIFFLAGS:
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

static void
ud_if_detach(ifnet_t ifp __unused)
{
}

/* ue0, 192.168.7.2/24 (the host side is 192.168.7.1). */
static void
ud_if_create(void)
{
	struct ifnet_init_eparams init;
	static lck_grp_t *grp;

	grp = lck_grp_alloc_init("sweet_usb", LCK_GRP_ATTR_NULL);
	ud.lock = lck_mtx_alloc_init(grp, LCK_ATTR_NULL);

	bzero(&init, sizeof(init));
	init.ver = IFNET_INIT_CURRENT_VERSION;
	init.len = sizeof(init);
	init.flags = IFNET_INIT_LEGACY;
	init.uniqueid = "sweet-usb-ecm";
	init.uniqueid_len = sizeof("sweet-usb-ecm") - 1;
	init.name = "ue";
	init.unit = 0;
	init.family = IFNET_FAMILY_ETHERNET;
	init.type = IFT_ETHER;
	init.output = ud_if_output;
	init.demux = ether_demux;
	init.add_proto = ether_add_proto;
	init.del_proto = ether_del_proto;
	init.check_multi = ether_check_multi;
	init.framer_extended = ether_frameout_extended;
	init.ioctl = ud_if_ioctl;
	init.detach = ud_if_detach;
	init.broadcast_addr = etherbroadcastaddr;
	init.broadcast_len = ETHER_ADDR_LEN;
	if (ifnet_allocate_extended(&init, &ud.ifp) != 0) {
		printf("SWEET USB: ifnet_allocate failed\n");
		ud.ifp = NULL;
		return;
	}
	ifnet_set_addrlen(ud.ifp, ETHER_ADDR_LEN);
	ifnet_set_hdrlen(ud.ifp, sizeof(struct ether_header));
	ifnet_set_mtu(ud.ifp, ETHERMTU);
	ifnet_set_baudrate(ud.ifp, 480000000);
	ifnet_set_flags(ud.ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST, 0xffff);
	if (ifnet_attach(ud.ifp, NULL) != 0) {
		printf("SWEET USB: ifnet_attach failed\n");
		ud.ifp = NULL;
		return;
	}
	ifnet_set_lladdr(ud.ifp, ud_phone_mac, ETHER_ADDR_LEN);

	errno_t err = proto_plumb(PF_INET, ud.ifp);
	socket_t so = NULL;
	if (err == 0) {
		err = sock_socket(PF_INET, SOCK_DGRAM, 0, NULL, NULL, &so);
	}
	if (err == 0) {
		struct ifaliasreq ifra;
		bzero(&ifra, sizeof(ifra));
		snprintf(ifra.ifra_name, sizeof(ifra.ifra_name), "ue0");
		struct sockaddr_in *a = (struct sockaddr_in *)&ifra.ifra_addr;
		a->sin_len = sizeof(*a);
		a->sin_family = AF_INET;
		a->sin_addr.s_addr = htonl(0xc0a80702);         /* 192.168.7.2 */
		a = (struct sockaddr_in *)&ifra.ifra_mask;
		a->sin_len = sizeof(*a);
		a->sin_family = AF_INET;
		a->sin_addr.s_addr = htonl(0xffffff00);
		a = (struct sockaddr_in *)&ifra.ifra_broadaddr;
		a->sin_len = sizeof(*a);
		a->sin_family = AF_INET;
		a->sin_addr.s_addr = htonl(0xc0a807ff);
		err = sock_ioctl(so, SIOCAIFADDR, &ifra);
		sock_close(so);
	}
	ifnet_set_flags(ud.ifp, IFF_UP | IFF_RUNNING, IFF_UP | IFF_RUNNING);
	printf("SWEET USB: ue0 192.168.7.2/24 %s (err %d)\n", err ? "NOT configured" : "up", err);
}

static void
ud_event(uint32_t ev)
{
	if (ev & 1) {                   /* device event */
		uint32_t type = (ev >> 8) & 0xf;
		if (type == 1) {        /* USB reset */
			dwc_wr(DCFG, dwc_rd(DCFG) & ~(0x7fu << 3));
			printf("SWEET USB: bus reset\n");
		} else if (type == 2) { /* connect done */
			uint32_t speed = dwc_rd(DSTS) & 7;
			/* ep0 max packet: 64 at high/full speed (modify action) */
			ud_epcmd(0, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3) | (2u << 30),
			    (1u << 8) | (1u << 10) | (0u << 25), 0);
			ud_epcmd(1, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3) | (2u << 30),
			    (1u << 8) | (1u << 10) | (1u << 25), 0);
			printf("SWEET USB: connected, speed %u\n", speed);
		} else if (type == 0) {
			printf("SWEET USB: disconnect\n");
		}
		return;
	}
	unsigned ep = (ev >> 1) & 0x1f;
	unsigned type = (ev >> 6) & 0xf;
	unsigned status = (ev >> 12) & 0xf;

	if (ep == EP_RX && type == 1) {
		ud_rx_done();
		return;
	}
	if (ep == EP_TX && type == 1) {
		lck_mtx_lock(ud.lock);
		ud.tx_busy = false;
		ud_tx_kick_locked();
		lck_mtx_unlock(ud.lock);
		return;
	}
	if (ep > 1) {
		return;
	}
	/*
	 * Control transfer state machine (Linux dwc3 ep0.c): SETUP completes ->
	 * request handled; the data stage (if any) is started right away; the
	 * status stage is started when the controller reports XferNotReady
	 * with status "control status"; its completion re-arms SETUP.
	 */
	if (type == 1) {                /* XferComplete */
		switch (ud.state) {
		case EP0_SETUP:
			ud_sync(UD_SETUP, 8);
			ud_setup();
			break;
		case EP0_DATA:
			ud.state = EP0_WAIT_STATUS3;
			break;
		case EP0_STATUS:
			ud_start_setup();
			break;
		default:
			break;
		}
		return;
	}
	if (type == 3 && status == 2) { /* XferNotReady, control status phase */
		if (ud.state == EP0_WAIT_STATUS2 || ud.state == EP0_WAIT_STATUS3) {
			uint32_t ctl = ud.state == EP0_WAIT_STATUS3 ? TRBCTL_STATUS3 : TRBCTL_STATUS2;
			ud.state = EP0_STATUS;
			ud_start(ep, UD_DATA, 0, ctl);
		}
	}
}

static void
ud_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint32_t ticks = 0;
	for (;;) {
		uint32_t count = dwc_rd(GEVNTCOUNT) & 0xffff;
		if (count) {
			ud_sync(UD_EVT, UD_EVT_SIZE);
			for (uint32_t done = 0; done < count; done += 4) {
				uint32_t ev = *(volatile uint32_t *)(ud.dma + UD_EVT + ud.evt_pos);
				ud.evt_pos = (ud.evt_pos + 4) % UD_EVT_SIZE;
				if (ud.events++ < 200) {
					printf("SWEET USB: event 0x%08x\n", ev);
				}
				ud_event(ev);
			}
			dwc_wr(GEVNTCOUNT, count);
		}
		if ((++ticks % 30000) == 0) {
			printf("SWEET USB: ue0 rx %llu tx %llu frames\n", ud.rx_frames, ud.tx_frames);
		}
		ud_sleep_ms(1);
	}
}

/*
 * Live console log over USB Ethernet: every 100 ms the new bytes of the
 * console ring (bsd/dev/sweet_ufs.c) go as UDP datagrams to
 * 192.168.7.1:5140 (on the PC: nc -ukl 5140). Starts with the last 64 KiB
 * so the boot is included. Nothing here may print (it would feed itself).
 * Boot-arg sweetusblog=0 disables it.
 */
extern uint32_t sweet_log_copy(uint64_t *pos, char *out, uint32_t max);
extern uint64_t sweet_log_position(void);

static void
ud_log_thread(void *arg __unused, wait_result_t wr __unused)
{
	static char chunk[1400];
	socket_t so = NULL;
	struct sockaddr_in to;
	uint64_t pos = sweet_log_position();

	pos = pos > 65536 ? pos - 65536 : 0;
	bzero(&to, sizeof(to));
	to.sin_len = sizeof(to);
	to.sin_family = AF_INET;
	to.sin_port = htons(5140);
	to.sin_addr.s_addr = htonl(0xc0a80701);         /* 192.168.7.1 */
	for (;;) {
		ud_sleep_ms(100);
		if (!ud.configured) {
			continue;
		}
		if (so == NULL && sock_socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, NULL, &so) != 0) {
			so = NULL;
			continue;
		}
		uint32_t n;
		while ((n = sweet_log_copy(&pos, chunk, sizeof(chunk))) > 0) {
			struct iovec iov = { .iov_base = chunk, .iov_len = n };
			struct msghdr msg = {
				.msg_name = &to, .msg_namelen = sizeof(to),
				.msg_iov = &iov, .msg_iovlen = 1,
			};
			size_t sent = 0;
			if (sock_send(so, &msg, 0, &sent) != 0) {
				break;          /* no route/ARP yet: retry later */
			}
		}
	}
}

static void
sweet_usb_gadget_start(vm_offset_t dwc)
{
	thread_t thread;

	ud.dwc = dwc;
	if (kmem_alloc_contig(kernel_map, &ud.dma, UD_SIZE, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("SWEET USB: no DMA memory\n");
		return;
	}
	ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)ud.dma);
	if (!pn) {
		return;
	}
	ud.dma_pa = ptoa_64(pn);
	ud_if_create();

	/* Device controller soft reset (PHY and global setup stay as ABL left them). */
	dwc_wr(DCTL, DCTL_CSFTRST);
	int i;
	for (i = 0; i < 500 && (dwc_rd(DCTL) & DCTL_CSFTRST); i++) {
		delay(1000);
	}
	printf("SWEET USB: soft reset %s after %d ms, GCTL 0x%08x DCFG 0x%08x\n",
	    (dwc_rd(DCTL) & DCTL_CSFTRST) ? "STUCK" : "done", i, dwc_rd(GCTL), dwc_rd(DCFG));
	if (dwc_rd(DCTL) & DCTL_CSFTRST) {
		return;
	}

	dwc_wr(GEVNTADRLO, (uint32_t)(ud.dma_pa + UD_EVT));
	dwc_wr(GEVNTADRHI, (uint32_t)((ud.dma_pa + UD_EVT) >> 32));
	dwc_wr(GEVNTSIZ, (1u << 31) | UD_EVT_SIZE);     /* interrupt masked: polled */
	dwc_wr(GEVNTCOUNT, dwc_rd(GEVNTCOUNT) & 0xffff);
	ud.evt_pos = 0;

	/* High speed, address 0 */
	uint32_t dcfg = dwc_rd(DCFG) & ~(7u | (0x7fu << 3));
	dwc_wr(DCFG, dcfg | 0);
	/* Events: disconnect, USB reset, connect done */
	dwc_wr(DEVTEN, 0x7);

	/* Endpoint 0 OUT/IN: start config, control type, max packet 512 until connect done. */
	if (ud_epcmd(0, DEPCMD_DEPSTARTCFG, 0, 0, 0) ||
	    ud_epcmd(0, DEPCMD_SETEPCONFIG, (0u << 1) | (512u << 3), (1u << 8) | (1u << 10) | (0u << 25), 0) ||
	    ud_epcmd(0, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(1, DEPCMD_SETEPCONFIG, (0u << 1) | (512u << 3), (1u << 8) | (1u << 10) | (1u << 25), 0) ||
	    ud_epcmd(1, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		printf("SWEET USB: endpoint 0 setup failed\n");
		return;
	}
	dwc_wr(DALEPENA, 0x3);
	ud_start_setup();

	vm_offset_t qs = ml_io_map(QSCRATCH_PA, 0x100);
	if (qs != 0) {
		uint32_t hs = usb_rd(qs, QSCRATCH_HS_PHY_CTRL), ss = usb_rd(qs, QSCRATCH_SS_PHY_CTRL);
		printf("SWEET USB: QSCRATCH HS_PHY_CTRL 0x%08x SS_PHY_CTRL 0x%08x GENERAL_CFG 0x%08x\n",
		    hs, ss, usb_rd(qs, 0x08));
		*(volatile uint32_t *)(qs + QSCRATCH_HS_PHY_CTRL) = hs | UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL;
		*(volatile uint32_t *)(qs + QSCRATCH_SS_PHY_CTRL) = ss | LANE0_PWR_PRESENT;
		__asm__ volatile ("dsb sy" ::: "memory");
		printf("SWEET USB: VBUS override on: HS_PHY_CTRL 0x%08x SS_PHY_CTRL 0x%08x\n",
		    usb_rd(qs, QSCRATCH_HS_PHY_CTRL), usb_rd(qs, QSCRATCH_SS_PHY_CTRL));
	}

	dwc_wr(DCTL, dwc_rd(DCTL) | DCTL_RUN_STOP);
	for (i = 0; i < 500 && (dwc_rd(DSTS) & DSTS_DEVCTRLHLT); i++) {
		delay(1000);
	}
	printf("SWEET USB: running (DSTS 0x%08x after %d ms), DMA at 0x%llx\n", dwc_rd(DSTS), i, ud.dma_pa);
	if (kernel_thread_start(ud_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	int livelog = 1;
	PE_parse_boot_argn("sweetusblog", &livelog, sizeof(livelog));
	if (livelog && ud.ifp != NULL && kernel_thread_start(ud_log_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
