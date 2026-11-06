/*
 * hmbench: one Metal-API workload on either of the phone's two Metal implementations, in a process of its own.
 *
 *   hmbench --device m2v|hm [--frames N] [--warm N] [--only W1,W2,..] [--dump DIR] [--log FILE] [--lib FILE]
 *
 * The device is made the way Metal.framework makes it (MTLRegisterDevices): the IOAcceleratorES service's port goes
 * to -initWithAcceleratorPort: of the plug-in's device class, loaded from its bundle with NSBundle. Nothing of the
 * system is changed: backboardd keeps its own device; this is one more v2 client of the kernel's GPU service.
 *   m2v  /System/Library/Extensions/SweetMetal2.bundle, SweetMTL2Device (metal2vulkan + Turnip; read-only use, the
 *        process has to be listed in /var/local/mtl2-clients)
 *   hm   /var/local/hmetal/HakiosMetal.bundle, HakiosMetalDevice (hakios-metal on NativeAdreno; bench/build.sh)
 * Both plug-ins register Objective-C classes of their own, so one process loads one of them.
 *
 * Workloads, all into an offscreen BGRA8 1080x2340 IOSurface-backed render target (a CoreAnimation framebuffer's shape):
 *   W1 an empty command buffer, commit + wait
 *   W2 one render pass that clears the target and draws nothing
 *   W3 a cleared pass with 40 textured, alpha-blended quads (one draw each; 256x256 RGBA8 texture, linear sampler)
 *   W4 a cleared pass with 200 small vertex-coloured quads (one draw each): the CPU cost of a draw
 * Per workload, after the warm-up: CPU encode us/frame (command buffer through -endEncoding) and us/draw, the commit
 * call's cost, commit->completed ms (completion handler), the GPU's own span (GPUStartTime/GPUEndTime where the
 * device gives both), frames/s with one frame in flight (commit, wait) and with three (a semaphore of 3), and the
 * final frame's FNV-1a checksum (rows of the surface, padding excluded) with 8 sample pixels, so that the two
 * devices can be shown to draw the same picture. --dump DIR writes the final frame (raw BGRA rows, 1080*4 bytes each).
 *
 * The shaders are hakios-metal's own hand-written AIR (tools/kernels/tri_attr_vs, tri_tex_fs, tri_color_fs; MIT),
 * put into a metallib by bench/mkmetallib.py: no Apple metallib anywhere. Ours, MIT.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define W 1080
#define H 2340
#define TEX 256

/* what Metal.framework sends a plug-in's device class (MTLRegisterDevices) */
@interface NSObject (HMBenchPlugin)
- (id)initWithAcceleratorPort:(io_service_t)port;
@end

static FILE *gLog;
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
	if (gLog) {
		va_start(ap, fmt);
		vfprintf(gLog, fmt, ap);
		va_end(ap);
		fflush(gLog);
	}
}

static double gTick;
static double now_us(void) { return (double)mach_absolute_time() * gTick; }

struct vtx { float x, y; float a[4]; };   /* tri_attr_vs: position Float2 @0, attribute 1 Float4 @8; stride 24 */

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

struct stat5 { double mean, p50, p90, min, max; };
static struct stat5 stats(double *v, int n)
{
	struct stat5 s = { 0 };
	if (n <= 0)
		return s;
	double sum = 0;
	for (int i = 0; i < n; i++)
		sum += v[i];
	qsort(v, n, sizeof(double), cmp_d);
	s.mean = sum / n;
	s.p50 = v[n / 2];
	s.p90 = v[(n * 9) / 10 < n ? (n * 9) / 10 : n - 1];
	s.min = v[0];
	s.max = v[n - 1];
	return s;
}

static void quad(struct vtx *v, float x0, float y0, float x1, float y1, const float c0[4], const float c1[4], int uv)
{
	/* two triangles; attribute 1 is the texture coordinate (uv=1: (u, v, 0, 1)) or the colour, per corner */
	float px[6] = { x0, x1, x0, x0, x1, x1 }, py[6] = { y0, y0, y1, y1, y0, y1 };
	float u[6] = { 0, 1, 0, 0, 1, 1 }, w[6] = { 1, 1, 0, 0, 1, 0 };
	for (int i = 0; i < 6; i++) {
		v[i].x = px[i];
		v[i].y = py[i];
		if (uv) {
			v[i].a[0] = u[i];
			v[i].a[1] = w[i];
			v[i].a[2] = 0;
			v[i].a[3] = 1;
		} else {
			float t = u[i] * 0.5f + (1 - w[i]) * 0.5f;
			for (int k = 0; k < 4; k++)
				v[i].a[k] = c0[k] + (c1[k] - c0[k]) * t;
		}
	}
}

