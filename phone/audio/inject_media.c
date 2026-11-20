/*
 * libredwood-media.dylib: code of ours inside mediaserverd (see inject.c for how it gets there).
 *
 * redfin: this file is HackiOS's (MIT, LICENSE.HackiOS; hackios-xnu-userland a79ff07 userland/src/inject_media.c), built
 * into RedwoodAudio.driver, which mediaserverd loads as a HAL plug-in: no injection needed. Changed for the
 * iPhone X's iOS 16.7.x on the Pixel 5: no iOS 16.5 addresses are trusted (methods replaced by class and selector;
 * the Metal device list and FigFlashlight's callers left alone), the flashlight is sysctl kern.sweet_torch.
 *
 * mediaserverd is the audio server, and also the camera's: the capture stack of CMCapture lives
 * in the same process. Soon after it starts, that stack "prewarms" the Core Image filters of the
 * portrait mode, for which Core Image wants an OpenGL ES context; there is no GPU, the context
 * has no functions behind it, and the daemon jumps to address 0 -- every time launchd restarts
 * it. Nobody is going to take a portrait here: the prewarming is skipped.
 *
 * What the daemon is wanted for meanwhile is neither sound nor the camera: it is where
 * VideoToolbox decodes for everybody else (the service com.apple.coremedia.decompressionsession),
 * and a HEIC picture is HEVC inside. Without the daemon ImageIO has "no way to decode a HEIC",
 * and the stock wallpaper of iOS 16, whose layers are four such pictures, is its gradient alone.
 * There is no hardware decoder here; VideoToolbox takes its software one (VCPHEVC.videocodec).
 *
 * The flashlight (only where the kernel has one: torch=3, /dev/redwood-torch): Control Center's
 * button and AVFlashlight reach FigFlashlight in here, which asks the capture stack's device vendor
 * (+[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor]) for the camera device and its back
 * camera's stream, and sets the stream's "TorchLevel" (0 to 1). Without a camera there is no
 * vendor (it is nil), every message to it does nothing, and FigFlashlight says it has set the level
 * when nothing has been. So where there is no vendor, FigFlashlight -- and only FigFlashlight,
 * by where it calls from -- is given one of ours, with a device and a stream whose torch level goes
 * to the kernel (see flashlight_*() below). Everybody else still gets none, as before.
 *
 * The camera (only where the kernel has one: cam=5, /dev/redwood-camera): the vendor is made for
 * everybody, with the create function of our capture device (camera_device.c) in place of the
 * one H10ISP.mediacapture would give for the iPhone's ISP; the capture graph is built around it and
 * its frames reach Camera.app's preview (see camera_device.c). Its stream takes the torch level too,
 * so FigFlashlight would need nothing of its own then (not tried yet).
 *
 * What the capture stack asks of hardware this phone has none of is refused here: there is no ISP
 * processing session, so there can be no video stabilization (see video_stabilization_method below).
 *
 * CMCapture is not loaded yet when this library is: the methods are replaced when its image
 * arrives.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <dirent.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

void redwood_camera_log_line(const char *line, size_t n);
void redwood_camera_log_check(const char *why);

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
	redwood_camera_log_line(line, (size_t)n);      /* (camera_device.c: /private/var/local/camera.log, when wanted) */
}

/* (SPI of libdyld; looked up at run time, the SDK's stubs need not have it) */
static const void *(*shared_cache_range)(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */

static uintptr_t slide;

/* +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:] */
static void
prewarm_portrait_filter(__unused id self, __unused SEL selector, __unused int version, __unused BOOL styles)
{
}

/* ---- the flashlight ------------------------------------------------------------------------------- */

static int torch = -1;                  /* /dev/redwood-torch */
static bool camera;                     /* /dev/redwood-camera is there */
extern int redwood_camera_device_create(const void *allocator, const void *options, void **out);
static Class vendor_class, device_class, stream_class;
static IMP original_shared_vendor;
static unsigned torch_said;

/*
 * FigFlashlight's functions that ask for the vendor, unslid (20F66): flashlight_IsAvailable,
 * _NotifyForCurrentState and _setupDevice lie together, _teardownDevice apart.
 */
static const struct { uintptr_t from, to; } flashlight_code[] = {
	/* redfin: HackiOS's were iOS 16.5's (20F66: 0x1a1fb0830-0x1a1fb0c5c, 0x1a1eee5c0-0x1a1eee6e8); this phone runs
	 * the iPhone X's 16.7.x, whose are not known yet: none, so FigFlashlight gets a vendor only with the camera's */
	{ 0, 0 },
};

/* +[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor] */
static id
shared_vendor(id self, SEL selector)
{
	uintptr_t caller = (uintptr_t)__builtin_return_address(0) - slide;
	id vendor = ((id (*)(id, SEL))original_shared_vendor)(self, selector);
	static id ours, camera_vendor;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

	if (vendor != NULL) {
		return vendor;
	}
	if (!camera && access("/dev/redwood-camera", F_OK) == 0) {
		camera = true;          /* (should the kernel have made it after mediaserverd started) */
		say("redwood: mediaserverd: the kernel has a camera (/dev/redwood-camera)\n");
	}
	if (camera) {
		pthread_mutex_lock(&lock);
		if (camera_vendor == NULL) {
			id made = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("BWFigCaptureDeviceVendor"), sel_registerName("alloc"));

			camera_vendor = ((id (*)(id, SEL, void *))objc_msgSend)(made, sel_registerName("initWithDeviceCreateFunction:"),
			    (void *)redwood_camera_device_create);
			say("redwood: mediaserverd: the camera's device vendor %s\n", camera_vendor ? "made (camera_device.c)" :
			    "CANNOT BE MADE");
		}
		pthread_mutex_unlock(&lock);
		return camera_vendor;
	}
	for (size_t i = 0; torch >= 0 && i < sizeof(flashlight_code) / sizeof(flashlight_code[0]); i++) {
		if (caller >= flashlight_code[i].from && caller < flashlight_code[i].to) {
			if (ours == NULL) {
				ours = class_createInstance(vendor_class, 0);
				say("redwood: mediaserverd: no camera, so no device vendor: FigFlashlight gets the flashlight's own\n");
			}
			return ours;
		}
	}
	return NULL;
}

static void
flashlight_set(uint32_t level)
{
	/* redfin: sysctl kern.sweet_torch (xnu 0918) in place of HackiOS's /dev/redwood-torch */
	int value = (int)level;
	bool taken = sysctlbyname("kern.sweet_torch", NULL, NULL, &value, sizeof(value)) == 0;

	if (!taken || torch_said < 40) {
		torch_said++;
		say("redwood: mediaserverd: the flashlight at %u of 1000%s\n", level, taken ? "" : " (NOT TAKEN by kern.sweet_torch)");
	}
}

/* the vendor's: one client, one device, always available to it */
static int
flashlight_register(__unused id self, __unused SEL selector, __unused int pid, __unused id description, __unused int priority,
    __unused BOOL steal, __unused BOOL share, __unused id handler)
{
	return 1;
}

static id
flashlight_copy_device(__unused id self, __unused SEL selector, __unused int client, __unused BOOL inform, int *error)
{
	if (error != NULL) {
		*error = 0;
	}
	return class_createInstance(device_class, 0);
}

static id
flashlight_copy_stream(__unused id self, __unused SEL selector, __unused int position, __unused int type, id device)
{
	return device != NULL && object_getClass(device) == device_class ? class_createInstance(stream_class, 0) : NULL;
}

static void
flashlight_take_back(__unused id self, __unused SEL selector, __unused id device, __unused int client, __unused BOOL inform)
{
	flashlight_set(0);
}

static BOOL
flashlight_active_equals(__unused id self, __unused SEL selector, id device)
{
	return device == NULL || object_getClass(device) == device_class;
}

