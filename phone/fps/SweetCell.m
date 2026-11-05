/*
 * SweetCell (ws-n-cellsb): carrier name, signal bars and registration in iOS's status bar, from wland's modem client
 * (/var/local/cell.status, phone/wifi/cell.c). Compiled into SweetFPS.dylib (SpringBoard + every app); off unless
 * /var/local/sweetcell.on exists. Mode = size of the flag file - 100 (any other size = 0).
 *
 * Why this shape: CommCenter has no baseband, so CoreTelephonyClient (XPC to CommCenter) has no subscription, no
 * registration and no signal, and SpringBoard's telephony state (and the status bar / Settings' Cellular pane built on
 * it) stays empty. SweetPIN already answers CoreTelephonyClient's SIM calls from wland; SweetCell is meant to do the
 * same for subscription / registration / operator / signal, so SpringBoard's own code builds the status bar.
 *
 * Step 2: the feed (mode 1). SpringBoard's SystemStatusServer keeps one STTelephonyStateProvider; SBTelephonyManager and
 * STTelephonyStatusDomainDataProvider (which publishes the status bar's telephony domain) are its observers and ask it
 * for -subscriptionInfoForSlot:. SweetCell answers that for slot 1 with an STMutableTelephonySubscriptionInfo built from
 * /var/local/cell.status (operator name, bars, registration, SIM status), says the device has cellular telephony and a
 * live radio, and calls the observers' -subscriptionInfoDidChangeForStateProvider:slot: whenever the status line changes
 * (polled every 5 s). Enum values iOS uses are not known offline, so they come from /var/local/sweetcell.conf
 * ("key=value" per line, read at every poll):
 *   hooks=0x1f   1 subscriptionInfoForSlot:  2 hasCellularTelephony  4 isSIMPresentForSlot:  8 isCellularRadioCapabilityActive
 *                16 isRadioModuleDead (NO)   (bits off = the original method)
 *   slot=1       which slot number is ours       maxbars=4
 *   reg_ok=2 reg_search=1 reg_none=0             -> registrationStatus and cellularRegistrationStatus
 *   dt_gsm=0 dt_wcdma=0 dt_lte=0 dt_nr=0          -> dataConnectionType
 *   notify=1     1 subscriptionInfoDidChange... to observers  2 stateDidChange...enabled:YES  4 [provider subscriptionInfoDidChange]
 * Mode = size of /var/local/sweetcell.on - 100 (an empty file = mode 1). /var/local/sweetcell.discover (any size):
 * also run the step-1 discovery once.
 *
 * Step 1, discovery, SpringBoard only, once, 12 s after start. Nothing is hooked.
 * /var/tmp/sweetcell.log gets:
 *   - CoreTelephonyClient methods about subscriptions, registration, operator, signal, data, network (+ type encodings)
 *   - every protocol named CoreTelephony* / CT* with "Delegate" (the callbacks SpringBoard implements) and its methods
 *   - CT data classes (CTXPCServiceSubscriptionContext/Info, CTSignalStrengthInfo, CTRegistrationDisplayStatus,
 *     CTServiceDescriptor, CTDataStatus, ...): superclass, ivars (with types), properties, methods
 *   - SpringBoard / SystemStatus classes with Telephony, Cellular, SubscriptionInfo, SignalStrength in the name
 *   - which objects conform to the delegate protocols (class names only), and the live SBTelephonyManager's description
 *   - the current /var/local/cell.status line as SpringBoard reads it (proves the file is readable from SpringBoard)
 * The modem identifiers in cell.status are already redacted by wland (iccid/imei lengths only).
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SC_FLAG "/var/local/sweetcell.on"
#define SC_LOG "/var/tmp/sweetcell.log"
#define SC_STATUS "/var/local/cell.status"

static int sc_mode;

static void
sc_write(NSString *what)
{
	FILE *f = fopen(SC_LOG, "a");
	if (f != NULL) {
		fprintf(f, "%s[%d] %s\n", getprogname(), getpid(), what.UTF8String);
		fclose(f);
	}
}

static int
sc_match(const char *s, const char *const *keys)
{
	for (; *keys != NULL; keys++) {
		if (strcasestr(s, *keys) != NULL) {
			return 1;
		}
	}
	return 0;
}

static void
sc_methods(Class c, NSMutableString *out, const char *const *filter)
{
	for (int meta = 0; meta < 2; meta++) {
		unsigned n = 0;
		Method *ms = class_copyMethodList(meta ? object_getClass(c) : c, &n);
		for (unsigned i = 0; i < n; i++) {
			const char *sel = sel_getName(method_getName(ms[i]));
			if (filter == NULL || sc_match(sel, filter)) {
				[out appendFormat:@"\n    %c%s %s", meta ? '+' : '-', sel, method_getTypeEncoding(ms[i]) ?: "?"];
			}
		}
		free(ms);
	}
}

static void
sc_class(Class c, NSMutableString *out, const char *const *filter)
{
	Class sup = class_getSuperclass(c);
	[out appendFormat:@"\n  class %s (super %s, image %s)", class_getName(c), sup ? class_getName(sup) : "-",
	    class_getImageName(c) ?: "?"];
	unsigned n = 0;
	Ivar *iv = class_copyIvarList(c, &n);
	for (unsigned i = 0; i < n; i++) {
		[out appendFormat:@"\n    ivar %s %s", ivar_getName(iv[i]) ?: "?", ivar_getTypeEncoding(iv[i]) ?: "?"];
	}
	free(iv);
	objc_property_t *ps = class_copyPropertyList(c, &n);
	for (unsigned i = 0; i < n; i++) {
		[out appendFormat:@"\n    prop %s %s", property_getName(ps[i]), property_getAttributes(ps[i]) ?: "?"];
	}
	free(ps);
	sc_methods(c, out, filter);
}

static void
sc_protocol(Protocol *p, NSMutableString *out)
{
	[out appendFormat:@"\n  protocol %s", protocol_getName(p)];
	for (int req = 1; req >= 0; req--) {
		for (int inst = 1; inst >= 0; inst--) {
			unsigned n = 0;
			struct objc_method_description *ds = protocol_copyMethodDescriptionList(p, (BOOL)req, (BOOL)inst, &n);
			for (unsigned i = 0; i < n; i++) {
				[out appendFormat:@"\n    %s%c%s %s", req ? "" : "opt ", inst ? '-' : '+', sel_getName(ds[i].name),
				    ds[i].types ?: "?"];
			}
			free(ds);
		}
	}
}

static void
sc_discover(void)
{
	static const char *const ct_keys[] = { "subscri", "registration", "operator", "carrier", "signal", "dataStatus",
		"servingNetwork", "network", "radio", "display", "context", "cellular", "delegate", "plmn", NULL };
	static const char *const data_classes[] = { "CTXPCServiceSubscriptionContext", "CTXPCServiceSubscriptionInfo",
		"CTSignalStrengthInfo", "CTRegistrationDisplayStatus", "CTServiceDescriptor", "CTDataStatus",
		"CTNetwork", "CTServingNetwork", "CTSubscriptionInfo", "CTEncryptionStatusInfo", "CTRatSelection",
		"CTTelephonyNetworkInfo", "CTCarrier", "CTServiceDescriptorContainer", "CTXPCContexts", NULL };
	static const char *const name_keys[] = { "Telephony", "Cellular", "SubscriptionInfo", "SignalStrength",
		"CarrierName", "Baseband", NULL };

	NSMutableString *s = [NSMutableString string];
	char line[1024] = "";
	FILE *f = fopen(SC_STATUS, "r");
	if (f != NULL) {
		if (fgets(line, sizeof(line), f) == NULL) {
			line[0] = 0;
		}
		fclose(f);
	}
	[s appendFormat:@"cell.status %s: %s", f ? "readable" : "NOT readable", line];

	Class ct = objc_getClass("CoreTelephonyClient");
	if (ct != Nil) {
		[s appendString:@"\n== CoreTelephonyClient"];
		sc_class(ct, s, ct_keys);
	} else {
		[s appendString:@"\n== CoreTelephonyClient: class not loaded"];
	}

	[s appendString:@"\n== delegate protocols"];
	unsigned np = 0;
	Protocol *__unsafe_unretained *pl = objc_copyProtocolList(&np);
	for (unsigned i = 0; i < np; i++) {
		const char *pn = protocol_getName(pl[i]);
		if ((strncmp(pn, "CoreTelephony", 13) == 0 || strncmp(pn, "CT", 2) == 0) && strstr(pn, "Delegate")) {
			sc_protocol(pl[i], s);
		}
	}

	[s appendString:@"\n== CT data classes"];
	for (const char *const *c = data_classes; *c != NULL; c++) {
		Class k = objc_getClass(*c);
		if (k != Nil) {
			sc_class(k, s, NULL);
		} else {
			[s appendFormat:@"\n  class %s: not loaded", *c];
		}
	}

	[s appendString:@"\n== Telephony / Cellular classes (not CT*)"];
	Protocol *__unsafe_unretained ctp[64];
	unsigned nctp = 0;
	for (unsigned j = 0; j < np && nctp < 64; j++) {
		const char *pn = protocol_getName(pl[j]);
		if (strncmp(pn, "CoreTelephonyClient", 19) == 0 && strstr(pn, "Delegate")) {
			ctp[nctp++] = pl[j];
		}
	}
	unsigned nc = 0;
	Class *cs = objc_copyClassList(&nc);
	NSMutableArray *dele = [NSMutableArray array];
	for (unsigned i = 0; i < nc; i++) {
		const char *nm = class_getName(cs[i]);
		if (strncmp(nm, "CT", 2) != 0 && strncmp(nm, "CoreTelephony", 13) != 0 && sc_match(nm, name_keys)) {
			sc_class(cs[i], s, NULL);
		}
		for (unsigned j = 0; j < nctp; j++) {
			if (class_conformsToProtocol(cs[i], ctp[j])) {
				[dele addObject:[NSString stringWithFormat:@"%s:%s", nm, protocol_getName(ctp[j])]];
			}
		}
	}
	free(cs);
	free(pl);
	[s appendFormat:@"\n== conforming classes: %@", [dele componentsJoinedByString:@" "]];

	Class tm = objc_getClass("SBTelephonyManager");
	SEL sh = sel_registerName("sharedTelephonyManager");
	if (tm != Nil && class_getClassMethod(tm, sh) != NULL) {
		id m = ((id (*)(id, SEL))objc_msgSend)(tm, sh);
		[s appendFormat:@"\n== SBTelephonyManager live: %@", [m description]];
	}
	sc_write(s);
}

/* ---- step 2: the feed ---- */

