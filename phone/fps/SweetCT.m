/*
 * SweetCT (ws-n-cellsb): an in-process stand-in for CommCenter's XPC service, for processes whose CoreTelephony calls
 * otherwise go nowhere (no CommCenter runs: no baseband). Compiled into SweetFPS.dylib. Per process, off unless
 * /var/local/sweetcell.fakect.<process name> exists (stat only, so app sandboxes can see it).
 *
 * CoreTelephonyClient gets its remote object from CoreTelephonyClientMux (-synchronousProxyWithErrorHandler:,
 * -proxyWithErrorHandler_sync:, -proxyWithQueue:errorHandler:). Those return an SCFakeCT (NSProxy) instead. Every
 * message is resolved against the CTXPCService* protocols; each call is logged to /var/tmp/sweetcell-<process>.log
 * (selector + short argument descriptions), and the reply block(s) are invoked with answers from sc_ct_answer() or,
 * for selectors it does not know, with nil / 0 for every argument (the block's own signature is read from its
 * descriptor, so this works for any reply shape).
 * Answers come from /var/local/cell.status (same parser as SweetCell): one subscription, slot 1.
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static FILE *ct_log;
static int ct_on, ct_hooked;

static void
ct_write(NSString *s)
{
	if (ct_log == NULL) {
		char p[256];
		snprintf(p, sizeof(p), "/var/tmp/sweetcell-%s.log", getprogname());
		ct_log = fopen(p, "a");
	}
	if (ct_log != NULL) {
		fprintf(ct_log, "%s[%d] %s\n", getprogname(), getpid(), s.UTF8String);
		fflush(ct_log);
	} else {
		NSLog(@"SweetCT: %@", s);
	}
}

/* ---- block signatures (Block ABI: flags bit 30 = has signature, bit 25 = has copy/dispose) ---- */
struct ct_block {
	void *isa;
	int flags, reserved;
	void (*invoke)(void);
	struct {
		unsigned long reserved, size;
		const void *rest[];
	} *desc;
};

static NSMethodSignature *
ct_block_sig(id block)
{
	struct ct_block *b = (__bridge struct ct_block *)block;
	if (b == NULL || !(b->flags & (1 << 30))) {
		return nil;
	}
	int i = (b->flags & (1 << 25)) ? 2 : 0;
	const char *sig = b->desc->rest[i];
	return sig ? [NSMethodSignature signatureWithObjCTypes:sig] : nil;
}

/* ---- the answers ---- */
static id
ct_context(void)
{
	static id ctx;
	if (ctx == nil) {
		Class c = objc_getClass("CTXPCServiceSubscriptionContext");
		NSUUID *u = [[NSUUID alloc] initWithUUIDString:@"5EE7CE11-0000-4000-8000-000000000001"];
		ctx = ((id (*)(id, SEL, id, long))objc_msgSend)([c alloc], sel_registerName("initWithUUID:andSlot:"), u, 1);
		@try {
			[ctx setValue:@YES forKey:@"isSimPresent"];
			[ctx setValue:@YES forKey:@"isSimGood"];
			[ctx setValue:@"Primary" forKey:@"label"];
			[ctx setValue:@"sweet-label-1" forKey:@"labelID"];
			[ctx setValue:@YES forKey:@"userDataPreferred"];
			[ctx setValue:@YES forKey:@"userDefaultVoice"];
		} @catch (NSException *e) {
			ct_write([NSString stringWithFormat:@"context KVC: %@", e.reason]);
		}
	}
	return ctx;
}

static id
ct_subscription_info(void)
{
	Class c = objc_getClass("CTXPCServiceSubscriptionInfo");
	id si = [c new];
	@try {
		NSArray *a = @[ ct_context() ];
		[si setValue:a forKey:@"subscriptions"];
		[si setValue:a forKey:@"subscriptionsInUse"];
		[si setValue:a forKey:@"subscriptionsValid"];
	} @catch (NSException *e) {
		ct_write([NSString stringWithFormat:@"subscription info KVC: %@", e.reason]);
	}
	return si;
}

/* fill block argument index ai (1-based in the block signature) for selector sel; return YES if set */
static BOOL
ct_answer(const char *sel, NSInvocation *bi, NSUInteger ai, const char *type)
{
	id v = nil;
	if (type[0] != '@') {
		return NO;
	}
	if (ai == 1 && (!strcmp(sel, "getSubscriptionInfo:") || !strcmp(sel, "getSubscriptionInfoWithError:"))) {
		v = ct_subscription_info();
	} else if (ai == 1 && !strcmp(sel, "getActiveContexts:")) {
		Class c = objc_getClass("CTXPCContexts");
		SEL is = sel_registerName("initWithSubscriptions:");
		if (c != Nil && [c instancesRespondToSelector:is]) {
			v = ((id (*)(id, SEL, id))objc_msgSend)([c alloc], is, @[ ct_context() ]);
		}
	}
	if (v == nil) {
		return NO;
	}
	[bi setArgument:&v atIndex:(NSInteger)ai];
	return YES;
}

/* ---- the proxy ---- */
@interface SCFakeCT : NSProxy
@end

@implementation SCFakeCT

