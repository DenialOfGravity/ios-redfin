/*
 * SweetPIN (ws-n-pin): unlock the SIM's PIN1 from iOS's own SIM PIN UI. Compiled into SweetFPS.dylib, which xnu 0999
 * inserts into SpringBoard and every /Applications process, so no kernel change. Off unless /var/local/sweetpin.on
 * exists (stat only: app sandboxes may stat /var/local but not read it). wland's cell client (phone/wifi/cell.c, with
 * "pin=1" in /var/local/cell.on) does the modem side: QMI UIM Verify PIN, over the unix socket /var/run/wland-cell.sock.
 *
 * Why this shape. There is no CommCenter baseband connection, so CoreTelephony's SIM calls (XPC to CommCenter) go
 * nowhere and nothing ever asks for a PIN. iOS 16 has no SBSIMLockManager / SBSIMLockAlertItem any more (the iOS 16.5
 * SDK's SpringBoard.tbd lists neither; iOS 7-11 headers had them): the SIM unlock UI lives in SIMSetupUIService.app
 * (TSSIMUnlockEntryView / TSSIMUnlockDetailView, from its accessibility bundle's safe categories) and talks to
 * CommCenter through CoreTelephonyClient, whose SIM selectors (iOS 14 runtime headers; class and kCTSIMSupportSIMStatus*
 * constants exported in the iOS 16.5 CoreTelephony.tbd) are:
 *     getSIMStatus:completion:  getSIMStatus:error:  getRemainingPINAttemptCount:completion:  ...:error:
 *     unlockPIN:pin:completion:  unlockPIN:pin:error:  promptForSIMUnlock:completion:
 * So, in every process with the flag:
 *   1. CoreTelephonyClient's SIM status / PIN attempts / unlockPIN are answered from wland (each hook only if the
 *      selector exists here; a completion block is called only after its type signature was checked at run time, and
 *      an unknown signature is logged and left to the original). The native UI then sees "PIN locked" and its unlockPIN
 *      reaches the modem.
 *   2. SpringBoard polls wland every 10 s; when PIN1 becomes required it asks for the native UI: a remote alert of
 *      SIMSetupUIService (SBSRemoteAlertDefinition/Handle, SpringBoardServices; bundle id read from the app's
 *      Info.plist, view controller class from /var/local/sweetpin.vc or else the one unlock view controller name found
 *      in the app's binary), and promptForSIMUnlock:completion: lands there too.
 *   3. If that cannot be shown (no class found, the handle invalidates), a plain UIKit SIM PIN alert (secure numeric
 *      field) in SpringBoard is the fallback, so the user can always unlock.
 * Mode = size of /var/local/sweetpin.on - 100 (any other size = 0):
 *   0 native, then the fallback   1 fallback alert only   2 discovery only (no UI)   3 native only
 * Discovery (every mode, once per process start): /var/tmp/sweetpin.log (SpringBoard; other processes NSLog) gets the
 * SIM unlock class names and their methods in this process, CoreTelephonyClient's SIM selectors, and (SpringBoard)
 * SIMSetupUIService's Info.plist keys and the unlock / remote-alert class names in its binary. That is the evidence
 * for the next step, since the iOS 16.7 shared cache is not off the phone.
 * /var/local/sweetpin.ask (any size; deleted when seen): SpringBoard asks again at its next poll (after a Cancel).
 *
 * The PIN: taken from the text field / the unlockPIN argument into a 9-byte buffer, sent to wland once, zeroed. It is
 * never logged, written, or retried: one user entry = one Verify PIN. wland refuses with 1 try left (never a PUK).
 */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SP_SOCK "/var/run/wland-cell.sock"
#define SP_FLAG "/var/local/sweetpin.on"
#define SP_LOG "/var/tmp/sweetpin.log"

static int sp_mode, sp_springboard, sp_ct_hooked;

static void
sp_log(NSString *what)
{
	NSLog(@"SweetPIN: %s[%d] %@", getprogname(), getpid(), what);
	FILE *f = fopen(SP_LOG, "a");
	if (f != NULL) {
		fprintf(f, "%s[%d] %s\n", getprogname(), getpid(), what.UTF8String);
		fclose(f);
	}
}

static void
sp_zero(void *p, size_t n)
{
	volatile uint8_t *v = p;
	while (n--) {
		*v++ = 0;
	}
}