/* the stream's -supportedProperties: none that FigFlashlight looks for ("HighCurrentTorchEnabled") */
static id
flashlight_stream_properties(__unused id self, __unused SEL selector)
{
	static CFDictionaryRef none;

	if (none == NULL) {
		none = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	return (id)none;
}

/* the stream's -setProperty:value: -- "TorchLevel", 0 to 1, as thousandths to the kernel */
static int
flashlight_stream_set(__unused id self, __unused SEL selector, id key, id value)
{
	float level = 0;

	if (key != NULL && CFEqual((CFTypeRef)key, CFSTR("TorchLevel"))) {
		if (value != NULL && CFGetTypeID((CFTypeRef)value) == CFNumberGetTypeID()) {
			CFNumberGetValue((CFNumberRef)value, kCFNumberFloatType, &level);
		}
		level = level < 0 ? 0 : level > 1 ? 1 : level;
		flashlight_set((uint32_t)(level * 1000 + 0.5f));
	}
	return 0;
}

/*
 * Video stabilization, of which this hardware has none: the capture graph's VIS node
 * (-[FigCaptureVISPipeline _newVISNodeWithUpstreamOutput:...]) is made with the ISP's processing
 * session, and this device has no such session, so the node comes back nil, the VIS pipeline with
 * it, the movie file sink's tail pipeline with that, and captureSession_buildGraphWithConfiguration
 * tears the whole graph down -- the preview too, for all that the preview has nothing to do with
 * stabilizing a recording. The connection's stabilization method is where the whole stack reads the
 * wish from (+[FigVideoCaptureConnectionConfiguration videoStabilizationMethods:includeIris:] ->
 * cspc_getVideoStabilizationType -> the connections' FigCaptureVideoStabilizationType), and 0 there
 * means none: the graph is then built without a VIS pipeline anywhere
 * (+[FigCaptureVISPipeline visPipelineIsRequiredForFigCaptureVideoStabilizationType:] says no to
 * every type but 2 and 3), and recordings are as steady as the hand that holds the phone.
 */
static int
video_stabilization_method(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return 0;
}

/*
 * The type the stack acts on. The connection configuration arrives from the application's
 * AVFoundation with the type in it, which is read straight out of the object and not computed from
 * the method above (-[FigCaptureSessionParsedConfiguration _computeVideoStabilizationTypes] only
 * fills in what the client left out), so the wish has to be refused here as well.
 */
static int
video_stabilization_type(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return 0;                       /* kFigCaptureVideoStabilizationType_None */
}

/*
 * The microphone, which the capture stack has none of here. A capture session's sound comes through
 * CoreMedia's own capture audio session -- BWAudioSourceNode's start sets its properties and reads
 * its pull format, all of it through a brokered plug-in of the audio HAL -- and that plug-in is not
 * on this phone: the node's start comes back with 'nope', the HAL's word for "no such thing".
 *
 * Camera.app's session has an audio input in it, because it is a session for photographs and for
 * video both. The graph starts its source nodes late (startDeferredSourceNodesIfNeeded) and takes
 * the first error it gets for the whole graph's, so the session reported AVCaptureSessionRuntimeError
 * to the application, which took the session down and built it again, two seconds later, for ever:
 * the camera's preview came in bursts of five frames with the sensor powering down in between.
 *
 * The node is told that it started. It then produces no sound -- which is all a phone with no
 * capture audio has to offer -- and the rest of the graph, the preview and the photograph, runs.
 * Only that one error is swallowed: a microphone that is there and fails for another reason still
 * takes the session down, as it should. (The kernel's own microphone, adsp=1 mic=4, is a different
 * path: AVAudioRecorder and Voice Memos record through it, and nothing here touches it.)
 */
#define ERROR_NO_SUCH_THING     1852797029      /* 'nope' */

/*
 * redfin (plan B1): a source told that it started has to behave like one that did, or nothing behind it can end.
 * Its output never got a sample nor an end: the movie file sink of a recording waits for audio past the stop
 * time (a video never finalized), and the graph's stop waits for every sink to see end-of-data -- with Live Photo
 * on, its audio converter and movie sink never did, -[BWGraph stop:] gave up after 10 s and mediaserverd was
 * killed in it. So, from the faked start, the node's output is given silence: sample buffers of zeros in the
 * output's own format (the one the graph configured; 44.1 kHz mono 16-bit if it cannot be found), timestamped by
 * the host clock, about every 20 ms; and at the node's stop its output is marked as ended (markEndOfLiveOutput),
 * which is what a real audio source does when its audio unit stops. /private/var/local/camera-no-silence keeps the
 * end and drops the silence. (The preferred way, the HAL plug-in's "Codec" input for the capture audio session,
 * needs to know what start: fails on: the error is said in full once.)
 */
#define SILENT_NODES    4
#define SILENT_CHUNK    1024            /* frames a buffer */

static struct silent {
	id                      node;           /* (not retained: a key; cleared at its stop) */
	id                      output;
	dispatch_queue_t        queue;
	dispatch_source_t       timer;
	CMAudioFormatDescriptionRef format;
	AudioStreamBasicDescription asbd;
	uint64_t                start;          /* mach_absolute_time() of the faked start */
	uint64_t                frames;         /* given so far */
	uint64_t                buffers;
	bool                    ours;           /* the format is our guess, not the graph's */
} silent[SILENT_NODES];
static pthread_mutex_t silent_lock = PTHREAD_MUTEX_INITIALIZER;
static mach_timebase_info_data_t silent_timebase;

static id
send_if(id object, const char *selector)
{
	SEL sel = sel_registerName(selector);

	if (object == nil || !((BOOL (*)(id, SEL, SEL))objc_msgSend)(object, sel_registerName("respondsToSelector:"), sel)) {
		return nil;
	}
	return ((id (*)(id, SEL))objc_msgSend)(object, sel);
}

/* a CoreMedia audio format description, if that is what this is (a plain CF object, never an Objective-C one of the graph's) */
static CMAudioFormatDescriptionRef
as_audio_format(id object)
{
	if (object == nil || strcmp(object_getClassName(object), "__NSCFType") != 0 ||
	    CFGetTypeID((CFTypeRef)object) != CMFormatDescriptionGetTypeID() ||
	    CMFormatDescriptionGetMediaType((CMFormatDescriptionRef)object) != kCMMediaType_Audio) {
		return NULL;
	}
	return (CMAudioFormatDescriptionRef)object;
}

/* what an object answers to, once per class (for finding the format's accessors on the phone) */
static void
say_methods(id object)
{
	static Class said[8];
	Class cls = object != nil ? object_getClass(object) : Nil;
	unsigned count = 0, i;
	char line[220];
	size_t n = 0;

	for (i = 0; cls != Nil && i < 8 && said[i] != Nil; i++) {
		if (said[i] == cls) {
			return;
		}
	}
	if (cls == Nil || i == 8) {
		return;
	}
	said[i] = cls;
	Method *methods = class_copyMethodList(cls, &count);

	say("redwood: mediaserverd: a %s answers to %u methods:\n", class_getName(cls), count);
	for (i = 0; i < count; i++) {
		const char *name = sel_getName(method_getName(methods[i]));

		if (n + strlen(name) + 2 >= sizeof(line) - 1) {
			say("redwood: mediaserverd:   %s\n", line);
			n = 0;
		}
		n += (size_t)snprintf(line + n, sizeof(line) - n, "%s ", name);
	}
	if (n > 0) {
		say("redwood: mediaserverd:   %s\n", line);
	}
	free(methods);
}

/* the format the graph configured on a node's output, or 44.1 kHz mono 16-bit */
static CMAudioFormatDescriptionRef
silent_format(id output, bool *ours)
{
	static const char *const outputs[] = { "liveFormat", "format", "configuredFormat" };
	static const char *const formats[] = { "formatDescription", "audioFormatDescription", "CMFormatDescription" };
	CMAudioFormatDescriptionRef fd = NULL;

	*ours = false;
	for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]) && fd == NULL; i++) {
		id f = send_if(output, outputs[i]);

		if (f == nil) {
			continue;
		}
		say_methods(f);
		if ((fd = as_audio_format(f)) != NULL) {
			break;
		}
		for (size_t j = 0; j < sizeof(formats) / sizeof(formats[0]) && fd == NULL; j++) {
			fd = as_audio_format(send_if(f, formats[j]));
		}
	}
	if (fd != NULL) {
		const AudioStreamBasicDescription *a = CMAudioFormatDescriptionGetStreamBasicDescription(fd);

		if (a != NULL && a->mFormatID == kAudioFormatLinearPCM && a->mSampleRate > 0 && a->mBytesPerFrame > 0) {
			CFRetain(fd);
			return fd;
		}
		say("redwood: mediaserverd: the capture audio's format is not linear PCM: the silence is given in our own\n");
		fd = NULL;
	}
	AudioStreamBasicDescription a = {
		.mSampleRate = 44100, .mFormatID = kAudioFormatLinearPCM,
		.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
		.mBytesPerPacket = 2, .mFramesPerPacket = 1, .mBytesPerFrame = 2, .mChannelsPerFrame = 1, .mBitsPerChannel = 16,
	};

	*ours = true;
	CMAudioFormatDescriptionCreate(NULL, &a, 0, NULL, 0, NULL, NULL, &fd);
	return fd;
}

static void
silent_tick(void *context)
{
	struct silent *sl = context;
	uint64_t now = mach_absolute_time(), due;
	const AudioStreamBasicDescription *a = &sl->asbd;
	double seconds = (double)(now - sl->start) * silent_timebase.numer / silent_timebase.denom / 1e9;
	size_t frame_bytes = a->mBytesPerFrame * ((a->mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? a->mChannelsPerFrame : 1);

	due = (uint64_t)(seconds * a->mSampleRate);
	if (due > sl->frames + (uint64_t)a->mSampleRate) {
		sl->frames = due - SILENT_CHUNK;        /* (more than a second behind: the gap is left, not filled) */
	}
	while (sl->frames + SILENT_CHUNK <= due) {
		CMBlockBufferRef block = NULL;
		CMSampleBufferRef buffer = NULL;
		size_t bytes = SILENT_CHUNK * frame_bytes;
		CMTime pts = CMTimeAdd(CMClockMakeHostTimeFromSystemUnits(sl->start), CMTimeMake((int64_t)sl->frames,
		    (int32_t)a->mSampleRate));

		if (CMBlockBufferCreateWithMemoryBlock(NULL, NULL, bytes, NULL, NULL, 0, bytes, kCMBlockBufferAssureMemoryNowFlag,
		    &block) != 0 || block == NULL) {
			return;
		}
		CMBlockBufferFillDataBytes(0, block, 0, bytes);
		if (CMAudioSampleBufferCreateReadyWithPacketDescriptions(NULL, block, sl->format, SILENT_CHUNK, pts, NULL,
		    &buffer) == 0 && buffer != NULL) {
			((void (*)(id, SEL, CMSampleBufferRef))objc_msgSend)(sl->output, sel_registerName("emitSampleBuffer:"), buffer);
			CFRelease(buffer);
			if (sl->buffers++ == 0) {
				say("redwood: mediaserverd: the capture audio's silence: the first buffer given (%.0f Hz, %u channel(s)%s)\n",
				    a->mSampleRate, a->mChannelsPerFrame, sl->ours ? ", our format" : ", the graph's format");
			}
		}
		CFRelease(block);
		sl->frames += SILENT_CHUNK;
	}
}

static void
nothing(void *context)
{
	(void)context;
}

/* the faked start: the output made live (if the start did not get that far) and silence from now on */
static void
silent_begin(id node)
{
	id output = send_if(node, "output");
	struct silent *sl = NULL;
	bool no_silence = access("/private/var/local/camera-no-silence", F_OK) == 0;

	if (output == nil) {
		say("redwood: mediaserverd: the audio source has no output: nothing to give silence to\n");
		return;
	}
	say_methods(output);
	if (send_if(output, "liveFormat") == nil && send_if(output, "format") != nil &&
	    ((BOOL (*)(id, SEL, SEL))objc_msgSend)(output, sel_registerName("respondsToSelector:"), sel_registerName("makeConfiguredFormatLive"))) {
		((void (*)(id, SEL))objc_msgSend)(output, sel_registerName("makeConfiguredFormatLive"));
		say("redwood: mediaserverd: the audio source's output made live (its start did not get that far)\n");
	}
	if (silent_timebase.denom == 0) {
		mach_timebase_info(&silent_timebase);
	}
	pthread_mutex_lock(&silent_lock);
	for (int i = 0; i < SILENT_NODES && sl == NULL; i++) {
		sl = silent[i].node == node ? &silent[i] : NULL;
	}
	for (int i = 0; i < SILENT_NODES && sl == NULL; i++) {
		sl = silent[i].node == nil ? &silent[i] : NULL;
	}
	if (sl == NULL || sl->timer != NULL) {
		pthread_mutex_unlock(&silent_lock);
		say("redwood: mediaserverd: the audio source's silence %s\n", sl == NULL ? "has no room (too many sources)" : "already runs");
		return;
	}
	sl->node = node;
	sl->output = output;
	sl->frames = sl->buffers = 0;
	sl->start = mach_absolute_time();
	if (sl->format != NULL) {
		CFRelease(sl->format);
	}
	sl->format = silent_format(output, &sl->ours);
	if (sl->format != NULL && !no_silence) {
		sl->asbd = *CMAudioFormatDescriptionGetStreamBasicDescription(sl->format);
		if (sl->queue == NULL) {
			sl->queue = dispatch_queue_create("redwood.capture-audio-silence", DISPATCH_QUEUE_SERIAL);
		}
		sl->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, sl->queue);
		dispatch_set_context(sl->timer, sl);
		dispatch_source_set_event_handler_f(sl->timer, silent_tick);
		dispatch_source_set_timer(sl->timer, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC), 20 * NSEC_PER_MSEC,
		    5 * NSEC_PER_MSEC);
		dispatch_resume(sl->timer);
	}
	pthread_mutex_unlock(&silent_lock);
	say("redwood: mediaserverd: the audio source gives %s from now on, and an end at its stop\n",
	    no_silence ? "nothing (camera-no-silence)" : sl->format != NULL ? "silence" : "nothing (no format for silence)");
}

/* the node's stop: no more silence, and its output ended */
static void
silent_end(id node)
{
	struct silent *sl = NULL;
	id output;

	pthread_mutex_lock(&silent_lock);
	for (int i = 0; i < SILENT_NODES && sl == NULL; i++) {
		sl = silent[i].node == node ? &silent[i] : NULL;
	}
	if (sl == NULL) {
		pthread_mutex_unlock(&silent_lock);
		return;                 /* (a source that really started: it ends its own output) */
	}
	if (sl->timer != NULL) {
		dispatch_source_cancel(sl->timer);
		dispatch_sync_f(sl->queue, NULL, nothing);      /* (a tick under way is done) */
		dispatch_release(sl->timer);
		sl->timer = NULL;
	}
	output = sl->output;
	sl->node = nil;
	sl->output = nil;
	pthread_mutex_unlock(&silent_lock);
	if (send_if(output, "liveFormat") == nil && ((BOOL (*)(id, SEL, SEL))objc_msgSend)(output, sel_registerName("respondsToSelector:"),
	    sel_registerName("liveFormat"))) {
		say("redwood: mediaserverd: the audio source stops (%llu silent buffers); its output had ended already\n",
		    (unsigned long long)sl->buffers);
		return;
	}
	if (!((BOOL (*)(id, SEL, SEL))objc_msgSend)(output, sel_registerName("respondsToSelector:"), sel_registerName("markEndOfLiveOutput"))) {
		say("redwood: mediaserverd: the audio source stops, and its output CANNOT BE ENDED (no -markEndOfLiveOutput)\n");
		return;
	}
	((void (*)(id, SEL))objc_msgSend)(output, sel_registerName("markEndOfLiveOutput"));
	say("redwood: mediaserverd: the audio source stops (%llu silent buffers): its output ended\n", (unsigned long long)sl->buffers);
}