#define SC_CONF "/var/local/sweetcell.conf"

static struct {
	unsigned hooks, notify;
	long slot;
	unsigned long maxbars, reg_ok, reg_search, reg_none, dt_gsm, dt_wcdma, dt_lte, dt_nr;
} sc_cf;

static NSObject *sc_lock;
static id sc_info;			/* STTelephonySubscriptionInfo for our slot, or nil before the first status line */
static char sc_last[1024];
static IMP sc_orig_info, sc_orig_hasct, sc_orig_simpresent, sc_orig_radioactive, sc_orig_dead;

static void
sc_read_conf(void)
{
	sc_cf.hooks = 0x1f;
	sc_cf.notify = 1;
	sc_cf.slot = 1;
	sc_cf.maxbars = 4;
	sc_cf.reg_ok = 2;
	sc_cf.reg_search = 1;
	sc_cf.reg_none = 0;
	sc_cf.dt_gsm = sc_cf.dt_wcdma = sc_cf.dt_lte = sc_cf.dt_nr = 0;
	FILE *f = fopen(SC_CONF, "r");
	if (f == NULL) {
		return;
	}
	char ln[128];
	while (fgets(ln, sizeof(ln), f) != NULL) {
		char k[32];
		unsigned long v;
		if (sscanf(ln, "%31[a-z_]=%li", k, (long *)&v) != 2) {
			continue;
		}
		if (!strcmp(k, "hooks")) sc_cf.hooks = (unsigned)v;
		else if (!strcmp(k, "notify")) sc_cf.notify = (unsigned)v;
		else if (!strcmp(k, "slot")) sc_cf.slot = (long)v;
		else if (!strcmp(k, "maxbars")) sc_cf.maxbars = v;
		else if (!strcmp(k, "reg_ok")) sc_cf.reg_ok = v;
		else if (!strcmp(k, "reg_search")) sc_cf.reg_search = v;
		else if (!strcmp(k, "reg_none")) sc_cf.reg_none = v;
		else if (!strcmp(k, "dt_gsm")) sc_cf.dt_gsm = v;
		else if (!strcmp(k, "dt_wcdma")) sc_cf.dt_wcdma = v;
		else if (!strcmp(k, "dt_lte")) sc_cf.dt_lte = v;
		else if (!strcmp(k, "dt_nr")) sc_cf.dt_nr = v;
	}
	fclose(f);
}

