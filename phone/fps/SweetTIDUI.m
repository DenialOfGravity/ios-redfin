/*
 * SweetTIDUI (ws-u-tidui): Touch ID feedback on the lock screen of our iPhone X rootfs (no home button), in the
 * iPhone 8 style. Compiled into SweetFPS.dylib (xnu 0999 inserts it into SpringBoard); runs only in SpringBoard.
 * RE notes: PF/logs/research/touchid-lockui-2026-11-18.md.
 *
 * Switch: /var/local/sweettidui, read once at SpringBoard launch (respring to apply). No file, or no 'n' in it:
 * nothing is hooked. Letters:
 *   n   "native" iPhone 8 call to action in the cover sheet footer: CoverSheet's own
 *       -[CSFixedFooterViewController _updateCallToActionTextAnimated:] (Press home to unlock / Press home to open
 *       after a match / "Try Again" with a shake after a failed finger, updateCallToActionForMesaMatchFailure) runs
 *       as on a home-button device. It returns at once when SBFEffectiveHomeButtonType() == 2 (no home button,
 *       MobileGestalt HomeButtonType, cached by -[BSPlatform homeButtonType]); while it runs, and only then,
 *       -[BSPlatform homeButtonType] answers 1. Nothing else in SpringBoard sees a home button.
 *   l   log what was hooked and each footer update to /var/tmp/sweettidui.log
 * The "iPhone X look" (the cover sheet's padlock driven by Touch ID) needs no code: it is MobileGestalt
 * z5G/N9jcMdgPm8UegLwbKg = true in the MG cache's CacheExtra (CoverSheet / SBUIPasscodeLockViewBase create the proud
 * lock when hasPearlSupport || MG(z5G) || hasPoseidonSupport), see the research doc.
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

static int tu_footer_depth;	/* > 0 while CoverSheet's footer call-to-action update runs (main thread) */
static pthread_t tu_main;
static bool tu_log;

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
			id v = [self_ respondsToSelector:sel_registerName("fixedFooterView")]
				? ((id (*)(id, SEL))objc_msgSend)(self_, sel_registerName("fixedFooterView")) : nil;
			id l = [v respondsToSelector:sel_registerName("callToActionLabel")]
				? ((id (*)(id, SEL))objc_msgSend)(v, sel_registerName("callToActionLabel")) : nil;
			id t = [l respondsToSelector:sel_registerName("text")]
				? ((id (*)(id, SEL))objc_msgSend)(l, sel_registerName("text")) : nil;
			tu_note([NSString stringWithFormat:@"footer update (animated %d): label %p text '%@'", anim, l, t]);
		}
	}));
	tu_note(@"n: CSFixedFooterViewController call to action as on a home-button device");
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
	if (strchr(buf, 'n') != NULL && !tu_hook_native_footer()) {
		/* CoverSheet not realized yet: try again once the main run loop runs */
		dispatch_async(dispatch_get_main_queue(), ^{
			tu_hook_native_footer();
		});
	}
}