static IMP original_audio_source_start, original_audio_source_stop;

/*
 * redfin (flag /var/local/camera-tsfix, present = on): the capture audio's silence filling off.
 * asn_audioUnitRenderProc (CMCapture) takes the gap since the last callback as
 * AudioConvertHostTimeToNanos(mHostTime - _nextExpectedHostTime), unsigned: a callback one host tick
 * early (the 48 -> 44.1 kHz conversion of AUHAL makes them) wraps. On Apple's 24 MHz timebase (125/3)
 * the product overflows to a "negative" gap and nothing happens; on our 19.2 MHz one (625/12) it comes
 * out as ~1.5e18 ns, which, scaled to 44.1 kHz and cut to 32 bits, asks
 * -_createSampleBufferForTimestampedAudioBufferList:audioBufferIndex: for ~20.7 M silent frames
 * (39 MiB) per callback: mediaserverd grew 1.5 GB/s with Live Photo on and was killed. The import
 * cannot be rebound (the shared cache turned the stub into a direct branch to CoreAudio, there is no
 * pointer to swap) and -_createSampleBuffer... is called directly, not by message: the node's own
 * switch, _silenceFramesFillingDisabled, is set instead (the render proc then skips the gap check).
 * The cost: a real dropout is no longer padded with silence (the next buffer just follows).
 */
static void
audio_source_no_silence_fill(id self)
{
	static Ivar ivar;
	static bool looked;

	if (access("/private/var/local/camera-tsfix", F_OK) != 0) {
		return;
	}
	if (!looked) {
		looked = true;
		ivar = class_getInstanceVariable(object_getClass(self), "_silenceFramesFillingDisabled");
		say("redwood: mediaserverd: camera-tsfix: BWAudioSourceNode._silenceFramesFillingDisabled %s\n",
		    ivar != NULL ? "found: set" : "NOT found: the silence filling stays on");
	}
	if (ivar != NULL) {
		*((volatile BOOL *)((char *)(void *)self + ivar_getOffset(ivar))) = YES;
	}
}

static BOOL
audio_source_start(id self, SEL selector, id *error)
{
	audio_source_no_silence_fill(self);
	BOOL started = ((BOOL (*)(id, SEL, id *))original_audio_source_start)(self, selector, error);
	audio_source_no_silence_fill(self);
	id made = error != NULL ? *error : NULL;
	static bool said;

	if (started || made == NULL || ((long (*)(id, SEL))objc_msgSend)(made, sel_registerName("code")) != ERROR_NO_SUCH_THING) {
		return started;
	}
	if (!said) {
		id text = send_if(made, "description");

		said = true;
		say("redwood: mediaserverd: there is no capture audio on this phone: the session's audio source gives silence, "
		    "and the camera runs without sound\n");
		say("redwood: mediaserverd: (the audio source's start said: %.180s)\n", text != nil ?
		    ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String")) : "?");
	}
	*error = NULL;                  /* (autoreleased by the capture stack: not ours to give back) */
	silent_begin(self);
	return true;
}

static BOOL
audio_source_stop(id self, SEL selector, id *error)
{
	BOOL stopped = original_audio_source_stop != NULL ? ((BOOL (*)(id, SEL, id *))original_audio_source_stop)(self, selector, error) : YES;

	silent_end(self);
	return stopped;
}

/*
 * A photograph's quality prioritization, "speed" at the most. Asked for "balanced" (Camera.app's) or
 * "quality", the capture device takes a scene for a low-light one whenever a frame was exposed at
 * least as long as the sensor's still image stabilization threshold
 * (-[BWFigVideoCaptureDevice _isLowLightSceneUsingFrameStatisticsByPortType:]) -- a threshold from
 * the sensor's tuning in CameraSetup.plist, 0 for a sensor it has no tuning for, as ours -- and
 * then asks the stream for a bracket of four frames for SIS fusion (captureType 4, BWSISNode), which
 * this port has nothing to fuse with: the photograph never came. With "speed" it asks for one frame
 * (-_resolveStillImageCaptureTypeFromStatisticsByPortType:..., captureType 1), which the stream
 * gives (camera_device.c, StillImageCaptureNow).
 */
static IMP original_quality_prioritization;

static int
quality_prioritization(id self, SEL selector)
{
	int asked = ((int (*)(id, SEL))original_quality_prioritization)(self, selector);
	static bool said;

	if (asked > 1 && !said) {
		said = true;
		say("redwood: mediaserverd: a photograph's quality prioritization %d taken as 1 (speed): one frame, no SIS bracket\n",
		    asked);
	}
	return asked > 1 ? 1 : asked;
}

/*
 * A photograph's noise reduction (BWGNRNode, between the still image coordinator and the photo
 * encoder) is a FigSampleBufferProcessor of the iPhone's, which is handed the picture and never
 * hands one back here: the photograph stopped there, with no error, and the request with it (the
 * shutter's second press asked for nothing more). The picture goes past it to the node's output as
 * it came, as with no noise reduction to do.
 */
static void
gnr_render(id self, SEL selector, void *sample_buffer, id input)
{
	id output = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("output"));
	static bool said;

	(void)selector, (void)input;
	if (!said) {
		said = true;
		say("redwood: mediaserverd: a photograph goes past the noise reduction, which has nothing to do it with here\n");
	}
	if (output != nil) {
		((void (*)(id, SEL, void *))objc_msgSend)(output, sel_registerName("emitSampleBuffer:"), sample_buffer);
	}
}

static bool
is_node(id node, const char *name)
{
	return node != nil && strcmp(object_getClassName(node), name) == 0;
}

/*
 * How long a photograph's encoding took, from the picture handed to the photo encoder node to the one
 * it hands on (HEVC in software, see register_hevc_encoder below: the slowest step of a photograph).
 */
static void
time_photo_encoder(id output, CMSampleBufferRef sample_buffer)
{
	CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample_buffer);
	static uint64_t began;
	static mach_timebase_info_data_t timebase;
	id connection, input;

	if (image == NULL || CVPixelBufferGetWidth(image) < 2000) {
		return;                 /* not a photograph */
	}
	if (is_node(((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("node")), "BWPhotoEncoderNode")) {
		if (began != 0) {
			if (timebase.denom == 0) {
				mach_timebase_info(&timebase);
			}
			say("redwood: mediaserverd: a photograph of %zux%zu encoded in %.0f ms\n", CVPixelBufferGetWidth(image),
			    CVPixelBufferGetHeight(image), (double)(mach_absolute_time() - began) * timebase.numer / timebase.denom / 1e6);
		}
		began = 0;
		return;
	}
	connection = ((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("connection"));
	input = connection != nil ? ((id (*)(id, SEL))objc_msgSend)(connection, sel_registerName("input")) : nil;
	if (input != nil && is_node(((id (*)(id, SEL))objc_msgSend)(input, sel_registerName("node")), "BWPhotoEncoderNode")) {
		began = mach_absolute_time();
	}
}

/*
 * redfin (ws-cam-video, 2026-11-20): a recording's video compressor, given no more than its encoder can take, and never
 * on the graph's thread. The only HEVC encoder here is VCPHEVC's software one (register_hevc_encoder below): 0.4 s a
 * synthetic 1080p frame (vt_test encode), ~5 s a noisy one like the sensor's at gain 8x, 14 s for the first -- against
 * the AVE's ~33 ms on an iPhone. And it encodes inside VTCompressionSessionEncodeFrame: -[BWVideoCompressorNode
 * renderSampleBuffer:forInput:] blocked for seconds a frame on the thread that feeds the whole recording branch: the
 * compressor got one picture every 4-5 s, the audio nothing (AudioConverterNode 0/s), the file coordinator's queue grew
 * without end (mediaserverd 680 % CPU, 85 C), the stop never drained, and -[BWGraph stop:] trapped after its 10 s wait
 * when Camera was left (EXC_BREAKPOINT, CMCapture +0x89de0).
 * A hardware encoder takes a frame and comes back at once; one that cannot keep up drops frames. So here the compressor's
 * render runs on a serial queue of its own (one per compressor node), the graph's call returns at once, and a picture
 * that comes while the last one is still being encoded is dropped (as the camera drops frames it cannot read). Markers
 * (sample buffers with no picture) and the end of data are queued in order behind it, never dropped. The video gets the
 * frame rate the encoder can make; the audio, the file and the stop go on. /private/var/local/camera-vid-nogate (looked
 * for at each new compressor) turns this off: every call straight through, on the graph's thread, as before.
 */
#define VC_NODES        4

static bool venus_encoder;              /* (ws-vt-venc; set by register_venus_encoder below) */

/* ws-vt-venc: Venus's encoder registered AND venusd answering (its socket there): without venusd the encoder refuses
 * (VideoToolbox then takes the next one: VCPHEVC for hvc1, and for avc1 its rtvc, which fails here), so the old
 * behaviour (avc1 made hvc1, gate, cap) applies */
static bool
venus_up(void)
{
	return venus_encoder && access("/private/var/run/venusd.sock", F_OK) == 0;
}

extern id objc_retain(id object);
extern void objc_release(id object);

static IMP original_vc_render, original_vc_end, original_vc_dropped;
static struct vc {
	id              node;           /* (retained while it has a queue) */
	dispatch_queue_t queue;
	bool            off, busy;
	unsigned        given, dropped, encoded, markers;
	uint64_t        first, since, began;
	double          ms_sum, ms_max;
} vcs[VC_NODES];
static pthread_mutex_t vc_lock = PTHREAD_MUTEX_INITIALIZER;

static double
vc_ms(uint64_t ticks)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (double)ticks * tb.numer / tb.denom / 1e6;
}

/* (under vc_lock) the node's slot: a new compressor takes a free one, or the oldest */
static struct vc *
vc_of(id node)
{
	static unsigned next;
	struct vc *v;

	for (int i = 0; i < VC_NODES; i++) {
		if (vcs[i].node == node) {
			return &vcs[i];
		}
	}
	v = &vcs[next++ % VC_NODES];
	if (v->node != nil) {
		say("redwood: mediaserverd: video compressor %p: done with %u pictures given, %u encoded, %u dropped\n", (void *)v->node,
		    v->given, v->encoded, v->dropped);
		/* (its queue is kept: work still on it holds the node it was queued for) */
		objc_release(v->node);
	}
	v->node = objc_retain(node);
	/* ws-vt-venc: with Venus's encoder EncodeFrame returns at once: no gate unless /private/var/local/camera-vid-gate */
	v->off = access("/private/var/local/camera-vid-nogate", F_OK) == 0 ||
	    (venus_up() && access("/private/var/local/camera-vid-gate", F_OK) != 0);
	v->busy = false;
	v->given = v->dropped = v->encoded = v->markers = 0;
	v->first = v->since = mach_absolute_time();
	v->ms_sum = v->ms_max = 0;
	if (v->queue == NULL) {
		v->queue = dispatch_queue_create("redwood.video-compressor", DISPATCH_QUEUE_SERIAL);
	}
	say("redwood: mediaserverd: video compressor %p: %s\n", (void *)node, v->off ? "every picture given on the graph's thread "
	    "(camera-vid-nogate, or the Venus encoder without camera-vid-gate)" : "encodes on a queue of its own, one picture at a time; the others dropped (camera-vid-nogate: off)");
	return v;
}

struct vc_job {
	struct vc       *v;
	int             kind;           /* 0 render (sample buffer), 1 the end of data, 2 a dropped sample (object) */
	id              node, input, object;
	SEL             selector;
	CMSampleBufferRef sample_buffer;
	bool            picture;
};

