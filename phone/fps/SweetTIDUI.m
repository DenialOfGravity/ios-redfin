/*
 * SweetTIDUI (ws-u-tidui): Touch ID feedback on the lock screen of our iPhone X rootfs (no home button).
 * Compiled into SweetFPS.dylib (xnu 0999 inserts it into SpringBoard); runs only in SpringBoard.
 * RE notes: PF/logs/research/touchid-lockui-2026-11-18.md.
 *
 * Switch: /var/local/sweettidui, read once at SpringBoard launch (respring to apply). No file, or no 'h' in it:
 * nothing is hooked. Letters:
 *   h   "iPhone X look": the cover sheet's padlock (proud lock) on a Touch ID device. CoverSheet creates it in
 *       -[CSCoverSheetViewController viewDidLoad] when hasPearlSupport || MG(z5G/N9jcMdgPm8UegLwbKg) ||
 *       hasPoseidonSupport, and the passcode pad when -[SBUIPasscodeLockViewBase _supportsProudLock] /
 *       -[CSPasscodeViewController setShowProudLock:]. Apple's own Touch-ID-with-padlock key z5G cannot be set
 *       through the MG cache's CacheExtra (libMobileGestalt then drops the whole cache), so: -[SBUIBiometricResource
 *       hasPearlSupport] answers YES for that one call site in viewDidLoad (return address inside its IMP) and
 *       nowhere else; _supportsProudLock answers YES; setShowProudLock: gets YES. The padlock then follows Touch ID
 *       natively: open when authenticated, jiggle on a failed finger (SBLockScreenManager jiggleLock ->
 *       -[CSCoverSheetViewController jiggleLockIcon]).
 *   l   log what was hooked to /var/tmp/sweettidui.log
 * (An iPhone 8 style footer variant, 'n', was tried and dropped by the owner 2026-11-18: ws-u-tidui 65bf8ba.)
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <stdio.h>
#include <string.h>

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
	tu_note([NSString stringWithFormat:@"switch '%.*s'", (int)strcspn(buf, "\n"), buf]);
	if (strchr(buf, 'h') != NULL && !tu_hook_proud_lock()) {
		/* CoverSheet not realized yet: try again once the main run loop runs */
		dispatch_async(dispatch_get_main_queue(), ^{
			tu_hook_proud_lock();
		});
	}
}