/* one request to wland, one line back; -1 = no wland (absent, pin=1 off, or a sandbox that denies the socket) */
static int
sp_call(const uint8_t *req, size_t n, char *out, size_t outsz, int wait_s)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", SP_SOCK);
	struct timeval tw = { 1, 0 }, tr = { wait_s, 0 };	/* a verify: wland waits up to 8 s for the modem */
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tw, sizeof(tw));
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tr, sizeof(tr));
	int on = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || write(fd, req, n) != (ssize_t)n) {
		close(fd);
		return -1;
	}
	size_t k = 0;
	while (k + 1 < outsz) {
		ssize_t r = read(fd, out + k, outsz - 1 - k);
		if (r <= 0) {
			break;
		}
		k += (size_t)r;
		if (memchr(out, '\n', k) != NULL) {
			break;
		}
	}
	out[k] = 0;
	close(fd);
	return k ? 0 : -1;
}

typedef struct {
	int ok;		/* wland answered */
	int on;		/* pin=1 in cell.on */
	int required;	/* PIN1 is asked for (pin1-required / enabled-unverified) */
	int ready;
	int puk;	/* puk1-required / pin1-blocked: nothing done here */
	int retries;
} sp_status;

static int
sp_field(const char *line, const char *key, char *val, size_t vsz)
{
	const char *p = strstr(line, key);
	if (p == NULL) {
		return 0;
	}
	p += strlen(key);
	size_t i = 0;
	while (p[i] && p[i] != ' ' && p[i] != '\n' && i + 1 < vsz) {
		val[i] = p[i];
		i++;
	}
	val[i] = 0;
	return 1;
}

static sp_status sp_last;
static uint64_t sp_last_ns;

/* wland's view, at most 1 s of waiting; sp_query_cached(age) reuses an answer younger than age seconds */
static sp_status
sp_query(void)
{
	sp_status s = { 0 };
	char line[200], v[32];
	if (sp_call((const uint8_t *)"S\n", 2, line, sizeof(line), 1) != 0 || strncmp(line, "pin ", 4) != 0) {
		sp_last = s;
		sp_last_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
		return s;
	}
	s.ok = 1;
	s.on = strstr(line, " on=1") != NULL;
	s.retries = -1;
	if (sp_field(line, "retries=", v, sizeof(v))) {
		s.retries = atoi(v);
	}
	if (sp_field(line, "app=", v, sizeof(v))) {
		s.ready = strcmp(v, "ready") == 0;
		s.required = strcmp(v, "pin1-required") == 0;
		s.puk = strcmp(v, "puk1-required") == 0 || strcmp(v, "pin1-blocked") == 0;
	}
	if (sp_field(line, "pin1=", v, sizeof(v))) {
		s.required |= strcmp(v, "enabled-unverified") == 0 && !s.ready;
		s.puk |= strcmp(v, "blocked") == 0 || strcmp(v, "perm-blocked") == 0;
	}
	sp_last = s;
	sp_last_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	return s;
}

static sp_status
sp_query_cached(unsigned age)
{
	if (sp_last_ns && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - sp_last_ns < (uint64_t)age * NSEC_PER_SEC) {
		return sp_last;
	}
	return sp_query();
}

/*
 * Verify: the digits go to wland once. Returns 1 ok, 0 wrong PIN / modem error, -1 refused or no wland;
 * *left = retries wland reports (-1 unknown), why = wland's reason for a refusal ("last-try", "off", ...).
 */
static int
sp_verify(const char *digits, size_t n, int *left, char *why, size_t whysz)
{
	uint8_t req[2 + 8];
	char line[160], v[32];
	*left = -1;
	snprintf(why, whysz, "format");
	if (n < 4 || n > 8) {
		return -1;
	}
	for (size_t i = 0; i < n; i++) {
		if (digits[i] < '0' || digits[i] > '9') {
			return -1;
		}
	}
	req[0] = 'V';
	req[1] = (uint8_t)n;
	memcpy(req + 2, digits, n);
	int r = sp_call(req, 2 + n, line, sizeof(line), 12);
	sp_zero(req, sizeof(req));
	if (r != 0) {
		snprintf(why, whysz, "no-wland");
		sp_log(@"verify: no answer from wland");
		return -1;
	}
	if (sp_field(line, "retries=", v, sizeof(v))) {
		*left = atoi(v);
	}
	if (strncmp(line, "ok", 2) == 0) {
		sp_log([NSString stringWithFormat:@"verify -> ok, retries left %d", *left]);
		return 1;
	}
	if (strncmp(line, "refused ", 8) == 0) {
		sp_field(line, "refused ", why, whysz);
		sp_log([NSString stringWithFormat:@"verify -> refused (%s), retries left %d", why, *left]);
		return -1;
	}
	snprintf(why, whysz, "wrong");
	sp_log([NSString stringWithFormat:@"verify -> error, retries left %d", *left]);
	return 0;
}