static void
vc_run(void *context)
{
	struct vc_job *j = context;
	uint64_t t0 = mach_absolute_time();

	if (j->kind == 0) {
		((void (*)(id, SEL, CMSampleBufferRef, id))original_vc_render)(j->node, j->selector, j->sample_buffer, j->input);
		CFRelease(j->sample_buffer);
	} else if (j->kind == 1) {
		((void (*)(id, SEL, id))original_vc_end)(j->node, j->selector, j->input);
		say("redwood: mediaserverd: video compressor %p: the end of data passed on (%u pictures given, %u encoded, %u dropped)\n",
		    (void *)j->node, j->v->given, j->v->encoded, j->v->dropped);
	} else {
		((void (*)(id, SEL, id, id))original_vc_dropped)(j->node, j->selector, j->object, j->input);
		objc_release(j->object);
	}
	if (j->picture) {
		double ms = vc_ms(mach_absolute_time() - t0);

		pthread_mutex_lock(&vc_lock);
		if (j->v->node == j->node) {
			j->v->busy = false;
			j->v->ms_sum += ms;
			j->v->ms_max = ms > j->v->ms_max ? ms : j->v->ms_max;
			if (j->v->encoded++ == 0) {
				say("redwood: mediaserverd: video compressor: the first picture took %.0f ms\n", ms);
			}
		}
		pthread_mutex_unlock(&vc_lock);
	}
	objc_release(j->input);
	objc_release(j->node);
	free(j);
}

/* (under vc_lock) a job for the node's queue */
static void
vc_queue(struct vc *v, int kind, id node, SEL selector, CMSampleBufferRef sample_buffer, id object, id input, bool picture)
{
	struct vc_job *j = calloc(1, sizeof(*j));

	if (j == NULL) {
		return;
	}
	j->v = v;
	j->kind = kind;
	j->node = objc_retain(node);
	j->input = input != nil ? objc_retain(input) : nil;
	j->object = object != nil ? objc_retain(object) : nil;
	j->selector = selector;
	j->sample_buffer = sample_buffer != NULL ? (CMSampleBufferRef)CFRetain(sample_buffer) : NULL;
	j->picture = picture;
	dispatch_async_f(v->queue, j, vc_run);
}

static void
vc_render(id self, SEL selector, CMSampleBufferRef sample_buffer, id input)
{
	bool picture = sample_buffer != NULL && CMSampleBufferGetImageBuffer(sample_buffer) != NULL;
	struct vc *v;

	pthread_mutex_lock(&vc_lock);
	v = vc_of(self);
	if (v->off || sample_buffer == NULL) {
		pthread_mutex_unlock(&vc_lock);
		((void (*)(id, SEL, CMSampleBufferRef, id))original_vc_render)(self, selector, sample_buffer, input);
		return;
	}
	if (vc_ms(mach_absolute_time() - v->since) >= 5000) {
		say("redwood: mediaserverd: video compressor: %u given, %u encoded (avg %.0f ms, max %.0f), %u dropped, %u markers\n",
		    v->given, v->encoded, v->encoded ? v->ms_sum / v->encoded : 0, v->ms_max, v->dropped, v->markers);
		v->since = mach_absolute_time();
	}
	if (picture && v->busy) {
		v->dropped++;           /* (the encoder still has the last one) */
	} else {
		if (picture) {
			v->busy = true;
			v->given++;
		} else {
			v->markers++;
		}
		vc_queue(v, 0, self, selector, sample_buffer, nil, input, picture);
	}
	pthread_mutex_unlock(&vc_lock);
}

/* -[BWVideoCompressorNode didReachEndOfDataForInput:]: behind what is queued */
static void
vc_end(id self, SEL selector, id input)
{
	struct vc *v;

	pthread_mutex_lock(&vc_lock);
	v = vc_of(self);
	if (v->off) {
		pthread_mutex_unlock(&vc_lock);
		((void (*)(id, SEL, id))original_vc_end)(self, selector, input);
		return;
	}
	say("redwood: mediaserverd: video compressor %p: the end of data, queued behind the picture being encoded%s\n", (void *)self,
	    v->busy ? "" : " (none)");
	vc_queue(v, 1, self, selector, NULL, nil, input, false);
	pthread_mutex_unlock(&vc_lock);
}

/* -[BWVideoCompressorNode handleDroppedSample:forInput:]: in order with the rest */
static void
vc_dropped(id self, SEL selector, id dropped, id input)
{
	struct vc *v;

	pthread_mutex_lock(&vc_lock);
	v = vc_of(self);
	if (v->off) {
		pthread_mutex_unlock(&vc_lock);
		((void (*)(id, SEL, id, id))original_vc_dropped)(self, selector, dropped, input);
		return;
	}
	vc_queue(v, 2, self, selector, NULL, dropped, input, false);
	pthread_mutex_unlock(&vc_lock);
}

/*
 * A node's output given no sample buffer at all, which no node takes: the session's audio source
 * (-[BWAudioSourceNode _generateSamples]), whose audio unit never started here (see audio_source_start
 * above), fills the silence by a timer with buffers it fails to make and emits them all the same --
 * NULL. Live Photo's staging node added that to an array (-[BWIrisStagingNode renderSampleBuffer:forInput:])
 * and mediaserverd died of the NSInvalidArgumentException, every half a minute with Live Photo on, the
 * preview black for a second each time. A NULL is not passed on.
 */
static IMP original_emit_sample_buffer;

/*
 * redfin (plan B8): why the preview's sink gets 420f when its input asked for BGRA (set_format_requirements below).
 * The first picture that reaches the image queue sink is said, with the node that gives it and what that node's own
 * input was asked for: the node that does not pass the BGRA on is the one to look at. (Only the first 3000 buffers
 * are looked at.)
 */
static const char *
format_list(id requirements)
{
	static char text[160];
	id formats = requirements != nil ? send_if(requirements, "supportedPixelFormats") : nil;
	id description = formats != nil ? send_if(formats, "description") : nil;
	const char *t = description != nil ? ((const char *(*)(id, SEL))objc_msgSend)(description, sel_registerName("UTF8String")) : "(none)";

	snprintf(text, sizeof(text), "%s", t != NULL ? t : "(none)");
	for (char *c = text; *c; c++) {
		*c = *c == '\n' ? ' ' : *c;
	}
	return text;
}

static void
preview_format(id output, CMSampleBufferRef sample_buffer)
{
	static unsigned looked;
	static bool said;
	CVImageBufferRef image;
	id connection, input, node, source_input;

	if (said || looked >= 3000) {
		return;
	}
	looked++;
	connection = send_if(output, "connection");
	input = connection != nil ? send_if(connection, "input") : nil;
	if (input == nil || !is_node(send_if(input, "node"), "BWImageQueueSinkNode") ||
	    (image = CMSampleBufferGetImageBuffer(sample_buffer)) == NULL) {
		return;
	}
	said = true;
	OSType pf = CVPixelBufferGetPixelFormatType(image);
	node = send_if(output, "node");
	source_input = node != nil ? send_if(node, "input") : nil;
	say("redwood: mediaserverd: the preview's sink gets %c%c%c%c %zux%zu from a %s (asked: %s)\n", pf >> 24, pf >> 16 & 255,
	    pf >> 8 & 255, pf & 255, CVPixelBufferGetWidth(image), CVPixelBufferGetHeight(image),
	    node != nil ? object_getClassName(node) : "?", format_list(send_if(input, "formatRequirements")));
	if (source_input != nil) {
		say("redwood: mediaserverd:   whose own input was asked for %s\n", format_list(send_if(source_input, "formatRequirements")));
	}
}


/*
 * redfin (ws-cam-smooth): /private/var/local/camera-pace (looked for every 2 s): the samples each kind of node
 * takes in the capture graph, counted and timed at the outputs that give them, said every 2 s with the longest
 * wait between two samples into that kind of node; a wait over 250 ms into the preview's sink (BWImageQueueSinkNode,
 * the preview on the screen) is said at once.
 */
#define PACE_KINDS      24
static struct {
	const char      *kind[PACE_KINDS];
	unsigned        count[PACE_KINDS];
	uint64_t        last[PACE_KINDS], gap_max[PACE_KINDS];
	uint64_t        since, checked;
	bool            on;
} graph_pace;
static pthread_mutex_t graph_pace_lock = PTHREAD_MUTEX_INITIALIZER;

static double
graph_pace_ms(uint64_t ticks)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (double)ticks * tb.numer / tb.denom / 1e6;
}

static void
graph_pace_sample(id output)
{
	uint64_t now = mach_absolute_time();

	if (graph_pace.checked == 0 || graph_pace_ms(now - graph_pace.checked) > 2000) {
		graph_pace.checked = now;
		graph_pace.on = access("/private/var/local/camera-pace", F_OK) == 0;
	}
	if (!graph_pace.on) {
		return;
	}
	id connection = send_if(output, "connection");
	id input = connection != nil ? send_if(connection, "input") : nil;
	id node = input != nil ? send_if(input, "node") : nil;
	const char *kind = node != nil ? object_getClassName(node) : "?";

	pthread_mutex_lock(&graph_pace_lock);
	int i;
	for (i = 0; i < PACE_KINDS && graph_pace.kind[i] != NULL && graph_pace.kind[i] != kind; i++) {
	}
	if (i < PACE_KINDS) {
		if (graph_pace.kind[i] == NULL) {
			graph_pace.kind[i] = kind;
		}
		graph_pace.count[i]++;
		if (graph_pace.last[i] != 0) {
			uint64_t gap = now - graph_pace.last[i];

			if (gap > graph_pace.gap_max[i]) {
				graph_pace.gap_max[i] = gap;
			}
			if (graph_pace_ms(gap) > 250 && strcmp(kind, "BWImageQueueSinkNode") == 0) {
				say("redwood: mediaserverd: pace: GAP %.0f ms into the preview's sink\n", graph_pace_ms(gap));
			}
		}
		graph_pace.last[i] = now;
	}
	if (graph_pace.since == 0) {
		graph_pace.since = now;
	} else if (graph_pace_ms(now - graph_pace.since) >= 2000) {
		char line[1200];
		size_t at = 0;
		double secs = graph_pace_ms(now - graph_pace.since) / 1000;

		for (int k = 0; k < PACE_KINDS && graph_pace.kind[k] != NULL && at < sizeof(line) - 80; k++) {
			if (graph_pace.count[k] != 0) {
				at += snprintf(line + at, sizeof(line) - at, " %s %.1f/s max %.0f ms;", graph_pace.kind[k] + (strncmp(graph_pace.kind[k],
				    "BW", 2) == 0 ? 2 : 0), graph_pace.count[k] / secs, graph_pace_ms(graph_pace.gap_max[k]));
			}
			graph_pace.count[k] = 0;
			graph_pace.gap_max[k] = 0;
		}
		graph_pace.since = now;
		for (size_t from = 0; from < at; from += 190) {
			say("redwood: mediaserverd: pace %.1f s:%.190s\n", secs, line + from);
		}
	}
	pthread_mutex_unlock(&graph_pace_lock);
}

static void
emit_sample_buffer(id self, SEL selector, CMSampleBufferRef sample_buffer)
{
	static bool said;

	if (sample_buffer == NULL) {
		if (!said) {
			said = true;
			say("redwood: mediaserverd: a node's output was given no sample buffer (the silent audio source's): not passed on\n");
		}
		return;
	}
	time_photo_encoder(self, sample_buffer);
	preview_format(self, sample_buffer);
	graph_pace_sample(self);
	((void (*)(id, SEL, CMSampleBufferRef))original_emit_sample_buffer)(self, selector, sample_buffer);
}

