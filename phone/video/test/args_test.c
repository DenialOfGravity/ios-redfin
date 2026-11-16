/*
 * Host test for venusctl's argument handling (redfin video P0): venusctl.c's main() linked against stubs that
 * record which command ran with which arguments. Run by phone/video/test/run.sh.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../venus_core.h"
#include "../venus_rails.h"

int verbose, cvp, force, vc_quiet, vc_fake_off, vc_frame_tag;
const char *vc_stop_at;
void (*vc_stop_hook)(const char *what);
static char called[128];
#define REC(...) (snprintf(called, sizeof(called), __VA_ARGS__), 0)
void pm_init(void) {}
int pm_step_open(const char *p) { (void)p; return 0; }
void pm_step(const char *fmt, ...) { (void)fmt; }
void stale_drop(const char *p) { (void)p; }
const char *boot_id(void) { return "x"; }
int probe(void) { return REC("probe"); }
int rails_report(void) { return REC("rails"); }
int power_on(void) { return REC("power_on cvp=%d", cvp); }
int power_off(void) { return REC("power_off force=%d", force); }
int pas_check(const char *fw, int ns) { return REC("pas_check %d", ns); }
int boot(const char *fw, int keep) { return REC("boot keep=%d", keep); }
int init(uint64_t phys) { return REC("init %#llx", (unsigned long long)phys); }
int ping(uint32_t d) { return REC("ping %#x", d); }
int session(uint32_t d) { return REC("session %#x", d); }
int decode(uint32_t d, const char *in, const char *o, int w, uint64_t b, int h) { return REC("decode %s %s %d %d", in, o, w, h); }
int memtest(uint64_t a, uint64_t l, int w) { return REC("memtest %d", w); }
int shutdown_cmd(void) { return REC("shutdown"); }
int watch(unsigned s, int si, uint32_t d) { return REC("watch %u sysinit=%d debug=%#x", s, si, d); }
int map_sid(uint32_t s) { return REC("map_sid %#x", s); }
int smmu_cmd(void) { return REC("smmu"); }

int venusctl_main(int argc, char **argv);

static int run(const char *want, int want_rc, int argc, char **argv)
{
	called[0] = 0;
	verbose = cvp = force = 0;
	int p[2];
	if (pipe(p))
		return 1;
	pid_t pid = fork();
	if (!pid) {	/* main() may print usage to stderr; a child keeps globals and stdio clean */
		close(p[0]);
		freopen("/dev/null", "w", stderr);
		int rc = venusctl_main(argc, argv);
		dprintf(p[1], "%d %s", rc, called);
		_exit(0);
	}
	close(p[1]);
	char buf[200] = "";
	ssize_t n = read(p[0], buf, sizeof(buf) - 1);
	buf[n > 0 ? n : 0] = 0;
	close(p[0]);
	waitpid(pid, NULL, 0);
	char exp[200];
	snprintf(exp, sizeof(exp), "%d %s", want_rc, want);
	int ok = !strcmp(buf, exp);
	printf("%s: %-40s -> %s%s%s\n", ok ? "ok  " : "FAIL", argv[1] ? argv[1] : "", buf, ok ? "" : " want ", ok ? "" : exp);
	return !ok;
}
#define RUN(want, rc, ...) do { char *a[] = { "venusctl", __VA_ARGS__, NULL }; \
	fails += run(want, rc, (int)(sizeof(a) / sizeof(a[0])) - 1, a); } while (0)

int main(void)
{
	int fails = 0;
	RUN("rails", 0, "rails");
	RUN("", 2, "rails", "extra");
	RUN("probe", 0, "probe");
	RUN("power_on cvp=0", 0, "power", "on");
	RUN("power_on cvp=1", 0, "power", "on", "--cvp");
	RUN("power_off force=1", 0, "power", "off", "--force");
	RUN("watch 300 sysinit=1 debug=0x18", 0, "watch", "300", "--sysinit");
	RUN("watch 180 sysinit=0 debug=0x3f", 0, "watch", "180", "--debug", "0x3f");
	RUN("", 2, "watch");
	RUN("boot keep=1", 0, "boot", "--keep");
	RUN("init 0xa2600000", 0, "init", "--phys", "0xa2600000");
	RUN("", 2, "frobnicate");
	RUN("", 2, "power", "on", "x", "y");
	printf("args_test: %s\n", fails ? "FAILED" : "all passed");
	return fails != 0;
}