/* NSString PIN -> transient digits -> wland; the C copy is zeroed */
static int
sp_verify_string(NSString *pin, int *left, char *why, size_t whysz)
{
	char d[9] = { 0 };
	size_t n = 0;
	if (![pin isKindOfClass:[NSString class]] || pin.length < 4 || pin.length > 8) {
		*left = -1;
		snprintf(why, whysz, "format");
		return -1;
	}
	for (NSUInteger i = 0; i < pin.length; i++) {
		unichar c = [pin characterAtIndex:i];
		d[n++] = c < 128 ? (char)c : 'x';
	}
	int r = sp_verify(d, n, left, why, whysz);
	sp_zero(d, sizeof(d));
	return r;
}

/* ---- block signatures (Clang block ABI): only call a completion whose argument types were checked ---- */

struct sp_block {
	void *isa;
	int flags, reserved;
	void (*invoke)(void);
	struct {
		unsigned long reserved, size;
		void *rest[];
	} *desc;
};

static NSMethodSignature *
sp_block_sig(id blk)
{
	struct sp_block *b = (__bridge void *)blk;
	if (b == NULL || !(b->flags & (1 << 30))) {	/* BLOCK_HAS_SIGNATURE */
		return nil;
	}
	int i = (b->flags & (1 << 25)) ? 2 : 0;		/* BLOCK_HAS_COPY_DISPOSE: copy, dispose come first */
	const char *t = b->desc->rest[i];
	@try {
		return t ? [NSMethodSignature signatureWithObjCTypes:t] : nil;
	} @catch (NSException *e) {
		return nil;
	}
}

/* "@@" = two object arguments, "q@" = integer then object, ... ('i' for any integer, 'B' bool) */
static NSString *
sp_shape(id blk)
{
	NSMethodSignature *s = sp_block_sig(blk);
	if (s == nil) {
		return @"?";
	}
	NSMutableString *m = [NSMutableString string];
	for (NSUInteger i = 1; i < s.numberOfArguments; i++) {
		char c = [s getArgumentTypeAtIndex:i][0];
		[m appendFormat:@"%c", c == '@' ? '@' : (c == 'B' || c == 'c') ? 'B' : strchr("iIsSlLqQ", c) ? 'i' : '?'];
	}
	return m;
}

/* ---- CoreTelephonyClient hooks ---- */

static NSString *
sp_ct_const(const char *name)
{
	void *h = dlopen("/System/Library/Frameworks/CoreTelephony.framework/CoreTelephony", RTLD_NOLOAD | RTLD_LAZY);
	CFStringRef *p = h ? dlsym(h, name) : NULL;
	return p && *p ? (__bridge NSString *)*p : nil;
}

static NSString *
sp_status_string(sp_status s)
{
	if (!s.ok || !s.on) {
		return nil;	/* not ours: the original call */
	}
	if (s.required) {
		return sp_ct_const("kCTSIMSupportSIMStatusPINLocked");
	}
	if (s.puk) {
		return sp_ct_const("kCTSIMSupportSIMStatusPUKLocked");
	}
	if (s.ready) {
		return sp_ct_const("kCTSIMSupportSIMStatusReady");
	}
	return nil;
}

static void sp_present(void);

static NSError *
sp_error(int r, const char *why)
{
	return r == 1 ? nil : [NSError errorWithDomain:NSPOSIXErrorDomain code:r == 0 ? EAUTH : EPERM
					      userInfo:@{ @"SweetPIN" : @(why) }];
}