/*
 * The photograph's encoder. Camera.app asks for HEIC -- HEVC inside a HEIF file, which CMPhoto makes
 * through VideoToolbox -- and VideoToolbox here has no HEVC encoder but the iPhone's hardware ones
 * (com.apple.videotoolbox.videoencoder.hevc and .hevc.gva, behind its AVE): the photo encoder node
 * failed with kVTCouldNotFindVideoEncoderErr (-12908). Apple's software HEVC codec is in the system
 * all the same, VCPHEVC.videocodec, whose decoder VideoToolbox takes here for every HEIC picture (see
 * the top), and there is an encoder in it, HEVCVideoEncoder_CreateInstance, that iOS never registers:
 * the codec's VCPHEVCRegisterEncoder is an empty function. It is registered here, as the codec
 * registers its decoder (VTRegisterVideoDecoderWithInfo, not hardware accelerated).
 *
 * redfin (plan B2): a video is H.264 when Camera.app's formats are "Most Compatible" (and for any app that asks for
 * avc1), and the only H.264 encoders VideoToolbox lists on an iPhone are the AVE's. What the phone has is said once
 * (VideoToolbox's encoder list; the encoder factories the video codec images export, found by walking their export
 * tries), and a software H.264 encoder is registered for 'avc1' if one is found among them. If none is, a
 * recording's video settings that ask for avc1 are given hvc1 instead (recording_video_settings below), so that the
 * video is recorded at all. /private/var/local/camera-no-avc: no H.264 encoder registered (HEVC for every video).
 */
#ifndef LC_DYLD_EXPORTS_TRIE
#define LC_DYLD_EXPORTS_TRIE    0x80000033
#endif

static bool avc_encoder;                /* a software encoder for 'avc1' is there (VideoToolbox's or registered here) */
static bool venus_encoder;              /* ws-vt-venc: Venus's hardware encoders registered (vtvenc.dylib) */

typedef int (*encoder_create)(uint32_t, CFAllocatorRef, void *);

static uint64_t
uleb(const uint8_t **p, const uint8_t *end)
{
	uint64_t v = 0;
	unsigned shift = 0;

	while (*p < end) {
		uint8_t byte = *(*p)++;

		v |= (uint64_t)(byte & 0x7f) << shift;
		shift += 7;
		if (!(byte & 0x80) || shift > 63) {
			break;
		}
	}
	return v;
}

/* every exported name of the image with `part' in it, by its export trie: said, and each given to found() */
static unsigned
walk_trie(const uint8_t *start, const uint8_t *end, const uint8_t *node, char *name, size_t length, const char *const *parts,
    void (*found)(const char *, void *), void *context, unsigned depth)
{
	const uint8_t *p = node;
	uint64_t terminal;
	unsigned count = 0, children;

	if (node < start || node >= end || depth > 64) {
		return 0;
	}
	terminal = uleb(&p, end);
	if (terminal > 0) {
		for (size_t i = 0; parts[i] != NULL; i++) {
			if (strstr(name, parts[i]) != NULL) {
				found(name, context);
				count++;
				break;
			}
		}
		p += terminal;
	}
	if (p >= end) {
		return count;
	}
	children = *p++;
	for (unsigned i = 0; i < children && p < end; i++) {
		size_t edge = strnlen((const char *)p, (size_t)(end - p));
		uint64_t offset;

		if (length + edge + 1 > 512) {
			return count;
		}
		memcpy(name + length, p, edge);
		name[length + edge] = 0;
		p += edge + 1;
		offset = uleb(&p, end);
		count += walk_trie(start, end, start + offset, name, length + edge, parts, found, context, depth + 1);
		name[length] = 0;
	}
	return count;
}

static void
exports_of(const struct mach_header *header, intptr_t image_slide, const char *const *parts, void (*found)(const char *, void *),
    void *context)
{
	const struct mach_header_64 *mh = (const void *)header;
	const struct load_command *lc = (const void *)(mh + 1);
	uint64_t linkedit_vm = 0, linkedit_off = 0, linkedit_size = 0;
	uint32_t trie_off = 0, trie_size = 0;
	char name[520] = "";

	if (mh->magic != MH_MAGIC_64) {
		return;
	}
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64 && strcmp(((const struct segment_command_64 *)lc)->segname, "__LINKEDIT") == 0) {
			linkedit_vm = ((const struct segment_command_64 *)lc)->vmaddr;
			linkedit_off = ((const struct segment_command_64 *)lc)->fileoff;
			linkedit_size = ((const struct segment_command_64 *)lc)->vmsize;
		} else if (lc->cmd == LC_DYLD_EXPORTS_TRIE) {
			trie_off = ((const struct linkedit_data_command *)lc)->dataoff;
			trie_size = ((const struct linkedit_data_command *)lc)->datasize;
		} else if ((lc->cmd == LC_DYLD_INFO || lc->cmd == LC_DYLD_INFO_ONLY) && trie_size == 0) {
			trie_off = ((const struct dyld_info_command *)lc)->export_off;
			trie_size = ((const struct dyld_info_command *)lc)->export_size;
		}
	}
	/* (only within the image's __LINKEDIT as mapped: a trie that is not there is not read) */
	if (linkedit_vm == 0 || trie_size == 0 || trie_off < linkedit_off || trie_off - linkedit_off + (uint64_t)trie_size > linkedit_size) {
		return;
	}
	const uint8_t *trie = (const uint8_t *)(uintptr_t)(linkedit_vm + (uint64_t)image_slide + (trie_off - linkedit_off));

	walk_trie(trie, trie + trie_size, trie, name, 0, parts, found, context, 0);
}

struct encoder_search {
	const char *image;
	encoder_create avc;
	char avc_name[160];
	unsigned said;
};

static void
encoder_found(const char *symbol, void *context)
{
	struct encoder_search *e = context;
	const char *name = symbol[0] == '_' ? symbol + 1 : symbol;
	bool avc = (strstr(name, "H264") != NULL || strstr(name, "AVC") != NULL || strstr(name, "Avc") != NULL ||
	    strstr(name, "h264") != NULL) && strstr(name, "Encoder") != NULL && strstr(name, "CreateInstance") != NULL &&
	    strstr(name, "Decoder") == NULL;

	if (e->said++ < 40) {
		const char *base = strrchr(e->image, '/');

		say("redwood: mediaserverd:   %s: %s%s\n", base ? base + 1 : e->image, name, avc ? " (an H.264 encoder?)" : "");
	}
	if (avc && e->avc == NULL) {
		void *handle = dlopen(e->image, RTLD_NOW | RTLD_NOLOAD);

		e->avc = handle != NULL ? (encoder_create)dlsym(handle, name) : NULL;
		snprintf(e->avc_name, sizeof(e->avc_name), "%s", name);
	}
}

/* VideoToolbox's encoders, said once; whether one for `codec' is there that is not hardware accelerated */
static bool
software_encoder_listed(void *vt, uint32_t codec, bool say_all)
{
	int (*copy_list)(CFDictionaryRef, CFArrayRef *) = dlsym(vt, "VTCopyVideoEncoderList");
	CFStringRef *k_codec = dlsym(vt, "kVTVideoEncoderList_CodecType"), *k_id = dlsym(vt, "kVTVideoEncoderList_EncoderID");
	CFStringRef *k_hw = dlsym(vt, "kVTVideoEncoderList_IsHardwareAccelerated");
	CFArrayRef list = NULL;
	bool there = false;

	if (copy_list == NULL || k_codec == NULL || k_id == NULL || copy_list(NULL, &list) != 0 || list == NULL) {
		say("redwood: mediaserverd: VideoToolbox's encoder list cannot be had\n");
		return false;
	}
	for (CFIndex i = 0; i < CFArrayGetCount(list); i++) {
		CFDictionaryRef e = CFArrayGetValueAtIndex(list, i);
		CFNumberRef type = CFDictionaryGetValue(e, *k_codec);
		CFStringRef id = CFDictionaryGetValue(e, *k_id);
		CFBooleanRef hw = k_hw != NULL ? CFDictionaryGetValue(e, *k_hw) : NULL;
		uint32_t t = 0;
		char text[120] = "?";

		if (type != NULL) {
			CFNumberGetValue(type, kCFNumberSInt32Type, &t);
		}
		if (id != NULL) {
			CFStringGetCString(id, text, sizeof(text), kCFStringEncodingUTF8);
		}
		if (say_all) {
			say("redwood: mediaserverd: VideoToolbox encoder %c%c%c%c %s%s\n", t >> 24, t >> 16 & 255, t >> 8 & 255, t & 255, text,
			    hw == kCFBooleanTrue ? " (hardware)" : "");
		}
		there |= t == codec && hw != kCFBooleanTrue;
	}
	CFRelease(list);
	return there;
}

static int
register_encoder(void *vt, uint32_t codec, CFStringRef id, CFStringRef name, void *create)
{
	int (*register_with_info)(uint32_t, CFDictionaryRef, void *) = dlsym(vt, "VTRegisterVideoEncoderWithInfo");
	CFStringRef *id_key = dlsym(vt, "kVTVideoEncoderList_EncoderID");
	CFStringRef *name_key = dlsym(vt, "kVTVideoEncoderList_EncoderName");
	CFStringRef *hardware_key = dlsym(vt, "kVTVideoEncoderList_IsHardwareAccelerated");
	int status;

	if (register_with_info == NULL || id_key == NULL || name_key == NULL || hardware_key == NULL) {
		return -1;
	}
	const void *keys[] = { *id_key, *name_key, *hardware_key };
	const void *values[] = { id, name, kCFBooleanFalse };
	CFDictionaryRef info = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	status = register_with_info(codec, info, create);
	CFRelease(info);
	return status;
}

static void
register_hevc_encoder(void)
{
	void *codec = dlopen("/System/Library/VideoCodecs/VCPHEVC.videocodec", RTLD_NOW | RTLD_LOCAL);
	void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW | RTLD_GLOBAL);
	void *create = codec != NULL ? dlsym(codec, "HEVCVideoEncoder_CreateInstance") : NULL;
	int status;

	if (vt == NULL || create == NULL || dlsym(vt, "VTRegisterVideoEncoderWithInfo") == NULL) {
		say("redwood: mediaserverd: no software HEVC encoder to register (%s)\n", codec == NULL ? "no VCPHEVC.videocodec" :
		    create == NULL ? "no HEVCVideoEncoder_CreateInstance" : "VideoToolbox without VTRegisterVideoEncoderWithInfo");
	} else {
		status = register_encoder(vt, 'hvc1', CFSTR("com.apple.videotoolbox.videoencoder.hevc.vcp"), CFSTR("HEVC (VCPHEVC, software)"),
		    create);
		say("redwood: mediaserverd: the software HEVC encoder of VCPHEVC.videocodec registered for 'hvc1' (%d)\n", status);
	}
}

/*
 * redfin (ws-vt-venc): Venus's hardware encoders, 'avc1' and 'hvc1' (phone/video/vtvenc.c, a venusd client), registered
 * as hardware encoders as the iPhone's AVE ones are, so a recording encodes on Venus in real time instead of in VCPHEVC's
 * software encoder (~1 s a 1080p frame). It registers nothing without /private/var/local/venus.on or with
 * /private/var/local/vtvenc-off; /private/var/local/camera-no-venc: not loaded at all (the software encoder as before).
 * With it, a recording's avc1 stays avc1, the bit rate is not capped and the compressor's drop gate is off (below).
 */
static void
register_venus_encoder(void)
{
	static const char path[] = "/private/var/local/lib/vtvenc.dylib";
	void *lib;
	int (*reg)(void);
	int n;

	if (access("/private/var/local/camera-no-venc", F_OK) == 0 || access(path, F_OK) != 0) {
		say("redwood: mediaserverd: no Venus video encoder (%s)\n", access(path, F_OK) != 0 ? "no vtvenc.dylib" : "camera-no-venc");
		return;
	}
	lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	reg = lib != NULL ? (int (*)(void))dlsym(lib, "VTVenusEncRegister") : NULL;
	n = reg != NULL ? reg() : -1;
	venus_encoder = n > 0;
	say("redwood: mediaserverd: Venus video encoder: %s (%d registered)\n", lib == NULL ? dlerror() : reg == NULL ?
	    "no VTVenusEncRegister" : venus_encoder ? "registered for avc1/hvc1" : "not registered (venus.on? vtvenc-off?)", n);
}