static void dcivac(const void *p, size_t n)
{
#if defined(__arm64__) || defined(__aarch64__)
	uintptr_t a = (uintptr_t)p & ~(uintptr_t)63, e = (uintptr_t)p + n;
	for (; a < e; a += 64)
		__asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
#else
	(void)p;
	(void)n;
#endif
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		const char *which = NULL, *only = NULL, *dump = NULL, *logpath = NULL;
		const char *libpath = "/var/local/hmetal/hmbench.metallib", *reg = NULL;
		int frames = 300, warm = 30;
		for (int i = 1; i < argc; i++) {
			if (!strcmp(argv[i], "--device") && i + 1 < argc) which = argv[++i];
			else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
			else if (!strcmp(argv[i], "--warm") && i + 1 < argc) warm = atoi(argv[++i]);
			else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
			else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
			else if (!strcmp(argv[i], "--log") && i + 1 < argc) logpath = argv[++i];
			else if (!strcmp(argv[i], "--lib") && i + 1 < argc) libpath = argv[++i];
			else if (!strcmp(argv[i], "--register") && i + 1 < argc) reg = argv[++i];
			else { fprintf(stderr, "usage: hmbench --device m2v|hm [--frames N] [--warm N] [--only W1,..] [--dump DIR] [--log FILE] [--lib F]\n"); return 2; }
		}
		if (!which || (strcmp(which, "m2v") && strcmp(which, "hm"))) {
			fprintf(stderr, "hmbench: --device m2v|hm\n");
			return 2;
		}
		if (frames < 1) frames = 1;
		char lp[256];
		if (!logpath) {
			snprintf(lp, sizeof(lp), "/var/local/hmetal/bench-%s.log", which);
			logpath = lp;
		}
		gLog = fopen(logpath, "w");
		mach_timebase_info_data_t tb;
		mach_timebase_info(&tb);
		gTick = (double)tb.numer / tb.denom / 1000.0;

		const char *bundlePath, *className;
		if (!strcmp(which, "m2v")) {
			bundlePath = "/System/Library/Extensions/SweetMetal2.bundle";
			className = "SweetMTL2Device";
		} else {
			bundlePath = "/var/local/hmetal/HakiosMetal.bundle";
			className = "HakiosMetalDevice";
			setenv("HAKIOS_METAL_ALL", "1", 1);     /* bench/plugin.py: the device for this process too, not backboardd alone */
			if (!getenv("HAKIOS_METAL_CACHE")) {
				mkdir("/var/local/hmetal/cache", 0777);
				setenv("HAKIOS_METAL_CACHE", "/var/local/hmetal/cache", 1);
			}
		}
		say("hmbench: device %s (%s, class %s), %d frames after %d warm-up, target BGRA8 %dx%d IOSurface\n", which,
		    bundlePath, className, frames, warm, W, H);

		io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("IOAcceleratorES"));
		if (!svc) { say("hmbench: no IOAcceleratorES service\n"); return 1; }
		CFTypeRef pn = IORegistryEntryCreateCFProperty(svc, CFSTR("MetalPluginName"), kCFAllocatorDefault, 0);
		CFTypeRef pc = IORegistryEntryCreateCFProperty(svc, CFSTR("MetalPluginClassName"), kCFAllocatorDefault, 0);
		say("hmbench: IOAcceleratorES 0x%x advertises %s / %s (the system's device; not used here)\n", svc,
		    pn ? [(__bridge NSString *)pn UTF8String] : "-", pc ? [(__bridge NSString *)pc UTF8String] : "-");

		double t0 = now_us();
		NSBundle *bundle = [NSBundle bundleWithPath:@(bundlePath)];
		NSError *err = nil;
		if (!bundle || ![bundle loadAndReturnError:&err]) {
			say("hmbench: bundle %s does not load: %s\n", bundlePath, err ? err.description.UTF8String : "no bundle");
			return 1;
		}
		Class cls = NSClassFromString(@(className));
		if (!cls) { say("hmbench: no class %s in the bundle\n", className); return 1; }
		/* an init-family call, so that ARC hands the receiver over as the plug-ins expect (SweetMTL2Device returns
		 * another object and lets go of self: a plain objc_msgSend here released it a second time) */
		id<MTLDevice> dev = [[cls alloc] initWithAcceleratorPort:svc];
		double tDev = now_us() - t0;
		if (!dev) { say("hmbench: -initWithAcceleratorPort: gave nil (see the plug-in's log)\n"); return 1; }
		say("hmbench: device \"%s\" (%s) in %.1f ms\n", dev.name.UTF8String, object_getClassName(dev), tDev / 1000);
		/* What Metal.framework does next with a plug-in's device (MTLRegisterDevices -> MTLAddDevice): it checks the class
		 * and fills the base class's limits (supportsFamily: and friends read them; unfilled, a NULL read). Only for a
		 * device that is an _MTLDevice of the system's: SweetMetal2 hands out the frontend's own object and needs none.
		 * --register SEL,SEL.. | none (default: hm initLimits,initGPUFamilySupport,initFeatureQueries; m2v none) */
		{
			Class base = NSClassFromString(@"_MTLDevice");
			if (base && [(id)dev isKindOfClass:base]) {
				unsigned n = 0;
				Method *m = class_copyMethodList(base, &n);
				say("hmbench: _MTLDevice's init/limit/family/feature methods:");
				for (unsigned i = 0; i < n; i++) {
					const char *nm = sel_getName(method_getName(m[i]));
					if (strcasestr(nm, "limit") || strcasestr(nm, "famil") || strcasestr(nm, "feature") || !strncmp(nm, "init", 4) ||
					    !strncmp(nm, "_init", 5) || strcasestr(nm, "setup"))
						say(" %s", nm);
				}
				say("\n");
				free(m);
				/* MTLAddDevice itself asserts on Metal's device array, which only MTLCopyAllDevices makes (and that would
				 * open the system's SweetMetal2 device in this process too): its setup messages are sent instead */
				if (!reg)
					reg = strcmp(which, "hm") ? "none" : "initLimits,initGPUFamilySupport,initFeatureQueries";
				char list[256];
				snprintf(list, sizeof(list), "%s", reg);
				for (char *save = NULL, *nm = strtok_r(list, ",", &save); nm; nm = strtok_r(NULL, ",", &save)) {
					if (!strcmp(nm, "none"))
						break;
					SEL sel = sel_registerName(nm);
					if ([(id)dev respondsToSelector:sel]) {
						((void (*)(id, SEL))objc_msgSend)((id)dev, sel);
						say("hmbench: -[_MTLDevice %s] sent (MTLAddDevice's setup)\n", nm);
					} else
						say("hmbench: the device does not answer -%s\n", nm);
				}
			}
		}

		id<MTLCommandQueue> q = [dev newCommandQueue];
		if (!q) { say("hmbench: no command queue\n"); return 1; }

		/* the render target: an IOSurface as CoreAnimation's framebuffer is one, rows 64-byte aligned */
		NSDictionary *sp = @{ (__bridge NSString *)kIOSurfaceWidth: @W, (__bridge NSString *)kIOSurfaceHeight: @H,
				      (__bridge NSString *)kIOSurfaceBytesPerElement: @4,
				      (__bridge NSString *)kIOSurfaceBytesPerRow: @((W * 4 + 255) & ~255),
				      (__bridge NSString *)kIOSurfacePixelFormat: @((uint32_t)'BGRA') };
		IOSurfaceRef surf = IOSurfaceCreate((__bridge CFDictionaryRef)sp);
		if (!surf) { say("hmbench: IOSurfaceCreate failed\n"); return 1; }
		size_t rowBytes = IOSurfaceGetBytesPerRow(surf);
		MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
											   width:W height:H mipmapped:NO];
		td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModeShared;
		id<MTLTexture> rt = [dev newTextureWithDescriptor:td iosurface:surf plane:0];
		if (!rt) { say("hmbench: no texture for the IOSurface\n"); return 1; }
		say("hmbench: target IOSurface %u, %zu bytes/row\n", IOSurfaceGetID(surf), rowBytes);

		/* the library and the pipelines */
		NSData *libData = [NSData dataWithContentsOfFile:@(libpath)];
		if (!libData) { say("hmbench: no %s\n", libpath); return 1; }
		dispatch_data_t dd = dispatch_data_create(libData.bytes, libData.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
		t0 = now_us();
		id<MTLLibrary> lib = [dev newLibraryWithData:dd error:&err];
		if (!lib) { say("hmbench: newLibraryWithData: %s\n", err.description.UTF8String); return 1; }
		say("hmbench: library: %s (%.1f ms)\n", [[lib.functionNames componentsJoinedByString:@" "] UTF8String], (now_us() - t0) / 1000);
		id<MTLFunction> vs = [lib newFunctionWithName:@"tri_attr_vs"], fsTex = [lib newFunctionWithName:@"tri_tex_fs"],
			fsCol = [lib newFunctionWithName:@"tri_color_fs"];
		if (!vs || !fsTex || !fsCol) { say("hmbench: a function is missing\n"); return 1; }

		MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
		vd.attributes[0].format = MTLVertexFormatFloat2;
		vd.attributes[0].offset = 0;
		vd.attributes[0].bufferIndex = 0;
		vd.attributes[1].format = MTLVertexFormatFloat4;
		vd.attributes[1].offset = 8;
		vd.attributes[1].bufferIndex = 0;
		vd.layouts[0].stride = sizeof(struct vtx);
		vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

		id<MTLRenderPipelineState> pTex = nil, pCol = nil;
		{
			MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
			d.vertexFunction = vs;
			d.fragmentFunction = fsTex;
			d.vertexDescriptor = vd;
			d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
			d.colorAttachments[0].blendingEnabled = YES;
			d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
			d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
			d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
			d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
			d.label = @"hmbench.tex";
			t0 = now_us();
			pTex = [dev newRenderPipelineStateWithDescriptor:d error:&err];
			say("hmbench: pipeline tri_attr_vs+tri_tex_fs (blend): %s (%.1f ms)\n",
			    pTex ? "ok" : err.description.UTF8String, (now_us() - t0) / 1000);
			d.fragmentFunction = fsCol;
			d.colorAttachments[0].blendingEnabled = NO;
			d.label = @"hmbench.color";
			t0 = now_us();
			pCol = [dev newRenderPipelineStateWithDescriptor:d error:&err];
			say("hmbench: pipeline tri_attr_vs+tri_color_fs: %s (%.1f ms)\n",
			    pCol ? "ok" : err.description.UTF8String, (now_us() - t0) / 1000);
		}

		/* the source texture: 256x256 RGBA8, a pattern with alpha from 96 to 255 */
		MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
											   width:TEX height:TEX mipmapped:NO];
		sd.usage = MTLTextureUsageShaderRead;
		sd.storageMode = MTLStorageModeShared;
		id<MTLTexture> src = [dev newTextureWithDescriptor:sd];
		{
			static uint8_t px[TEX * TEX * 4];
			for (int y = 0; y < TEX; y++)
				for (int x = 0; x < TEX; x++) {
					uint8_t *p = px + (y * TEX + x) * 4;
					p[0] = (uint8_t)x;
					p[1] = (uint8_t)y;
					p[2] = (uint8_t)(((x >> 5) ^ (y >> 5)) & 1 ? 230 : 40);
					p[3] = (uint8_t)(96 + ((x + y) * 159) / (2 * (TEX - 1)));
				}
			[src replaceRegion:MTLRegionMake2D(0, 0, TEX, TEX) mipmapLevel:0 withBytes:px bytesPerRow:TEX * 4];
		}
		MTLSamplerDescriptor *smd = [MTLSamplerDescriptor new];
		smd.minFilter = MTLSamplerMinMagFilterLinear;
		smd.magFilter = MTLSamplerMinMagFilterLinear;
		smd.sAddressMode = MTLSamplerAddressModeClampToEdge;
		smd.tAddressMode = MTLSamplerAddressModeClampToEdge;
		id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:smd];

		/* geometry: W3 40 quads (5 x 8, overlapping), W4 200 small quads (10 x 20) */
		enum { N3 = 40, N4 = 200 };
		id<MTLBuffer> vb3 = [dev newBufferWithLength:sizeof(struct vtx) * 6 * N3 options:MTLResourceStorageModeShared];
		id<MTLBuffer> vb4 = [dev newBufferWithLength:sizeof(struct vtx) * 6 * N4 options:MTLResourceStorageModeShared];
		{
			struct vtx *v = vb3.contents;
			static const float z[4] = { 0 };
			for (int i = 0; i < N3; i++) {
				int cx = i % 5, cy = i / 5;
				float x0 = -1.0f + cx * 0.38f, y0 = -1.0f + cy * 0.235f;
				quad(v + 6 * i, x0, y0, x0 + 0.5f, y0 + 0.31f, z, z, 1);
			}
			v = vb4.contents;
			for (int i = 0; i < N4; i++) {
				int cx = i % 10, cy = i / 10;
				float x0 = -0.98f + cx * 0.196f, y0 = -0.98f + cy * 0.098f;
				float c0[4] = { cx / 9.0f, cy / 19.0f, 0.25f, 1 }, c1[4] = { 1 - cx / 9.0f, 0.5f, cy / 19.0f, 1 };
				quad(v + 6 * i, x0, y0, x0 + 0.15f, y0 + 0.07f, c0, c1, 0);
			}
		}

		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = rt;
		rp.colorAttachments[0].loadAction = MTLLoadActionClear;
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		rp.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.2, 0.3, 1.0);

		say("hmbench: setup done; workloads follow\n");
		say("RESULT  dev  wl  draws  enc_us(mean/p50/p90)  us/draw  commit_us  done_ms(mean/p50/p90)  gpu_ms(mean/p50)  fps1  fps3  checksum\n");

		const char *names[4] = { "W1", "W2", "W3", "W4" };
		for (int wl = 0; wl < 4; wl++) {
			if (only && !strstr(only, names[wl]))
				continue;
			int draws = wl == 2 ? N3 : wl == 3 ? N4 : 0;
			id<MTLRenderPipelineState> pso = wl == 2 ? pTex : wl == 3 ? pCol : nil;
			if (wl >= 2 && !pso) {
				say("RESULT  %s  %s  skipped: no pipeline\n", which, names[wl]);
				continue;
			}
			id<MTLBuffer> vb = wl == 2 ? vb3 : vb4;
			double *enc = calloc(frames, sizeof(double)), *com = calloc(frames, sizeof(double)),
			       *done = calloc(frames, sizeof(double)), *gpu = calloc(frames, sizeof(double));
			__block int ngpu = 0;
			__block int errors = 0;
			__block NSString *firstErr = nil;
			/* one frame: returns the command buffer, committed; tEnc, tCommit filled */
			id<MTLCommandBuffer> (^frame)(double *, double *) = ^id<MTLCommandBuffer>(double *tEnc, double *tCommit) {
				double a = now_us();
				id<MTLCommandBuffer> cb = [q commandBuffer];
				if (wl >= 1) {
					id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
					if (pso) {
						[e setRenderPipelineState:pso];
						[e setVertexBuffer:vb offset:0 atIndex:0];
						if (wl == 2) {
							[e setFragmentTexture:src atIndex:0];
							[e setFragmentSamplerState:smp atIndex:0];
						}
						for (int d = 0; d < draws; d++)
							[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:6 * d vertexCount:6];
					}
					[e endEncoding];
				}
				double b = now_us();
				[cb commit];
				double c = now_us();
				if (tEnc) *tEnc = b - a;
				if (tCommit) *tCommit = c - b;
				return cb;
			};
			int bad = 0;
			for (int i = 0; i < warm; i++) {
				id<MTLCommandBuffer> cb = frame(NULL, NULL);
				[cb waitUntilCompleted];
				if (cb.status == MTLCommandBufferStatusError)
					bad++;
			}
			/* one frame in flight: commit, wait */
			double s1 = now_us();
			for (int i = 0; i < frames; i++) {
				double te, tc;
				id<MTLCommandBuffer> cb = frame(&te, &tc);
				double c = now_us();
				__block double fin = 0;
				dispatch_semaphore_t sem = dispatch_semaphore_create(0);
				[cb addCompletedHandler:^(id<MTLCommandBuffer> b) { (void)b; fin = now_us(); dispatch_semaphore_signal(sem); }];
				[cb waitUntilCompleted];
				dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5LL * NSEC_PER_SEC));
				enc[i] = te;
				com[i] = tc;
				done[i] = ((fin > 0 ? fin : now_us()) - c) / 1000.0;
				CFTimeInterval g0 = cb.GPUStartTime, g1 = cb.GPUEndTime;
				if (g0 > 0 && g1 > g0)
					gpu[ngpu++] = (g1 - g0) * 1000.0;
				if (cb.status == MTLCommandBufferStatusError) {
					errors++;
					if (!firstErr)
						firstErr = cb.error.description;
				}
			}
			double fps1 = frames / ((now_us() - s1) / 1e6);
			/* three frames in flight */
			dispatch_semaphore_t inflight = dispatch_semaphore_create(3);
			double s3 = now_us();
			id<MTLCommandBuffer> last = nil;
			for (int i = 0; i < frames; i++) {
				dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
				double a = now_us();
				id<MTLCommandBuffer> cb = [q commandBuffer];
				[cb addCompletedHandler:^(id<MTLCommandBuffer> b) { (void)b; dispatch_semaphore_signal(inflight); }];
				(void)a;
				if (wl >= 1) {
					id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
					if (pso) {
						[e setRenderPipelineState:pso];
						[e setVertexBuffer:vb offset:0 atIndex:0];
						if (wl == 2) {
							[e setFragmentTexture:src atIndex:0];
							[e setFragmentSamplerState:smp atIndex:0];
						}
						for (int d = 0; d < draws; d++)
							[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:6 * d vertexCount:6];
					}
					[e endEncoding];
				}
				[cb commit];
				last = cb;
			}
			[last waitUntilCompleted];
			for (int i = 0; i < 3; i++)
				dispatch_semaphore_wait(inflight, dispatch_time(DISPATCH_TIME_NOW, 5LL * NSEC_PER_SEC));
			double fps3 = frames / ((now_us() - s3) / 1e6);
			for (int i = 0; i < 3; i++)
				dispatch_semaphore_signal(inflight);

			/* the final frame's picture: rows of the surface, padding excluded */
			char sum[160] = "-";
			if (wl >= 1) {
				IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
				const uint8_t *base = IOSurfaceGetBaseAddress(surf);
				dcivac(base, rowBytes * H);
				uint64_t h = 0xcbf29ce484222325ULL;
				for (int y = 0; y < H; y++) {
					const uint8_t *r = base + (size_t)y * rowBytes;
					for (int x = 0; x < W * 4; x++) {
						h ^= r[x];
						h *= 0x100000001b3ULL;
					}
				}
				int pts[8][2] = { { 10, 10 }, { 540, 1170 }, { 100, 300 }, { 700, 900 }, { 300, 2000 }, { 1000, 2300 }, { 270, 1500 }, { 800, 120 } };
				int n = snprintf(sum, sizeof(sum), "%016llx", (unsigned long long)h);
				for (int k = 0; k < 8 && n < (int)sizeof(sum) - 10; k++) {
					const uint8_t *p = base + (size_t)pts[k][1] * rowBytes + pts[k][0] * 4;
					n += snprintf(sum + n, sizeof(sum) - n, " %02x%02x%02x%02x", p[2], p[1], p[0], p[3]);   /* RGBA */
				}
				if (dump) {
					char path[300];
					snprintf(path, sizeof(path), "%s/bench-%s-%s.bgra", dump, which, names[wl]);
					FILE *f = fopen(path, "wb");
					if (f) {
						for (int y = 0; y < H; y++)
							fwrite(base + (size_t)y * rowBytes, 1, W * 4, f);
						fclose(f);
					}
				}
				IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
			}
			struct stat5 se = stats(enc, frames), sc = stats(com, frames), sd5 = stats(done, frames), sg = stats(gpu, ngpu);
			char gs[48] = "n/a";
			if (ngpu > 0)
				snprintf(gs, sizeof(gs), "%.3f/%.3f", sg.mean, sg.p50);
			say("RESULT  %s  %s  %d  %.1f/%.1f/%.1f  %.2f  %.1f  %.3f/%.3f/%.3f  %s  %.1f  %.1f  %s\n", which, names[wl], draws,
			    se.mean, se.p50, se.p90, draws ? se.mean / draws : 0.0, sc.mean, sd5.mean, sd5.p50, sd5.p90, gs, fps1, fps3, sum);
			if (errors || bad)
				say("hmbench: %s: %d of %d measured frames and %d warm-up frames ended in error (first: %s)\n", names[wl], errors,
				    frames, bad, firstErr ? firstErr.UTF8String : "-");
			free(enc);
			free(com);
			free(done);
			free(gpu);
		}
		say("hmbench: done\n");
	}
	return 0;
}
