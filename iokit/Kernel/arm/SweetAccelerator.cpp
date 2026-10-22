/*
 * The GPU as Metal sees it. Metal.framework (MTLRegisterDevices) iterates
 * IOServiceMatching("IOAcceleratorES"), loads
 * /System/Library/Extensions/<MetalPluginName>.bundle, takes the class named
 * MetalPluginClassName (a subclass of Metal's _MTLDevice) and calls
 * -initWithAcceleratorPort: with the service. SweetAccelerator publishes
 * itself under that class name with the properties that point Metal at
 * SweetMetal.bundle (phone/metal), whose device drives the Adreno 640
 * through bsd/dev/sweet_gpu.c.
 *
 * Boot-arg sweetmtl=1 publishes it; without it Metal finds no device and
 * Core Animation keeps its software renderer.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

class IOAcceleratorES : public IOService {
	OSDeclareDefaultStructors(IOAcceleratorES);
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

class SweetAccelerator : public IOAcceleratorES {
	OSDeclareDefaultStructors(SweetAccelerator);

public:
	IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetAccelerator, IOAcceleratorES);

IOService *
SweetAccelerator::probe(IOService *provider, SInt32 *score)
{
	uint32_t on = 0;
	PE_parse_boot_argn("sweetmtl", &on, sizeof(on));
	return on ? IOService::probe(provider, score) : NULL;
}

bool
SweetAccelerator::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty("MetalPluginName", "SweetMetal");
	setProperty("MetalPluginClassName", "SweetMTLDevice");
	setProperty("IOGLBundleName", "SweetMetal");
	setProperty("model", "Adreno 640");
	registerService();
	IOLog("SWEET MTL: IOAcceleratorES published (MetalPluginName SweetMetal)\n");
	return true;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
