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
 * Step 1 (this commit), mode 0 = discovery only, SpringBoard only, once, 12 s after start. Nothing is hooked.
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
	unsigned nc = 0;
	Class *cs = objc_copyClassList(&nc);
	NSMutableArray *dele = [NSMutableArray array];
	for (unsigned i = 0; i < nc; i++) {
		const char *nm = class_getName(cs[i]);
		if (strncmp(nm, "CT", 2) != 0 && strncmp(nm, "CoreTelephony", 13) != 0 && sc_match(nm, name_keys)) {
			sc_class(cs[i], s, NULL);
		}
		for (unsigned j = 0; j < np; j++) {
			const char *pn = protocol_getName(pl[j]);
			if (strncmp(pn, "CoreTelephonyClient", 19) == 0 && strstr(pn, "Delegate") &&
			    class_conformsToProtocol(cs[i], pl[j])) {
				[dele addObject:[NSString stringWithFormat:@"%s:%s", nm, pn]];
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

__attribute__((constructor)) static void
sc_init(void)
{
	struct stat st;
	if (stat(SC_FLAG, &st) != 0 || strcmp(getprogname(), "SpringBoard") != 0) {
		return;
	}
	sc_mode = st.st_size >= 100 && st.st_size <= 109 ? (int)(st.st_size - 100) : 0;
	sc_write([NSString stringWithFormat:@"on, mode %d (discovery in 12 s)", sc_mode]);
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 12 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		@try {
			sc_discover();
		} @catch (NSException *e) {
			sc_write([NSString stringWithFormat:@"discovery exception %@", e]);
		}
	});
}
