/*
 * SweetControl.app (ws-n-ctl): one menu for the port's live switches. Knobs come from Knobs.json (in the bundle, or
 * /var/local/sweetctl/knobs.json on the phone when present: edit that to add knobs without a rebuild). Reads are
 * direct (sysctl, stat, fopen); every write goes through the setuid helper /var/local/bin/sweetctl. A change whose
 * knob says effect respring/render/userspace/reboot is listed at the top until it is acted on, with the button that
 * makes it take effect. Kernel sysctls can be pinned ("Apply at boot": /var/local/sweetctl/boot.conf, applied by
 * SweetFPS.dylib from SpringBoard once per boot). The FPS overlay's settings live in a notify state (SweetHUD.m).
 */
#import <UIKit/UIKit.h>
#include <notify.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <math.h>
#include <unistd.h>

#define SWEETCTL	"/var/local/bin/sweetctl"
#define USER_KNOBS	@"/var/local/sweetctl/knobs.json"
#define BOOT_CONF	@"/var/local/sweetctl/boot.conf"
#define HUD_NOTIFY	"org.iosonpixel.sweethud"

extern char **environ;

/* ---------- helper and readers ---------- */

static int
run_helper(NSArray<NSString *> *args, NSString **out)
{
	int fds[2];
	if (pipe(fds) != 0) {
		return -1;
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
	posix_spawn_file_actions_addclose(&fa, fds[0]);
	char **argv = calloc(args.count + 2, sizeof(char *));
	argv[0] = (char *)SWEETCTL;
	for (NSUInteger i = 0; i < args.count; i++) {
		argv[i + 1] = (char *)args[i].UTF8String;
	}
	pid_t pid;
	int r = posix_spawn(&pid, SWEETCTL, &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	free(argv);
	close(fds[1]);
	NSMutableData *d = [NSMutableData data];
	char buf[4096];
	ssize_t n;
	while (r == 0 && (n = read(fds[0], buf, sizeof(buf))) > 0) {
		[d appendBytes:buf length:(NSUInteger)n];
	}
	close(fds[0]);
	if (r != 0) {
		if (out) {
			*out = [NSString stringWithFormat:@"cannot run %s: %s", SWEETCTL, strerror(r)];
		}
		return -1;
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if (out) {
		*out = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] ?: @"";
	}
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int
sc_kind(const char *name)
{
	int mib[CTL_MAXNAME + 2];
	size_t miblen = CTL_MAXNAME;
	if (sysctlnametomib(name, mib + 2, &miblen) != 0) {
		return -1;
	}
	mib[0] = 0;
	mib[1] = 4;
	unsigned char buf[256];
	size_t len = sizeof(buf);
	if (sysctl(mib, (u_int)miblen + 2, buf, &len, NULL, 0) != 0 || len < 4) {
		return -1;
	}
	uint32_t k;
	memcpy(&k, buf, 4);
	return (int)(k & CTLTYPE);
}

/* number, string or nil (no such sysctl) */
static id
sc_read(NSString *name)
{
	const char *n = name.UTF8String;
	int kind = sc_kind(n);
	if (kind < 0) {
		return nil;
	}
	if (kind == CTLTYPE_STRING) {
		char s[1024];
		size_t l = sizeof(s) - 1;
		if (sysctlbyname(n, s, &l, NULL, 0) != 0) {
			return nil;
		}
		s[l] = 0;
		return [@(s) stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
	}
	uint64_t v = 0;
	size_t l = sizeof(v);
	if (sysctlbyname(n, &v, &l, NULL, 0) != 0) {
		return nil;
	}
	return l == 4 ? @((int32_t)(uint32_t)v) : @((long long)v);
}

static NSString *
read_file(NSString *path)
{
	NSString *s = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
	if (s == nil && access(path.fileSystemRepresentation, F_OK) == 0) {
		NSString *o = nil;
		if (run_helper(@[ @"cat", path ], &o) == 0) {
			s = o;
		}
	}
	return s;
}

static NSString *
kv_read(NSString *path, NSString *key)
{
	for (NSString *raw in [read_file(path) componentsSeparatedByString:@"\n"]) {
		NSString *l = [raw stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
		if ([l hasPrefix:@"export "]) {
			l = [l substringFromIndex:7];
		}
		if ([l hasPrefix:[key stringByAppendingString:@"="]]) {
			return [l substringFromIndex:key.length + 1];
		}
	}
	return nil;
}

static NSArray<NSString *> *
words_read(NSString *path)
{
	NSString *s = read_file(path);
	if (s == nil) {
		return nil;
	}
	NSMutableArray *w = [NSMutableArray array];
	for (NSString *x in [s componentsSeparatedByCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]]) {
		if (x.length) {
			[w addObject:x];
		}
	}
	return w;
}

static uint64_t
hud_state_get(void)
{
	int tok = 0;
	uint64_t st = 0;
	if (notify_register_check(HUD_NOTIFY, &tok) == NOTIFY_STATUS_OK) {
		notify_get_state(tok, &st);
		notify_cancel(tok);
	}
	return st;
}

static double
num(id v)
{
	if ([v isKindOfClass:[NSNumber class]]) {
		return [v doubleValue];
	}
	if ([v isKindOfClass:[NSString class]]) {
		return strtod([v UTF8String], NULL);	/* accepts 0x.. too */
	}
	return 0;
}

static NSString *
fmtnum(double v)
{
	return v == floor(v) ? [NSString stringWithFormat:@"%lld", (long long)v] : [NSString stringWithFormat:@"%g", v];
}

/* ---------- model ---------- */

@interface Model : NSObject
@property (strong) NSArray<NSDictionary *> *sections;
@property (strong) NSArray<NSDictionary *> *actions;
@property (strong) NSMutableDictionary<NSString *, NSMutableOrderedSet<NSString *> *> *pending;	/* effect -> titles */
@property (copy) NSString *source, *helperState, *lastError;
@property BOOL helperRoot;
+ (instancetype)shared;
- (id)valueOf:(NSDictionary *)k;	/* NSNumber, NSString, or nil = unset / absent */
- (BOOL)set:(NSDictionary *)k to:(id)v;	/* v nil = unset */
- (NSString *)textOf:(NSDictionary *)k;
- (BOOL)pinned:(NSDictionary *)k;
- (BOOL)pin:(NSDictionary *)k on:(BOOL)on;
- (BOOL)runAction:(NSDictionary *)a output:(NSString **)out;
@end

@implementation Model

+ (instancetype)shared
{
	static Model *m;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		m = [Model new];
		[m load];
	});
	return m;
}

- (void)load
{
	NSData *d = [NSData dataWithContentsOfFile:USER_KNOBS];
	self.source = USER_KNOBS;
	if (d == nil) {
		d = [NSData dataWithContentsOfFile:[[NSBundle mainBundle] pathForResource:@"Knobs" ofType:@"json"]];
		self.source = @"built-in";
	}
	NSError *e = nil;
	NSDictionary *j = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:&e] : nil;
	if (![j isKindOfClass:[NSDictionary class]]) {
		self.lastError = [NSString stringWithFormat:@"knob catalog unreadable (%@): %@", self.source, e.localizedDescription];
		j = @{};
	}
	self.sections = j[@"sections"] ?: @[];
	self.actions = j[@"actions"] ?: @[];
	self.pending = [NSMutableDictionary dictionary];
	[self checkHelper];
}

- (void)checkHelper
{
	NSString *o = nil;
	int r = run_helper(@[ @"id" ], &o);
	self.helperRoot = r == 0;
	self.helperState = r == 0 ? @"sweetctl: root" : (r < 0 ? @"sweetctl missing: install /var/local/bin/sweetctl"
			  : [NSString stringWithFormat:@"sweetctl is not setuid root (%@): chown root:wheel + chmod 4755",
				   [o stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]]]);
}