static void
sp_hook_ct(void)
{
	Class c = objc_getClass("CoreTelephonyClient");
	if (c == Nil || sp_ct_hooked) {
		return;
	}
	sp_ct_hooked = 1;
	NSMutableArray *done = [NSMutableArray array];
	Method m;

	SEL s1 = sel_registerName("getSIMStatus:completion:");
	if ((m = class_getInstanceMethod(c, s1)) != NULL) {
		static void (*o1)(id, SEL, id, id);
		o1 = (void (*)(id, SEL, id, id))method_getImplementation(m);
		method_setImplementation(m, imp_implementationWithBlock(^(id self, id ctx, id done_) {
			NSString *st = nil;
			if (done_ != nil && [sp_shape(done_) isEqualToString:@"@@"] && (st = sp_status_string(sp_query_cached(5))) != nil) {
				((void (^)(id, id))done_)(st, nil);
				return;
			}
			o1(self, s1, ctx, done_);
		}));
		[done addObject:@"getSIMStatus:completion:"];
	}
	SEL s2 = sel_registerName("getSIMStatus:error:");
	if ((m = class_getInstanceMethod(c, s2)) != NULL && method_getTypeEncoding(m) != NULL && method_getTypeEncoding(m)[0] == '@') {
		static id (*o2)(id, SEL, id, NSError **);
		o2 = (id (*)(id, SEL, id, NSError **))method_getImplementation(m);
		method_setImplementation(m, imp_implementationWithBlock(^id(id self, id ctx, NSError **err) {
			NSString *st = sp_status_string(sp_query_cached(5));
			if (st != nil) {
				if (err) {
					*err = nil;
				}
				return st;
			}
			return o2(self, s2, ctx, err);
		}));
		[done addObject:@"getSIMStatus:error:"];
	}
	SEL s3 = sel_registerName("getRemainingPINAttemptCount:completion:");
	if ((m = class_getInstanceMethod(c, s3)) != NULL) {
		static void (*o3)(id, SEL, id, id);
		o3 = (void (*)(id, SEL, id, id))method_getImplementation(m);
		method_setImplementation(m, imp_implementationWithBlock(^(id self, id ctx, id done_) {
			sp_status s = sp_query_cached(5);
			NSString *shape = done_ ? sp_shape(done_) : @"";
			if (s.ok && s.on && s.retries >= 0) {
				if ([shape isEqualToString:@"@@"]) {
					((void (^)(id, id))done_)(@(s.retries), nil);
					return;
				}
				if ([shape isEqualToString:@"i@"]) {
					((void (^)(long, id))done_)(s.retries, nil);
					return;
				}
				sp_log([NSString stringWithFormat:@"getRemainingPINAttemptCount:completion: block shape %@: left to the original", shape]);
			}
			o3(self, s3, ctx, done_);
		}));
		[done addObject:@"getRemainingPINAttemptCount:completion:"];
	}
	SEL s4 = sel_registerName("getRemainingPINAttemptCount:error:");
	if ((m = class_getInstanceMethod(c, s4)) != NULL && method_getTypeEncoding(m) != NULL &&
	    method_getTypeEncoding(m)[0] == '@') {
		static id (*o4)(id, SEL, id, NSError **);
		o4 = (id (*)(id, SEL, id, NSError **))method_getImplementation(m);
		method_setImplementation(m, imp_implementationWithBlock(^id(id self, id ctx, NSError **err) {
			sp_status s = sp_query_cached(5);
			if (s.ok && s.on && s.retries >= 0) {
				if (err) {
					*err = nil;
				}
				return @(s.retries);
			}
			return o4(self, s4, ctx, err);
		}));
		[done addObject:@"getRemainingPINAttemptCount:error:"];
	}
	SEL s5 = sel_registerName("unlockPIN:pin:completion:");
	if ((m = class_getInstanceMethod(c, s5)) != NULL) {
		static void (*o5)(id, SEL, id, id, id);
		o5 = (void (*)(id, SEL, id, id, id))method_getImplementation(m);
		method_setImplementation(m, imp_implementationWithBlock(^(id self, id ctx, NSString *pin, id done_) {
			sp_status s = sp_query_cached(5);
			if (!(s.ok && s.on)) {
				o5(self, s5, ctx, pin, done_);
				return;
			}
			NSString *shape = done_ ? sp_shape(done_) : @"";
			id blk = done_;
			dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
				int left;
				char why[32];
				int r = sp_verify_string(pin, &left, why, sizeof(why));
				NSError *e = sp_error(r, why);
				dispatch_async(dispatch_get_main_queue(), ^{
					if ([shape isEqualToString:@"@"]) {
						((void (^)(id))blk)(e);
					} else if ([shape isEqualToString:@"B@"]) {
						((void (^)(BOOL, id))blk)(r == 1, e);
					} else if ([shape length]) {
						sp_log([NSString stringWithFormat:@"unlockPIN completion shape %@: not called", shape]);
					}
				});
			});
		}));
		[done addObject:@"unlockPIN:pin:completion:"];
	}
	SEL s6 = sel_registerName("unlockPIN:pin:error:");
	if ((m = class_getInstanceMethod(c, s6)) != NULL) {
		static void (*o6)(id, SEL, id, id, NSError **);
		o6 = (void (*)(id, SEL, id, id, NSError **))method_getImplementation(m);
		method_setImplementation(m, imp_implementationWithBlock(^(id self, id ctx, NSString *pin, NSError **err) {
			sp_status s = sp_query_cached(5);
			if (!(s.ok && s.on)) {
				o6(self, s6, ctx, pin, err);
				return;
			}
			int left;
			char why[32];
			int r = sp_verify_string(pin, &left, why, sizeof(why));
			if (err) {
				*err = sp_error(r, why);
			}
		}));
		[done addObject:@"unlockPIN:pin:error:"];
	}
	SEL s7 = sel_registerName("promptForSIMUnlock:completion:");
	if (sp_springboard && (m = class_getInstanceMethod(c, s7)) != NULL) {
		static void (*o7)(id, SEL, id, id);
		o7 = (void (*)(id, SEL, id, id))method_getImplementation(m);
		method_setImplementation(m, imp_implementationWithBlock(^(id self, id ctx, id done_) {
			sp_status s = sp_query_cached(5);
			if (!(s.ok && s.on && s.required)) {
				o7(self, s7, ctx, done_);
				return;
			}
			sp_log(@"promptForSIMUnlock: -> SIM PIN UI");
			dispatch_async(dispatch_get_main_queue(), ^{
				sp_present();
			});
			if (done_ != nil && [sp_shape(done_) isEqualToString:@"@"]) {
				((void (^)(id))done_)(nil);
			}
		}));
		[done addObject:@"promptForSIMUnlock:completion:"];
	}
	sp_log([NSString stringWithFormat:@"CoreTelephonyClient hooked: %@", [done componentsJoinedByString:@" "]]);
}

