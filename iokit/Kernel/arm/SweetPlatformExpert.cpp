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