/* value after "key=" up to a space or '|' (0 if absent) */
static int
sc_field(const char *line, const char *key, char *out, size_t n)
{
	const char *p = strstr(line, key);
	if (p == NULL) {
		out[0] = 0;
		return 0;
	}
	p += strlen(key);
	size_t i = 0;
	while (p[i] && p[i] != ' ' && p[i] != '|' && p[i] != '\n' && i + 1 < n) {
		out[i] = p[i];
		i++;
	}
	out[i] = 0;
	return 1;
}

static long
sc_num(const char *line, const char *key, long dflt)
{
	char b[24];
	return sc_field(line, key, b, sizeof(b)) && b[0] ? strtol(b, NULL, 10) : dflt;
}

static unsigned long
sc_bars(const char *line, const char *radio)
{
	const char *sig = strstr(line, "sig=");
	if (sig == NULL || strncmp(sig, "sig=none", 8) == 0) {
		return 0;
	}
	long v;
	const long *th;
	static const long th_lte[] = { -95, -105, -115, -120 }, th_gsm[] = { -85, -95, -105, -110 };
	if (!strcmp(radio, "lte") || !strcmp(radio, "nr5g")) {
		v = sc_num(sig, "rsrp=", -200);
		th = th_lte;
	} else {
		v = sc_num(sig, "rssi=", -200);
		th = th_gsm;
	}
	unsigned long b = v >= th[0] ? 4 : v >= th[1] ? 3 : v >= th[2] ? 2 : v >= th[3] ? 1 : 0;
	return b * sc_cf.maxbars / 4;
}

