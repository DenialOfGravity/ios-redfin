/*
 * vtdecode: can this phone decode video, how fast, and in hardware or software?
 *   vtdecode FILE [SECONDS]
 * 1. AVAssetReader decodes the first video track to BGRA (the whole AVFoundation/VideoToolbox path) and
 *    prints frames, wall time and fps.
 * 2. VTIsHardwareDecodeSupported for the track's codec, and a VTDecompressionSession created with
 *    RequireHardwareAcceleratedVideoDecoder: prints its OSStatus (0 = hardware decoder exists).
 *   vtdecode --raw IN.h264 [--hw require|enable|any|off] [--inproc] [--frames N] [--quiet]
 * Video 4b (the vtvenus plugin, phone/video/vtvenus.c): an Annex B H.264 file (venusc's splitter, venus_h264.c)
 * through one VTDecompressionSession to '420v' buffers: CMSampleBuffers of AVCC access units, pts n/30 s, async +
 * temporal processing, then FinishDelayedFrames. --hw: require (default) / enable hardware (the decoder
 * specification keys), any (no keys), off (EnableHardware... = false: the software reference). --inproc:
 * kVTDecompressionSessionOption_AllowClientProcessDecode, so the decoder runs in this process instead of
 * mediaserverd (a plugin crash only kills vtdecode). Prints the session's hardware flag, then per output frame
 * (output order) pts, size, flags and the CRC32 of the visible luma (venusc's / venusctl's CRC), then frames,
 * dropped, errors, wall time (first DecodeFrame to the last frame) and fps. Exit 0: every AU came back as a frame.
 */
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <pthread.h>
#include <sys/time.h>
#include "venus_h264.h"

/* Older iOS SDK headers (theos 16.5, Linux builds) declare these for macOS only; VideoToolbox exports them. */
extern const CFStringRef kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder;
extern const CFStringRef kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder;
extern const CFStringRef kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder;
extern const CFStringRef kVTDecompressionSessionOption_AllowClientProcessDecode;	/* private */
extern OSStatus VTDecompressionSessionCreateWithOptions(CFAllocatorRef, CMVideoFormatDescriptionRef, CFDictionaryRef,
    CFDictionaryRef, const VTDecompressionOutputCallbackRecord *, CFDictionaryRef, VTDecompressionSessionRef *);

/* ---- --raw (video 4b) ---- */
static pthread_mutex_t raw_mu = PTHREAD_MUTEX_INITIALIZER;
static int raw_out, raw_drop, raw_err, raw_quiet;
static double raw_t_last;
static uint8_t *raw_luma;
static size_t raw_luma_cap;

static double raw_now(void)
{
	struct timeval t;
	gettimeofday(&t, NULL);
	return t.tv_sec + t.tv_usec / 1e6;
}

static void raw_output(void *refcon, void *frame, OSStatus st, VTDecodeInfoFlags flags, CVImageBufferRef ib, CMTime pts,
    CMTime dur)
{
	pthread_mutex_lock(&raw_mu);
	raw_t_last = raw_now();
	if (st || !ib) {
		if (st)
			raw_err++;
		else
			raw_drop++;
		if (!raw_quiet)
			printf("vtdecode: frame (AU %ld): status %d, info flags %#x, %s\n", (long)(intptr_t)frame, (int)st,
			    (unsigned)flags, ib ? "image" : "no image");
		pthread_mutex_unlock(&raw_mu);
		return;
	}
	CVPixelBufferRef pb = (CVPixelBufferRef)ib;
	OSType f = CVPixelBufferGetPixelFormatType(pb);
	size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb);
	uint32_t crc = 0;
	int have = 0;
	if (CVPixelBufferIsPlanar(pb) && !CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly)) {
		const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
		size_t bpr = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), pw = CVPixelBufferGetWidthOfPlane(pb, 0),
		       ph = CVPixelBufferGetHeightOfPlane(pb, 0);
		if (y && pw * ph <= raw_luma_cap) {
			for (size_t r = 0; r < ph; r++)
				memcpy(raw_luma + r * pw, y + r * bpr, pw);
			crc = crc32_buf(raw_luma, pw * ph);
			have = 1;
		}
		CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
	}
	if (!raw_quiet) {
		if (have)
			printf("vtdecode: frame %d: AU %ld, pts %.4f s, %zux%zu %c%c%c%c, info flags %#x, luma crc32 %08x\n", raw_out,
			    (long)(intptr_t)frame, CMTimeGetSeconds(pts), w, h, f >> 24, f >> 16, f >> 8, f, (unsigned)flags, crc);
		else
			printf("vtdecode: frame %d: AU %ld, %zux%zu %c%c%c%c: not planar or not lockable, no crc\n", raw_out,
			    (long)(intptr_t)frame, w, h, f >> 24, f >> 16, f >> 8, f);
	}
	raw_out++;
	pthread_mutex_unlock(&raw_mu);
}