/* dyld's callback (also for the images already loaded): no objc calls here, the hook runs on the main queue */
static void
sp_image_added(const struct mach_header *mh, intptr_t slide)
{
	Dl_info di;
	if (sp_ct_hooked || dladdr(mh, &di) == 0 || di.dli_fname == NULL ||
	    strstr(di.dli_fname, "/CoreTelephony.framework/CoreTelephony") == NULL) {
		return;
	}
	dispatch_async(dispatch_get_main_queue(), ^{
		sp_hook_ct();
	});
}

/* ---- discovery (evidence for the native path; nothing about the PIN) ---- */

static void
sp_dump_methods(Class c, NSMutableString *out)
{
	for (int meta = 0; meta < 2; meta++) {
		unsigned n = 0;
		Method *ms = class_copyMethodList(meta ? object_getClass(c) : c, &n);
		for (unsigned i = 0; i < n; i++) {
			[out appendFormat:@" %c%s", meta ? '+' : '-', sel_getName(method_getName(ms[i]))];
		}
		free(ms);
	}
}

static void
sp_discover_classes(void)
{
	unsigned n = 0;
	Class *cs = objc_copyClassList(&n);
	NSMutableString *s = [NSMutableString string];
	for (unsigned i = 0; i < n; i++) {
		const char *nm = class_getName(cs[i]);
		if (strstr(nm, "SIMUnlock") || strstr(nm, "SIMLock") || strstr(nm, "SIMPIN") || strstr(nm, "SimUnlock") ||
		    strstr(nm, "SIMPin") || (strstr(nm, "Unlock") && strstr(nm, "SIM"))) {
			[s appendFormat:@"\n  class %s (super %s):", nm, class_getName(class_getSuperclass(cs[i]))];
			sp_dump_methods(cs[i], s);
		}
	}
	free(cs);
	Class ct = objc_getClass("CoreTelephonyClient");
	if (ct != Nil) {
		unsigned k = 0;
		Method *ms = class_copyMethodList(ct, &k);
		[s appendString:@"\n  CoreTelephonyClient SIM/PIN:"];
		for (unsigned i = 0; i < k; i++) {
			const char *sel = sel_getName(method_getName(ms[i]));
			if (strcasestr(sel, "pin") || strcasestr(sel, "puk") || strstr(sel, "SIMStatus") || strstr(sel, "SIMUnlock")) {
				[s appendFormat:@" %s(%s)", sel, method_getTypeEncoding(ms[i]) ?: "?"];
			}
		}
		free(ms);
	}
	sp_log([NSString stringWithFormat:@"discovery: classes%@", s.length ? s : @" (none)"]);
}

