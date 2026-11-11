/*
 * SweetHUD (ws-n-ctl): optional on-screen FPS / GPU / temperature / CPU / RAM overlay, compiled into SweetFPS.dylib
 * (inserted into SpringBoard and apps by xnu 0999, boot sweetfbx & 0x100). Off unless configured.
 *
 * Configuration is one 64-bit Darwin notify state, name "org.iosonpixel.sweethud" (readable inside app sandboxes,
 * unlike /var/local). SweetControl.app sets it and posts the name; every process with the dylib re-reads it at once,
 * so on/off and layout changes need no restart. SpringBoard restores it after a boot from /var/local/sweethud
 * (a line "state=0x..."; the app writes it through sweetctl). Bits:
 *   0      on
 *   1-2    where: 0 SpringBoard only (one window over everything), 1 every app, 2 both
 *   3-5    position: 0 top-left 1 top-centre 2 top-right 3 bottom-left 4 bottom-centre 5 bottom-right
 *   8-15   fields: 0x01 fps, 0x02 GPU MHz, 0x04 hottest temp, 0x08 CPU %, 0x10 free RAM, 0x20 Wi-Fi dBm,
 *          0x40 fps source tag ("fb" = kernel frame counter, "dl" = this process's display link)
 *   16-20  update interval in 100 ms (0 = 500 ms)
 *   21-24  font size = 8 + N pt (0 = 11 pt)
 *   25-28  background opacity in tenths (0 = 0.6)
 *   29     fps colour: green >= 80, yellow >= 50, red below
 * FPS: kern.sweet_fb_frames (xnu 1060: frames that reached the panel, panel buffer + direct scanout), per interval.
 * Without that sysctl (older kernels): this process's CADisplayLink callback rate ("dl"), which is only an estimate.
 *
 * SpringBoard also runs `/var/local/bin/sweetctl boot-apply --once` 5 s after it starts (the kern.sweet_* values the
 * user pinned in SweetControl), so pinned values come back after every reboot without a launchd change.
 */
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#include <mach/mach.h>
#include <notify.h>
#include <spawn.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HUD_NOTIFY	"org.iosonpixel.sweethud"
#define HUD_FILE	"/var/local/sweethud"
#define SWEETCTL	"/var/local/bin/sweetctl"

extern char **environ;

static uint64_t hud_state;
static int hud_is_sb;

@interface SweetHUDWindow : UIWindow
@end
@implementation SweetHUDWindow
- (UIView *)hitTest:(CGPoint)p withEvent:(UIEvent *)e
{
	return nil;	/* never takes a touch */
}
- (BOOL)canBecomeKeyWindow
{
	return NO;
}
- (BOOL)_canBecomeKeyWindow
{
	return NO;
}
@end

@interface SweetHUD : NSObject
@property (strong) SweetHUDWindow *window;
@property (strong) UILabel *label;
@property (strong) NSTimer *timer;
@property (strong) CADisplayLink *link;
@end

static SweetHUD *hud;

/* ---- samples ---- */

static int
sc_int(const char *name, long long *out)
{
	uint64_t v = 0;
	size_t l = sizeof(v);
	if (sysctlbyname(name, &v, &l, NULL, 0) != 0) {
		return -1;
	}
	*out = l == 4 ? (long long)(int32_t)(uint32_t)v : (long long)v;
	return 0;
}

static double
cpu_percent(void)
{
	static uint64_t lastBusy, lastTotal;
	natural_t n = 0;
	processor_info_array_t info = NULL;
	mach_msg_type_number_t cnt = 0;
	if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &n, &info, &cnt) != KERN_SUCCESS) {
		return -1;
	}
	uint64_t busy = 0, total = 0;
	for (natural_t i = 0; i < n; i++) {
		unsigned *t = (unsigned *)&info[i * CPU_STATE_MAX];
		busy += t[CPU_STATE_USER] + t[CPU_STATE_SYSTEM] + t[CPU_STATE_NICE];
		total += t[CPU_STATE_USER] + t[CPU_STATE_SYSTEM] + t[CPU_STATE_NICE] + t[CPU_STATE_IDLE];
	}
	vm_deallocate(mach_task_self(), (vm_address_t)info, cnt * sizeof(integer_t));
	double r = total > lastTotal ? 100.0 * (double)(busy - lastBusy) / (double)(total - lastTotal) : -1;
	lastBusy = busy;
	lastTotal = total;
	return r;
}

