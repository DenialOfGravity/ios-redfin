/*
 * vtenc_test (ws-vt-venc): a VTCompressionSession the way a recording makes one, through whatever encoder VideoToolbox
 * picks (vtvenc.dylib registered first, as mediaserverd does), frames from IOSurface-backed 420v buffers, the output
 * written as an Annex B elementary stream (parameter sets before every sync sample) for ffprobe/ffmpeg.
 * usage: vtenc_test [-c avc1|hvc1] [-s WxH] [-n FRAMES] [-f FPS] [-b BITRATE] [-o OUT] [-i IN.nv12 (tight NV12 WxH)]
 *                   [-p pool|own|venus] [-r 0|1 (real time pacing at FPS, default 1)] [-e ENCODER-ID] [-k KEYSECS]
 *                   [-N (don't register vtvenc)] [-L DYLIB] [-V (register VCPHEVC's software encoder first, as
 *                   mediaserverd does)] [-R (let VideoToolbox run the encoder remotely, in mediaserverd)]
 *                   [-z N (luma noise, N/8 of +-16 per pixel, every frame)]
 *   -p pool: buffers from the session's pixel buffer pool (default); own: CVPixelBufferCreate with IOSurface (any
 *      layout CoreVideo picks); venus: own buffers in Venus's NV12 layout (stride 128, rows 32: zero-copy in venusd).
 * Prints the encoder VideoToolbox took, ms per frame, fps, sync samples, bytes, CPU of this process.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static FILE *out;
static unsigned n_out, n_sync, n_err, n_drop;
static size_t bytes;
static int hevc;
static double t_first, t_last, lat_sum, lat_max;
static double t_sent[100000];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void put_sc(const uint8_t *p, size_t n)
{
	static const uint8_t sc[4] = { 0, 0, 0, 1 };
	fwrite(sc, 1, 4, out);
	fwrite(p, 1, n, out);
}

static void cb(void *ref, void *frame, OSStatus st, VTEncodeInfoFlags flags, CMSampleBufferRef sb)
{
	(void)ref;
	pthread_mutex_lock(&mu);
	unsigned idx = (unsigned)(uintptr_t)frame;
	if (st || !sb) {
		if (st)
			n_err++;
		else
			n_drop++;
		if (n_err + n_drop < 5)
			printf("vtenc_test: frame %u: status %d flags %#x (no sample)\n", idx, (int)st, (unsigned)flags);
		pthread_mutex_unlock(&mu);
		return;
	}
	double t = now();
	if (!n_out)
		t_first = t;
	t_last = t;
	if (idx < 100000 && t_sent[idx] > 0) {
		double l = 1e3 * (t - t_sent[idx]);
		lat_sum += l;
		lat_max = l > lat_max ? l : lat_max;
	}
	n_out++;
	CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
	CFDictionaryRef a = att && CFArrayGetCount(att) ? CFArrayGetValueAtIndex(att, 0) : NULL;
	int sync = !(a && CFDictionaryGetValue(a, kCMSampleAttachmentKey_NotSync) == kCFBooleanTrue);
	n_sync += sync;
	CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sb);
	if (out && sync && fd) {
		size_t cnt = 0;
		const uint8_t *p;
		size_t n;
		int nh = 0;
		if (hevc) {
			CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, 0, NULL, NULL, &cnt, &nh);
			for (size_t i = 0; i < cnt; i++)
				if (!CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, i, &p, &n, NULL, NULL))
					put_sc(p, n);
		} else {
			CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, 0, NULL, NULL, &cnt, &nh);
			for (size_t i = 0; i < cnt; i++)
				if (!CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, i, &p, &n, NULL, NULL))
					put_sc(p, n);
		}
		if (n_out == 1)
			printf("vtenc_test: format: %u parameter sets, NAL length %d\n", (unsigned)cnt, nh);
	}
	CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
	size_t len = bb ? CMBlockBufferGetDataLength(bb) : 0;
	uint8_t *buf = malloc(len ? len : 1);
	if (bb && buf && !CMBlockBufferCopyDataBytes(bb, 0, len, buf)) {
		for (size_t o = 0; o + 4 <= len;) {
			size_t nl = (size_t)buf[o] << 24 | (size_t)buf[o + 1] << 16 | (size_t)buf[o + 2] << 8 | buf[o + 3];
			if (o + 4 + nl > len)
				break;
			if (out)
				put_sc(buf + o + 4, nl);
			o += 4 + nl;
		}
	}
	free(buf);
	bytes += len;
	pthread_mutex_unlock(&mu);
}

static void list_encoders(void)
{
	CFArrayRef list = NULL;
	if (VTCopyVideoEncoderList(NULL, &list) || !list)
		return;
	for (CFIndex i = 0; i < CFArrayGetCount(list); i++) {
		CFDictionaryRef e = CFArrayGetValueAtIndex(list, i);
		CFNumberRef type = CFDictionaryGetValue(e, kVTVideoEncoderList_CodecType);
		CFStringRef id = CFDictionaryGetValue(e, kVTVideoEncoderList_EncoderID);
		CFBooleanRef hw = CFDictionaryGetValue(e, kVTVideoEncoderList_IsHardwareAccelerated);
		uint32_t t = 0;
		char text[160] = "?";
		if (type)
			CFNumberGetValue(type, kCFNumberSInt32Type, &t);
		if (id)
			CFStringGetCString(id, text, sizeof text, kCFStringEncodingUTF8);
		printf("vtenc_test: encoder %c%c%c%c %s%s\n", t >> 24, t >> 16 & 255, t >> 8 & 255, t & 255, text,
		       hw == kCFBooleanTrue ? " (hardware)" : "");
	}
	CFRelease(list);
}

static void setnum(VTCompressionSessionRef s, CFStringRef k, double v)
{
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberDoubleType, &v);
	OSStatus st = VTSessionSetProperty(s, k, n);
	if (st)
		printf("vtenc_test: set %s: %d\n", CFStringGetCStringPtr(k, kCFStringEncodingUTF8), (int)st);
	CFRelease(n);
}

static void dnum(CFMutableDictionaryRef d, CFStringRef k, int v)
{
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
	CFDictionarySetValue(d, k, n);
	CFRelease(n);
}

static CVPixelBufferRef make_buffer(int w, int h, const char *mode)
{
	CFMutableDictionaryRef a = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
							     &kCFTypeDictionaryValueCallBacks);
	CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
						   &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(a, kCVPixelBufferIOSurfacePropertiesKey, empty);
	if (!strcmp(mode, "venus")) {
		dnum(a, kCVPixelBufferBytesPerRowAlignmentKey, 128);
		dnum(a, kCVPixelBufferExtendedPixelsBottomKey, ((h + 31) & ~31) - h);
		dnum(a, kCVPixelBufferPlaneAlignmentKey, 4096);
	}
	CVPixelBufferRef pb = NULL;
	CVReturn r = CVPixelBufferCreate(NULL, (size_t)w, (size_t)h, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, a, &pb);
	CFRelease(a);
	CFRelease(empty);
	return r ? NULL : pb;
}

static int noise;
static void fill(CVPixelBufferRef pb, int w, int h, int i, const uint8_t *src)
{
	CVPixelBufferLockBaseAddress(pb, 0);
	uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *c = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
	size_t yb = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), cb = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
	for (int r = 0; r < h; r++)
		if (src)
			memcpy(y + r * yb, src + (size_t)r * w, (size_t)w);
		else
			for (int x = 0; x < w; x++)
				y[r * yb + x] = (uint8_t)(((x + i * 4) ^ (r + i * 2)) & 0xff) / 2 + 40 + ((x / 64 + r / 64 + i / 8) & 1) * 60;
	if (noise)	/* sensor-like noise on luma (the camera at gain 8x): what makes the bit rate */
		for (int r = 0; r < h; r++)
			for (int x = 0; x < w; x++)
				y[r * yb + x] = (uint8_t)(y[r * yb + x] + (int)((arc4random() & 31) - 16) * noise / 8);
	for (int r = 0; r < h / 2; r++)
		if (src)
			memcpy(c + r * cb, src + (size_t)w * h + (size_t)r * w, (size_t)w);
		else
			for (int x = 0; x < w; x += 2)
				c[r * cb + x] = (uint8_t)(128 + ((x + i) & 63) - 32), c[r * cb + x + 1] = (uint8_t)(128 + ((r - i) & 63) - 32);
	CVPixelBufferUnlockBaseAddress(pb, 0);
}

