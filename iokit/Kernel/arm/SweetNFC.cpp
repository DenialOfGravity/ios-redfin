/*
 * redfin 1119: NFC P1, the IOKit half (bsd/dev/sweet_nfc.c holds the switch, the kernel control and the gateway).
 *
 * iOS 16.7's libNFC_HAL finds the NFC controller with IOServiceNameMatching("AppleStockholmControl") plus
 * IOPropertyMatch { IONameMatched = "nfc,primary,gpio" } (sub_1f5de1650 in NFHardwareSerialGetDefaultInterfaceType,
 * retried 100 x 100 ms after a slow tree walk), then IOServiceOpen(type 0) and IOConnectCallScalarMethod(sel, 1 in).
 * On an iPhone the kext matched the device-tree nub /arm-io/uart3/stockholm by IONameMatch "nfc,primary,gpio" and
 * so carries IONameMatched; here a kernel nub "stockholm" (the D22 node's plain properties, no GPIO phandles, no
 * rf-config-tlvs / rf-clock-tlv / calibration, required-gpios without support_download_request) and an
 * AppleStockholmControl on it with that property set by hand stand in. No ADT change, no new image needed for the
 * name lookup. Published and withdrawn only by sweet_nfc.c (kern.sweet_nfc_p1 / boot-arg sweetnfc bit 2), whose
 * p1 lock serialises the calls. Property reads on both are logged (IOUserClient.cpp, "SWEET NFC: ... reads").
 *
 * AppleStockholmControlUserClient selectors (libNFC_HAL _NFHardwareGPIOSetBoolean): 0 Power (VEN, gpio12),
 * 1 DownloadRequest (1 is REFUSED: kIOReturnNotPermitted), 2 PMU standby, 3 DeviceWake, 5 VBAT (accepted no-ops),
 * 6/7/8 SPMI only (kIOReturnUnsupported). All decided and logged in sweet_nfc_uc_call().
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOKitKeys.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>

extern "C" {
#include <sys/errno.h>
int SweetNFCPublish(bool on, const char *why);
bool SweetNFCIs(const void *entry);
int sweet_nfc_uc_call(uint32_t sel, uint32_t nin, uint64_t v);
void sweet_nfc_uc_event(int open, uint32_t type);
}

class SweetStockholmNub : public IOService
{
	OSDeclareDefaultStructors(SweetStockholmNub);
};
OSDefineMetaClassAndStructors(SweetStockholmNub, IOService);

class AppleStockholmControl : public IOService
{
	OSDeclareDefaultStructors(AppleStockholmControl);
};
OSDefineMetaClassAndStructors(AppleStockholmControl, IOService);

class AppleStockholmControlUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AppleStockholmControlUserClient);
	UInt32 fType;
public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
	    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AppleStockholmControlUserClient, IOUserClient);

bool
AppleStockholmControlUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fType = type;
	sweet_nfc_uc_event(1, type);
	return true;
}

IOReturn
AppleStockholmControlUserClient::clientClose(void)
{
	sweet_nfc_uc_event(0, fType);
	terminate();
	return kIOReturnSuccess;
}

IOReturn
AppleStockholmControlUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	uint64_t v = (args->scalarInputCount && args->scalarInput) ? args->scalarInput[0] : 0;
	int r = sweet_nfc_uc_call(selector, args->scalarInputCount, v);

	for (uint32_t i = 0; args->scalarOutput && i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = 0;
	}
	switch (r) {
	case 0:
		return kIOReturnSuccess;
	case 1:
		return kIOReturnNotPermitted;
	case 2:
		return kIOReturnUnsupported;
	default:
		return kIOReturnBadArgument;
	}
}

static IOService *nfc_nub, *nfc_ctl;

static void
put_data(IORegistryEntry *e, const char *key, const void *bytes, unsigned len)
{
	OSData *v = OSData::withBytes(bytes, len);
	if (v) {
		e->setProperty(key, v);
		v->release();
	}
}

/* D22AP /arm-io/uart3/stockholm, plain properties only (NUL-terminated strings, as the device tree stores them) */
static void
put_stockholm(IORegistryEntry *e)
{
	static const char compat[] = "nfc,primary,gpio";
	static const char rgpio[] = "support_wake_stockholm|support_device_wake|support_venable";
	static const uint8_t one[4] = { 1, 0, 0, 0 };

	put_data(e, "compatible", compat, sizeof(compat));
	put_data(e, "device_type", "stockholm", 10);
	put_data(e, "nfcWithRadio", one, sizeof(one));
	put_data(e, "supports-nfc-reader-mode", "", 0);
	put_data(e, "rf-antenna-name", "RTM4_1", 7);
	put_data(e, "required-gpios", rgpio, sizeof(rgpio));
	e->setProperty("SweetNFC", true);
}