/*
 * redfin (B2): the H.264 search. Only with /private/var/local/camera-encoder-scan, and not in the constructor: r20's
 * build asked VideoToolbox for its encoder list there (inside dyld's initializer, before the capture stack's methods
 * were replaced) and mediaserverd crash-looped in Core Image (r19a, 2026-10-25); whether that list was the cause is
 * not known, so it is off unless asked for, and then done on a queue of its own seconds after the start. Without
 * it there is no H.264 encoder, and recordings that ask for avc1 are made HEVC (recording_video_settings).
 */
static void
search_avc_encoder(void *context)
{
	void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW | RTLD_GLOBAL);
	int status;

	(void)context;
	if (vt == NULL) {
		return;
	}
	say("redwood: mediaserverd: the H.264 encoder search (camera-encoder-scan)\n");
	if (software_encoder_listed(vt, 'avc1', true)) {
		avc_encoder = true;
		say("redwood: mediaserverd: VideoToolbox has a software H.264 encoder of its own\n");
		return;
	}
	if (access("/private/var/local/camera-no-avc", F_OK) == 0) {
		say("redwood: mediaserverd: no H.264 encoder registered (camera-no-avc): videos asking for avc1 are made HEVC\n");
		return;
	}
	/* the video codecs' images loaded (they are VideoToolbox's plug-ins; nothing of theirs runs at loading) */
	DIR *dir = opendir("/System/Library/VideoCodecs");
	struct dirent *entry;

	while (dir != NULL && (entry = readdir(dir)) != NULL) {
		char path[300];

		if (entry->d_name[0] == '.') {
			continue;
		}
		snprintf(path, sizeof(path), "/System/Library/VideoCodecs/%s", entry->d_name);
		say("redwood: mediaserverd: video codec %s%s\n", entry->d_name, dlopen(path, RTLD_NOW | RTLD_LOCAL) != NULL ? "" : " (not loadable)");
	}
	if (dir != NULL) {
		closedir(dir);
	}
	/* their encoder factories, and VideoToolbox's own */
	static const char *const parts[] = { "Encoder_CreateInstance", "EncoderCreateInstance", "RegisterEncoder", "Encoder_Create", NULL };
	struct encoder_search e = { 0 };


	say("redwood: mediaserverd: encoder factories exported by the video codecs and VideoToolbox:\n");
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *image = _dyld_get_image_name(i);

		if (image != NULL && (strstr(image, "/VideoCodecs/") != NULL || strstr(image, "VideoToolbox.framework") != NULL ||
		    strstr(image, "VCP") != NULL)) {
			e.image = image;
			exports_of(_dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i), parts, encoder_found, &e);
		}
	}
	if (e.avc == NULL) {
		say("redwood: mediaserverd: no software H.264 encoder found: videos asking for avc1 are made HEVC\n");
		return;
	}
	/* TODO(phase 2): the factory's signature is taken to be VideoToolbox's (as HEVCVideoEncoder_CreateInstance's); check a 1080p30 recording */
	status = register_encoder(vt, 'avc1', CFSTR("com.apple.videotoolbox.videoencoder.h264.redwood"), CFSTR("H.264 (software)"), (void *)e.avc);
	avc_encoder = status == 0 && software_encoder_listed(vt, 'avc1', false);
	say("redwood: mediaserverd: %s registered for 'avc1' (%d)%s\n", e.avc_name, status, avc_encoder ? "" :
	    "; NOT LISTED after: videos asking for avc1 are made HEVC");
}

/*
 * redfin (B2): a recording's video settings: avc1 (H.264) made hvc1 (HEVC) when there is no H.264 encoder here, without
 * the H.264-only compression properties (profile level, entropy mode), which the HEVC encoder would refuse.
 */
static IMP original_video_settings;

/*
 * redfin (ws-cam-video): the software HEVC encoder's time a frame follows the bit rate it is asked for: a noisy 1080p frame
 * (the sensor's at gain 8x) took 5.3 s at Camera.app's 15.3 Mbit/s and 0.8 s at 3 Mbit/s (vt_test encode, VT_NOISE).
 * A recording's AverageBitRate is capped: /private/var/local/camera-vid-bitrate holds the cap in bits a second (read at
 * each recording); no file: 3000000; 0: no cap.
 */
static int64_t
vc_bitrate_cap(void)
{
	char text[32] = "";
	int fd = open("/private/var/local/camera-vid-bitrate", O_RDONLY);
	ssize_t n;

	if (fd < 0) {
		return venus_up() ? 0 : 3000000;        /* ws-vt-venc: Venus keeps up: no cap unless the file asks */
	}
	n = read(fd, text, sizeof(text) - 1);
	close(fd);
	text[n > 0 ? n : 0] = 0;
	return text[0] >= '0' && text[0] <= '9' ? strtoll(text, NULL, 10) : 3000000;
}

static id
recording_video_settings(id self, SEL selector)
{
	id settings = ((id (*)(id, SEL))original_video_settings)(self, selector);
	CFTypeRef codec, properties;
	CFMutableDictionaryRef changed;
	static bool said;
	static id said_for, said_for_cap;

	/* (redfin ws-cam-video) what a recording asks of the encoder, said once for each settings object */
	if (settings != nil && settings != said_for) {
		id text = send_if(settings, "description");
		const char *t = text != nil ? ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String")) : NULL;
		char line[1600];
		size_t n = 0;

		said_for = settings;
		for (; t != NULL && *t && n < sizeof(line) - 1; t++) {
			if (*t != '\n' && !(*t == ' ' && n > 0 && line[n - 1] == ' ')) {
				line[n++] = *t;
			}
		}
		line[n] = 0;
		for (size_t from = 0; from < n; from += 200) {
			say("redwood: mediaserverd: a recording's video settings:%s %.200s\n", from ? " ..." : "", line + from);
		}
	}

	if (settings == nil || CFGetTypeID((CFTypeRef)settings) != CFDictionaryGetTypeID() ||
	    (codec = CFDictionaryGetValue((CFDictionaryRef)settings, CFSTR("AVVideoCodecKey"))) == NULL ||
	    CFGetTypeID(codec) != CFStringGetTypeID()) {
		return settings;
	}
	bool to_hevc = !avc_encoder && !venus_up() && CFEqual(codec, CFSTR("avc1"));
	int64_t cap = vc_bitrate_cap(), asked = 0;
	CFNumberRef rate = NULL;

	properties = CFDictionaryGetValue((CFDictionaryRef)settings, CFSTR("AVVideoCompressionPropertiesKey"));
	if (properties != NULL && CFGetTypeID(properties) == CFDictionaryGetTypeID() &&
	    (rate = CFDictionaryGetValue(properties, CFSTR("AverageBitRate"))) != NULL && CFGetTypeID(rate) == CFNumberGetTypeID()) {
		CFNumberGetValue(rate, kCFNumberSInt64Type, &asked);
	}
	bool capped = (to_hevc || CFEqual(codec, CFSTR("hvc1"))) && cap > 0 && asked > cap;
	if (!to_hevc && !capped) {
		return settings;
	}
	changed = CFDictionaryCreateMutableCopy(NULL, 0, (CFDictionaryRef)settings);
	if (to_hevc) {
		CFDictionarySetValue(changed, CFSTR("AVVideoCodecKey"), CFSTR("hvc1"));
	}
	if (properties != NULL && CFGetTypeID(properties) == CFDictionaryGetTypeID()) {
		CFMutableDictionaryRef p = CFDictionaryCreateMutableCopy(NULL, 0, properties);

		if (to_hevc) {
			CFDictionaryRemoveValue(p, CFSTR("ProfileLevel"));
			CFDictionaryRemoveValue(p, CFSTR("H264EntropyMode"));
		}
		if (capped) {
			CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt64Type, &cap);

			CFDictionarySetValue(p, CFSTR("AverageBitRate"), n);
			CFRelease(n);
		}
		CFDictionarySetValue(changed, CFSTR("AVVideoCompressionPropertiesKey"), p);
		CFRelease(p);
	}
	if (to_hevc && !said) {
		said = true;
		say("redwood: mediaserverd: a recording asked for H.264, for which there is no encoder here: made HEVC\n");
	}
	if (capped && said_for_cap != settings) {
		said_for_cap = settings;
		say("redwood: mediaserverd: a recording's bit rate %lld capped at %lld for the software encoder (camera-vid-bitrate)\n",
		    (long long)asked, (long long)cap);
	}
	return (id)CFAutorelease(changed);
}

/*
 * Camera.app's filter thumbnails (the grid of the photo filters, each filter applied to the live
 * picture). -[BWMultiFilterThumbnailNode prepareForCurrentConfigurationToBecomeLive] asks its filter
 * renderer for -filterDescriptor and writes into what it returns; the renderer is a Metal one and this
 * process has no Metal device (MTL2 lists the processes that do), so it is nil, the descriptor NULL and
 * mediaserverd died (SIGSEGV at 0x8) as soon as the preview's first picture was given (r18e, iOS
 * 16.7.x). The node is made ready as any node is (its superclass's method) and passes nothing on: no
 * thumbnails, the preview and photographs unaffected.
 */
static void
thumbnail_prepare(id self, SEL selector)
{
	Class super_class = class_getSuperclass(objc_getClass("BWMultiFilterThumbnailNode"));
	IMP ready = super_class != Nil ? class_getMethodImplementation(super_class, selector) : NULL;
	static bool said;

	if (!said) {
		said = true;
		say("redwood: mediaserverd: the filter thumbnails are not made (their renderer wants Metal): the node made ready as a %s\n",
		    super_class != Nil ? class_getName(super_class) : "(none)");
	}
	if (ready != NULL) {
		((void (*)(id, SEL))ready)(self, selector);
	}
}

static void
thumbnail_render(id self, SEL selector, void *sample_buffer, id input)
{
	(void)self, (void)selector, (void)sample_buffer, (void)input;
}

/*
 * The preview in BGRA. The preview's sink (BWImageQueueSinkNode, Camera.app's CAImageQueue) takes 420f/420v,
 * which backboardd's Core Animation samples as one MTLPixelFormatYCBCR8_420_2P texture: a format Turnip (and
 * SweetMetal2) does not have, so every preview frame failed ("Failed to allocate MTLTexture from IOSurface") and
 * the preview stayed black. Its input is asked for BGRA instead; VideoToolbox's software transfer (vt_test) makes it
 * where the graph rotates the picture anyway. /private/var/local/camera-preview-yuv (any content) turns this off.
 */
static IMP original_set_format_requirements;

static void
set_format_requirements(id self, SEL selector, id requirements)
{
	static int wanted = -1;
	id node = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("node"));

	if (wanted < 0) {
		wanted = access("/private/var/local/camera-preview-yuv", F_OK) != 0;
	}
	if (wanted && requirements != nil && is_node(node, "BWImageQueueSinkNode") &&
	    strcmp(object_getClassName(requirements), "BWVideoFormatRequirements") == 0) {
		id formats = ((id (*)(id, SEL))objc_msgSend)(requirements, sel_registerName("supportedPixelFormats"));
		id description = formats != nil ? ((id (*)(id, SEL))objc_msgSend)(formats, sel_registerName("description")) : nil;
		const char *text = description != nil ? ((const char *(*)(id, SEL))objc_msgSend)(description, sel_registerName("UTF8String")) : "(none)";
		CFNumberRef bgra = CFNumberCreate(NULL, kCFNumberSInt32Type, &(int32_t){ 0x42475241 /* BGRA */ });
		CFArrayRef only = CFArrayCreate(NULL, (const void **)&bgra, 1, &kCFTypeArrayCallBacks);

		say("redwood: mediaserverd: the preview's sink asked for %s: given BGRA\n", text);
		((void (*)(id, SEL, id))objc_msgSend)(requirements, sel_registerName("setSupportedPixelFormats:"), (id)only);
		((void (*)(id, SEL, id))objc_msgSend)(requirements, sel_registerName("setPreferredPixelFormats:"), (id)only);
		CFRelease(only);
		CFRelease(bgra);
		((void (*)(id, SEL, id))original_set_format_requirements)(self, selector, requirements);
		/* (redfin, B8: what the input holds after it took them) */
		say("redwood: mediaserverd:   the sink's input now asks for %s\n", format_list(send_if(self, "formatRequirements")));
		return;
	}
	((void (*)(id, SEL, id))original_set_format_requirements)(self, selector, requirements);
}