int main(int argc, char **argv)
{
	const char *codec = "avc1", *outp = NULL, *inp = NULL, *pool_mode = "pool", *enc_id = NULL,
		   *dylib = "/var/local/lib/vtvenc.dylib";
	int w = 1920, h = 1080, nf = 300, rt = 1, reg = 1, vcp = 0, local = 1;
	double fps = 30, br = 15300000, keysecs = 1;
	int o;
	setvbuf(stdout, NULL, _IONBF, 0);
	while ((o = getopt(argc, argv, "c:s:n:f:b:o:i:p:r:e:k:NL:VRz:")) != -1) {
		switch (o) {
		case 'c': codec = optarg; break;
		case 's': sscanf(optarg, "%dx%d", &w, &h); break;
		case 'n': nf = atoi(optarg); break;
		case 'f': fps = atof(optarg); break;
		case 'b': br = atof(optarg); break;
		case 'o': outp = optarg; break;
		case 'i': inp = optarg; break;
		case 'p': pool_mode = optarg; break;
		case 'r': rt = atoi(optarg); break;
		case 'e': enc_id = optarg; break;
		case 'k': keysecs = atof(optarg); break;
		case 'N': reg = 0; break;
		case 'L': dylib = optarg; break;
		case 'V': vcp = 1; break;
		case 'R': local = 0; break;
		case 'z': noise = atoi(optarg); break;
		default: fprintf(stderr, "usage: see the source header\n"); return 2;
		}
	}
	if (nf > 99999)
		nf = 99999;
	hevc = !strcmp(codec, "hvc1");
	if (vcp) {
		void *c = dlopen("/System/Library/VideoCodecs/VCPHEVC.videocodec", RTLD_NOW | RTLD_LOCAL);
		void *create = c ? dlsym(c, "HEVCVideoEncoder_CreateInstance") : NULL;
		int (*re)(uint32_t, CFDictionaryRef, void *) = dlsym(RTLD_DEFAULT, "VTRegisterVideoEncoderWithInfo");
		if (create && re) {
			const void *k[] = { kVTVideoEncoderList_EncoderID, kVTVideoEncoderList_EncoderName,
				kVTVideoEncoderList_IsHardwareAccelerated };
			const void *v[] = { CFSTR("com.apple.videotoolbox.videoencoder.hevc.vcp"), CFSTR("HEVC (VCPHEVC, software)"),
				kCFBooleanFalse };
			CFDictionaryRef info = CFDictionaryCreate(NULL, k, v, 3, &kCFTypeDictionaryKeyCallBacks,
								  &kCFTypeDictionaryValueCallBacks);
			printf("vtenc_test: VCPHEVC registered for hvc1 (%d)\n", re('hvc1', info, create));
			CFRelease(info);
		}
	}
	if (reg) {
		void *d = dlopen(dylib, RTLD_NOW | RTLD_LOCAL);
		int (*r)(void) = d ? (int (*)(void))dlsym(d, "VTVenusEncRegister") : NULL;
		printf("vtenc_test: %s: %s\n", dylib, r ? "" : dlerror());
		if (r)
			printf("vtenc_test: VTVenusEncRegister: %d encoders registered\n", r());
	}
	list_encoders();
	uint8_t *src = NULL;
	size_t fsz = (size_t)w * h * 3 / 2, nsrc = 0;
	if (inp) {
		FILE *f = fopen(inp, "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			long L = ftell(f);
			fseek(f, 0, SEEK_SET);
			nsrc = L > 0 ? (size_t)L / fsz : 0;
			src = nsrc ? malloc(nsrc * fsz) : NULL;
			if (src && fread(src, 1, nsrc * fsz, f) != nsrc * fsz)
				nsrc = 0;
			fclose(f);
		}
		printf("vtenc_test: %s: %zu frames\n", inp, nsrc);
	}
	if (outp)
		out = fopen(outp, "wb");
	CFMutableDictionaryRef spec = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
								&kCFTypeDictionaryValueCallBacks);
	if (enc_id) {
		CFStringRef s = CFStringCreateWithCString(NULL, enc_id, kCFStringEncodingUTF8);
		CFDictionarySetValue(spec, kVTVideoEncoderSpecification_EncoderID, s);
		CFRelease(s);
	}
	VTCompressionSessionRef s = NULL;
	double t0 = now();
	/* VideoToolbox runs encoders in mediaserverd (VTCompressionSessionRemote) unless the process is that server or
	 * the session is created with kVTCompressionSessionOption_AllowClientProcessEncode (iOS 16.7 VT,
	 * VTCompressionSessionCreateWithOptions): our encoder is registered in this process, so encode here */
	typedef OSStatus (*cwo_t)(CFAllocatorRef, int32_t, int32_t, CMVideoCodecType, CFDictionaryRef, CFDictionaryRef,
				  CFAllocatorRef, VTCompressionOutputCallback, void *, CFDictionaryRef, VTCompressionSessionRef *);
	cwo_t cwo = (cwo_t)dlsym(RTLD_DEFAULT, "VTCompressionSessionCreateWithOptions");
	CFStringRef *k_local = (CFStringRef *)dlsym(RTLD_DEFAULT, "kVTCompressionSessionOption_AllowClientProcessEncode");
	CFDictionaryRef opts = NULL;
	if (k_local && local) {
		const void *ok[] = { *k_local }, *ov[] = { kCFBooleanTrue };
		opts = CFDictionaryCreate(NULL, ok, ov, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	printf("vtenc_test: encoders %s\n", opts && cwo ? "in this process (AllowClientProcessEncode)" : "where VideoToolbox puts them");
	OSStatus st = opts && cwo ? cwo(NULL, w, h, hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264, spec, NULL, NULL, cb,
				       NULL, opts, &s) :
		VTCompressionSessionCreate(NULL, w, h, hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264, spec, NULL,
					   NULL, cb, NULL, &s);
	if (opts)
		CFRelease(opts);
	CFRelease(spec);
	printf("vtenc_test: VTCompressionSessionCreate %s %dx%d: %d (%.0f ms)\n", codec, w, h, (int)st, 1e3 * (now() - t0));
	if (st || !s)
		return 1;
	VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
	VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
	setnum(s, kVTCompressionPropertyKey_AverageBitRate, br);
	setnum(s, kVTCompressionPropertyKey_ExpectedFrameRate, fps);
	setnum(s, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, keysecs);
	VTSessionSetProperty(s, kVTCompressionPropertyKey_ProfileLevel,
			     hevc ? kVTProfileLevel_HEVC_Main_AutoLevel : kVTProfileLevel_H264_High_AutoLevel);
	VTSessionSetProperty(s, kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2);
	VTSessionSetProperty(s, kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2);
	VTSessionSetProperty(s, kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
	CFTypeRef id = NULL, hw = NULL;
	VTSessionCopyProperty(s, CFSTR("EncoderID"), NULL, &id);
	VTSessionCopyProperty(s, CFSTR("UsingHardwareAcceleratedVideoEncoder"), NULL, &hw);
	char idt[160] = "?";
	if (id && CFGetTypeID(id) == CFStringGetTypeID())
		CFStringGetCString(id, idt, sizeof idt, kCFStringEncodingUTF8);
	printf("vtenc_test: the session's encoder: %s, hardware %s\n", idt, hw == kCFBooleanTrue ? "yes" : hw ? "no" : "?");
	VTCompressionSessionPrepareToEncodeFrames(s);
	CVPixelBufferPoolRef pool = !strcmp(pool_mode, "pool") ? VTCompressionSessionGetPixelBufferPool(s) : NULL;
	if (pool) {	/* the session's pool must give 2-plane buffers, else our own */
		CVPixelBufferRef t = NULL;
		if (CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &t) || !t || CVPixelBufferGetPlaneCount(t) != 2) {
			OSType f = t ? CVPixelBufferGetPixelFormatType(t) : 0;
			printf("vtenc_test: the session's pool gives %.4s buffers: own 420v buffers instead\n", (char *)&f);
			pool = NULL;
			pool_mode = "own";
		}
		if (t)
			CVPixelBufferRelease(t);
	}
	CVPixelBufferRef bufs[8] = { 0 };
	if (!pool)
		for (int i = 0; i < 8; i++)
			bufs[i] = make_buffer(w, h, pool_mode);
	struct rusage r0, r1;
	getrusage(RUSAGE_SELF, &r0);
	double ts = now();
	for (int i = 0; i < nf; i++) {
		CVPixelBufferRef pb = NULL;
		if (pool) {
			if (CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb))
				pb = NULL;
		} else {
			pb = CVPixelBufferRetain(bufs[i % 8]);
		}
		if (!pb) {
			printf("vtenc_test: no pixel buffer for frame %d\n", i);
			break;
		}
		if (i == 0)
			printf("vtenc_test: buffer %zux%zu bytes per row %zu/%zu, plane 1 at +%td, IOSurface %s\n",
			       CVPixelBufferGetWidth(pb), CVPixelBufferGetHeight(pb), CVPixelBufferGetBytesPerRowOfPlane(pb, 0),
			       CVPixelBufferGetBytesPerRowOfPlane(pb, 1),
			       (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 1) - (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 0),
			       CVPixelBufferGetIOSurface(pb) ? "yes" : "no");
		if (!pool && i >= 8 && !src && !noise)
			;	/* own buffers keep their pattern after the first 8 (no CPU per frame, as a camera) */
		else
			fill(pb, w, h, i, src ? src + fsz * (size_t)(i % nsrc) : NULL);
		if (rt) {
			double due = ts + i / fps, t = now();
			if (due > t)
				usleep((useconds_t)((due - t) * 1e6));
		}
		CMTime pts = CMTimeMake(i * 1000, (int32_t)(fps * 1000)), dur = CMTimeMake(1000, (int32_t)(fps * 1000));
		t_sent[i] = now();
		st = VTCompressionSessionEncodeFrame(s, pb, pts, dur, NULL, (void *)(uintptr_t)i, NULL);
		CVPixelBufferRelease(pb);
		if (st) {
			printf("vtenc_test: EncodeFrame %d: %d\n", i, (int)st);
			break;
		}
	}
	double te = now();
	st = VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
	double tc = now();
	getrusage(RUSAGE_SELF, &r1);
	double cpu = (r1.ru_utime.tv_sec - r0.ru_utime.tv_sec) + (r1.ru_utime.tv_usec - r0.ru_utime.tv_usec) / 1e6 +
		     (r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) + (r1.ru_stime.tv_usec - r0.ru_stime.tv_usec) / 1e6;
	printf("vtenc_test: CompleteFrames %d (%.0f ms after the last frame)\n", (int)st, 1e3 * (tc - te));
	printf("vtenc_test: %u of %d frames out (%u sync, %u dropped, %u errors), %zu bytes = %.0f kbit/s at %.0f fps; "
	       "submit %.1f fps, output %.1f fps (first->last), latency mean %.1f ms max %.1f ms; CPU %.2f s = %.0f%% of a core "
	       "over %.1f s\n", n_out, nf, n_sync, n_drop, n_err, bytes, n_out ? bytes * 8.0 * fps / n_out / 1e3 : 0, fps,
	       nf / (te - ts), n_out > 1 ? (n_out - 1) / (t_last - t_first) : 0, n_out ? lat_sum / n_out : 0, lat_max, cpu,
	       100 * cpu / (tc - ts), tc - ts);
	VTCompressionSessionInvalidate(s);
	CFRelease(s);
	if (out)
		fclose(out);
	return n_out ? 0 : 1;
}