static NSString *sp_app_path, *sp_app_id, *sp_vc;

/* SpringBoard: where SIMSetupUIService is, its bundle id, and the unlock / remote-alert class names in its binary */
static void
sp_discover_app(void)
{
	NSArray *cands = @[ @"/Applications/SIMSetupUIService.app", @"/System/Library/CoreServices/SIMSetupUIService.app" ];
	for (NSString *p in cands) {
		if ([[NSFileManager defaultManager] fileExistsAtPath:p]) {
			sp_app_path = p;
			break;
		}
	}
	if (sp_app_path == nil) {
		NSMutableArray *sims = [NSMutableArray array];
		for (NSString *a in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@"/Applications" error:nil]) {
			if ([a rangeOfString:@"SIM" options:NSCaseInsensitiveSearch].location != NSNotFound) {
				[sims addObject:a];
			}
		}
		sp_log([NSString stringWithFormat:@"discovery: no SIMSetupUIService.app; /Applications with SIM: %@", sims]);
		return;
	}
	NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[sp_app_path stringByAppendingPathComponent:@"Info.plist"]];
	sp_app_id = info[@"CFBundleIdentifier"];
	NSString *exe = info[@"CFBundleExecutable"] ?: @"SIMSetupUIService";
	sp_log([NSString stringWithFormat:@"discovery: %@ id %@ keys %@", sp_app_path, sp_app_id,
		[[info.allKeys sortedArrayUsingSelector:@selector(compare:)] componentsJoinedByString:@","]]);
	NSData *bin = [NSData dataWithContentsOfFile:[sp_app_path stringByAppendingPathComponent:exe]
					     options:NSDataReadingMappedIfSafe error:nil];
	const char *b = bin.bytes;
	NSMutableOrderedSet *names = [NSMutableOrderedSet orderedSet];
	NSMutableArray *vcs = [NSMutableArray array];
	for (NSUInteger i = 0, start = 0; i <= bin.length; i++) {
		char ch = i < bin.length ? b[i] : 0;
		int word = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
		if (word) {
			continue;
		}
		NSUInteger len = i - start;
		if (ch == 0 && len >= 8 && len <= 96 && start > 0 && b[start - 1] == 0) {
			NSString *w = [[NSString alloc] initWithBytes:b + start length:len encoding:NSASCIIStringEncoding];
			if ([w rangeOfString:@"Unlock"].location != NSNotFound || [w rangeOfString:@"RemoteAlert"].location != NSNotFound ||
			    [w hasSuffix:@"ViewController"]) {
				[names addObject:w];
				if ([w rangeOfString:@"Unlock"].location != NSNotFound && [w hasSuffix:@"ViewController"] &&
				    [w rangeOfString:@"PUK"].location == NSNotFound && [w rangeOfString:@"Puk"].location == NSNotFound) {
					[vcs addObject:w];
				}
			}
		}
		start = i + 1;
	}
	sp_log([NSString stringWithFormat:@"discovery: %lu names in %@: %@", (unsigned long)names.count, exe,
		[[names.array subarrayWithRange:NSMakeRange(0, MIN(names.count, 200u))] componentsJoinedByString:@" "]]);
	NSString *forced = [[NSString stringWithContentsOfFile:@"/var/local/sweetpin.vc" encoding:NSUTF8StringEncoding error:nil]
		stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
	if (forced.length) {
		sp_vc = forced;
	} else {
		/* a remote-alert unlock controller first, then the only unlock controller */
		for (NSString *v in vcs) {
			if ([v rangeOfString:@"Remote"].location != NSNotFound || [v rangeOfString:@"Alert"].location != NSNotFound) {
				sp_vc = v;
				break;
			}
		}
		if (sp_vc == nil && vcs.count == 1) {
			sp_vc = vcs[0];
		}
	}
	sp_log([NSString stringWithFormat:@"discovery: unlock view controllers %@; native path uses %@%@", vcs, sp_vc ?: @"(none)",
		forced.length ? @" (sweetpin.vc)" : @""]);
}