- (uint64_t)hudState
{
	uint64_t st = hud_state_get();
	if (st != 0) {
		return st;
	}
	for (NSDictionary *s in self.sections) {	/* nothing set yet: the catalog's defaults (overlay off) */
		for (NSDictionary *k in s[@"knobs"]) {
			if ([k[@"kind"] isEqual:@"hud"] && k[@"default"]) {
				int sh = [k[@"shift"] intValue], w = [k[@"width"] intValue];
				st |= ((uint64_t)[k[@"default"] longLongValue] & ((1ULL << w) - 1)) << sh;
			}
		}
	}
	return st;
}

- (id)valueOf:(NSDictionary *)k
{
	NSString *kind = k[@"kind"];
	if ([kind isEqual:@"sysctl"] || [kind isEqual:@"info"]) {
		if ([k[@"noread"] boolValue]) {
			return k[@"_last"];
		}
		return sc_read(k[@"name"]);
	}
	if ([kind isEqual:@"kv"]) {
		NSString *s = kv_read(k[@"file"], k[@"key"]);
		return s;
	}
	if ([kind isEqual:@"words"]) {
		NSArray *w = words_read(k[@"file"]);
		NSUInteger i = [k[@"index"] unsignedIntegerValue];
		return i < w.count ? w[i] : nil;
	}
	if ([kind isEqual:@"size"]) {
		struct stat st;
		if (stat([k[@"file"] fileSystemRepresentation], &st) != 0) {
			return nil;
		}
		double scale = k[@"scale"] ? [k[@"scale"] doubleValue] : 1;
		return @(((double)st.st_size - [k[@"base"] doubleValue]) / scale);
	}
	if ([kind isEqual:@"flag"]) {
		return @(access([k[@"file"] fileSystemRepresentation], F_OK) == 0);
	}
	if ([kind isEqual:@"hud"]) {
		int sh = [k[@"shift"] intValue], w = [k[@"width"] intValue];
		return @(([self hudState] >> sh) & ((1ULL << w) - 1));
	}
	return nil;
}