static NSArray *
ct_protocols(void)
{
	static NSArray *ps;
	if (ps == nil) {
		NSMutableArray *a = [NSMutableArray array];
		unsigned n = 0;
		Protocol *__unsafe_unretained *pl = objc_copyProtocolList(&n);
		for (unsigned i = 0; i < n; i++) {
			if (strncmp(protocol_getName(pl[i]), "CTXPCService", 12) == 0) {
				[a addObject:pl[i]];
			}
		}
		free(pl);
		ps = a;
	}
	return ps;
}

- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel
{
	for (Protocol *p in ct_protocols()) {
		for (int req = 1; req >= 0; req--) {
			struct objc_method_description d = protocol_getMethodDescription(p, sel, (BOOL)req, YES);
			if (d.types != NULL) {
				return [NSMethodSignature signatureWithObjCTypes:d.types];
			}
		}
	}
	ct_write([NSString stringWithFormat:@"no protocol has %s: answered as v@:", sel_getName(sel)]);
	return [NSMethodSignature signatureWithObjCTypes:"v@:"];
}

- (void)forwardInvocation:(NSInvocation *)inv
{
	const char *sel = sel_getName(inv.selector);
	NSMethodSignature *ms = inv.methodSignature;
	NSMutableString *args = [NSMutableString string];
	for (NSUInteger i = 2; i < ms.numberOfArguments; i++) {
		const char *t = [ms getArgumentTypeAtIndex:i];
		if (t[0] == '@' && t[1] == '?') {
			[args appendString:@" <block>"];
		} else if (t[0] == '@') {
			__unsafe_unretained id o = nil;
			[inv getArgument:&o atIndex:(NSInteger)i];
			NSString *d = [o description] ?: @"nil";
			[args appendFormat:@" %@", d.length > 120 ? [d substringToIndex:120] : d];
		} else {
			uint64_t v = 0;
			NSUInteger sz = 0;
			NSGetSizeAndAlignment(t, &sz, NULL);
			if (sz <= 8) {
				[inv getArgument:&v atIndex:(NSInteger)i];
			}
			[args appendFormat:@" %s=%llu", t, v];
		}
	}
	BOOL known = NO;
	for (NSUInteger i = 2; i < ms.numberOfArguments; i++) {
		const char *t = [ms getArgumentTypeAtIndex:i];
		if (!(t[0] == '@' && t[1] == '?')) {
			continue;
		}
		__unsafe_unretained id blk = nil;
		[inv getArgument:&blk atIndex:(NSInteger)i];
		NSMethodSignature *bs = ct_block_sig(blk);
		if (blk == nil || bs == nil) {
			continue;
		}
		NSInvocation *bi = [NSInvocation invocationWithMethodSignature:bs];
		bi.target = blk;
		for (NSUInteger a = 1; a < bs.numberOfArguments; a++) {
			if (ct_answer(sel, bi, a, [bs getArgumentTypeAtIndex:a])) {
				known = YES;
			}
		}
		[bi invoke];
	}
	if (ms.methodReturnLength > 0) {
		uint8_t zero[64] = { 0 };
		if (ms.methodReturnLength <= sizeof(zero)) {
			[inv setReturnValue:zero];
		}
	}
	ct_write([NSString stringWithFormat:@"ct %s%@%s", sel, args, known ? " -> answered" : ""]);
}

- (BOOL)respondsToSelector:(SEL)sel
{
	return YES;
}

@end

static SCFakeCT *ct_fake;

static void
ct_hook(void)
{
	Class mux = objc_getClass("CoreTelephonyClientMux");
	if (ct_hooked || mux == Nil) {
		return;
	}
	ct_hooked = 1;
	ct_fake = [SCFakeCT alloc];
	const char *sels[] = { "synchronousProxyWithErrorHandler:", "proxyWithErrorHandler_sync:" };
	for (int i = 0; i < 2; i++) {
		Method m = class_getInstanceMethod(mux, sel_registerName(sels[i]));
		if (m != NULL) {
			method_setImplementation(m, imp_implementationWithBlock(^id(id me, id eh) {
				return ct_fake;
			}));
		}
	}
	/* -proxyWithQueue:errorHandler: takes dispatch::queue (one pointer) by value: passed like a pointer */
	Method m = class_getInstanceMethod(mux, sel_registerName("proxyWithQueue:errorHandler:"));
	if (m != NULL) {
		method_setImplementation(m, imp_implementationWithBlock(^id(id me, void *q, id eh) {
			return ct_fake;
		}));
	}
	ct_write(@"CoreTelephonyClientMux proxies -> SCFakeCT");
}

static void
ct_image_added(const struct mach_header *mh, intptr_t slide)
{
	Dl_info di;
	if (ct_hooked || dladdr(mh, &di) == 0 || di.dli_fname == NULL ||
	    strstr(di.dli_fname, "/CoreTelephony.framework/CoreTelephony") == NULL) {
		return;
	}
	ct_hook();	/* before any CoreTelephonyClient call can use the real proxy */
}

__attribute__((constructor)) static void
ct_init(void)
{
	char p[256];
	struct stat st;
	snprintf(p, sizeof(p), "/var/local/sweetcell.fakect.%s", getprogname());
	if (stat(p, &st) != 0) {
		return;
	}
	ct_on = 1;
	ct_write(@"fake CommCenter on");
	_dyld_register_func_for_add_image(ct_image_added);
}