/* cell.status -> a new subscription info; nil if the class is missing */
static id
sc_build(const char *line)
{
	Class mc = objc_getClass("STMutableTelephonySubscriptionInfo");
	if (mc == Nil) {
		return nil;
	}
	id m = [[mc alloc] init];
	char reg[32], radio[16], sim[64], op[64] = "";
	sc_field(line, "reg=", reg, sizeof(reg));
	sc_field(line, "radio=", radio, sizeof(radio));
	sc_field(line, "sim=", sim, sizeof(sim));
	const char *q = strstr(line, "plmn=");
	if (q != NULL && (q = strchr(q, '\'')) != NULL) {
		const char *e = strchr(q + 1, '\'');
		if (e != NULL && e > q + 1 && (size_t)(e - q - 1) < sizeof(op)) {
			memcpy(op, q + 1, (size_t)(e - q - 1));
			op[e - q - 1] = 0;
		}
	}
	if (!op[0]) {
		sc_field(line, "plmn=", op, sizeof(op));
	}
	int registered = !strcmp(reg, "registered");
	unsigned long rs = registered ? sc_cf.reg_ok : !strcmp(reg, "searching") ? sc_cf.reg_search : sc_cf.reg_none;
	unsigned long dt = !registered ? 0 : !strcmp(radio, "lte") ? sc_cf.dt_lte : !strcmp(radio, "nr5g") ? sc_cf.dt_nr :
	    !strcmp(radio, "wcdma") ? sc_cf.dt_wcdma : sc_cf.dt_gsm;
	NSString *ss = strstr(line, "usim:ready") ? @"kCTSIMSupportSIMStatusReady" :
	    strstr(line, "pin1-required") ? @"kCTSIMSupportSIMStatusPINLocked" :
	    strstr(line, "puk") ? @"kCTSIMSupportSIMStatusPUKLocked" :
	    strstr(sim, "present") ? @"kCTSIMSupportSIMStatusNotReady" : @"kCTSIMSupportSIMStatusNotInserted";
	((void (*)(id, SEL, id))objc_msgSend)(m, sel_registerName("setIdentifier:"), @"sweetcell-slot1");
	((void (*)(id, SEL, id))objc_msgSend)(m, sel_registerName("setSIMLabel:"), @"Primary");
	((void (*)(id, SEL, id))objc_msgSend)(m, sel_registerName("setShortSIMLabel:"), @"P");
	((void (*)(id, SEL, id))objc_msgSend)(m, sel_registerName("setSIMStatus:"), ss);
	((void (*)(id, SEL, unsigned long))objc_msgSend)(m, sel_registerName("setRegistrationStatus:"), rs);
	((void (*)(id, SEL, unsigned long))objc_msgSend)(m, sel_registerName("setCellularRegistrationStatus:"), rs);
	((void (*)(id, SEL, unsigned long))objc_msgSend)(m, sel_registerName("setDataConnectionType:"), dt);
	((void (*)(id, SEL, unsigned long))objc_msgSend)(m, sel_registerName("setSignalStrengthBars:"),
	    registered ? sc_bars(line, radio) : 0);
	((void (*)(id, SEL, unsigned long))objc_msgSend)(m, sel_registerName("setMaxSignalStrengthBars:"), sc_cf.maxbars);
	((void (*)(id, SEL, id))objc_msgSend)(m, sel_registerName("setOperatorName:"),
	    registered && op[0] ? [NSString stringWithUTF8String:op] : nil);
	((void (*)(id, SEL, BOOL))objc_msgSend)(m, sel_registerName("setPreferredForDataConnections:"), YES);
	return [m copy];
}