/* ---- SpringBoard: presenting ---- */

static UIWindow *sp_window;
static id sp_handle;
static int sp_showing;

static void sp_fallback(NSString *note, int left);

@interface SweetPINObserver : NSObject
@end
@implementation SweetPINObserver
- (void)remoteAlertHandleDidActivate:(id)h
{
	sp_log(@"native: SIMSetupUIService remote alert active");
}
- (void)remoteAlertHandleDidDeactivate:(id)h
{
	sp_log(@"native: remote alert deactivated");
	sp_showing = 0;
}
- (void)remoteAlertHandle:(id)h didInvalidateWithError:(NSError *)e
{
	BOOL was = [h respondsToSelector:@selector(isActive)] && ((BOOL (*)(id, SEL))objc_msgSend)(h, @selector(isActive));
	sp_log([NSString stringWithFormat:@"native: remote alert invalidated (%@)", e.localizedDescription ?: @"no error"]);
	dispatch_async(dispatch_get_main_queue(), ^{
		sp_handle = nil;
		sp_showing = 0;
		if (e != nil && !was && sp_mode == 0) {
			sp_fallback(nil, -2);
		}
	});
}
@end

static SweetPINObserver *sp_observer;

static BOOL
sp_native(void)
{
	Class def = objc_getClass("SBSRemoteAlertDefinition"), hc = objc_getClass("SBSRemoteAlertHandle"),
	      ac = objc_getClass("SBSRemoteAlertActivationContext");
	SEL di = sel_registerName("initWithServiceName:viewControllerClassName:"),
	    nh = sel_registerName("newHandleWithDefinition:configurationContext:"), aw = sel_registerName("activateWithContext:"),
	    ao = sel_registerName("addObserver:");
	if (sp_app_id == nil || sp_vc == nil || def == Nil || hc == Nil || ac == Nil || ![def instancesRespondToSelector:di] ||
	    ![hc respondsToSelector:nh] || ![hc instancesRespondToSelector:aw]) {
		sp_log([NSString stringWithFormat:@"native: not possible (service %@, controller %@, SBSRemoteAlert %s)", sp_app_id,
			sp_vc, def && hc && ac ? "present" : "missing"]);
		return NO;
	}
	id d = ((id (*)(id, SEL, id, id))objc_msgSend)([def alloc], di, sp_app_id, sp_vc);
	id h = ((id (*)(id, SEL, id, id))objc_msgSend)(hc, nh, d, nil);
	if (h == nil) {
		sp_log(@"native: no handle");
		return NO;
	}
	if (sp_observer == nil) {
		sp_observer = [SweetPINObserver new];
	}
	if ([h respondsToSelector:ao]) {
		((void (*)(id, SEL, id))objc_msgSend)(h, ao, sp_observer);
	}
	sp_handle = h;
	sp_showing = 1;
	((void (*)(id, SEL, id))objc_msgSend)(h, aw, [ac new]);
	sp_log([NSString stringWithFormat:@"native: activating %@ / %@", sp_app_id, sp_vc]);
	return YES;
}

static void
sp_close_window(void)
{
	sp_window.hidden = YES;
	sp_window = nil;
	sp_showing = 0;
}

static void
sp_message(NSString *title, NSString *msg)
{
	UIAlertController *a = [UIAlertController alertControllerWithTitle:title message:msg preferredStyle:UIAlertControllerStyleAlert];
	[a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
		sp_close_window();
	}]];
	[sp_window.rootViewController presentViewController:a animated:YES completion:nil];
}

/*
 * The plain UIKit SIM PIN alert: one Verify per OK tap. left = -2: ask only if wland's last status (the tick's,
 * no socket wait on the main thread) says PIN1 is required; else it is the retry count after a wrong PIN.
 */