struct method_spec {
	const char *selector, *types;
	IMP imp;
};

static Class
make_class(const char *name, const struct method_spec *methods, size_t count)
{
	Class cls = objc_allocateClassPair(objc_getClass("NSObject"), name, 0);

	for (size_t i = 0; i < count; i++) {
		class_addMethod(cls, sel_registerName(methods[i].selector), methods[i].imp, methods[i].types);
	}
	objc_registerClassPair(cls);
	return cls;
}

static void
flashlight_classes(void)
{
	static const struct method_spec vendor[] = {
		{ "registerClientWithPID:clientDescription:clientPriority:canStealFromClientsWithSamePriority:"
		  "deviceSharingWithOtherClientsAllowed:deviceAvailabilityChangedHandler:", "i@:i@iBB@", (IMP)flashlight_register },
		{ "copyDeviceForClient:informClientWhenDeviceAvailableAgain:error:", "@@:iB^i", (IMP)flashlight_copy_device },
		{ "copyStreamForFlashlightWithPosition:deviceType:forDevice:", "@@:ii@", (IMP)flashlight_copy_stream },
		{ "takeBackDevice:forClient:informClientWhenDeviceAvailableAgain:", "v@:@iB", (IMP)flashlight_take_back },
		{ "activeDeviceEquals:", "B@:@", (IMP)flashlight_active_equals },
	}, stream[] = {
		{ "supportedProperties", "@@:", (IMP)flashlight_stream_properties },
		{ "setProperty:value:", "i@:@@", (IMP)flashlight_stream_set },
	};

	vendor_class = make_class("RedwoodFlashlightVendor", vendor, sizeof(vendor) / sizeof(vendor[0]));
	device_class = make_class("RedwoodFlashlightDevice", NULL, 0);
	stream_class = make_class("RedwoodFlashlightStream", stream, sizeof(stream) / sizeof(stream[0]));
}

/* ---- (redfin ws-cam-noml) the capture graph's inference and Live Photo extras, off by a flag ------- */

/*
 * With Metal in mediaserverd (hm-int2: every process on hakios-metal) the photo preview's graph gets what an
 * iPhone X gets: Espresso networks run on the GPU (BWEspressoInferenceProvider 101/102/103 -- MPSCNN
 * cnnConvArray_*, TARR_* -- the smart camera's scene classification and its kin, ~27 ms a convolution and ~211 ms a
 * command buffer on the Adreno 620), Live Photo's stabilization (kernelCornerDetect*, kernelNccMatch,
 * kernel*Solver, kernelWarpImage) and SDOF (SDOFRenderingV3). Without Metal (before hm-int2) none of it was built.
 * /private/var/local/camera-noml starting with '1' answers NO to the switches the graph builder reads for them, so
 * the nodes are not made; a photograph and the Live Photo movie still are. Read at each call (the graph is built
 * when the session starts), so a new camera session follows the file with no restart. No file, or anything else in
 * it: the stack's own answers, as before.
 */
static bool
noml_on(void)
{
	char c = 0;
	int fd = open("/private/var/local/camera-noml", O_RDONLY);

	if (fd < 0) {
		return false;
	}
	if (read(fd, &c, 1) != 1) {
		c = 0;
	}
	close(fd);
	return c == '1';
}

#define NOML_SWITCH(fn, what)                                                                                     \
	static IMP original_##fn;                                                                                 \
	static BOOL                                                                                               \
	fn(id self, SEL selector)                                                                                 \
	{                                                                                                         \
		BOOL asked = original_##fn != NULL ? ((BOOL (*)(id, SEL))original_##fn)(self, selector) : NO;    \
		static bool said;                                                                                 \
                                                                                                                  \
		if (!asked || !noml_on()) {                                                                       \
			return asked;                                                                             \
		}                                                                                                 \
		if (!said) {                                                                                      \
			said = true;                                                                              \
			say("redwood: mediaserverd: camera-noml: %s asked for, answered NO\n", what);             \
		}                                                                                                 \
		return NO;                                                                                        \
	}

NOML_SWITCH(noml_smart_camera, "the smart camera (scene classification)")
NOML_SWITCH(noml_face_tracking_plus, "face tracking plus")
NOML_SWITCH(noml_iris_sdof, "Live Photo SDOF")
NOML_SWITCH(noml_iris_vis, "Live Photo stabilization")
NOML_SWITCH(noml_preview_depth, "preview depth")
NOML_SWITCH(noml_source_depth, "the source's depth")
NOML_SWITCH(noml_matte, "a Live Photo's portrait effects matte")

/*
 * The class methods replaced, each as soon as its class exists. `expected` is the unslid address
 * of the code in iOS 16.5 (20F66): a method that is not what it was is left alone. `original`, where
 * there is one, keeps what was there (the vendor's: given as it is wherever there is one, and where
 * there is none, ours only with the kernel's camera or flashlight).
 */
static struct replacement {
	const char *class_name, *selector;
	uintptr_t expected;
	IMP replacement, *original;
	bool instance;                  /* an instance method, not a class method */
	bool done;
	bool inherited_too;             /* redfin: a method the class only inherits gets an override of its own (original: the superclass's) */
} replacements[] = {
	{ "BWCoreImageFilterRenderer", "prewarmPortraitFilterVersion:semanticStyleFilters:", 0 /* 16.5: 0x1a2115c0cUL */,
	  (IMP)prewarm_portrait_filter, NULL, false, false },
	{ "BWFigCaptureDeviceVendor", "sharedCaptureDeviceVendor", 0 /* 16.5: 0x1a1ef0d04UL */,
	  (IMP)shared_vendor, &original_shared_vendor, false, false },
	{ "BWAudioSourceNode", "start:", 0 /* 16.5: 0x1a1eefa18UL */, (IMP)audio_source_start, &original_audio_source_start, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationMethod", 0 /* 16.5: 0x1a1e99ef8UL */,
	  (IMP)video_stabilization_method, NULL, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationType", 0 /* 16.5: 0x1a20d8f48UL */,
	  (IMP)video_stabilization_type, NULL, true, false },
	{ "FigCaptureStillImageSettings", "qualityPrioritization", 0 /* 16.5: 0x1a1f533dcUL */,
	  (IMP)quality_prioritization, &original_quality_prioritization, true, false },
	{ "BWGNRNode", "renderSampleBuffer:forInput:", 0 /* 16.5: 0x1a1ec83b8UL */, (IMP)gnr_render, NULL, true, false },
	{ "BWNodeOutput", "emitSampleBuffer:", 0 /* 16.5: 0x1a1ea37e0UL */, (IMP)emit_sample_buffer, &original_emit_sample_buffer, true, false },
	/* (redfin, 16.7.x: not in HackiOS's list) */
	{ "BWMultiFilterThumbnailNode", "prepareForCurrentConfigurationToBecomeLive", 0, (IMP)thumbnail_prepare, NULL, true, false },
	{ "BWMultiFilterThumbnailNode", "renderSampleBuffer:forInput:", 0, (IMP)thumbnail_render, NULL, true, false },
	{ "BWNodeInput", "setFormatRequirements:", 0, (IMP)set_format_requirements, &original_set_format_requirements, true, false },
	/* (redfin, plan B1/B2) */
	{ "BWAudioSourceNode", "stop:", 0, (IMP)audio_source_stop, &original_audio_source_stop, true, false, true },
	{ "FigCaptureMovieFileRecordingSettings", "videoSettings", 0, (IMP)recording_video_settings, &original_video_settings, true, false,
	  true },
	/* (redfin ws-cam-video) the software encoder's compressor given what it can take (camera-vid-nogate: all) */
	{ "BWVideoCompressorNode", "renderSampleBuffer:forInput:", 0, (IMP)vc_render, &original_vc_render, true, false },
	{ "BWVideoCompressorNode", "didReachEndOfDataForInput:", 0, (IMP)vc_end, &original_vc_end, true, false },
	{ "BWVideoCompressorNode", "handleDroppedSample:forInput:", 0, (IMP)vc_dropped, &original_vc_dropped, true, false },
	/* (redfin ws-cam-noml) /private/var/local/camera-noml = 1: the graph's inference and Live Photo extras off */
	{ "FigCaptureSourceConfiguration", "smartCameraEnabled", 0, (IMP)noml_smart_camera, &original_noml_smart_camera, true, false },
	{ "FigMetadataObjectCaptureConnectionConfiguration", "faceTrackingPlusEnabled", 0, (IMP)noml_face_tracking_plus,
	  &original_noml_face_tracking_plus, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "irisSDOFEnabled", 0, (IMP)noml_iris_sdof, &original_noml_iris_sdof, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "irisVISEnabled", 0, (IMP)noml_iris_vis, &original_noml_iris_vis, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "previewDepthDataDeliveryEnabled", 0, (IMP)noml_preview_depth,
	  &original_noml_preview_depth, true, false },
	{ "FigCaptureSourceConfiguration", "depthDataDeliveryEnabled", 0, (IMP)noml_source_depth, &original_noml_source_depth, true, false },
	{ "FigCaptureIrisSinkConfiguration", "portraitEffectsMatteDeliveryEnabled", 0, (IMP)noml_matte, &original_noml_matte, true, false },
};

/* ---- the Metal device leaves this process before the capture stack asks for it -------------------- */

/*
 * With the Metal properties on the accelerator service (boot-arg metal=1) MTLCreateSystemDefaultDevice()
 * answers in every process, and in mediaserverd the first to ask is the capture stack: CMCapture's
 * FigPreloadShadersInternal reaches -[_MTLDevice newLibraryWithURL:error:] through three functions of
 * AppleCVAPhoto, asking for System/Library/PrivateFrameworks/AppleCVAPhoto.framework/Resources/
 * cvadepth-iOS.metallib. That file is not in this image (ios165/select-system.py drops .metallib under
 * the framework directories), so the URL is nil and Metal aborts on `url must not be nil' -- twenty-eight
 * times in one boot, launchd restarting the daemon after each (logs/135751-run.log, docs/metal.md 10.38).
 * AppleCVAPhoto itself survives a library it cannot make: it checks the result and logs "Error creating
 * metal library". What kills the process is Metal's own assertion, and that is reached only because there
 * IS a device to send the message to.
 *
 * So the device is taken out of this process, exactly as inject.c does for the window server: Metal keeps
 * every device it has made in one static NSMutableArray (0x1e1efe9a0, __DATA_DIRTY.__bss, so one per
 * process), and MTLCreateSystemDefaultDevice() is no more than "if that array holds exactly one object,
 * return it" -- an empty array answers nil without a single instruction of Metal's being touched. The
 * array is built on first use, so it has to be asked for first; that makes the device object, which is
 * what the capture stack's next instruction would have done anyway, so the process pays nothing new.
 * The device is retained and never released, for inject.c's reason: what our -dealloc would do to a GPU
 * is not measured.
 *
 * WHEN, and why not where inject.c does it. There the hook is -[CAWindowServer _detectDisplays], the last
 * Objective-C method on the way in and an address that can be checked. Here the way in is three C
 * functions of AppleCVAPhoto with no method of ours to stand on, so this runs from the add-image notifier
 * this library already registers, the first time Metal is in the process at all -- which is before any
 * code of the image that brought it has run. Being early cannot cost correctness: if the device can be
 * made, it is made and removed; if the accelerator service is not matched yet and the array is built
 * empty, nil is what the capture stack gets, and nil is the whole point.
 *
 * Boot-arg mediamtl=1 leaves the device where it is. It is a switch of its own and not inject.c's camtl=1
 * because the two processes do not risk the same thing: a window server that aborts is a crash loop and a
 * black screen, while mediaserverd aborting costs HEIC decoding, the camera and the sound. A boot may
 * reasonably want Metal in one and not the other. Either way the metallib has to be in the image first,
 * or the same assertion comes straight back.
 */
