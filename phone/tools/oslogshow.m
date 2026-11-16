/*
 * oslogshow: read the *persisted* unified log (like `log show`), not the live stream.
 *   oslogshow FROM_EPOCH TO_EPOCH [-l] [PROCESS...]
 * LoggingSupport's OSLogEventLocalStore + OSLogEventStream. Signed with oslog-ents.plist.
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

@interface OSLogEventLocalStore : NSObject
+ (id)localStore;
- (void)prepareWithCompletionHandler:(void (^)(id source, NSError *error))handler;
@end
@interface OSLogEventStream : NSObject
- (id)initWithSource:(id)source;
- (void)setEventHandler:(void (^)(id event))handler;
- (void)setInvalidationHandler:(void (^)(int reason, id position))handler;
- (void)setFlags:(unsigned long long)flags;
- (void)activateStreamFromDate:(NSDate *)from toDate:(NSDate *)to;
@end
@interface OSLogEventProxy : NSObject
- (NSString *)composedMessage;
- (NSString *)process;
- (NSString *)sender;
- (int)processIdentifier;
- (unsigned long long)logType;
- (struct timeval *)unixDate;
@end

static const char *type_name(unsigned long long t)
{
	switch (t) {
	case 0x00: return "Df";
	case 0x01: return "In";
	case 0x02: return "Db";
	case 0x10: return "Er";
	case 0x11: return "Ft";
	default: return "  ";
	}
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc < 3) {
			printf("usage: oslogshow FROM_EPOCH TO_EPOCH [-l] [PROCESS...]\n");
			return 2;
		}
		double from = atof(argv[1]), to = atof(argv[2]);
		int levels = 0, first = 3;
		if (first < argc && !strcmp(argv[first], "-l")) {
			levels = 1;
			first++;
		}
		NSMutableArray *procs = [NSMutableArray array];
		for (int i = first; i < argc; i++)
			[procs addObject:@(argv[i])];
		setvbuf(stdout, NULL, _IOLBF, 0);
		dlopen("/System/Library/PrivateFrameworks/LoggingSupport.framework/LoggingSupport", RTLD_NOW);
		Class storeClass = NSClassFromString(@"OSLogEventLocalStore");
		if (!storeClass) {
			printf("oslogshow: no OSLogEventLocalStore\n");
			return 1;
		}
		__block OSLogEventStream *stream = nil;
		[[storeClass localStore] prepareWithCompletionHandler:^(id source, NSError *err) {
			if (!source) {
				printf("oslogshow: no source: %s\n", err.description.UTF8String);
				exit(1);
			}
			stream = [[NSClassFromString(@"OSLogEventStream") alloc] initWithSource:source];
			[stream setFlags:levels ? 0x1 | 0x2 | 0x4 : 0];
			[stream setEventHandler:^(OSLogEventProxy *e) {
				NSString *p = e.process ?: @"?";
				if (procs.count) {
					BOOL hit = NO;
					for (NSString *want in procs)
						if ([p rangeOfString:want].location != NSNotFound)
							hit = YES;
					if (!hit)
						return;
				}
				struct timeval *tv = e.unixDate;
				struct tm tm;
				time_t s = tv ? tv->tv_sec : 0;
				localtime_r(&s, &tm);
				printf("%02d:%02d:%02d.%03d %s %s[%d] (%s) %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
				       tv ? (int)(tv->tv_usec / 1000) : 0, type_name(e.logType), p.UTF8String,
				       e.processIdentifier, (e.sender ?: @"").UTF8String,
				       (e.composedMessage ?: @"").UTF8String);
			}];
			[stream setInvalidationHandler:^(int reason, id pos) {
				printf("oslogshow: stream ended (%d)\n", reason);
				exit(0);
			}];
			[stream activateStreamFromDate:[NSDate dateWithTimeIntervalSince1970:from]
					    toDate:[NSDate dateWithTimeIntervalSince1970:to]];
		}];
		[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:170]];
		printf("oslogshow: timeout\n");
	}
	return 0;
}