- (NSString *)textOf:(NSDictionary *)k
{
	id v = [self valueOf:k];
	if (v == nil) {
		NSString *kind = k[@"kind"];
		if ([kind isEqual:@"sysctl"] || [kind isEqual:@"info"]) {
			return [k[@"noread"] boolValue] ? @"" : @"n/a";
		}
		return k[@"default"] ? [NSString stringWithFormat:@"unset (%@)", k[@"default"]] : @"unset";
	}
	if ([v isKindOfClass:[NSString class]] && ![k[@"type"] length]) {
		return v;
	}
	double x = num(v);
	if (k[@"div"]) {
		x /= [k[@"div"] doubleValue];
		return [NSString stringWithFormat:@"%.1f", x];
	}
	NSString *type = k[@"type"];
	if ([type isEqual:@"bool"]) {
		return x != 0 ? @"on" : @"off";
	}
	if ([type isEqual:@"enum"]) {
		for (NSDictionary *o in k[@"options"]) {
			if ([o[@"v"] doubleValue] == x) {
				return o[@"t"];
			}
		}
	}
	if ([type isEqual:@"bits"]) {
		return [NSString stringWithFormat:@"0x%llx", (long long)x];
	}
	if (![type length] && [v isKindOfClass:[NSString class]]) {
		return v;
	}
	return fmtnum(x);
}

- (void)note:(NSDictionary *)k
{
	NSString *eff = k[@"effect"] ?: @"live";
	if ([eff isEqual:@"live"]) {
		return;
	}
	if (self.pending[eff] == nil) {
		self.pending[eff] = [NSMutableOrderedSet orderedSet];
	}
	[self.pending[eff] addObject:k[@"title"]];
}

- (BOOL)helper:(NSArray<NSString *> *)args
{
	NSString *o = nil;
	int r = run_helper(args, &o);
	if (r != 0) {
		self.lastError = [NSString stringWithFormat:@"sweetctl %@: %@", [args componentsJoinedByString:@" "],
				  o.length ? o : [NSString stringWithFormat:@"exit %d", r]];
		return NO;
	}
	self.lastError = nil;
	return YES;
}

- (BOOL)set:(NSDictionary *)k to:(id)v
{
	NSString *kind = k[@"kind"];
	NSString *vs = v ? fmtnum(num(v)) : nil;
	BOOL ok = NO;
	if ([kind isEqual:@"sysctl"]) {
		ok = v && [self helper:@[ @"set", k[@"name"], vs ]];
		if (ok && [k[@"noread"] boolValue]) {
			NSMutableDictionary *mk = (NSMutableDictionary *)k;
			if ([mk isKindOfClass:[NSMutableDictionary class]]) {
				mk[@"_last"] = v;
			}
		}
		if (ok && [self pinned:k]) {
			[self helper:@[ @"boot-set", k[@"name"], vs ]];	/* keep the pin in step */
		}
	} else if ([kind isEqual:@"kv"]) {
		ok = v ? [self helper:@[ @"kv-set", k[@"file"], k[@"key"], vs ]] : [self helper:@[ @"kv-del", k[@"file"], k[@"key"] ]];
	} else if ([kind isEqual:@"words"]) {
		NSMutableArray *w = [words_read(k[@"file"]) mutableCopy] ?: [NSMutableArray array];
		NSUInteger i = [k[@"index"] unsignedIntegerValue];
		if (v == nil && i == 0) {
			ok = [self helper:@[ @"flag", k[@"file"], @"0" ]];
		} else {
			if (v == nil) {
				[w removeObjectsInRange:NSMakeRange(MIN(i, w.count), w.count - MIN(i, w.count))];
			} else {
				while (w.count <= i) {
					[w addObject:@"0"];
				}
				w[i] = vs;
			}
			ok = [self helper:@[ @"write", k[@"file"], [w componentsJoinedByString:@" "] ]];
		}
	} else if ([kind isEqual:@"size"]) {
		double scale = k[@"scale"] ? [k[@"scale"] doubleValue] : 1;
		long sz = v ? lround([k[@"base"] doubleValue] + scale * num(v)) : -1;
		if (v && sz < 0) {
			self.lastError = @"value out of range for a size flag";
			return NO;
		}
		ok = [self helper:@[ @"size", k[@"file"], [NSString stringWithFormat:@"%ld", sz] ]];
	} else if ([kind isEqual:@"flag"]) {
		ok = [self helper:@[ @"flag", k[@"file"], num(v) != 0 ? @"1" : @"0" ]];
	} else if ([kind isEqual:@"hud"]) {
		int sh = [k[@"shift"] intValue], w = [k[@"width"] intValue];
		uint64_t mask = ((1ULL << w) - 1) << sh;
		uint64_t st = ([self hudState] & ~mask) | (((uint64_t)(long long)num(v) << sh) & mask);
		int tok = 0;
		if (notify_register_check(HUD_NOTIFY, &tok) == NOTIFY_STATUS_OK) {
			notify_set_state(tok, st);
			notify_cancel(tok);
		}
		notify_post(HUD_NOTIFY);
		[self helper:@[ @"write", @"/var/local/sweethud", [NSString stringWithFormat:@"state=0x%llx", st] ]];
		ok = YES;	/* the overlay changed even if the boot copy could not be written */
	}
	if (ok) {
		[self note:k];
	}
	return ok;
}