/* the NAL units of one Annex B access unit: next one at *pos, its start and length (no start code, no trailing 0s) */
static int raw_nal(const uint8_t *s, size_t len, size_t *pos, size_t *off, size_t *n)
{
	size_t i = *pos;
	while (i + 3 <= len && !(s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1))
		i++;
	if (i + 3 > len)
		return 0;
	size_t b = i + 3, e = b;
	while (e + 3 <= len && !(s[e] == 0 && s[e + 1] == 0 && s[e + 2] == 1))
		e++;
	if (e + 3 > len)
		e = len;
	*pos = e;
	while (e > b && s[e - 1] == 0)
		e--;
	*off = b, *n = e - b;
	return 1;
}

static int raw_main(int argc, char **argv)
{
	const char *in = NULL, *hw = "require";
	int inproc = 0, maxf = -1;
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--hw") && i + 1 < argc)
			hw = argv[++i];
		else if (!strcmp(argv[i], "--inproc"))
			inproc = 1;
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
			maxf = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--quiet"))
			raw_quiet = 1;
		else if (!in && argv[i][0] != '-')
			in = argv[i];
		else
			in = NULL, i = argc;
	}
	if (!in || maxf == 0 || (strcmp(hw, "require") && strcmp(hw, "enable") && strcmp(hw, "any") && strcmp(hw, "off"))) {
		fprintf(stderr, "usage: vtdecode --raw IN.h264 [--hw require|enable|any|off] [--inproc] [--frames N] [--quiet]\n");
		return 2;
	}
	FILE *fp = fopen(in, "rb");
	if (!fp) {
		printf("vtdecode: can't read %s\n", in);
		return 1;
	}
	fseek(fp, 0, SEEK_END);
	size_t len = (size_t)ftell(fp);
	fseek(fp, 0, SEEK_SET);
	uint8_t *s = malloc(len + 4);
	if (!s || fread(s, 1, len, fp) != len) {
		printf("vtdecode: can't read %s\n", in);
		return 1;
	}
	fclose(fp);
	int cap = (int)(len / 4 + 1);
	uint32_t *off = malloc(sizeof(uint32_t) * (size_t)cap), *alen = malloc(sizeof(uint32_t) * (size_t)cap);
	struct h264_sps sps = { 0, 0, 0, 0, 0, 0 };
	int nau = off && alen ? split_aus(s, len, off, alen, cap, &sps) : 0;
	if (nau < 0)
		nau = -nau - 1;
	/* the first SPS and PPS of the file: the format description */
	const uint8_t *ps[2] = { NULL, NULL };
	size_t psn[2] = { 0, 0 };
	for (size_t pos = 0, o, n; (!ps[0] || !ps[1]) && raw_nal(s, len, &pos, &o, &n);) {
		int t = n ? s[o] & 0x1f : 0;
		if ((t == 7 || t == 8) && !ps[t - 7])
			ps[t - 7] = s + o, psn[t - 7] = n;
	}
	if (!nau || !ps[0] || !ps[1]) {
		printf("vtdecode: %s: %d access units, %s SPS, %s PPS: not usable\n", in, nau, ps[0] ? "an" : "no",
		    ps[1] ? "a" : "no");
		return 1;
	}
	int want = maxf > 0 && maxf < nau ? maxf : nau;
	printf("vtdecode: %s: %zu bytes, %d access units, sending %d; SPS %dx%d visible %dx%d\n", in, len, nau, want, sps.w,
	    sps.h, sps.cw, sps.ch);
	CMVideoFormatDescriptionRef fd = NULL;
	OSStatus st = CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, ps, psn, 4, &fd);
	if (st || !fd) {
		printf("vtdecode: format description from SPS/PPS: OSStatus %d\n", (int)st);
		return 1;
	}
	raw_luma_cap = (size_t)(sps.w > 0 ? sps.w : 4096) * (sps.h > 0 ? sps.h : 2304) + (1 << 20);
	raw_luma = malloc(raw_luma_cap);
	printf("VTIsHardwareDecodeSupported(avc1): %d\n", VTIsHardwareDecodeSupported(kCMVideoCodecType_H264));

	NSMutableDictionary *spec = [NSMutableDictionary dictionary];
	if (!strcmp(hw, "require"))
		spec[(__bridge id)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder] = @YES;
	if (!strcmp(hw, "require") || !strcmp(hw, "enable"))
		spec[(__bridge id)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder] = @YES;
	if (!strcmp(hw, "off"))
		spec[(__bridge id)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder] = @NO;
	NSDictionary *attrs = @{ (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) };
	NSDictionary *opts = inproc ? @{ (__bridge id)kVTDecompressionSessionOption_AllowClientProcessDecode: @YES } : nil;
	VTDecompressionOutputCallbackRecord cb = { raw_output, NULL };
	VTDecompressionSessionRef vs = NULL;
	st = VTDecompressionSessionCreateWithOptions(NULL, fd, (__bridge CFDictionaryRef)spec, (__bridge CFDictionaryRef)attrs,
	    &cb, (__bridge CFDictionaryRef)opts, &vs);
	printf("vtdecode: session (--hw %s%s): OSStatus %d\n", hw, inproc ? ", in process" : ", remote (mediaserverd)", (int)st);
	if (st || !vs)
		return 1;
	CFBooleanRef hwflag = NULL;
	VTSessionCopyProperty(vs, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL, &hwflag);
	printf("vtdecode: using hardware decoder: %s\n", hwflag == kCFBooleanTrue ? "yes" : hwflag ? "no" : "unknown");
	if (hwflag)
		CFRelease(hwflag);

	uint8_t *avcc = malloc(len + 4 * (size_t)cap + 16);
	int sent = 0, derr = 0;
	double t0 = raw_now();
	for (int n = 0; n < want && avcc; n++) {
		const uint8_t *a = s + off[n];
		size_t q = 0;
		for (size_t pos = 0, o, k; raw_nal(a, alen[n], &pos, &o, &k);) {
			int t = k ? a[o] & 0x1f : 0;
			if (!k || t == 7 || t == 8 || t == 9)	/* parameter sets live in the format description; no AUDs */
				continue;
			avcc[q] = (uint8_t)(k >> 24), avcc[q + 1] = (uint8_t)(k >> 16), avcc[q + 2] = (uint8_t)(k >> 8), avcc[q + 3] = (uint8_t)k;
			memcpy(avcc + q + 4, a + o, k);
			q += 4 + k;
		}
		if (!q)
			continue;
		CMBlockBufferRef bb = NULL;
		CMSampleBufferRef sb = NULL;
		CMSampleTimingInfo ti = { CMTimeMake(1, 30), CMTimeMake(n, 30), kCMTimeInvalid };
		if (CMBlockBufferCreateWithMemoryBlock(NULL, NULL, q, NULL, NULL, 0, q, kCMBlockBufferAssureMemoryNowFlag, &bb) ||
		    CMBlockBufferReplaceDataBytes(avcc, bb, 0, q) ||
		    CMSampleBufferCreateReady(NULL, bb, fd, 1, 1, &ti, 1, &q, &sb)) {
			printf("vtdecode: AU %d: no sample buffer\n", n);
			if (bb)
				CFRelease(bb);
			break;
		}
		VTDecodeInfoFlags info = 0;
		st = VTDecompressionSessionDecodeFrame(vs, sb,
		    kVTDecodeFrame_EnableAsynchronousDecompression | kVTDecodeFrame_EnableTemporalProcessing, (void *)(intptr_t)n, &info);
		CFRelease(sb);
		CFRelease(bb);
		sent++;
		if (st) {
			derr++;
			printf("vtdecode: AU %d: DecodeFrame OSStatus %d\n", n, (int)st);
			if (derr > 5)
				break;
		}
	}
	st = VTDecompressionSessionFinishDelayedFrames(vs);
	OSStatus st2 = VTDecompressionSessionWaitForAsynchronousFrames(vs);
	pthread_mutex_lock(&raw_mu);
	double wall = (raw_t_last > t0 ? raw_t_last : raw_now()) - t0;
	int out = raw_out, drop = raw_drop, err = raw_err;
	pthread_mutex_unlock(&raw_mu);
	VTDecompressionSessionInvalidate(vs);
	CFRelease(vs);
	printf("vtdecode: %d frames out of %d AUs sent, %d dropped, %d errors, %d DecodeFrame errors (finish %d, wait %d); "
	    "wall %.2f s = %.1f fps\n", out, sent, drop, err, derr, (int)st, (int)st2, wall, wall > 0 ? out / wall : 0.0);
	return out == sent && !derr && !err ? 0 : 1;
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc > 1 && !strcmp(argv[1], "--raw"))
			return raw_main(argc, argv);
		if (argc < 2) {
			fprintf(stderr, "usage: vtdecode FILE [SECONDS] | vtdecode --raw IN.h264 [--hw require|enable|any|off] "
			    "[--inproc] [--frames N] [--quiet]\n");
			return 2;
		}
		double limit = argc > 2 ? atof(argv[2]) : 15;
		NSURL *u = [NSURL fileURLWithPath:@(argv[1])];
		AVURLAsset *asset = [AVURLAsset URLAssetWithURL:u options:nil];
		NSArray *tracks = [asset tracksWithMediaType:AVMediaTypeVideo];
		printf("tracks: %lu\n", (unsigned long)tracks.count);
		if (!tracks.count)
			return 1;
		AVAssetTrack *t = tracks[0];
		CMFormatDescriptionRef fd = (__bridge CMFormatDescriptionRef)t.formatDescriptions.firstObject;
		FourCharCode codec = CMFormatDescriptionGetMediaSubType(fd);
		CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(fd);
		printf("codec %c%c%c%c %dx%d nominal %.1f fps\n", codec >> 24, codec >> 16, codec >> 8, codec,
		    d.width, d.height, t.nominalFrameRate);
		printf("VTIsHardwareDecodeSupported: %d\n", VTIsHardwareDecodeSupported(codec));
		VTDecompressionSessionRef s = NULL;
		NSDictionary *spec = @{ (__bridge id)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES };
		OSStatus st = VTDecompressionSessionCreate(NULL, fd, (__bridge CFDictionaryRef)spec, NULL, NULL, &s);
		printf("VTDecompressionSession (require hardware): OSStatus %d\n", (int)st);
		if (s)
			VTDecompressionSessionInvalidate(s);
		s = NULL;
		st = VTDecompressionSessionCreate(NULL, fd, NULL, NULL, NULL, &s);
		printf("VTDecompressionSession (any decoder, software allowed): OSStatus %d\n", (int)st);
		if (s) {
			CFBooleanRef hw = NULL;
			VTSessionCopyProperty(s, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL, &hw);
			printf("  using hardware decoder: %s\n", hw == kCFBooleanTrue ? "yes" : "no");
			VTDecompressionSessionInvalidate(s);
		}

		NSError *err = nil;
		AVAssetReader *r = [AVAssetReader assetReaderWithAsset:asset error:&err];
		if (!r) {
			printf("reader: %s\n", err.description.UTF8String);
			return 1;
		}
		AVAssetReaderTrackOutput *o = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:t
		    outputSettings:@{ (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA) }];
		[r addOutput:o];
		if (![r startReading]) {
			printf("startReading failed: %s\n", r.error.description.UTF8String);
			return 1;
		}
		CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
		unsigned frames = 0;
		for (;;) {
			CMSampleBufferRef sb = [o copyNextSampleBuffer];
			if (!sb)
				break;
			frames++;
			CFRelease(sb);
			if (CFAbsoluteTimeGetCurrent() - t0 > limit)
				break;
		}
		double dt = CFAbsoluteTimeGetCurrent() - t0;
		printf("decoded %u frames in %.2f s = %.1f fps (status %ld %s)\n", frames, dt, frames / dt,
		    (long)r.status, r.error ? r.error.description.UTF8String : "");
	}
	return 0;
}