#define MTL_DEVICE_ARRAY        0x1e1efe9a0UL   /* Metal's static NSMutableArray *_deviceArray */

extern id objc_retain(id object);

static bool metal_left_alone;                   /* boot-arg mediamtl=1 */

static bool
boot_arg(const char *what)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;

	return sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
	    strstr(arguments, what) != NULL;
}

static void
keep_metal_from_the_capture_stack(void)
{
	static bool done;
	unsigned long (*ask)(id, SEL) = (unsigned long (*)(id, SEL))objc_msgSend;
	void (*tell)(id, SEL) = (void (*)(id, SEL))objc_msgSend;
	id *slot = (id *)(MTL_DEVICE_ARRAY + slide);
	id (*create)(void);
	id device, array;
	unsigned long held;

	if (done || metal_left_alone) {
		return;
	}
	/* Metal is not in this process yet: the image that brings it has not arrived, and the notifier
	 * will be back here when it does. Nothing is printed for that -- a line per image would bury
	 * the log, and the constructor has already said that this is armed. */
	if ((create = (id (*)(void))dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice")) == NULL) {
		return;
	}
	done = true;
	if ((device = create()) == NULL) {
		say("redwood: mediaserverd: no Metal device in this process, so the capture stack's shader "
		    "preload has nothing to ask\n");
		return;
	}
	if ((array = *slot) == NULL) {
		say("redwood: mediaserverd: a device, but nothing at %#lx; the device list is NOT emptied\n",
		    MTL_DEVICE_ARRAY);
		return;
	}
	if (!((BOOL (*)(id, SEL, Class))objc_msgSend)(array, sel_registerName("isKindOfClass:"),
	    objc_getClass("NSMutableArray"))) {
		say("redwood: mediaserverd: %#lx holds a %s, not Metal's device array; left alone\n",
		    MTL_DEVICE_ARRAY, object_getClassName(array));
		return;
	}
	held = ask(array, sel_registerName("count"));
	objc_retain(device);
	tell(array, sel_registerName("removeAllObjects"));
	say("redwood: mediaserverd: %s taken out of the capture stack (%lu in the list, %lu now); "
	    "MTLCreateSystemDefaultDevice() = %s\n", object_getClassName(device), held,
	    ask(array, sel_registerName("count")), create() != NULL ? "A DEVICE STILL" : "nil");
}

/*
 * redfin (r19a crash loop, 2026-10-25): every Core Image prewarm of the capture stack made a no-op, not only the
 * portrait filters' (replacements[0]): mediaserverd died at pc 0 in Core Image from a CMCapture dispatch block ~5 s
 * after it started (Core Image's GL path, which has no functions here). Every method whose name begins with
 * "prewarm" (after any '_') of a class of the image BWCoreImageFilterRenderer is in, whose class name speaks of Core
 * Image, filters or styles, and which returns nothing (or a BOOL: NO) is replaced; each is said.
 * /private/var/local/camera-no-prewarm-fix leaves them all as they are.
 */
static void
prewarm_none(void)
{
}

static long
prewarm_no(void)
{
	return 0;
}

static bool
prewarm_class(const char *name)
{
	static const char *const parts[] = { "CoreImage", "Filter", "SemanticStyle", "SmartStyle", "Style", "Portrait", "DeepFusion",
		"CIContext", "Renderer", NULL };

	for (int i = 0; parts[i] != NULL; i++) {
		if (strstr(name, parts[i]) != NULL) {
			return true;
		}
	}
	return false;
}

static void
prewarm_sweep_class(Class cls, bool meta, unsigned *count)
{
	unsigned n = 0;
	Method *methods = class_copyMethodList(cls, &n);

	for (unsigned i = 0; i < n; i++) {
		const char *sel = sel_getName(method_getName(methods[i])), *types = method_getTypeEncoding(methods[i]);

		while (*sel == '_') {
			sel++;
		}
		if (strncasecmp(sel, "prewarm", 7) != 0 || types == NULL) {
			continue;
		}
		if (strstr(sel, "ompletion") != NULL || strstr(sel, "andler") != NULL || strstr(sel, "lock") != NULL) {
			say("redwood: mediaserverd: %c[%s %s] (takes a completion) left alone\n", meta ? '+' : '-', class_getName(cls),
			    sel_getName(method_getName(methods[i])));
			continue;
		}
		IMP was = method_getImplementation(methods[i]);

		if (was == (IMP)prewarm_portrait_filter) {
			continue;
		}
		if (types[0] == 'v') {
			method_setImplementation(methods[i], (IMP)prewarm_none);
		} else if (types[0] == 'B' || types[0] == 'c') {
			method_setImplementation(methods[i], (IMP)prewarm_no);
		} else {
			say("redwood: mediaserverd: %c[%s %s] (returns %c) left alone\n", meta ? '+' : '-', class_getName(cls),
			    sel_getName(method_getName(methods[i])), types[0]);
			continue;
		}
		(*count)++;
		say("redwood: mediaserverd: %c[%s %s] (at %#lx unslid) made a no-op\n", meta ? '+' : '-', class_getName(cls),
		    sel_getName(method_getName(methods[i])), (uintptr_t)was - slide);
	}
	free(methods);
}

static void
prewarm_sweep(void)
{
	static bool done;
	Class renderer = objc_getClass("BWCoreImageFilterRenderer");
	const char *image;
	const char **names;
	unsigned n = 0, count = 0;

	if (done || renderer == Nil) {
		return;
	}
	done = true;
	if (access("/private/var/local/camera-no-prewarm-fix", F_OK) == 0) {
		say("redwood: mediaserverd: the capture stack's Core Image prewarms left as they are (camera-no-prewarm-fix)\n");
		return;
	}
	if ((image = class_getImageName(renderer)) == NULL || (names = objc_copyClassNamesForImage(image, &n)) == NULL) {
		say("redwood: mediaserverd: the capture stack's classes cannot be listed: its prewarms left as they are\n");
		return;
	}
	for (unsigned i = 0; i < n; i++) {
		Class cls;

		if (!prewarm_class(names[i]) || (cls = objc_getClass(names[i])) == Nil) {
			continue;
		}
		prewarm_sweep_class(cls, false, &count);
		prewarm_sweep_class(object_getClass((id)cls), true, &count);
	}
	free(names);
	say("redwood: mediaserverd: %u Core Image prewarm methods of %u classes made no-ops\n", count, n);
}

static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
	/* before anything of the image that was just added runs, and this is the only place in this
	 * library that is told about a new image at all */
	keep_metal_from_the_capture_stack();
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		struct replacement *r = &replacements[i];
		Class cls;
		Method method;
		IMP original;

		if (r->done || (cls = objc_getClass(r->class_name)) == NULL) {
			continue;
		}
		r->done = true;
		method = r->instance ? class_getInstanceMethod(cls, sel_registerName(r->selector)) :
		    class_getClassMethod(cls, sel_registerName(r->selector));
		original = method ? method_getImplementation(method) : NULL;
		/* redfin: a method the class only inherits is not replaced (that would replace its superclass's) */
		if (method != NULL && r->instance && class_getSuperclass(cls) != Nil &&
		    class_getInstanceMethod(class_getSuperclass(cls), sel_registerName(r->selector)) == method) {
			if (r->inherited_too && r->original != NULL &&
			    class_addMethod(cls, sel_registerName(r->selector), r->replacement, method_getTypeEncoding(method))) {
				*r->original = original;
				say("redwood: mediaserverd: -[%s %s] is its superclass's (at %#lx unslid): given one of its own\n", r->class_name,
				    r->selector, (uintptr_t)original - slide);
				continue;
			}
			say("redwood: -[%s %s] is its superclass's: left alone\n", r->class_name, r->selector);
			continue;
		}
		/*
		 * redfin: `expected' is iOS 16.5's (HackiOS's phone); on the iPhone X's 16.7.x it is 0 here, unknown: the
		 * method is replaced by its class and selector alone, and where its code lies is said, so that it can be
		 * put here once it is known to be the same method.
		 */
		if (original == NULL || (r->expected != 0 && (uintptr_t)original != r->expected + slide)) {
			say("redwood: %c[%s %s]: found %#lx, expected %#lx; left alone\n", r->instance ? '-' : '+', r->class_name,
			    r->selector,
			    original ? (uintptr_t)original - slide : 0, r->expected);
			continue;
		}
		if (r->original != NULL) {
			*r->original = original;
		}
		method_setImplementation(method, r->replacement);
		say("redwood: mediaserverd: %c[%s %s] (at %#lx unslid) replaced\n", r->instance ? '-' : '+', r->class_name, r->selector,
		    (uintptr_t)original - slide);
	}
	prewarm_sweep();
}

__attribute__((constructor))
static void
redwood_media_init(void)
{
	const char *name = getprogname();
	size_t length = 0;

	if (name == NULL || strcmp(name, "mediaserverd") != 0) {
		return;
	}
	redwood_camera_log_check("load");
	say("redwood: mediaserverd: RedwoodAudio built %s %s (camera plug-in loaded)\n", __DATE__, __TIME__);
	shared_cache_range = (const void *(*)(size_t *))dlsym(RTLD_DEFAULT, "_dyld_get_shared_cache_range");
	slide = shared_cache_range != NULL ? (uintptr_t)shared_cache_range(&length) - CACHE_BASE : 0;
	/* redfin: MTL_DEVICE_ARRAY is iOS 16.5's address; on this build Metal's device list is left alone always */
	metal_left_alone = true;
	(void)boot_arg;
	{
		int level = 0;
		size_t size = sizeof(level);

		torch = sysctlbyname("kern.sweet_torch", &level, &size, NULL, 0) == 0 ? 1 : -1;
	}
	camera = access("/dev/redwood-camera", F_OK) == 0;
	if (torch >= 0) {
		flashlight_classes();
		say("redwood: mediaserverd: the kernel has a flashlight (kern.sweet_torch)\n");
	}
	if (!camera) {
		/* redfin: without the kernel's camera (sweetcamdev=1) nothing of the capture stack is touched */
		say("redwood: mediaserverd: no /dev/redwood-camera: the capture stack is left as it is\n");
		return;
	}
	say("redwood: mediaserverd: the kernel has a camera (/dev/redwood-camera); slide %#lx; RedwoodAudio built %s %s\n", slide,
	    __DATE__, __TIME__);
	/* (what is to be replaced, said first: a crash log shows how far it got) */
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		say("redwood: mediaserverd: to replace %c[%s %s]\n", replacements[i].instance ? '-' : '+', replacements[i].class_name,
		    replacements[i].selector);
	}
	register_hevc_encoder();
	register_venus_encoder();
	_dyld_register_func_for_add_image(image_added);         /* also called for every image already there */
	say("redwood: mediaserverd: the replacements armed\n");
	if (access("/private/var/local/camera-encoder-scan", F_OK) == 0) {
		dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), NULL,
		    search_avc_encoder);
	}
}