- (BOOL)pinned:(NSDictionary *)k
{
	if (access(BOOT_CONF.fileSystemRepresentation, F_OK) != 0) {
		return NO;
	}
	NSString *s = [NSString stringWithContentsOfFile:BOOT_CONF encoding:NSUTF8StringEncoding error:nil];
	if (s == nil) {
		NSString *o = nil;
		if (run_helper(@[ @"boot-list" ], &o) == 0) {
			s = o;
		}
	}
	for (NSString *l in [s componentsSeparatedByString:@"\n"]) {
		if ([l hasPrefix:[k[@"name"] stringByAppendingString:@"="]]) {
			return YES;
		}
	}
	return NO;
}

- (BOOL)pin:(NSDictionary *)k on:(BOOL)on
{
	id v = [self valueOf:k];
	if (on) {
		return v && [self helper:@[ @"boot-set", k[@"name"], fmtnum(num(v)) ]];
	}
	return [self helper:@[ @"boot-del", k[@"name"] ]];
}

- (BOOL)runAction:(NSDictionary *)a output:(NSString **)out
{
	int r = run_helper(a[@"cmd"], out);
	if (r == 0) {
		NSString *c = [a[@"cmd"] firstObject];
		if ([c isEqual:@"respring"]) {
			[self.pending removeObjectForKey:@"respring"];
		} else if ([c isEqual:@"render"]) {
			[self.pending removeObjectForKey:@"respring"];
			[self.pending removeObjectForKey:@"render"];
		} else if ([c isEqual:@"kill"] && [a[@"cmd"] count] > 1) {
			[self.pending removeObjectForKey:[@"daemon:" stringByAppendingString:a[@"cmd"][1]]];
		}
	}
	return r == 0;
}
@end

/* ---------- knob page ---------- */

@interface KnobVC : UITableViewController <UITextFieldDelegate>
@property (strong) NSDictionary *knob;
@property (strong) UITextField *field;
@end

enum { KS_ABOUT, KS_EDIT, KS_EXTRA, KS_N };

@implementation KnobVC

- (instancetype)initWithKnob:(NSDictionary *)k
{
	if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
		_knob = k;
		self.title = k[@"title"];
	}
	return self;
}

- (Model *)m
{
	return [Model shared];
}

