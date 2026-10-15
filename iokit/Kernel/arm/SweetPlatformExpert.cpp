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
	bool getTargetName(char *name, int maxLength) APPLE_KEXT_OVERRIDE;
	bool getProductName(char *name, int maxLength) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetPlatformExpert, IODTPlatformExpert);

bool
SweetPlatformExpert::start(IOService *provider)
{
	IOLog("SWEET: starting device-tree platform expert\n");
	if (!IODTPlatformExpert::start(provider)) {
		return false;
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
}

bool
SweetPlatformExpert::getTargetName(char *name, int maxLength)
{
	if (name == NULL || maxLength <= 0) {
		return false;
	}
	strlcpy(name, "sweet", (size_t)maxLength);
	return true;
}

bool
SweetPlatformExpert::getProductName(char *name, int maxLength)
{
	if (name == NULL || maxLength <= 0) {
		return false;
	}
	strlcpy(name, "Redmi Note 10 Pro", (size_t)maxLength);
	return true;
}
#endif /* SWEET */