static long long
free_mb(void)
{
	vm_statistics64_data_t vs;
	mach_msg_type_number_t c = HOST_VM_INFO64_COUNT;
	if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &c) != KERN_SUCCESS) {
		return -1;
	}
	vm_size_t ps = 0;
	host_page_size(mach_host_self(), &ps);
	return (long long)(vs.free_count + vs.inactive_count) * (long long)ps / (1024 * 1024);
}

@implementation SweetHUD {
	uint64_t _lastFrames;
	CFTimeInterval _lastT;
	unsigned _dlTicks;
}

- (void)tick:(CADisplayLink *)l
{
	_dlTicks++;
}

- (void)apply
{
	uint64_t s = hud_state;
	int on = (s & 1) != 0;
	int where = (int)((s >> 1) & 3);
	if (on) {
		on = hud_is_sb ? where != 1 : where != 0;
	}
	if (!on) {
		[self.timer invalidate];
		self.timer = nil;
		[self.link invalidate];
		self.link = nil;
		self.window.hidden = YES;
		return;
	}
	if (self.window == nil) {
		UIWindowScene *scene = nil;
		for (UIScene *sc in [UIApplication sharedApplication].connectedScenes) {
			if ([sc isKindOfClass:[UIWindowScene class]]) {
				scene = (UIWindowScene *)sc;
				if (sc.activationState == UISceneActivationStateForegroundActive) {
					break;
				}
			}
		}
		self.window = scene ? [[SweetHUDWindow alloc] initWithWindowScene:scene]
				    : [[SweetHUDWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
		self.window.windowLevel = 10000000;
		self.window.userInteractionEnabled = NO;
		self.window.backgroundColor = [UIColor clearColor];
		self.window.rootViewController = [UIViewController new];
		self.window.rootViewController.view.userInteractionEnabled = NO;
		self.window.rootViewController.view.backgroundColor = [UIColor clearColor];
		self.label = [UILabel new];
		self.label.numberOfLines = 0;
		self.label.textColor = [UIColor whiteColor];
		self.label.layer.cornerRadius = 6;
		self.label.layer.masksToBounds = YES;
		self.label.textAlignment = NSTextAlignmentCenter;
		[self.window.rootViewController.view addSubview:self.label];
	}
	unsigned fsz = (unsigned)((s >> 21) & 15), alpha = (unsigned)((s >> 25) & 15);
	self.label.font = [UIFont monospacedDigitSystemFontOfSize:fsz ? 8 + fsz : 11 weight:UIFontWeightSemibold];
	self.label.backgroundColor = [UIColor colorWithWhite:0 alpha:alpha ? alpha / 10.0 : 0.6];
	self.window.hidden = NO;
	unsigned iv = (unsigned)((s >> 16) & 31);
	NSTimeInterval ivs = iv ? iv / 10.0 : 0.5;
	if (self.timer == nil || fabs(self.timer.timeInterval - ivs) > 0.01) {
		[self.timer invalidate];
		self.timer = [NSTimer timerWithTimeInterval:ivs target:self selector:@selector(update) userInfo:nil repeats:YES];
		[[NSRunLoop mainRunLoop] addTimer:self.timer forMode:NSRunLoopCommonModes];
	}
	long long dummy;
	if (sc_int("kern.sweet_fb_frames", &dummy) != 0 && self.link == nil) {
		self.link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
		self.link.preferredFrameRateRange = CAFrameRateRangeMake(30, 120, 120);
		[self.link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
	}
	[self update];
}

- (void)update
{
	uint64_t s = hud_state;
	unsigned f = (unsigned)((s >> 8) & 0xff);
	NSMutableArray<NSString *> *parts = [NSMutableArray array];
	CFTimeInterval now = CACurrentMediaTime();
	double dt = _lastT > 0 ? now - _lastT : 0;
	_lastT = now;
	double fps = -1;
	const char *src = "dl";
	long long frames;
	if (sc_int("kern.sweet_fb_frames", &frames) == 0) {
		src = "fb";
		if (dt > 0 && _lastFrames > 0) {
			fps = (double)((uint64_t)frames - _lastFrames) / dt;
		}
		_lastFrames = (uint64_t)frames;
	} else {
		if (dt > 0) {
			fps = _dlTicks / dt;
		}
		_dlTicks = 0;
	}
	UIColor *fpsColour = nil;
	if (f & 0x01) {
		[parts addObject:fps >= 0 ? [NSString stringWithFormat:@"%3.0f fps%s%s", fps, (f & 0x40) ? " " : "", (f & 0x40) ? src : ""]
					  : @"--- fps"];
		if (s & (1ULL << 29)) {
			fpsColour = fps >= 80 ? [UIColor greenColor] : fps >= 50 ? [UIColor yellowColor] : [UIColor redColor];
		}
	}
	long long v;
	if ((f & 0x02) && sc_int("kern.sweet_gpu_mhz", &v) == 0) {
		[parts addObject:[NSString stringWithFormat:@"GPU %lld", v]];
	}
	if ((f & 0x04) && sc_int("kern.sweet_temp_max", &v) == 0) {
		[parts addObject:[NSString stringWithFormat:@"%.1f°C", v / 10.0]];
	}
	if (f & 0x08) {
		double c = cpu_percent();
		if (c >= 0) {
			[parts addObject:[NSString stringWithFormat:@"CPU %2.0f%%", c]];
		}
	}
	if ((f & 0x10) && (v = free_mb()) >= 0) {
		[parts addObject:[NSString stringWithFormat:@"%lld MB", v]];
	}
	if ((f & 0x20) && sc_int("kern.sweet_wifi_live_rssi", &v) == 0) {
		[parts addObject:[NSString stringWithFormat:@"%lld dBm", v]];
	}
	if (parts.count == 0) {
		[parts addObject:@"HUD"];
	}
	NSString *text = [parts componentsJoinedByString:@"  "];
	NSMutableAttributedString *as = [[NSMutableAttributedString alloc] initWithString:text];
	if (fpsColour != nil) {
		NSRange r = [text rangeOfString:@"fps"];
		if (r.location != NSNotFound) {
			[as addAttribute:NSForegroundColorAttributeName value:fpsColour range:NSMakeRange(0, r.location + r.length)];
		}
	}
	self.label.attributedText = as;
	CGSize sz = [self.label sizeThatFits:CGSizeMake(400, 100)];
	sz.width += 12;
	sz.height += 6;
	CGRect b = self.window.bounds;
	unsigned pos = (unsigned)((s >> 3) & 7);
	CGFloat x = pos % 3 == 0 ? 8 : pos % 3 == 1 ? (b.size.width - sz.width) / 2 : b.size.width - sz.width - 8;
	CGFloat y = pos < 3 ? 48 : b.size.height - sz.height - 30;	/* below the status bar / above the home bar */
	self.label.frame = CGRectMake(x, y, sz.width, sz.height);
}
@end

static void
hud_reload(void)
{
	int tok = 0;
	uint64_t st = 0;
	if (notify_register_check(HUD_NOTIFY, &tok) == NOTIFY_STATUS_OK) {
		notify_get_state(tok, &st);
		notify_cancel(tok);
	}
	hud_state = st;
	dispatch_async(dispatch_get_main_queue(), ^{
		if (hud == nil) {
			hud = [SweetHUD new];
		}
		[hud apply];
	});
}

static void
sb_restore_and_boot_apply(void)
{
	/* the state does not survive a reboot: SpringBoard puts back the file's (unless something set one already) */
	FILE *f = fopen(HUD_FILE, "r");
	if (f != NULL) {
		char line[128];
		uint64_t st = 0;
		while (fgets(line, sizeof(line), f) != NULL) {
			if (strncmp(line, "state=", 6) == 0) {
				st = strtoull(line + 6, NULL, 0);
			}
		}
		fclose(f);
		int tok = 0;
		uint64_t cur = 0;
		if (notify_register_check(HUD_NOTIFY, &tok) == NOTIFY_STATUS_OK) {
			notify_get_state(tok, &cur);
			if (cur == 0 && st != 0) {
				notify_set_state(tok, st);
				notify_post(HUD_NOTIFY);
			}
			notify_cancel(tok);
		}
	}
	if (access(SWEETCTL, X_OK) == 0) {
		pid_t pid;
		char *argv[] = { (char *)SWEETCTL, (char *)"boot-apply", (char *)"--once", NULL };
		if (posix_spawn(&pid, SWEETCTL, NULL, NULL, argv, environ) == 0) {
			int stv;
			waitpid(pid, &stv, 0);
		}
	}
}

__attribute__((constructor)) static void
hud_init(void)
{
	hud_is_sb = strcmp(getprogname(), "SpringBoard") == 0;
	NSString *bid = [NSBundle mainBundle].bundleIdentifier;
	if (!hud_is_sb && bid == nil) {
		return;	/* not an app (a daemon that inherited the insertion) */
	}
	int tok = 0;
	notify_register_dispatch(HUD_NOTIFY, &tok, dispatch_get_main_queue(), ^(int t) {
		hud_reload();
	});
	[[NSNotificationCenter defaultCenter] addObserverForName:UIApplicationDidFinishLaunchingNotification object:nil
							   queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification *n) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			hud_reload();
		});
	}];
	if (hud_is_sb) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
			sb_restore_and_boot_apply();
		});
	}
}
