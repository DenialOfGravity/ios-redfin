/*
 * avplay: does a file play through the stock AVPlayer, and does its video come out? (redfin track V, the last step of
 * the Venus test plan; vtdecode covers VTDecompressionSession, this covers AVFoundation's player as an app uses it)
 *   avplay FILE|URL [SECONDS]
 * Plays FILE (or an http(s) URL, e.g. an HLS playlist) muted for SECONDS (10), with an AVPlayerItemVideoOutput
 * ('420v' IOSurface buffers) pulled every 10 ms: prints the track's codec and size, the item's status, then once a
 * second "t=T frames=N new=K WxH fmt" (frames = new pixel buffers the output handed over), and at the end the
 * item's error and access logs. Exit 0: the item played and at least one frame came out.
 * Whether VideoToolbox picked the hardware decoder shows in /var/local/vtvenus.log (vtvenus's CreateInstance line in
 * mediaserverd) and venusd's log (a session opened), not here.
 */
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>
#include <stdio.h>
#include <stdlib.h>

static NSString *fourcc(FourCharCode c)
{
	return [NSString stringWithFormat:@"%c%c%c%c", (char)(c >> 24), (char)(c >> 16), (char)(c >> 8), (char)c];
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc < 2) {
			fprintf(stderr, "usage: avplay FILE|URL [SECONDS]\n");
			return 2;
		}
		double secs = argc > 2 ? atof(argv[2]) : 10;
		NSString *a = [NSString stringWithUTF8String:argv[1]];
		NSURL *url = [a hasPrefix:@"http"] ? [NSURL URLWithString:a] : [NSURL fileURLWithPath:a];
		AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
		NSArray *vt = [asset tracksWithMediaType:AVMediaTypeVideo];
		if (vt.count) {
			AVAssetTrack *t = vt[0];
			CMFormatDescriptionRef fd = (__bridge CMFormatDescriptionRef)t.formatDescriptions.firstObject;
			CMVideoDimensions d = fd ? CMVideoFormatDescriptionGetDimensions(fd) : (CMVideoDimensions){ 0, 0 };
			printf("avplay: track codec %s %dx%d, %.2f fps, %.0f kbit/s, %.1f s\n",
			       fd ? fourcc(CMFormatDescriptionGetMediaSubType(fd)).UTF8String : "?", d.width, d.height,
			       t.nominalFrameRate, t.estimatedDataRate / 1000, CMTimeGetSeconds(asset.duration));
		} else {
			printf("avplay: no video track in the asset (yet: a stream lists tracks once it plays)\n");
		}
		AVPlayerItem *item = [AVPlayerItem playerItemWithAsset:asset];
		NSDictionary *attrs = @{ (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
					 (id)kCVPixelBufferIOSurfacePropertiesKey: @{} };
		AVPlayerItemVideoOutput *out = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:attrs];
		[item addOutput:out];
		AVPlayer *player = [AVPlayer playerWithPlayerItem:item];
		player.muted = YES;
		[player play];
		double t0 = CFAbsoluteTimeGetCurrent(), next = 1;
		long frames = 0, last = 0;
		int w = 0, h = 0;
		OSType fmt = 0;
		AVPlayerItemStatus st = AVPlayerItemStatusUnknown;
		while (CFAbsoluteTimeGetCurrent() - t0 < secs) {
			[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
			if (item.status != st) {
				st = item.status;
				printf("avplay: item status %s%s%s\n", st == AVPlayerItemStatusReadyToPlay ? "ready" :
				       st == AVPlayerItemStatusFailed ? "FAILED" : "unknown", item.error ? ": " : "",
				       item.error ? item.error.description.UTF8String : "");
				if (st == AVPlayerItemStatusFailed)
					break;
			}
			CMTime it = [out itemTimeForHostTime:CACurrentMediaTime()];
			if ([out hasNewPixelBufferForItemTime:it]) {
				CVPixelBufferRef pb = [out copyPixelBufferForItemTime:it itemTimeForDisplay:NULL];
				if (pb) {
					frames++;
					w = (int)CVPixelBufferGetWidth(pb), h = (int)CVPixelBufferGetHeight(pb);
					fmt = CVPixelBufferGetPixelFormatType(pb);
					CVPixelBufferRelease(pb);
				}
			}
			double t = CFAbsoluteTimeGetCurrent() - t0;
			if (t >= next) {
				printf("avplay: t=%.1f item=%.2f frames=%ld new=%ld %dx%d %s rate %.1f\n", t,
				       CMTimeGetSeconds(item.currentTime), frames, frames - last, w, h, fmt ? fourcc(fmt).UTF8String : "-",
				       player.rate);
				fflush(stdout);
				last = frames, next += 1;
			}
		}
		[player pause];
		for (AVPlayerItemErrorLogEvent *e in item.errorLog.events)
			printf("avplay: error log: %ld %s %s\n", (long)e.errorStatusCode, e.errorDomain.UTF8String,
			       e.errorComment.UTF8String ?: "");
		for (AVPlayerItemAccessLogEvent *e in item.accessLog.events)
			printf("avplay: access log: dropped %ld, observed %.0f kbit/s, indicated %.0f kbit/s, stalls %ld\n",
			       (long)e.numberOfDroppedVideoFrames, e.observedBitrate / 1000, e.indicatedBitrate / 1000,
			       (long)e.numberOfStalls);
		printf("avplay: %ld frames in %.1f s%s\n", frames, CFAbsoluteTimeGetCurrent() - t0,
		       item.status == AVPlayerItemStatusFailed ? ", item FAILED" : "");
		return item.status != AVPlayerItemStatusFailed && frames > 0 ? 0 : 1;
	}
}