static bool
start_on(IOService *svc, IOService *provider, const char *name)
{
	if (!svc->attach(provider)) {
		svc->release();
		return false;
	}
	svc->setName(name);
	if (!svc->start(provider)) {
		svc->detach(provider);
		svc->release();
		return false;
	}
	svc->registerService();
	return true;
}

/* 0 ok, EALREADY in that state already, ENXIO no platform expert yet, ENOMEM. Caller: sweet_nfc.c, p1 lock held. */
int
SweetNFCPublish(bool on, const char *why)
{
	if (!on) {
		if (nfc_ctl == NULL && nfc_nub == NULL) {
			return EALREADY;
		}
		IOService *c = nfc_ctl, *n = nfc_nub;
		nfc_ctl = NULL;
		nfc_nub = NULL;
		if (c) {
			c->terminate(kIOServiceSynchronous);
			c->release();
		}
		if (n) {
			n->terminate(kIOServiceSynchronous);
			n->release();
		}
		IOLog("SWEET NFC: AppleStockholmControl and the stockholm nub withdrawn (%s)\n", why);
		return 0;
	}
	if (nfc_ctl != NULL) {
		return EALREADY;
	}
	IOService *platform = IOService::getPlatform();
	if (platform == NULL) {
		return ENXIO;
	}
	SweetStockholmNub *nub = OSTypeAlloc(SweetStockholmNub);
	if (nub == NULL || !nub->init()) {
		OSSafeReleaseNULL(nub);
		return ENOMEM;
	}
	put_stockholm(nub);
	if (!start_on(nub, platform, "stockholm")) {
		return ENOMEM;
	}
	AppleStockholmControl *ctl = OSTypeAlloc(AppleStockholmControl);
	if (ctl == NULL || !ctl->init()) {
		OSSafeReleaseNULL(ctl);
		nub->terminate(kIOServiceSynchronous);
		nub->release();
		return ENOMEM;
	}
	put_stockholm(ctl);
	ctl->setProperty("IONameMatched", "nfc,primary,gpio");     /* what libNFC_HAL's IOPropertyMatch asks for */
	ctl->setProperty(kIOUserClientClassKey, "AppleStockholmControlUserClient");
	if (!start_on(ctl, nub, "AppleStockholmControl")) {
		nub->terminate(kIOServiceSynchronous);
		nub->release();
		return ENOMEM;
	}
	nfc_nub = nub;                  /* attach keeps the registry's references; these are ours */
	nfc_ctl = ctl;
	IOLog("SWEET NFC: published stockholm nub under %s and AppleStockholmControl (IONameMatched nfc,primary,gpio, "
	    "user client AppleStockholmControlUserClient) (%s); no AppleStockholmSPMI: nfcd picks interface type 1, "
	    "com.apple.uart.stockholm\n", platform->getName(), why);
	return 0;
}

bool
SweetNFCIs(const void *entry)
{
	return entry != NULL && (entry == nfc_nub || entry == nfc_ctl);
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
