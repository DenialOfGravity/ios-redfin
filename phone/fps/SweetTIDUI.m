/*
 * SweetTIDUI (ws-u-tidui): Touch ID feedback on the lock screen of our iPhone X rootfs (no home button).
 * Compiled into SweetFPS.dylib (xnu 0999 inserts it into SpringBoard); runs only in SpringBoard.
 * RE notes: PF/logs/research/touchid-lockui-2026-11-18.md.
 *
 * Switch: /var/local/sweettidui, read once at SpringBoard launch (respring to apply). No file, or neither 'h' nor
 * 'n' in it: nothing is hooked. Letters (independent, both may be set):
 *   h   "iPhone X look": the cover sheet's padlock (proud lock) on a Touch ID device. CoverSheet creates it in
 *       -[CSCoverSheetViewController viewDidLoad] when hasPearlSupport || MG(z5G/N9jcMdgPm8UegLwbKg) ||
 *       hasPoseidonSupport, and the passcode pad when -[SBUIPasscodeLockViewBase _supportsProudLock] /
 *       -[CSPasscodeViewController setShowProudLock:]. Apple's own Touch-ID-with-padlock key z5G cannot be set
 *       through the MG cache's CacheExtra (libMobileGestalt then drops the whole cache), so: -[SBUIBiometricResource
 *       hasPearlSupport] answers YES for that one call site in viewDidLoad (return address inside its IMP) and
 *       nowhere else; _supportsProudLock answers YES; setShowProudLock: gets YES. The padlock then follows Touch ID
 *       natively: open when authenticated, jiggle on a failed finger (SBLockScreenManager jiggleLock ->
 *       -[CSCoverSheetViewController jiggleLockIcon]).
 *   n   "iPhone 8 style" call to action in the cover sheet footer: CoverSheet's own
 *       -[CSFixedFooterViewController _updateCallToActionTextAnimated:] (Press home to unlock / Press home to open
 *       after a match / "Try Again" with a shake after a failed finger, updateCallToActionForMesaMatchFailure) runs
 *       as on a home-button device. It returns at once when SBFEffectiveHomeButtonType() == 2 (no home button,
 *       MobileGestalt HomeButtonType, cached by -[BSPlatform homeButtonType]); while it runs, and only then,
 *       -[BSPlatform homeButtonType] answers 1. The same for -[CSFixedFooterView initWithFrame:], which otherwise
 *       creates no container (shake view) for that label. Nothing else in SpringBoard sees a home button.
 *   l   log what was hooked and each hooked call to /var/tmp/sweettidui.log
 */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

static int tu_footer_depth;	/* > 0 while CoverSheet's footer call-to-action update runs (main thread) */
static pthread_t tu_main;
static bool tu_log;
static uintptr_t tu_vdl_lo, tu_vdl_hi;	/* -[CSCoverSheetViewController viewDidLoad] code range */

static void
tu_note(NSString *what)
{
	NSLog(@"SweetTIDUI: %@", what);
	if (!tu_log) {
		return;
	}
	FILE *f = fopen("/var/tmp/sweettidui.log", "a");
	if (f != NULL) {
		fprintf(f, "%s SpringBoard[%d] %s\n", [[NSDate date] description].UTF8String, getpid(), what.UTF8String);
		fclose(f);
	}
}

static id
tu_get(id o, const char *sel)
{
	SEL s = sel_registerName(sel);
	return [o respondsToSelector:s] ? ((id (*)(id, SEL))objc_msgSend)(o, s) : nil;
}