- (NSString *)type
{
	return self.knob[@"type"] ?: @"int";
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tv
{
	return KS_N;
}

- (NSArray *)extraRows
{
	NSMutableArray *r = [NSMutableArray array];
	if ([self.knob[@"kind"] isEqual:@"sysctl"] && ![self.knob[@"nopin"] boolValue]) {
		[r addObject:@"pin"];
	}
	if ([self.knob[@"unset"] boolValue]) {
		[r addObject:@"unset"];
	}
	if (self.knob[@"default"] != nil) {
		[r addObject:@"default"];
	}
	return r;
}

- (NSInteger)tableView:(UITableView *)tv numberOfRowsInSection:(NSInteger)s
{
	if (s == KS_ABOUT) {
		return 2;
	}
	if (s == KS_EDIT) {
		NSString *t = [self type];
		if ([t isEqual:@"enum"]) {
			return (NSInteger)[self.knob[@"options"] count];
		}
		if ([t isEqual:@"bits"]) {
			return (NSInteger)[self.knob[@"bits"] count] + 1;
		}
		return 1;
	}
	return (NSInteger)[self extraRows].count;
}

- (NSString *)tableView:(UITableView *)tv titleForHeaderInSection:(NSInteger)s
{
	return s == KS_EDIT ? @"Value" : nil;
}

- (NSString *)tableView:(UITableView *)tv titleForFooterInSection:(NSInteger)s
{
	if (s == KS_ABOUT) {
		NSMutableString *f = [NSMutableString string];
		if (self.knob[@"desc"]) {
			[f appendString:self.knob[@"desc"]];
		}
		NSString *eff = self.knob[@"effect"] ?: @"live";
		NSDictionary *names = @{ @"live": @"Takes effect at once.", @"respring": @"Takes effect after a respring.",
					 @"render": @"Takes effect after restarting the render server.",
					 @"userspace": @"Takes effect after a userspace reboot.",
					 @"reboot": @"Takes effect at the next boot." };
		[f appendFormat:@"%@%@", f.length ? @"\n\n" : @"", names[eff] ?: [NSString stringWithFormat:@"Takes effect after %@.", eff]];
		if (self.knob[@"min"] && self.knob[@"max"]) {
			[f appendFormat:@" Range %@ to %@.", self.knob[@"min"], self.knob[@"max"]];
		}
		return f;
	}
	if (s == KS_EDIT && self.m.lastError) {
		return self.m.lastError;
	}
	return nil;
}

- (UITableViewCell *)cell:(UITableViewCellStyle)st
{
	UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:st reuseIdentifier:nil];
	c.selectionStyle = UITableViewCellSelectionStyleDefault;
	return c;
}

- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip
{
	NSDictionary *k = self.knob;
	id v = [self.m valueOf:k];
	if (ip.section == KS_ABOUT) {
		UITableViewCell *c = [self cell:UITableViewCellStyleValue1];
		c.selectionStyle = UITableViewCellSelectionStyleNone;
		if (ip.row == 0) {
			c.textLabel.text = @"Current";
			c.detailTextLabel.text = [self.m textOf:k];
		} else {
			c.textLabel.text = k[@"name"] ?: (k[@"key"] ? [NSString stringWithFormat:@"%@ in %@", k[@"key"], [k[@"file"] lastPathComponent]]
					       : [k[@"file"] lastPathComponent] ?: k[@"kind"]);
			c.textLabel.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
			c.textLabel.adjustsFontSizeToFitWidth = YES;
		}
		return c;
	}
	if (ip.section == KS_EDIT) {
		NSString *t = [self type];
		UITableViewCell *c = [self cell:UITableViewCellStyleDefault];
		if ([t isEqual:@"enum"]) {
			NSDictionary *o = k[@"options"][ip.row];
			c.textLabel.text = o[@"t"];
			c.textLabel.numberOfLines = 0;
			c.accessoryType = v && num(v) == [o[@"v"] doubleValue] ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
			return c;
		}
		if ([t isEqual:@"bits"]) {
			NSArray *bits = k[@"bits"];
			if ((NSUInteger)ip.row == bits.count) {
				return [self numberCell:v];
			}
			NSDictionary *b = bits[ip.row];
			c.textLabel.text = b[@"t"];
			c.textLabel.numberOfLines = 0;
			c.selectionStyle = UITableViewCellSelectionStyleNone;
			UISwitch *sw = [UISwitch new];
			sw.on = v && ((long long)num(v) & [b[@"b"] longLongValue]) != 0;
			sw.tag = ip.row;
			[sw addTarget:self action:@selector(bitFlipped:) forControlEvents:UIControlEventValueChanged];
			c.accessoryView = sw;
			return c;
		}
		if ([t isEqual:@"bool"]) {
			c.textLabel.text = @"On";
			c.selectionStyle = UITableViewCellSelectionStyleNone;
			UISwitch *sw = [UISwitch new];
			sw.on = v && num(v) != 0;
			[sw addTarget:self action:@selector(boolFlipped:) forControlEvents:UIControlEventValueChanged];
			c.accessoryView = sw;
			return c;
		}
		return [self numberCell:v];
	}
	NSString *row = [self extraRows][ip.row];
	UITableViewCell *c = [self cell:UITableViewCellStyleDefault];
	if ([row isEqual:@"pin"]) {
		c.textLabel.text = @"Apply at boot";
		c.selectionStyle = UITableViewCellSelectionStyleNone;
		UISwitch *sw = [UISwitch new];
		sw.on = [self.m pinned:k];
		[sw addTarget:self action:@selector(pinFlipped:) forControlEvents:UIControlEventValueChanged];
		c.accessoryView = sw;
	} else if ([row isEqual:@"unset"]) {
		c.textLabel.text = @"Unset (program default)";
		c.textLabel.textColor = [UIColor systemRedColor];
	} else {
		c.textLabel.text = [NSString stringWithFormat:@"Reset to default (%@)", k[@"default"]];
		c.textLabel.textColor = self.view.tintColor;
	}
	return c;
}