static void
sp_fallback(NSString *note, int left)
{
	if (sp_mode == 2 || sp_mode == 3) {
		return;
	}
	sp_status s = sp_last;
	if (left == -2 && !(s.ok && s.on && s.required)) {
		sp_close_window();
		return;
	}
	if (left != -2) {
		s.retries = left;
	}
	if (sp_window == nil) {
		UIWindowScene *scene = nil;
		for (UIScene *sc in UIApplication.sharedApplication.connectedScenes) {
			if ([sc isKindOfClass:[UIWindowScene class]]) {
				scene = (UIWindowScene *)sc;
				break;
			}
		}
		sp_window = scene ? [[UIWindow alloc] initWithWindowScene:scene] : [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
		sp_window.windowLevel = UIWindowLevelAlert + 100;
		sp_window.rootViewController = [UIViewController new];
		sp_window.rootViewController.view.backgroundColor = UIColor.clearColor;
		sp_window.hidden = NO;
	}
	sp_showing = 1;
	NSString *msg = [NSString stringWithFormat:@"%@Enter the SIM PIN.%@", note ? [note stringByAppendingString:@"\n"] : @"",
		s.retries >= 0 ? [NSString stringWithFormat:@" %d attempt%s left.", s.retries, s.retries == 1 ? "" : "s"] : @""];
	UIAlertController *a = [UIAlertController alertControllerWithTitle:@"SIM PIN" message:msg preferredStyle:UIAlertControllerStyleAlert];
	[a addTextFieldWithConfigurationHandler:^(UITextField *t) {
		t.secureTextEntry = YES;
		t.keyboardType = UIKeyboardTypeNumberPad;
		t.textContentType = UITextContentTypeOneTimeCode;	/* no password autofill / saving */
		t.placeholder = @"PIN";
	}];
	__weak UIAlertController *wa = a;
	[a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction *x) {
		sp_log(@"fallback: cancelled");
		sp_close_window();
	}]];
	[a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
		UITextField *t = wa.textFields.firstObject;
		NSString *pin = t.text;
		t.text = @"";
		dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
			int left;
			char why[32];
			int r = sp_verify_string(pin, &left, why, sizeof(why));
			NSString *w = @(why);
			dispatch_async(dispatch_get_main_queue(), ^{
				if (r == 1) {
					sp_message(@"SIM Unlocked", nil);
				} else if (r == 0) {
					sp_fallback(@"Incorrect PIN.", left);
				} else if ([w isEqualToString:@"last-try"]) {
					sp_message(@"SIM PIN not sent", @"Only one attempt is left: a wrong PIN would lock the SIM (PUK). "
						   "Unlock it in another phone.");
				} else if ([w isEqualToString:@"format"]) {
					sp_fallback(@"The PIN has 4 to 8 digits.", left);
				} else {
					sp_message(@"SIM PIN not sent", [NSString stringWithFormat:@"wland: %@", w]);
				}
			});
		});
	}]];
	[sp_window.rootViewController presentViewController:a animated:YES completion:nil];
}

static void
sp_present(void)
{
	if (sp_showing || sp_mode == 2) {
		return;
	}
	if ((sp_mode == 0 || sp_mode == 3) && sp_native()) {
		return;
	}
	sp_fallback(nil, -2);
}

static int sp_was_required = 0;

static void
sp_tick(void)
{
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		sp_status s = sp_query();
		struct stat st;
		int ask = stat("/var/local/sweetpin.ask", &st) == 0;
		if (ask) {
			unlink("/var/local/sweetpin.ask");
		}
		int req = s.ok && s.on && s.required;
		if (req && (!sp_was_required || ask)) {
			sp_log([NSString stringWithFormat:@"wland: SIM PIN required (%d left)%s", s.retries, ask ? " (sweetpin.ask)" : ""]);
			dispatch_async(dispatch_get_main_queue(), ^{
				sp_present();
			});
		}
		if (sp_was_required && s.ok && s.ready) {
			sp_log(@"wland: SIM ready");
		}
		if (s.ok) {
			sp_was_required = req;
		}
		sp_tick();
	});
}

__attribute__((constructor)) static void
sp_init(void)
{
	struct stat st;
	if (stat(SP_FLAG, &st) != 0) {
		return;
	}
	sp_mode = st.st_size >= 100 && st.st_size <= 103 ? (int)(st.st_size - 100) : 0;
	sp_springboard = strcmp(getprogname(), "SpringBoard") == 0;
	_dyld_register_func_for_add_image(sp_image_added);	/* CoreTelephony may load later; called now for loaded images */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		sp_discover_classes();
		if (sp_springboard) {
			sp_discover_app();
			sp_tick();
		}
	});
	sp_log([NSString stringWithFormat:@"on, mode %d", sp_mode]);
}
