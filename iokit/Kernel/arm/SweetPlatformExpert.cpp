/*
 * Minimal device-tree platform expert for Redmi Note 10 Pro (sweet).
 * Uses XNU's normal platform, power-domain and nub initialization. Hardware
 * interrupt and power-management services are supplied by separate drivers.
 */
#include <pexpert/arm64/board_config.h>

#if defined(SWEET)
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>

class SweetPlatformExpert : public IODTPlatformExpert {
	OSDeclareDefaultStructors(SweetPlatformExpert);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void processTopLevel(IORegistryEntry *root) APPLE_KEXT_OVERRIDE;
	const char *deleteList(void) APPLE_KEXT_OVERRIDE;
	const char *excludeList(void) APPLE_KEXT_OVERRIDE;
	long getGMTTimeOfDay(void) APPLE_KEXT_OVERRIDE;
	void setGMTTimeOfDay(long secs) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetPlatformExpert, IODTPlatformExpert);

/*
 * flame: the shim arms the APSS watchdog (qcom,wdt@17c10000, ~30 s bite)
 * and sets the PON reason to bootloader, so a kernel that hangs or panics
 * resets the phone into fastboot. Pet it while threads run. flamewdt=0
 * turns it off; flamereboot=N stops petting after about N seconds, which
 * bounds an unattended test run.
 */
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define FLAME_WDT_PA    0x17c10000
#define FLAME_WDT_RST   0x04
#define FLAME_WDT_EN    0x08
#define FLAME_WDT_PET_S 5

static void
flame_wdt_thread(void *arg)
{
	volatile uint32_t *wdt = (volatile uint32_t *)arg;
	int reboot_s = 0;
	PE_parse_boot_argn("flamereboot", &reboot_s, sizeof(reboot_s));
	for (int t = 0; reboot_s <= 0 || t < reboot_s; t += FLAME_WDT_PET_S) {
		wdt[FLAME_WDT_RST / 4] = 1;
		IOSleep(FLAME_WDT_PET_S * 1000);
	}
	IOLog("FLAME WDT: flamereboot=%d reached, no more pets: reset follows\n", reboot_s);
	for (;;) {
		IOSleep(1000000);
	}
}

static void
flame_wdt_start(void)
{
	int enabled = 1;
	PE_parse_boot_argn("flamewdt", &enabled, sizeof(enabled));
	volatile uint32_t *wdt = (volatile uint32_t *)ml_io_map(FLAME_WDT_PA, 0x1000);
	if (wdt == NULL) {
		IOLog("FLAME WDT: mapping failed\n");
		return;
	}
	if (!(wdt[FLAME_WDT_EN / 4] & 1)) {
		IOLog("FLAME WDT: not armed by the shim\n");
		return;
	}
	wdt[FLAME_WDT_RST / 4] = 1;
	if (!enabled) {
		wdt[FLAME_WDT_EN / 4] = 0;
		IOLog("FLAME WDT: disabled (flamewdt=0)\n");
		return;
	}
	IOCreateThread(flame_wdt_thread, (void *)wdt);
	IOLog("FLAME WDT: petting every %d s\n", FLAME_WDT_PET_S);
}

/*
 * Time of day: the PM8150 RTC (SPMI SID 0, peripheral 0x6000) counts seconds
 * from whenever it was last set; Android cannot write it either
 * (qcom,qpnp-rtc-write = 0) and keeps an offset. Here the offset is boot-arg
 * sweetrtcoff (tools/flame-run.sh computes it from the Mac's clock). The
 * RTC is published as the IORTC resource: IOKitInitializeTime waits up to
 * 30 s for it (every boot did) before reading the calendar.
 */
#include "../../../bsd/dev/sweet_spmi.h"

#define PM8150_RTC_CTRL         0x6046
#define PM8150_RTC_RDATA0       0x6048