static id
sc_provider(void)
{
	Class tm = objc_getClass("SBTelephonyManager");
	SEL sh = sel_registerName("sharedTelephonyManager"), sp = sel_registerName("telephonyStateProvider");
	if (tm == Nil || class_getClassMethod(tm, sh) == NULL) {
		return nil;
	}
	id m = ((id (*)(id, SEL))objc_msgSend)(tm, sh);
	return m != nil && [m respondsToSelector:sp] ? ((id (*)(id, SEL))objc_msgSend)(m, sp) : nil;
}

static void
sc_notify(id prov)
{
	Ivar oi = class_getInstanceVariable(object_getClass(prov), "_observers");
	NSHashTable *obs = oi ? object_getIvar(prov, oi) : nil;
	NSArray *all = [obs isKindOfClass:[NSHashTable class]] ? obs.allObjects : @[];
	SEL s1 = sel_registerName("subscriptionInfoDidChangeForStateProvider:slot:");
	SEL s2 = sel_registerName("stateDidChangeForStateProvider:enabled:inactiveEnabled:");
	NSMutableString *who = [NSMutableString string];
	for (id o in all) {
		[who appendFormat:@" %s", object_getClassName(o)];
		if ((sc_cf.notify & 2) && [o respondsToSelector:s2]) {
			((void (*)(id, SEL, id, BOOL, BOOL))objc_msgSend)(o, s2, prov, YES, NO);
		}
		if ((sc_cf.notify & 1) && [o respondsToSelector:s1]) {
			((void (*)(id, SEL, id, long))objc_msgSend)(o, s1, prov, sc_cf.slot);
		}
	}
	SEL s4 = sel_registerName("subscriptionInfoDidChange");
	if ((sc_cf.notify & 4) && [prov respondsToSelector:s4]) {
		((void (*)(id, SEL))objc_msgSend)(prov, s4);
	}
	sc_write([NSString stringWithFormat:@"notified (notify %u):%@", sc_cf.notify, who]);
}

static void
sc_hook(Class c, const char *sel, IMP *orig, id block)
{
	Method m = class_getInstanceMethod(c, sel_registerName(sel));
	if (m == NULL) {
		sc_write([NSString stringWithFormat:@"hook %s: no such method", sel]);
		return;
	}
	*orig = method_setImplementation(m, imp_implementationWithBlock(block));
}

static int sc_installed;