- (UITableViewCell *)numberCell:(id)v
{
	UITableViewCell *c = [self cell:UITableViewCellStyleDefault];
	c.selectionStyle = UITableViewCellSelectionStyleNone;
	UIStepper *st = [UIStepper new];
	st.minimumValue = self.knob[@"min"] ? [self.knob[@"min"] doubleValue] : -1e9;
	st.maximumValue = self.knob[@"max"] ? [self.knob[@"max"] doubleValue] : 1e12;
	st.stepValue = self.knob[@"step"] ? [self.knob[@"step"] doubleValue] : 1;
	st.value = v ? num(v) : num(self.knob[@"default"]);
	[st addTarget:self action:@selector(stepped:) forControlEvents:UIControlEventValueChanged];
	c.accessoryView = st;
	UITextField *f = [[UITextField alloc] initWithFrame:CGRectMake(16, 0, 150, 44)];
	f.text = v ? fmtnum(num(v)) : @"";
	f.placeholder = self.knob[@"default"] ? [NSString stringWithFormat:@"%@", self.knob[@"default"]] : @"value";
	f.keyboardType = UIKeyboardTypeNumbersAndPunctuation;
	f.returnKeyType = UIReturnKeyDone;
	f.delegate = self;
	f.font = [UIFont monospacedDigitSystemFontOfSize:17 weight:UIFontWeightRegular];
	[c.contentView addSubview:f];
	self.field = f;
	return c;
}

- (void)commit:(id)v
{
	[self.m set:self.knob to:v];
	[self.tableView reloadData];
}

- (void)stepped:(UIStepper *)st
{
	[self commit:@(st.value)];
}

- (BOOL)textFieldShouldReturn:(UITextField *)f
{
	[f resignFirstResponder];
	if (f.text.length) {
		[self commit:@(num(f.text))];
	}
	return YES;
}

- (void)boolFlipped:(UISwitch *)sw
{
	[self commit:@(sw.on ? 1 : 0)];
}

- (void)bitFlipped:(UISwitch *)sw
{
	long long cur = (long long)num([self.m valueOf:self.knob] ?: self.knob[@"default"]);
	long long b = [self.knob[@"bits"][sw.tag][@"b"] longLongValue];
	[self commit:@(sw.on ? (cur | b) : (cur & ~b))];
}

- (void)pinFlipped:(UISwitch *)sw
{
	[self.m pin:self.knob on:sw.on];
	[self.tableView reloadData];
}

- (void)tableView:(UITableView *)tv didSelectRowAtIndexPath:(NSIndexPath *)ip
{
	[tv deselectRowAtIndexPath:ip animated:YES];
	if (ip.section == KS_EDIT && [[self type] isEqual:@"enum"]) {
		[self commit:self.knob[@"options"][ip.row][@"v"]];
	} else if (ip.section == KS_EXTRA) {
		NSString *row = [self extraRows][ip.row];
		if ([row isEqual:@"unset"]) {
			[self commit:nil];
		} else if ([row isEqual:@"default"]) {
			[self commit:self.knob[@"default"]];
		}
	}
}
@end

/* ---------- main list ---------- */

@interface RootVC : UITableViewController
@property (strong) NSTimer *timer;
@end

/* sections: 0 status + pending, 1..n catalog, n+1 actions */
@implementation RootVC

- (Model *)m
{
	return [Model shared];
}

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"Sweet Control";
	self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Respring" style:UIBarButtonItemStylePlain
										 target:self action:@selector(respring)];
	self.refreshControl = [UIRefreshControl new];
	[self.refreshControl addTarget:self action:@selector(pulled) forControlEvents:UIControlEventValueChanged];
}

