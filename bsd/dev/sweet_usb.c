/*
 * SWEET: USB bring-up, step 1 - read-only state probe.
 *
 * The phone's USB is a Synopsys DWC3 (USB 2.0 only, maximum-speed
 * high-speed) at 0x0a600000 behind Qualcomm's dwc-usb3-msm wrapper, with a
 * QUSB2 PHY at 0x088e2000 (flame: Synopsys femto HS PHY, same address); its power domain is the USB30_PRIM GDSC and its
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
extern vm_offset_t ml_io_map_wcomb(vm_offset_t, vm_size_t);
void sweet_usb_probe(void);
static void sweet_usb_gadget_start(vm_offset_t dwc);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);

#define GCC_PA                  0x00100000
#define GCC_SIZE                0x00090000
#define GCC_USB30_PRIM_GDSCR    0x0f004         /* bit 31: power on (same on lito) */
/* DWC3 stream ID in the apps SMMU: redfin DT iommus = <&apps_smmu 0xe0 0x0> (flame/sunfish 0x140) */
#define USB_SID                 0xe0u
#define CBCR_CLK_OFF            (1u << 31)

static const struct {
	const char *name;
	uint32_t    off;
} usb_cbcr[] = {
	/* redfin (SM7250, gcc-lito.c halt_reg); index 0 master and 3 cfg-noc-axi gate DWC3 reads.
	 * (flame/SM8150: sleep 0xf014, utmi 0xf018, cfg-noc 0xf078, aggre 0xf07c, aux 0xf050, clkref 0x8c008) */
	{ "master", 0x0f010 },
	{ "sleep", 0x0f018 },
	{ "utmi", 0x0f01c },
	{ "cfg-noc-axi", 0x0f07c },
	{ "aggre-axi", 0x0f080 },
	{ "phy-aux", 0x0f054 },
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
	/*
	 * flame's HS PHY at 0x088e2000 is a Synopsys femto PHY, not the Redmi's
	 * QUSB2: its registers differ, so they are not read. ABL's PHY setup
	 * is used as is (the driver never writes the PHY).
	 */
	if (1) {
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

static bool ud_dma_uncached;
static vm_offset_t ud_smmu_cb;  /* our apps SMMU context bank (flame), for fault logging */

static void
ud_sync(uint32_t off, uint32_t len)
{
	if (ud_dma_uncached) {
		__asm__ volatile ("dsb sy" ::: "memory");
		return;
	}
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

/* The first frames each way, for link debugging (boot-arg sweetusbframes=N, default 40). */
static int ud_frame_log = -1;

static void
ud_log_frame(const char *dir, const uint8_t *f, uint32_t len)
{
	if (ud_frame_log < 0) {
		ud_frame_log = 40;
		PE_parse_boot_argn("sweetusbframes", &ud_frame_log, sizeof(ud_frame_log));
	}
	if (len < 14) {
		return;
	}
	if (len >= 42 && f[12] == 0x08 && f[13] == 0x06) {
		/* ARP: always, in full (opcode, sender MAC/IP, target MAC/IP) */
		printf("SWEET ARP %s op %u sha %02x:%02x:%02x:%02x:%02x:%02x spa %u.%u.%u.%u tha %02x:%02x:%02x:%02x:%02x:%02x tpa %u.%u.%u.%u (eth dst %02x:..:%02x)\n",
		    dir, f[21], f[22], f[23], f[24], f[25], f[26], f[27], f[28], f[29], f[30], f[31],
		    f[32], f[33], f[34], f[35], f[36], f[37], f[38], f[39], f[40], f[41], f[0], f[5]);
		return;
	}
	if (ud_frame_log <= 0) {
		return;
	}
	ud_frame_log--;
	printf("SWEET FRAME %s len %u dst %02x:%02x:%02x:%02x:%02x:%02x src %02x:%02x:%02x:%02x:%02x:%02x type %02x%02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
	    dir, len, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13],
	    len > 14 ? f[14] : 0, len > 15 ? f[15] : 0, len > 20 ? f[20] : 0, len > 21 ? f[21] : 0,
	    len > 38 ? f[38] : 0, len > 39 ? f[39] : 0, len > 40 ? f[40] : 0, len > 41 ? f[41] : 0);
}

static void
ud_rx_done(void)
{
	volatile uint32_t *t = (volatile uint32_t *)(ud.dma + UD_TRB_RX);
	ud_sync(UD_TRB_RX, 16);
	uint32_t len = UD_FRAME_MAX - (t[2] & 0xffffff);
	ud_sync(UD_RX, UD_FRAME_MAX);
	ud_log_frame("rx", (const uint8_t *)(ud.dma + UD_RX), len);
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
		ud_log_frame("tx", (const uint8_t *)(ud.dma + UD_TX), (uint32_t)len);
		/*
		 * ARP replies go to the Ethernet broadcast address: macOS's ECM
		 * driver shows the host as 02:00:5e:77:ee:01 (our iMACAddress) on
		 * the wire, but en17's own lladdr is 02:00:00:00:00:00, and its
		 * ARP drops replies unicast to ..:01, so 192.168.7.2 never
		 * resolved (frames carrying IP are accepted either way).
		 */
		uint8_t *fr = (uint8_t *)(ud.dma + UD_TX);
		if (len >= 42 && fr[12] == 0x08 && fr[13] == 0x06 && fr[20] == 0 && fr[21] == 2) {
			memset(fr, 0xff, ETHER_ADDR_LEN);
		}
		/* Pad to the Ethernet minimum (60 bytes without FCS), as a MAC
		 * would: macOS drops shorter frames, e.g. our 42-byte ARP replies,
		 * so the Mac never resolved 192.168.7.2. */
		while (len < 60) {
			((uint8_t *)(ud.dma + UD_TX))[len++] = 0;
		}
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
	/* redfin (0969): poll every 1 ms while events flow, every 10 ms after 200 quiet polls; sweetudpoll=1: always 1 ms */
	uint32_t quiet = 0, fast = 0;
	PE_parse_boot_argn("sweetudpoll", &fast, sizeof(fast));
	for (;;) {
		uint32_t count = dwc_rd(GEVNTCOUNT) & 0xffff;
		quiet = count ? 0 : quiet < 200 ? quiet + 1 : quiet;
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
		if ((++ticks % 10000) == 0) {
			volatile uint32_t *rt = (volatile uint32_t *)(ud.dma + UD_TRB_RX);
			volatile uint32_t *tt = (volatile uint32_t *)(ud.dma + UD_TRB_TX);
			ud_sync(UD_TRB_RX, 16);
			ud_sync(UD_TRB_TX, 16);
			printf("SWEET USB: ue0 rx %llu tx %llu frames, events %llu; rx trb %08x %08x tx trb %08x %08x busy %d DSTS %08x\n",
			    ud.rx_frames, ud.tx_frames, (unsigned long long)ud.events, rt[2], rt[3], tt[2], tt[3],
			    ud.tx_busy, dwc_rd(DSTS));
		}
		if (ticks <= 60000 && (ticks % 5000) == 0) {
			printf("SWEET USB: t=%us DCFG %08x DCTL %08x DSTS %08x GUSB2PHYCFG %08x GUSB3PIPECTL %08x GCTL %08x GSTS %08x EVCNT %x\n",
			    ticks / 1000, dwc_rd(DCFG), dwc_rd(DCTL), dwc_rd(DSTS), dwc_rd(0xc200),
			    dwc_rd(0xc2c0), dwc_rd(GCTL), dwc_rd(0xc118), dwc_rd(GEVNTCOUNT));
			if (ud_smmu_cb) {
				printf("SWEET USB: busERR addr %08x%08x; SMMU cb fsr %x far %08x%08x fsynr0 %x fsynr1 %x\n",
				    dwc_rd(0xc134), dwc_rd(0xc130), usb_rd(ud_smmu_cb, 0x58),
				    usb_rd(ud_smmu_cb, 0x64), usb_rd(ud_smmu_cb, 0x60),
				    usb_rd(ud_smmu_cb, 0x68), usb_rd(ud_smmu_cb, 0x6c));
			}
		}
		ud_sleep_ms((fast == 1 || quiet < 200 || ud.tx_busy) ? 1 : 10);
	}
}

/*
 * Command server on 192.168.7.2:5141 (UDP): each datagram is poke script
 * text (bsd/dev/sweet_poke.c: r/w/m/p/s lines), run at once in this
 * thread, results in the console (and so in the live log); "rtc" prints the
 * PM8150 RTC ("SWEET RTC: N", for tools/flame-rtc.py); "tap X Y" and
 * "swipe X1 Y1 X2 Y2 MS" touch the screen (panel pixels); "panic" panics
 * (to test the panic path); "reboot" resets
 * the phone with PSCI (the shim already set the PMIC reason to
 * bootloader, so it lands in fastboot). tools/flame-cmd.py sends them.
 */
extern void sweet_poke_run(const char *text, unsigned len);
struct sync_args;
extern int sync(proc_t p, struct sync_args *uap, int32_t *retval);

/*
 * Synthetic touches through the touchscreen's own path (SweetHID), in panel
 * pixels (0..1079, 0..2279): "tap X Y", "swipe X1 Y1 X2 Y2 MS" (finger 9,
 * so a real finger is not disturbed). For driving the UI from the Mac.
 */
extern void sweet_hid_touch(int finger, int down, int x, int y);

static int
ud_num(const char **p)
{
	int v = 0;
	while (**p == ' ') {
		(*p)++;
	}
	while (**p >= '0' && **p <= '9') {
		v = v * 10 + (*(*p)++ - '0');
	}
	return v;
}

static void
ud_touch_cmd(const char *cmd)
{
	const char *p = cmd + (cmd[0] == 't' ? 4 : 6);
	int x1 = ud_num(&p), y1 = ud_num(&p);
	if (cmd[0] == 't') {
		sweet_hid_touch(9, 1, x1, y1);
		ud_sleep_ms(80);
		sweet_hid_touch(9, 0, x1, y1);
		printf("SWEET CMD: tap %d %d\n", x1, y1);
		return;
	}
	int x2 = ud_num(&p), y2 = ud_num(&p), ms = ud_num(&p);
	int steps = ms > 0 ? ms / 10 : 20;
	if (steps < 2) {
		steps = 2;
	}
	for (int i = 0; i <= steps; i++) {
		sweet_hid_touch(9, 1, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
		ud_sleep_ms(10);
	}
	sweet_hid_touch(9, 0, x2, y2);
	printf("SWEET CMD: swipe %d,%d -> %d,%d in %d steps\n", x1, y1, x2, y2, steps);
}

static void
ud_cmd_thread(void *arg __unused, wait_result_t wr __unused)
{
	static char cmd[1400];
	socket_t so = NULL;
	struct sockaddr_in me;

	for (int tries = 0; so == NULL; tries++) {
		if (sock_socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, NULL, &so) != 0) {
			so = NULL;
			ud_sleep_ms(1000);
			continue;
		}
		bzero(&me, sizeof(me));
		me.sin_len = sizeof(me);
		me.sin_family = AF_INET;
		me.sin_port = htons(5141);
		me.sin_addr.s_addr = htonl(INADDR_ANY);
		if (sock_bind(so, (struct sockaddr *)&me) != 0) {
			sock_close(so);
			so = NULL;
			ud_sleep_ms(1000);
		}
	}
	printf("SWEET CMD: listening on udp 5141 (poke lines, \"reboot\")\n");
	for (;;) {
		struct iovec iov = { .iov_base = cmd, .iov_len = sizeof(cmd) - 1 };
		struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
		size_t got = 0;
		if (sock_receive(so, &msg, 0, &got) != 0 || got == 0) {
			ud_sleep_ms(10);
			continue;
		}
		cmd[got] = 0;
		printf("SWEET CMD: %u bytes: %.60s\n", (unsigned)got, cmd);
		if (strncmp(cmd, "sync", 4) == 0 || strncmp(cmd, "reboot", 6) == 0) {
			/* Flush dirty file data (e.g. sweet_files.c writes) before a reset. */
			int32_t rv = 0;
			(void)sync(kernproc, NULL, &rv);
			printf("SWEET CMD: synced\n");
			if (cmd[0] == 's') {
				continue;
			}
		}
		if (strncmp(cmd, "key ", 4) == 0) {
			/* sunfish: "key N [MS]": press button N (0 power, 1 vol+, 2 vol-, 3 home) for MS ms */
			extern void sweet_hid_key(int key, int down);
			int k = -1, ms = 150;
			const char *q = cmd + 4;
			if (*q >= '0' && *q <= '3') {
				k = *q - '0';
				q++;
				while (*q == ' ') {
					q++;
				}
				if (*q >= '0' && *q <= '9') {
					ms = 0;
					while (*q >= '0' && *q <= '9') {
						ms = ms * 10 + (*q++ - '0');
					}
				}
			}
			if (k < 0 || ms > 5000) {
				printf("SWEET CMD: key: usage key N [MS] (0 power, 1 vol+, 2 vol-, 3 home)\n");
				continue;
			}
			sweet_hid_key(k, 1);
			delay((uint32_t)ms * 1000);
			sweet_hid_key(k, 0);
			printf("SWEET CMD: key %d pressed %d ms\n", k, ms);
			continue;
		}
		if (strncmp(cmd, "tap ", 4) == 0 || strncmp(cmd, "swipe ", 6) == 0) {
			ud_touch_cmd(cmd);
			continue;
		}
		if (strncmp(cmd, "rtc", 3) == 0) {
			extern int sweet_rtc_seconds(uint32_t *secs);   /* SweetPlatformExpert.cpp */
			uint32_t rtc = 0;
			struct timeval tv;
			int rc = sweet_rtc_seconds(&rtc);
			microtime(&tv);
			printf("SWEET CMD: rtc %u%s, time of day %ld\n", rtc, rc ? " (read failed)" : "", (long)tv.tv_sec);
			if (!rc) {
				printf("SWEET RTC: %u\n", rtc);
			}
			continue;
		}
		if (strncmp(cmd, "panic", 5) == 0) {
			/* Tests the panic path: backtraces must reach the persistent log. */
			panic("SWEET CMD: panic requested over USB");
		}
		if (strncmp(cmd, "reboot", 6) == 0) {
			ud_sleep_ms(1000);      /* let HFS finish the journal writes */
			printf("SWEET CMD: rebooting (PSCI SYSTEM_RESET)\n");
			ud_sleep_ms(500);       /* let the log go out */
			register uint64_t x0 __asm__("x0") = 0x84000009ULL;
			__asm__ volatile ("smc #0" : "+r"(x0) : : "x1", "x2", "x3", "memory");
			continue;
		}
		sweet_poke_run(cmd, (unsigned)got);
		printf("SWEET CMD: done\n");
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

	/* sunfish: replay the whole console ring by default (boot-arg
	 * sweetusblogback = bytes) so early boot (cpufreq, keys) is included. */
	uint32_t back = 4u << 20;
	(void)PE_parse_boot_argn("sweetusblogback", &back, sizeof(back));
	pos = pos > back ? pos - back : 0;
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
		uint32_t n, burst = 0;
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
			if (++burst % 32u == 0) {
				ud_sleep_ms(5);  /* pace a large backlog */
			}
		}
	}
}

/*
 * flame: the DWC3 masters through the apps SMMU (MMU-500 at 0x15000000) as
 * stream 0x140 (DT iommus). ABL removes its stream-match entry when it
 * leaves fastboot, and the SMMU faults unmatched streams (sCR0.USFCFG), so
 * the first DMA (the ep0 TRB fetch) raised a global fault and the SoC
 * reset. A bypass S2CR is refused (it reads back as type 2, fault: the
 * hypervisor's policy), so, like Linux, the stream gets its own stage-1
 * context bank: our tables identity-map the 2 MB block holding the DMA
 * buffers (below 4 GB, see sweetusbdma), as Normal non-cacheable.
 */
#define AS_PA           0x15000000ULL
#define AS_SIZE         0x100000
#define UD_PT_OFF       0x100000        /* page tables 1 MB above the DMA buffers */

static int
ud_smmu_setup(uint64_t dma_pa)
{
	vm_offset_t as = ml_io_map(AS_PA, AS_SIZE);
	if (!as) {
		return -1;
	}
	uint32_t idr0 = usb_rd(as, 0x20), idr1 = usb_rd(as, 0x24);
	unsigned nsmr = idr0 & 0xff, ncb = idr1 & 0xff;
	uint32_t pagesz = (idr1 >> 31) ? 0x10000 : 0x1000;
	uint32_t cbbase = (1u << (((idr1 >> 28) & 7) + 1)) * pagesz;
	uint64_t used[2] = {0, 0};
	int free_smr = -1, cb = -1;
	for (unsigned i = 0; i < nsmr; i++) {
		uint32_t m = usb_rd(as, 0x800 + 4 * i), c = usb_rd(as, 0xc00 + 4 * i);
		if (!(m >> 31)) {
			if (free_smr < 0) {
				free_smr = (int)i;
			}
			continue;
		}
		if (((c >> 16) & 3) == 0) {
			used[(c & 0xff) / 64] |= 1ULL << ((c & 0xff) % 64);
		}
		if (((USB_SID ^ (m & 0x7fff)) & ~((m >> 16) & 0x7fff) & 0x7fff) == 0) {
			printf("SWEET USB: SMMU smr%u %x already matches %#x (s2cr %x)\n", i, m, USB_SID, c);
			return -1;
		}
	}
	/* Lowest free bank, as Linux allocates: the top banks may be secure (sCR1.NSNUMCBO). */
	for (int c = 0; c < (int)ncb; c++) {
		if (!(used[c / 64] & (1ULL << (c % 64))) && !(usb_rd(as, cbbase + c * pagesz) & 1) &&
		    usb_rd(as, pagesz + 4 * c) == 0) {
			cb = c;
			break;
		}
	}
	/*
	 * ABL's own DMA streams (UFS 0x300 etc., SMR0-3) point at banks with
	 * CBAR 0x1f000 (S1, S2 bypass), CBA2R 1 and SCTLR 0xe0: translation
	 * OFF, i.e. physical addresses, the bypass the hypervisor refuses to
	 * take as an S2CR type (poke dump, 2026-10-20). Do the same for 0x140.
	 * (Pointing 0x140 at the UFS bank itself stalled this thread; our own
	 * bank with tables gave translation faults.) sweetusbsmmu=1: tables.
	 */
	int mode = 0;
	PE_parse_boot_argn("sweetusbsmmu", &mode, sizeof(mode));
	if (mode == 0 && free_smr >= 0 && cb >= 0) {
		vm_offset_t cbv = as + cbbase + cb * pagesz;
		*(volatile uint32_t *)(cbv + 0x00) = 0xe0;                      /* SCTLR: M=0, like ABL */
		*(volatile uint32_t *)(as + pagesz + 0x800 + 4 * cb) = 1;      /* CBA2R: AArch64 */
		*(volatile uint32_t *)(as + pagesz + 4 * cb) = 0x1f000;        /* CBAR as ABL's banks */
		__asm__ volatile ("dsb sy" ::: "memory");
		*(volatile uint32_t *)(as + 0xc00 + 4 * free_smr) = (uint32_t)cb;
		*(volatile uint32_t *)(as + 0x800 + 4 * free_smr) = (1u << 31) | USB_SID;
		__asm__ volatile ("dsb sy" ::: "memory");
		ud_smmu_cb = cbv;
		printf("SWEET USB: SMMU smr%d %x s2cr %x -> cb%d cbar %x cba2r %x sctlr %x (translation off, as ABL)\n",
		    free_smr, usb_rd(as, 0x800 + 4 * free_smr), usb_rd(as, 0xc00 + 4 * free_smr), cb,
		    usb_rd(as, pagesz + 4 * cb), usb_rd(as, pagesz + 0x800 + 4 * cb), usb_rd(cbv, 0));
		return 0;
	}
	if (free_smr < 0 || cb < 0 || (dma_pa >> 39)) {
		printf("SWEET USB: SMMU: smr %d cb %d dma 0x%llx unusable\n", free_smr, cb, dma_pa);
		return -1;
	}
	/*
	 * Tables in kernel memory (like the GPU SMMU setup that works, whose
	 * tables were in high RAM): L1 (T0SZ 25, 39-bit input) -> L2 -> one
	 * 2 MB identity block holding the DMA buffers. Walks non-cacheable;
	 * the tables are cleaned to the point of coherency.
	 */
	vm_offset_t pt = 0;
	if (kmem_alloc_contig(kernel_map, &pt, 0x2000, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return -1;
	}
	uint64_t pt_pa = ptoa_64(pmap_find_phys(kernel_pmap, (addr64_t)pt)) + (pt & PAGE_MASK);
	uint64_t pt2_pa = ptoa_64(pmap_find_phys(kernel_pmap, (addr64_t)(pt + 0x1000))) + ((pt + 0x1000) & PAGE_MASK);
	uint64_t blk = dma_pa & ~0x1fffffULL;
	((volatile uint64_t *)pt)[(blk >> 30) & 0x1ff] = pt2_pa | 3;
	((volatile uint64_t *)(pt + 0x1000))[(blk >> 21) & 0x1ff] =
	    blk | (1u << 10) | (3u << 8) | (1u << 6) | 1u;      /* AF, ISH, AP=01, attr0, block */
	flush_dcache64((addr64_t)pt_pa, 0x1000, TRUE);
	flush_dcache64((addr64_t)pt2_pa, 0x1000, TRUE);
	__asm__ volatile ("dsb sy" ::: "memory");

	vm_offset_t c = as + cbbase + cb * pagesz;
	ud_smmu_cb = c;
#define CBW(off, v) (*(volatile uint32_t *)(c + (off)) = (v))
	CBW(0x00, 0);                                           /* SCTLR off while programming */
	*(volatile uint32_t *)(as + pagesz + 0x800 + 4 * cb) = 1;       /* CBA2R: AArch64 */
	*(volatile uint32_t *)(as + pagesz + 4 * cb) = (1u << 16) | (0xfu << 12) | (3u << 8) | (cb + 1);
	CBW(0x10, 2);                                           /* TCR2: 40-bit PA */
	CBW(0x20, (uint32_t)pt_pa);
	CBW(0x24, (uint32_t)(pt_pa >> 32));
	/*
	 * Non-cacheable, non-shareable table walks: the tables are written
	 * through an uncached mapping, and ABL may have left cache lines for
	 * this RAM (a cacheable walk found no valid entry: translation fault
	 * at the first TRB fetch while the descriptors read back right).
	 */
	CBW(0x30, 25 | (1u << 23));
	CBW(0x38, 0x0000ff44);                                  /* MAIR0: attr0 Normal NC */
	CBW(0x58, 0xffffffff);                                  /* clear FSR */
	CBW(0x00, 1 | 2 | 4 | 0x20 | 0x40);                     /* M TRE AFE CFRE CFIE */
#undef CBW
	*(volatile uint32_t *)(as + 0x68) = 0;                  /* TLBIALLNSNH */
	*(volatile uint32_t *)(as + 0x70) = 0;                  /* TLBGSYNC */
	for (int i = 0; i < 1000 && (usb_rd(as, 0x74) & 1); i++) {
		delay(1);
	}
	*(volatile uint32_t *)(as + 0xc00 + 4 * free_smr) = (uint32_t)cb;      /* translate via cb */
	*(volatile uint32_t *)(as + 0x800 + 4 * free_smr) = (1u << 31) | USB_SID;
	__asm__ volatile ("dsb sy" ::: "memory");
	/* Self-test: read back our descriptors and ask the SMMU to translate (ATS1PR). */
	printf("SWEET USB: SMMU pt L1[%llu] %llx L2[%llu] %llx tcr %x tcr2 %x mair0 %x ttbr0 %x:%x\n",
	    (blk >> 30) & 0x1ff, ((volatile uint64_t *)pt)[(blk >> 30) & 0x1ff], (blk >> 21) & 0x1ff,
	    ((volatile uint64_t *)(pt + 0x1000))[(blk >> 21) & 0x1ff], usb_rd(c, 0x30), usb_rd(c, 0x10),
	    usb_rd(c, 0x38), usb_rd(c, 0x24), usb_rd(c, 0x20));
	*(volatile uint32_t *)(c + 0x800) = (uint32_t)dma_pa + 0x1000;       /* ATS1PR lo */
	*(volatile uint32_t *)(c + 0x804) = 0;
	__asm__ volatile ("dsb sy" ::: "memory");
	for (int i = 0; i < 1000 && (usb_rd(c, 0x8f0) & 1); i++) {
		delay(1);
	}
	printf("SWEET USB: SMMU ATS1PR(0x%llx): atsr %x par %08x%08x fsr %x\n", dma_pa + 0x1000,
	    usb_rd(c, 0x8f0), usb_rd(c, 0x54), usb_rd(c, 0x50), usb_rd(c, 0x58));
	*(volatile uint32_t *)(c + 0x58) = 0xffffffff;
	printf("SWEET USB: SMMU smr%d %x s2cr %x -> cb%d cbar %x sctlr %x ttbr0 0x%llx (identity 2 MB at 0x%llx)\n",
	    free_smr, usb_rd(as, 0x800 + 4 * free_smr), usb_rd(as, 0xc00 + 4 * free_smr), cb,
	    usb_rd(as, pagesz + 4 * cb), usb_rd(as, cbbase + cb * pagesz), pt_pa, blk);
	return 0;
}

/*
 * flame: the Synopsys femto HS PHY of the primary port (hsphy@88e2000, the
 * DWC3 node's usb-phy; 88e3000 is the second, disabled controller's). ABL runs fastboot at
 * SuperSpeed and leaves this PHY in an unknown power state; the device
 * controller ran but no host ever saw a connection. Same register sequence
 * as Linux qcom_snps_hsphy_init (phy-qcom-snps-femto-v2.c) minus the
 * regulators and the GCC block reset.
 */
/* redfin (lito): the primary port's femto PHY is hsphy@88e3000; 0x088e2000 is the
 * EUD enable register there, never written. */
#define HSPHY_PA        0x088e3000
#define HP_UTMI_CTRL0   0x3c
#define HP_UTMI_CTRL5   0x50
#define HP_COMMON0      0x54
#define HP_COMMON1      0x58
#define HP_COMMON2      0x5c
#define HP_CTRL1        0x60
#define HP_CTRL2        0x64
#define HP_CFG0         0x94
#define HP_REFCLK       0xa0

static void
hp_rmw(vm_offset_t p, uint32_t off, uint32_t mask, uint32_t val)
{
	uint32_t v = usb_rd(p, off);
	*(volatile uint32_t *)(p + off) = (v & ~mask) | (val & mask);
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
ud_hsphy_dump(vm_offset_t p, const char *when)
{
	printf("SWEET USB: hsphy %s: utmi0 %x utmi5 %x common0 %x common1 %x common2 %x ctrl1 %x ctrl2 %x cfg0 %x refclk %x\n",
	    when, usb_rd(p, HP_UTMI_CTRL0), usb_rd(p, HP_UTMI_CTRL5), usb_rd(p, HP_COMMON0),
	    usb_rd(p, HP_COMMON1), usb_rd(p, HP_COMMON2), usb_rd(p, HP_CTRL1), usb_rd(p, HP_CTRL2),
	    usb_rd(p, HP_CFG0), usb_rd(p, HP_REFCLK));
}

static void
ud_hsphy_init(void)
{
	int doinit = 1;
	PE_parse_boot_argn("sweethsphy", &doinit, sizeof(doinit));
	vm_offset_t p = ml_io_map(HSPHY_PA, 0x1000);
	if (!p) {
		return;
	}
	ud_hsphy_dump(p, "as ABL left it");
	if (!doinit) {
		return;
	}
	hp_rmw(p, HP_CFG0, 1u << 1, 1u << 1);           /* UTMI_PHY_CMN_CTRL_OVERRIDE_EN */
	hp_rmw(p, HP_UTMI_CTRL5, 1u << 1, 1u << 1);     /* POR */
	hp_rmw(p, HP_COMMON0, 7u << 4, 0);              /* FSEL */
	hp_rmw(p, HP_COMMON1, 1u << 5, 1u << 5);        /* PLLBTUNE */
	hp_rmw(p, HP_REFCLK, 3u, 2u);                   /* REFCLK_SEL default */
	hp_rmw(p, HP_COMMON1, 1u << 4, 1u << 4);        /* VBUSVLDEXTSEL0 */
	hp_rmw(p, HP_CTRL1, 1u << 0, 1u << 0);          /* VBUSVLDEXT0 */
	hp_rmw(p, HP_COMMON2, 1u << 0, 1u << 0);        /* VREGBYPASS */
	hp_rmw(p, HP_CTRL2, (1u << 3) | (1u << 2), (1u << 3) | (1u << 2));     /* SUSPEND_N(_SEL) */
	hp_rmw(p, HP_UTMI_CTRL0, 1u << 0, 1u << 0);     /* SLEEPM */
	hp_rmw(p, HP_COMMON0, 1u << 2, 0);              /* SIDDQ off: power up */
	delay(100);
	hp_rmw(p, HP_UTMI_CTRL5, 1u << 1, 0);           /* POR release */
	hp_rmw(p, HP_CTRL2, 1u << 3, 0);
	hp_rmw(p, HP_CFG0, 1u << 1, 0);
	/* redfin DT qcom,param-override-seq <0xC7 0x6C 0xCC 0x70> (lito-redfin-usb.dtsi) */
	hp_rmw(p, 0x6c, 0xff, 0xc7);
	hp_rmw(p, 0x70, 0xff, 0xcc);
	delay(1000);
	ud_hsphy_dump(p, "after init");
}

/*
 * sunfish: the HS PHY at 0x088e2000 is a QUSB2 v2 (mainline sm7150.dtsi: "qcom,sm7150-qusb2-phy",
 * "qcom,qusb2-v2-phy"), not flame's femto PHY: ud_hsphy_init wrote femto registers into it. This is Linux
 * qusb2_phy_init for qusb2_v2_phy_cfg: GCC_QUSB2PHY_PRIM_BCR pulse (clears what a warm reset leaves),
 * power down, the v2 table, power up, PLL ready check. Boot-arg sweetqusb=0: the old femto path.
 */
#define GCC_QUSB2PHY_PRIM_BCR_PA        0x00126000
static const struct { uint16_t off; uint8_t val; } qusb2_v2_tbl[] = {
	{ 0x004, 0x03 }, { 0x18c, 0x7c }, { 0x02c, 0x80 }, { 0x184, 0x0a }, { 0x0b4, 0x19 }, { 0x194, 0x40 },
	{ 0x198, 0x20 }, { 0x214, 0x21 }, { 0x220, 0x00 }, { 0x224, 0x58 },
	{ 0x240, 0x30 }, { 0x244, 0x29 }, { 0x248, 0xca }, { 0x24c, 0x04 }, { 0x250, 0x03 },   /* PORT_TUNE1..5 */
	{ 0x23c, 0x00 },
};

static int
ud_qusb2_init(void)
{
	/* redfin: the primary HS PHY is the femto hsphy@88e3000 (ud_hsphy_init); there is no QUSB2 v2 and
	 * 0x088e2000 is its eud register. Upstream sunfish 0151 stays off unless sweetqusb=1. */
	int doinit = 0;
	PE_parse_boot_argn("sweetqusb", &doinit, sizeof(doinit));
	if (!doinit) {
		return 0;
	}
	vm_offset_t p = ml_io_map(QUSB2_PA, QUSB2_SIZE);
	vm_offset_t bcr = ml_io_map(GCC_QUSB2PHY_PRIM_BCR_PA, 0x1000);
	if (!p || !bcr) {
		return 0;
	}
	printf("SWEET USB: qusb2 before: PLL_STATUS %x POWERDOWN %x TUNE1 %x\n",
	    usb_rd(p, 0x1a0), usb_rd(p, 0x210), usb_rd(p, 0x240));
	*(volatile uint32_t *)bcr = 1;                  /* block reset assert */
	__asm__ volatile ("dsb sy" ::: "memory");
	delay(150);
	*(volatile uint32_t *)bcr = 0;
	__asm__ volatile ("dsb sy" ::: "memory");
	delay(100);
	/* disable: VREF_SUPPLY_TRIM | CLAMP_N_EN | POWER_DOWN (qusb2_v2_phy_cfg.disable_ctrl) */
	*(volatile uint32_t *)(p + 0x210) = usb_rd(p, 0x210) | (1u << 5) | (1u << 1) | (1u << 0);
	for (unsigned i = 0; i < sizeof(qusb2_v2_tbl) / sizeof(qusb2_v2_tbl[0]); i++) {
		*(volatile uint32_t *)(p + qusb2_v2_tbl[i].off) = qusb2_v2_tbl[i].val;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(p + 0x210) = usb_rd(p, 0x210) & ~1u;     /* enable: clear POWER_DOWN */
	__asm__ volatile ("dsb sy" ::: "memory");
	delay(300);
	uint32_t st = usb_rd(p, 0x1a0);
	printf("SWEET USB: qusb2 init: PLL_STATUS %x (%s) POWERDOWN %x TUNE1 %x\n", st,
	    (st & 1) ? "core ready" : "NOT READY", usb_rd(p, 0x210), usb_rd(p, 0x240));
	return 1;
}

static void
sweet_usb_gadget_start(vm_offset_t dwc)
{
	thread_t thread;

	ud.dwc = dwc;
	/*
	 * flame: DMA buffers at a fixed physical address below 4 GB (RAM XNU
	 * does not manage; ABL's own USB buffers were at 0xfd34c000), mapped
	 * write-combining, identity-mapped for the DWC3 in the apps SMMU
	 * (ud_smmu_setup). Boot-arg sweetusbdma=0: kernel memory, no SMMU setup
	 * (the Redmi path).
	 */
	uint64_t lowpa = 0;
	PE_parse_boot_argn("sweetusbdma", &lowpa, sizeof(lowpa));
	if (lowpa) {
		ud.dma = ml_io_map_wcomb((vm_offset_t)lowpa, UD_SIZE);
		if (!ud.dma) {
			printf("SWEET USB: low DMA map failed\n");
			return;
		}
		bzero((void *)ud.dma, UD_SIZE);
		ud.dma_pa = lowpa;
		ud_dma_uncached = true;
	} else {
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
	}
	ud_if_create();
	if (ud_smmu_setup(ud.dma_pa) != 0) {
		printf("SWEET USB: SMMU setup failed; not starting the device\n");
		return;
	}

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

	printf("SWEET USB: step event buffer at 0x%llx\n", ud.dma_pa + UD_EVT);
	dwc_wr(GEVNTADRLO, (uint32_t)(ud.dma_pa + UD_EVT));
	dwc_wr(GEVNTADRHI, (uint32_t)((ud.dma_pa + UD_EVT) >> 32));
	dwc_wr(GEVNTSIZ, (1u << 31) | UD_EVT_SIZE);     /* interrupt masked: polled */
	dwc_wr(GEVNTCOUNT, dwc_rd(GEVNTCOUNT) & 0xffff);
	ud.evt_pos = 0;

	printf("SWEET USB: step dcfg\n");
	/* High speed, address 0 */
	uint32_t dcfg = dwc_rd(DCFG) & ~(7u | (0x7fu << 3));
	dwc_wr(DCFG, dcfg | 0);
	/* Events: disconnect, USB reset, connect done */
	dwc_wr(DEVTEN, 0x7);

	printf("SWEET USB: step ep0 commands\n");
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
	printf("SWEET USB: step start setup\n");
	ud_start_setup();
	printf("SWEET USB: step qscratch\n");
	if (!ud_qusb2_init()) {
		ud_hsphy_init();
	}

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
	if (ud.ifp != NULL && kernel_thread_start(ud_cmd_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	if (ud.ifp != NULL) {
		extern void sweet_files_start(void);
		sweet_files_start();    /* tcp 2223: bsd/dev/sweet_files.c */
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