static void
sc_install(void)
{
	Class c = objc_getClass("STTelephonyStateProvider");
	if (sc_installed) {
		return;
	}
	if (c == Nil) {
		sc_write(@"STTelephonyStateProvider not loaded: no feed");
		return;
	}
	sc_hook(c, "subscriptionInfoForSlot:", &sc_orig_info, ^id(id me, long slot) {
		id mine = nil;
		if ((sc_cf.hooks & 1) && slot == sc_cf.slot) {
			@synchronized(sc_lock) {
				mine = sc_info;
			}
		}
		return mine ?: ((id (*)(id, SEL, long))sc_orig_info)(me, sel_registerName("subscriptionInfoForSlot:"), slot);
	});
	sc_hook(c, "hasCellularTelephony", &sc_orig_hasct, ^BOOL(id me) {
		return (sc_cf.hooks & 2) ? YES : ((BOOL (*)(id, SEL))sc_orig_hasct)(me, sel_registerName("hasCellularTelephony"));
	});
	sc_hook(c, "isSIMPresentForSlot:", &sc_orig_simpresent, ^BOOL(id me, long slot) {
		return ((sc_cf.hooks & 4) && slot == sc_cf.slot) ? YES :
		    ((BOOL (*)(id, SEL, long))sc_orig_simpresent)(me, sel_registerName("isSIMPresentForSlot:"), slot);
	});
	sc_hook(c, "isCellularRadioCapabilityActive", &sc_orig_radioactive, ^BOOL(id me) {
		return (sc_cf.hooks & 8) ? YES :
		    ((BOOL (*)(id, SEL))sc_orig_radioactive)(me, sel_registerName("isCellularRadioCapabilityActive"));
	});
	sc_hook(c, "isRadioModuleDead", &sc_orig_dead, ^BOOL(id me) {
		return (sc_cf.hooks & 16) ? NO : ((BOOL (*)(id, SEL))sc_orig_dead)(me, sel_registerName("isRadioModuleDead"));
	});
	sc_installed = 1;
	sc_write([NSString stringWithFormat:@"feed hooks installed (hooks 0x%x)", sc_cf.hooks]);
}

static void
sc_poll(void)
{
	sc_read_conf();
	sc_install();
	char line[1024] = "";
	FILE *f = fopen(SC_STATUS, "r");
	if (f != NULL) {
		if (fgets(line, sizeof(line), f) == NULL) {
			line[0] = 0;
		}
		fclose(f);
	}
	/* the "t=NNNs" tail changes every write: compare without it */
	char *t = strstr(line, " t=");
	if (t != NULL) {
		*t = 0;
	}
	static unsigned last_hooks, last_notify;
	if (line[0] && (strcmp(line, sc_last) != 0 || sc_cf.hooks != last_hooks || sc_cf.notify != last_notify)) {
		snprintf(sc_last, sizeof(sc_last), "%s", line);
		last_hooks = sc_cf.hooks;
		last_notify = sc_cf.notify;
		id info = sc_build(line);
		@synchronized(sc_lock) {
			sc_info = info;
		}
		sc_write([NSString stringWithFormat:@"status -> %@", [info description]]);
		id prov = sc_provider();
		if (prov != nil) {
			sc_notify(prov);
			id back = ((id (*)(id, SEL, long))objc_msgSend)(prov, sel_registerName("subscriptionInfoForSlot:"), sc_cf.slot);
			sc_write([NSString stringWithFormat:@"provider: hasCellularTelephony %d, slot %ld info is ours %d",
			    ((BOOL (*)(id, SEL))objc_msgSend)(prov, sel_registerName("hasCellularTelephony")), sc_cf.slot, back == info]);
		} else {
			sc_write(@"no SBTelephonyManager / provider yet");
			sc_last[0] = 0;	/* try again next poll */
		}
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		@try {
			sc_poll();
		} @catch (NSException *e) {
			sc_write([NSString stringWithFormat:@"poll exception %@", e]);
		}
	});
}

__attribute__((constructor)) static void
sc_init(void)
{
	struct stat st;
	if (stat(SC_FLAG, &st) != 0 || strcmp(getprogname(), "SpringBoard") != 0) {
		return;
	}
	sc_mode = st.st_size == 0 ? 1 : st.st_size >= 100 && st.st_size <= 109 ? (int)(st.st_size - 100) : 0;
	sc_lock = [NSObject new];
	sc_read_conf();
	int disc = stat("/var/local/sweetcell.discover", &st) == 0;
	sc_write([NSString stringWithFormat:@"on, mode %d%s", sc_mode, disc ? ", discovery in 12 s" : ""]);
	if (sc_mode & 1) {
		sc_install();	/* before SpringBoard's telephony objects ask (constructor time) */
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			sc_poll();
		});
	}
	if (disc) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 12 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			@try {
				sc_discover();
			} @catch (NSException *e) {
				sc_write([NSString stringWithFormat:@"discovery exception %@", e]);
			}
		});
	}
}
