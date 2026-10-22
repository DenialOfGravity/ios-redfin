/*
 * A stand-in for AppleMobileFileIntegrity's IOService, which this kernel
 * does not have (no AMFI kext). Userland looks it up by class name:
 * ExtensionFoundation's +[NSDictionary _EX_dictionaryWithSignedURL:] reads
 * signed caches (the extension point cache) only after asking AMFI whether
 * the file's cdhash is in the trust cache. Without an answer, sandboxed
 * readers (lsd) get no extension point cache, so MobileInstallation rejects
 * every app extension ("Extension cache does not have an entry for ..."),
 * LaunchServices registers none, and e.g. PosterBoard finds no poster
 * providers (black wallpaper).
 *
 * User client, IOConnectCallMethod:
 *   6 trust cache query  [1] + struct cdhash -> 0 (trusted)
 * Every cdhash is trusted: code signing is not enforced here anyway, and
 * there is no trust cache to look in. Other selectors are logged once and
 * answered kIOReturnUnsupported, which is what their callers already got
 * from the missing service. Boot-arg sweetamfi=0 leaves the service out.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <pexpert/pexpert.h>

class AppleMobileFileIntegrity : public IOService {
	OSDeclareDefaultStructors(AppleMobileFileIntegrity);

public:
	IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleMobileFileIntegrity, IOService);

IOService *
AppleMobileFileIntegrity::probe(IOService *provider, SInt32 *score)
{
	uint32_t on = 1;
	PE_parse_boot_argn("sweetamfi", &on, sizeof(on));
	return on ? IOService::probe(provider, score) : NULL;
}

bool
AppleMobileFileIntegrity::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "AppleMobileFileIntegrityUserClient");
	registerService();
	IOLog("SWEET AMFI: AppleMobileFileIntegrity published (trust cache queries answer trusted)\n");
	return true;
}

class AppleMobileFileIntegrityUserClient : public IOUserClient {
	OSDeclareDefaultStructors(AppleMobileFileIntegrityUserClient);

public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleMobileFileIntegrityUserClient, IOUserClient);

IOReturn
AppleMobileFileIntegrityUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn
AppleMobileFileIntegrityUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
	static uint32_t logged;	/* one line per selector */
	if (selector == 6) {
		if (!(logged & (1u << 6))) {
			logged |= 1u << 6;
			IOLog("SWEET AMFI: trust cache query (%u-byte cdhash): trusted\n", args->structureInputSize);
		}
		return kIOReturnSuccess;
	}
	if (selector < 32 && !(logged & (1u << selector))) {
		logged |= 1u << selector;
		IOLog("SWEET AMFI: selector %u (in %u/%u) unsupported\n", selector, args->scalarInputCount,
		    args->structureInputSize);
	}
	return kIOReturnUnsupported;
}
#endif
