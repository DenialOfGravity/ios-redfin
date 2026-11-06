/*
 * m2shim: a stand-in SweetMetal2 executable that lets the window server run on hakios-metal without a reboot (stage H3).
 *
 * The kernel names the Metal plug-in (IOAcceleratorES MetalPluginName/MetalPluginClassName = SweetMetal2 /
 * SweetMTL2Device, boot-arg sweetmtl=2, xnu/patches/0116) and Metal opens /System/Library/Extensions/SweetMetal2.bundle;
 * nothing in SweetMetal2 reads a choice of driver at load. So for the test the bundle's executable is this file, and
 * the real SweetMetal2 bundle is kept, unchanged, at /var/local/SweetMetal2.bundle.pre-hmca:
 *
 *   - backboardd, while /var/local/hmetal/bbd-hm exists: /var/local/hmetal/HakiosMetal.bundle's HakiosMetalDevice
 *     (hakios' CoreAnimation plug-in, bench/plugin.py's edits), its lines also into /var/local/hmetal/bbd-hm.log;
 *   - every other case (every other process; backboardd without the flag; hakios-metal giving no device; the flag
 *     tripped): the real SweetMetal2 (dlopen of its Mach-O, its own SweetMTL2Device by symbol), as before.
 *
 * Trip: each backboardd start on hakios-metal appends its time to /var/local/hmetal/bbd-hm.starts; the third start within
 * 120 s renames the flag to bbd-hm.tripped and takes SweetMetal2, so a window server that keeps dying on hakios-metal
 * comes back on SweetMetal2 by itself. Switching: touch / rm the flag, then psx -k backboardd.
 * Manual reference counting, as the plug-ins.
 */
#import <Foundation/Foundation.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define M2_REAL   "/var/local/SweetMetal2.bundle.pre-hmca/SweetMetal2"
#define HM_BUNDLE "/var/local/hmetal/HakiosMetal.bundle"
#define HM_FLAG   "/var/local/hmetal/bbd-hm"
#define HM_STARTS "/var/local/hmetal/bbd-hm.starts"
#define SHIM_LOG  "/var/local/hmetal/shim.log"

static void
shim_log(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	int n = snprintf(line, sizeof(line), "m2shim[%d %s] %ld: ", getpid(), getprogname(), (long)time(NULL));

	va_start(ap, fmt);
	vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
	va_end(ap);
	strlcat(line, "\n", sizeof(line));
	int fd = open(SHIM_LOG, O_WRONLY | O_APPEND | O_CREAT, 0666);
	if (fd >= 0) {
		(void)!write(fd, line, strlen(line));
		close(fd);
	}
}

/* the third hakios-metal start within 120 s: no more */
static bool
shim_tripped(void)
{
	long now = (long)time(NULL), t, recent = 0;
	FILE *f = fopen(HM_STARTS, "r");

	while (f != NULL && fscanf(f, "%ld", &t) == 1) {
		if (now - t < 120) recent++;
	}
	if (f != NULL) fclose(f);
	if (recent >= 2) {
		rename(HM_FLAG, HM_FLAG ".tripped");
		shim_log("tripped: %ld hakios-metal starts in 120 s before this one; flag -> bbd-hm.tripped, SweetMetal2", recent);
		return true;
	}
	if ((f = fopen(HM_STARTS, "a")) != NULL) {
		fprintf(f, "%ld\n", now);
		fclose(f);
	}
	return false;
}

@interface _MTLDevice : NSObject
@end

@interface SweetMTL2Device : _MTLDevice
- (id)initWithAcceleratorPort:(mach_port_t)port;
@end

@implementation SweetMTL2Device
- (id)initWithAcceleratorPort:(mach_port_t)port
{
	id device = nil;

	/* (self not released: before -init, _MTLDevice's -dealloc reads what -init never set; both plug-ins do the same) */
	if (strcmp(getprogname(), "backboardd") == 0 && access(HM_FLAG, F_OK) == 0 && !shim_tripped()) {
		setenv("HAKIOS_METAL_LOG", "/var/local/hmetal/bbd-hm.log", 1);
		NSBundle *b = [NSBundle bundleWithPath:@HM_BUNDLE];
		NSError *e = nil;
		if (b != nil && [b loadAndReturnError:&e]) {
			Class cls = NSClassFromString(@"HakiosMetalDevice");
			device = cls != Nil ? ((id (*)(id, SEL, mach_port_t))objc_msgSend)([cls alloc], @selector(initWithAcceleratorPort:), port) : nil;
			shim_log("hakios-metal: class %s, device %s", cls != Nil ? "found" : "MISSING",
				 device != nil ? [[[device name] description] UTF8String] : "nil");
		} else {
			shim_log("hakios-metal: bundle not loaded: %s", e != nil ? [[e description] UTF8String] : "?");
		}
		if (device != nil) {
			return device;
		}
		shim_log("hakios-metal gave no device: SweetMetal2 instead");
	}
	void *h = dlopen(M2_REAL, RTLD_NOW);
	Class real = h != NULL ? (Class)dlsym(h, "OBJC_CLASS_$_SweetMTL2Device") : Nil;
	if (real == Nil) {
		shim_log("SweetMetal2 not loaded from " M2_REAL ": %s", dlerror() ?: "no class symbol");
		return nil;
	}
	device = ((id (*)(id, SEL, mach_port_t))objc_msgSend)([real alloc], @selector(initWithAcceleratorPort:), port);
	if (strcmp(getprogname(), "backboardd") == 0) {
		shim_log("SweetMetal2: device %s", device != nil ? "yes" : "nil");
	}
	return device;
}
@end