static bool
tu_hook_native_footer(void)
{
	Class plat = objc_getClass("BSPlatform");
	Class foot = objc_getClass("CSFixedFooterViewController");
	SEL shb = sel_registerName("homeButtonType");
	SEL sup = sel_registerName("_updateCallToActionTextAnimated:");
	Method mhb = plat != Nil ? class_getInstanceMethod(plat, shb) : NULL;
	Method mup = foot != Nil ? class_getInstanceMethod(foot, sup) : NULL;
	if (mhb == NULL || mup == NULL) {
		tu_note([NSString stringWithFormat:@"n: not hooked (BSPlatform homeButtonType %p, CSFixedFooterViewController "
			"_updateCallToActionTextAnimated: %p)", mhb, mup]);
		return false;
	}
	__block long (*ohb)(id, SEL) = NULL;
	ohb = (long (*)(id, SEL))method_setImplementation(mhb, imp_implementationWithBlock(^long(id self_) {
		long t = ohb(self_, shb);
		if (tu_footer_depth > 0 && t == 2 && pthread_equal(pthread_self(), tu_main)) {
			return 1;
		}
		return t;
	}));
	__block void (*oup)(id, SEL, BOOL) = NULL;
	oup = (void (*)(id, SEL, BOOL))method_setImplementation(mup, imp_implementationWithBlock(^(id self_, BOOL anim) {
		bool main = pthread_equal(pthread_self(), tu_main);
		if (main) {
			tu_footer_depth++;
		}
		oup(self_, sup, anim);
		if (main) {
			tu_footer_depth--;
		}
		if (tu_log) {
			id l = tu_get(tu_get(self_, "fixedFooterView"), "callToActionLabel");
			NSMutableString *chain = [NSMutableString string];
			for (UIView *v = l; v != nil; v = v.superview) {
				CGRect r = [v convertRect:v.bounds toView:nil];
				[chain appendFormat:@" <%s h%d a%.2f %.0f,%.0f %.0fx%.0f>", object_getClassName(v), v.hidden,
					v.alpha, r.origin.x, r.origin.y, r.size.width, r.size.height];
			}
			tu_note([NSString stringWithFormat:@"footer update (animated %d): text '%@' window %p:%@", anim,
				tu_get(l, "text"), [(UIView *)l window], chain]);
		}
	}));
	/* the label's container: -[CSFixedFooterView initWithFrame:] adds the shake view (and the page control) only
	 * when the home button type is not 2; without it setCallToActionLabel: leaves the label without a superview */
	Class fv = objc_getClass("CSFixedFooterView");
	SEL sinit = sel_registerName("initWithFrame:");
	Method minit = fv != Nil ? class_getInstanceMethod(fv, sinit) : NULL;
	if (minit != NULL) {
		__block id (*oinit)(id, SEL, CGRect) = NULL;
		oinit = (id (*)(id, SEL, CGRect))method_setImplementation(minit, imp_implementationWithBlock(^id(id self_,
			CGRect r) {
			bool main = pthread_equal(pthread_self(), tu_main);
			if (main) {
				tu_footer_depth++;
			}
			id v = oinit(self_, sinit, r);
			if (main) {
				tu_footer_depth--;
			}
			tu_note([NSString stringWithFormat:@"n: CSFixedFooterView %p init (shake view %p)", v,
				tu_get(v, "callToActionShakeView")]);
			return v;
		}));
	}
	tu_note([NSString stringWithFormat:@"n: CSFixedFooterViewController call to action as on a home-button device "
		"(CSFixedFooterView initWithFrame: %s)", minit ? "hooked" : "missing"]);
	return true;
}