- (void)viewWillAppear:(BOOL)a
{
	[super viewWillAppear:a];
	[self.tableView reloadData];
	self.timer = [NSTimer scheduledTimerWithTimeInterval:2 repeats:YES block:^(NSTimer *t) {
		/* only the sections with read-only status rows: the rest changes only through this app */
		NSMutableIndexSet *is = [NSMutableIndexSet indexSet];
		[self.m.sections enumerateObjectsUsingBlock:^(NSDictionary *sec, NSUInteger i, BOOL *stop) {
			for (NSDictionary *k in sec[@"knobs"]) {
				if ([k[@"kind"] isEqual:@"info"]) {
					[is addIndex:i + 1];
					break;
				}
			}
		}];
		if (is.count && !self.tableView.isDragging) {
			[self.tableView reloadSections:is withRowAnimation:UITableViewRowAnimationNone];
		}
	}];
}

- (void)viewWillDisappear:(BOOL)a
{
	[super viewWillDisappear:a];
	[self.timer invalidate];
}

- (void)pulled
{
	[self.m checkHelper];
	[self.tableView reloadData];
	[self.refreshControl endRefreshing];
}

- (void)respring
{
	[self runAction:@{ @"title": @"Respring", @"cmd": @[ @"respring" ] }];
}

- (NSArray<NSString *> *)pendingKeys
{
	return [self.m.pending.allKeys sortedArrayUsingSelector:@selector(compare:)];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tv
{
	return (NSInteger)self.m.sections.count + 2;
}

- (NSInteger)tableView:(UITableView *)tv numberOfRowsInSection:(NSInteger)s
{
	if (s == 0) {
		return 1 + (NSInteger)[self pendingKeys].count;
	}
	if ((NSUInteger)s <= self.m.sections.count) {
		return (NSInteger)[self.m.sections[s - 1][@"knobs"] count];
	}
	return (NSInteger)self.m.actions.count;
}

- (NSString *)tableView:(UITableView *)tv titleForHeaderInSection:(NSInteger)s
{
	if (s == 0) {
		return nil;
	}
	if ((NSUInteger)s <= self.m.sections.count) {
		return self.m.sections[s - 1][@"title"];
	}
	return @"Restart and reboot";
}

- (NSString *)tableView:(UITableView *)tv titleForFooterInSection:(NSInteger)s
{
	if (s == 0) {
		return self.m.lastError;
	}
	if ((NSUInteger)s <= self.m.sections.count) {
		return self.m.sections[s - 1][@"footer"];
	}
	return [NSString stringWithFormat:@"Knobs: %@. Changes are logged in /var/local/sweetctl/changes.log; each file's original is kept as FILE.pre-sweetctl.",
	    self.m.source];
}

- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip
{
	UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
	if (ip.section == 0) {
		if (ip.row == 0) {
			c.textLabel.text = self.m.helperState;
			c.textLabel.numberOfLines = 0;
			c.textLabel.textColor = self.m.helperRoot ? [UIColor systemGreenColor] : [UIColor systemRedColor];
			c.selectionStyle = UITableViewCellSelectionStyleNone;
			return c;
		}
		NSString *eff = [self pendingKeys][ip.row - 1];
		NSOrderedSet *what = self.m.pending[eff];
		NSDictionary *label = @{ @"respring": @"Respring now", @"render": @"Restart render server now",
					 @"userspace": @"Userspace reboot now", @"reboot": @"Applies at next boot" };
		c.textLabel.text = label[eff] ?: [NSString stringWithFormat:@"Restart %@ now", [eff stringByReplacingOccurrencesOfString:@"daemon:" withString:@""]];
		c.textLabel.textColor = [eff isEqual:@"reboot"] ? [UIColor secondaryLabelColor] : self.view.tintColor;
		c.detailTextLabel.text = [what.array componentsJoinedByString:@", "];
		c.detailTextLabel.numberOfLines = 2;
		return c;
	}
	if ((NSUInteger)ip.section <= self.m.sections.count) {
		NSDictionary *k = self.m.sections[ip.section - 1][@"knobs"][ip.row];
		c.textLabel.text = k[@"title"];
		c.textLabel.numberOfLines = 0;
		BOOL info = [k[@"kind"] isEqual:@"info"];
		if ([k[@"type"] isEqual:@"bool"]) {
			UISwitch *sw = [UISwitch new];
			id v = [self.m valueOf:k];
			sw.on = v ? num(v) != 0 : num(k[@"default"]) != 0;
			sw.alpha = v ? 1 : 0.5;	/* unset: showing the default */
			sw.tag = ip.section * 1000 + ip.row;
			[sw addTarget:self action:@selector(flipped:) forControlEvents:UIControlEventValueChanged];
			c.accessoryView = sw;
			if ([k[@"kind"] isEqual:@"sysctl"] && [self.m pinned:k]) {
				c.detailTextLabel.text = @"pinned";
			}
		} else {
			c.detailTextLabel.text = [self.m textOf:k];
			if ([k[@"kind"] isEqual:@"sysctl"] && [self.m pinned:k]) {
				c.detailTextLabel.text = [c.detailTextLabel.text stringByAppendingString:@" · pinned"];
			}
			c.accessoryType = info ? UITableViewCellAccessoryNone : UITableViewCellAccessoryDisclosureIndicator;
		}
		if (info) {
			c.selectionStyle = UITableViewCellSelectionStyleNone;
		}
		return c;
	}
	NSDictionary *a = self.m.actions[ip.row];
	c.textLabel.text = a[@"title"];
	c.textLabel.textColor = [a[@"danger"] boolValue] ? [UIColor systemRedColor] : self.view.tintColor;
	c.detailTextLabel.text = a[@"desc"];
	c.detailTextLabel.numberOfLines = 2;
	return c;
}

- (NSDictionary *)knobForTag:(NSInteger)tag
{
	return self.m.sections[tag / 1000 - 1][@"knobs"][tag % 1000];
}

- (void)flipped:(UISwitch *)sw
{
	[self.m set:[self knobForTag:sw.tag] to:@(sw.on ? 1 : 0)];
	[self.tableView reloadData];
}

- (void)runAction:(NSDictionary *)a
{
	void (^go)(void) = ^{
		NSString *o = nil;
		BOOL ok = [self.m runAction:a output:&o];
		if (!ok) {
			UIAlertController *ac = [UIAlertController alertControllerWithTitle:a[@"title"] message:o.length ? o : @"failed"
									      preferredStyle:UIAlertControllerStyleAlert];
			[ac addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
			[self presentViewController:ac animated:YES completion:nil];
		} else if (o.length && ![[a[@"cmd"] firstObject] isEqual:@"respring"] && ![[a[@"cmd"] firstObject] isEqual:@"render"]) {
			UIAlertController *ac = [UIAlertController alertControllerWithTitle:a[@"title"] message:o
									      preferredStyle:UIAlertControllerStyleAlert];
			[ac addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
			[self presentViewController:ac animated:YES completion:nil];
		}
		[self.tableView reloadData];
	};
	if (a[@"confirm"]) {
		UIAlertController *ac = [UIAlertController alertControllerWithTitle:a[@"title"] message:a[@"confirm"]
								      preferredStyle:UIAlertControllerStyleAlert];
		[ac addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
		[ac addAction:[UIAlertAction actionWithTitle:a[@"title"] style:UIAlertActionStyleDestructive handler:^(UIAlertAction *x) {
			go();
		}]];
		[self presentViewController:ac animated:YES completion:nil];
	} else {
		go();
	}
}

- (void)tableView:(UITableView *)tv didSelectRowAtIndexPath:(NSIndexPath *)ip
{
	[tv deselectRowAtIndexPath:ip animated:YES];
	if (ip.section == 0) {
		if (ip.row == 0) {
			return;
		}
		NSString *eff = [self pendingKeys][ip.row - 1];
		NSDictionary *cmds = @{ @"respring": @[ @"respring" ], @"render": @[ @"render" ], @"userspace": @[ @"userspace-reboot" ] };
		NSArray *cmd = cmds[eff];
		if (cmd == nil && [eff hasPrefix:@"daemon:"]) {
			cmd = @[ @"kill", [eff substringFromIndex:7] ];
		}
		if (cmd != nil) {
			NSMutableDictionary *a = [@{ @"title": [tv cellForRowAtIndexPath:ip].textLabel.text ?: eff, @"cmd": cmd } mutableCopy];
			if ([eff isEqual:@"userspace"]) {
				a[@"confirm"] = @"All of userland restarts on the same kernel.";
			}
			[self runAction:a];
		}
		return;
	}
	if ((NSUInteger)ip.section <= self.m.sections.count) {
		NSDictionary *k = self.m.sections[ip.section - 1][@"knobs"][ip.row];
		if (![k[@"kind"] isEqual:@"info"]) {
			NSMutableDictionary *mk = [k mutableCopy];	/* the page may note the last written value (noread knobs) */
			[self.navigationController pushViewController:[[KnobVC alloc] initWithKnob:mk] animated:YES];
		}
		return;
	}
	[self runAction:self.m.actions[ip.row]];
}
@end

/* ---------- app ---------- */

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts
{
	self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
	RootVC *root = [[RootVC alloc] initWithStyle:UITableViewStyleInsetGrouped];
	UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:root];
	nav.navigationBar.prefersLargeTitles = YES;
	self.window.rootViewController = nav;
	[self.window makeKeyAndVisible];
	return YES;
}
@end

int
main(int argc, char **argv)
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
	}
}