static int
sweet_rtc_read(uint32_t *secs)
{
	uint8_t b[4], again;
	if (sweet_spmi_init() != 0) {
		return -1;
	}
	for (int tries = 0; tries < 3; tries++) {
		for (unsigned i = 0; i < 4; i++) {
			if (sweet_spmi_read(0, PM8150_RTC_RDATA0 + i, &b[i]) != 0) {
				return -1;
			}
		}
		/* The low byte rolled over between the reads: read again (rtc-pm8xxx does the same). */
		if (sweet_spmi_read(0, PM8150_RTC_RDATA0, &again) != 0) {
			return -1;
		}
		if (again >= b[0]) {
			*secs = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
			return 0;
		}
	}
	return -1;
}

/* For the USB command server's "rtc" (bsd/dev/sweet_usb.c): offset calibration from the Mac. */
extern "C" int
sweet_rtc_seconds(uint32_t *secs)
{
	return sweet_rtc_read(secs);
}

long
SweetPlatformExpert::getGMTTimeOfDay(void)
{
	uint32_t rtc = 0;
	uint64_t off = 0;
	if (sweet_rtc_read(&rtc) != 0) {
		return 0;
	}
	(void)PE_parse_boot_argn("sweetrtcoff", &off, sizeof(off));
	return (long)((uint64_t)rtc + off);
}

void
SweetPlatformExpert::setGMTTimeOfDay(long secs)
{
	uint32_t rtc = 0;
	if (sweet_rtc_read(&rtc) == 0) {
		IOLog("SWEET RTC: time set to %ld (RTC %u, offset %ld); the RTC is read-only, not kept\n",
		    secs, rtc, secs - (long)rtc);
	}
}

extern "C" void sweet_log_mirror_start(void);
extern "C" void sweet_poke_start(void);

bool
SweetPlatformExpert::start(IOService *provider)
{
	sweet_log_mirror_start();       /* crash rescue: bsd/dev/sweet_ufs.c */
	IOLog("SWEET: starting device-tree platform expert\n");
	if (!IODTPlatformExpert::start(provider)) {
		return false;
	}
	flame_wdt_start();
	sweet_poke_start();             /* register scripts: bsd/dev/sweet_poke.c */
	uint32_t rtc = 0;
	uint8_t ctrl = 0;
	if (sweet_rtc_read(&rtc) == 0 && sweet_spmi_read(0, PM8150_RTC_CTRL, &ctrl) == 0) {
		IOLog("SWEET RTC: PM8150 RTC %u s (ctrl %02x), time of day %ld\n", rtc, ctrl, getGMTTimeOfDay());
		publishResource("IORTC", this);
	} else {
		IOLog("SWEET RTC: PM8150 RTC not readable; no time of day\n");
	}
	registerService();
	IOLog("SWEET: platform expert registered\n");
	return true;
}

const char *
SweetPlatformExpert::deleteList(void)
{
	return "()";
}

const char *
SweetPlatformExpert::excludeList(void)
{
	return "('chosen', 'defaults', 'cpus', 'options')";
}

void
SweetPlatformExpert::processTopLevel(IORegistryEntry *root)
{
	/*
	 * The stock implementation requires an Apple NVRAM /options node.
	 * SWEET has no NVRAM controller; publish real CPU/device nodes without
	 * inventing one. createNubs adds the topology's logical CPU/cluster IDs.
	 */
	IORegistryEntry *cpus = root->childFromPath("cpus", gIODTPlane);
	if (cpus != NULL) {
		createNubs(this, IODTFindMatchingEntries(cpus, kIODTExclusive, NULL));
		cpus->release();
	}
	createNubs(this, IODTFindMatchingEntries(root, kIODTExclusive, excludeList()));

	/*
	 * No AppleARMIO driver publishes the /arm-io children: do it here so
	 * built-in drivers (the display, AppleCLCD) can match them. The GIC is
	 * driven by the platform code and gets no nub. The nubs hang off this
	 * platform expert: IOPlatformDevice::compareName asks its provider.
	 */
	IORegistryEntry *armio = root->childFromPath("arm-io", gIODTPlane);
	if (armio != NULL) {
		createNubs(this, IODTFindMatchingEntries(armio, kIODTExclusive, "('gic')"));
		armio->release();
	}
}

/*
 * hw.target / hw.product come from IODTPlatformExpert: the first root
 * "compatible" string and "model" (D22AP / iPhone10,3 in the ADT), which
 * is what the iPhone userland (MobileGestalt) expects.
 */
#endif /* SWEET */