static bool
tu_hook_proud_lock(void)
{
	Class cs = objc_getClass("CSCoverSheetViewController");
	Class br = objc_getClass("SBUIBiometricResource");
	Class pv = objc_getClass("SBUIPasscodeLockViewBase");
	Class pc = objc_getClass("CSPasscodeViewController");
	SEL svdl = sel_registerName("viewDidLoad");
	SEL spearl = sel_registerName("hasPearlSupport");
	SEL ssup = sel_registerName("_supportsProudLock");
	SEL sshow = sel_registerName("setShowProudLock:");
	/* class_getInstanceMethod also finds inherited ones: use only methods the class itself implements */
	Method mvdl = NULL, mpearl = NULL, msup = NULL, mshow = NULL;
	unsigned n = 0;
	Method *ml = cs != Nil ? class_copyMethodList(cs, &n) : NULL;
	for (unsigned i = 0; i < n; i++) {
		if (method_getName(ml[i]) == svdl) {
			mvdl = ml[i];
		}
	}
	free(ml);
	mpearl = br != Nil ? class_getInstanceMethod(br, spearl) : NULL;
	msup = pv != Nil ? class_getInstanceMethod(pv, ssup) : NULL;
	mshow = pc != Nil ? class_getInstanceMethod(pc, sshow) : NULL;
	if (mvdl == NULL || mpearl == NULL) {
		tu_note([NSString stringWithFormat:@"h: not hooked (CSCoverSheetViewController viewDidLoad %p, "
			"SBUIBiometricResource hasPearlSupport %p)", mvdl, mpearl]);
		return false;
	}
	/* the call is at +0x560 of the 16.7 (20H392) X cache's viewDidLoad; the method is ~0x900 long */
	tu_vdl_lo = (uintptr_t)method_getImplementation(mvdl);
	tu_vdl_hi = tu_vdl_lo + 0xa00;
	__block BOOL (*opearl)(id, SEL) = NULL;
	opearl = (BOOL (*)(id, SEL))method_setImplementation(mpearl, imp_implementationWithBlock(^BOOL(id self_) {
		BOOL r = opearl(self_, spearl);
		uintptr_t ra = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
		if (!r && ra >= tu_vdl_lo && ra < tu_vdl_hi) {
			tu_note([NSString stringWithFormat:@"h: hasPearlSupport YES for CSCoverSheetViewController viewDidLoad "
				"(+%#lx)", (unsigned long)(ra - tu_vdl_lo)]);
			return YES;
		}
		return r;
	}));
	if (msup != NULL) {
		method_setImplementation(msup, imp_implementationWithBlock(^BOOL(id self_) {
			return YES;
		}));
	}
	if (mshow != NULL) {
		__block void (*oshow)(id, SEL, BOOL) = NULL;
		oshow = (void (*)(id, SEL, BOOL))method_setImplementation(mshow, imp_implementationWithBlock(^(id self_,
			BOOL on) {
			oshow(self_, sshow, YES);
		}));
	}
	tu_note([NSString stringWithFormat:@"h: proud lock on (viewDidLoad IMP %#lx, passcode pad _supportsProudLock %s, "
		"setShowProudLock: %s)", (unsigned long)tu_vdl_lo, msup ? "hooked" : "missing", mshow ? "hooked" : "missing"]);
	return true;
}

__attribute__((constructor)) static void
tu_init(void)
{
	if (strcmp(getprogname(), "SpringBoard") != 0) {
		return;
	}
	char buf[32] = "";
	FILE *f = fopen("/var/local/sweettidui", "r");
	if (f == NULL) {
		return;
	}
	if (fgets(buf, sizeof(buf), f) == NULL) {
		buf[0] = 0;
	}
	fclose(f);
	tu_log = strchr(buf, 'l') != NULL;
	tu_main = pthread_self();	/* constructors run on SpringBoard's main thread */
	tu_note([NSString stringWithFormat:@"switch '%.*s'", (int)strcspn(buf, "\n"), buf]);
	bool want_n = strchr(buf, 'n') != NULL, want_h = strchr(buf, 'h') != NULL;
	bool ok_n = !want_n || tu_hook_native_footer();
	bool ok_h = !want_h || tu_hook_proud_lock();
	if (!ok_n || !ok_h) {
		/* CoverSheet not realized yet: try again once the main run loop runs */
		dispatch_async(dispatch_get_main_queue(), ^{
			if (!ok_n) {
				tu_hook_native_footer();
			}
			if (!ok_h) {
				tu_hook_proud_lock();
			}
		});
	}
}
