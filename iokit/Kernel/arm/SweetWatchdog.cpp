/*
 * Built-in IOWatchdog for SWEET.
 *
 * watchdogd (launchd job with _PanicOnCrash) opens IOServiceMatching(
 * "IOWatchdog") with type 1 and exits when it is missing, which panics the
 * system. On Apple hardware the kext arms a SoC watchdog that resets the
 * device unless watchdogd keeps checking in. SWEET has no reset path, so
 * this service accepts every call (check-in 1, enable/disable userspace
 * monitoring 3/4, 0/5, struct calls 2 and 9) and does nothing; each
 * selector is logged the first time it is used.
 *
 * watchdogd also writes the device boot stage to the PMU: it looks up
 * the IOService with IOPMUPrimary = 1 and crashes when there is none
 * (customer builds ignore -wdt_no_pmu). SWEET has no Apple PMU, so this
 * service carries IOPMUPrimary and accepts property writes.
 */
#include <pexpert/arm64/board_config.h>

#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <sys/proc.h>

class IOWatchdog : public IOService {
	OSDeclareDefaultStructors(IOWatchdog);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOWatchdog, IOService);

IOReturn
IOWatchdog::setProperties(__unused OSObject *properties)
{
	return kIOReturnSuccess;
}

bool
IOWatchdog::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "IOWatchdogUserClient");
	setProperty("IOPMUPrimary", 1ULL, 32);
	registerService();
	IOLog("SWEET WDT: IOWatchdog registered (accepts check-ins, never fires)\n");
	return true;
}

class IOWatchdogUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOWatchdogUserClient);

public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOWatchdogUserClient, IOUserClient);

IOReturn
IOWatchdogUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

static uint64_t sweet_wdt_logged;       /* selectors already logged (< 64) */

IOReturn
IOWatchdogUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = 0;
	}
	if (args->structureOutputDescriptor != NULL) {
		IOByteCount len = args->structureOutputDescriptor->getLength();
		for (IOByteCount off = 0; off < len; off += 8) {
			static const uint64_t zero = 0;
			args->structureOutputDescriptor->writeBytes(off, &zero, len - off < 8 ? len - off : 8);
		}
	} else if (args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}

	if (selector >= 64 || !(sweet_wdt_logged & (1ULL << selector))) {
		if (selector < 64) {
			sweet_wdt_logged |= 1ULL << selector;
		}
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("SWEET WDT: %s sel %u in %u/%u out %u/%u -> ok\n", name, selector,
		    args->scalarInputCount, args->structureInputSize,
		    args->scalarOutputCount, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}
#endif /* SWEET */
