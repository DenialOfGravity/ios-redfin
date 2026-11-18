/*
 * qseeprobe (ws-qsee-a, extended in ws-qsee-b): QSEE/QTEE queries over /dev/pmem PMEM_SMC (xnu 0094), for the fingerprint track
 * (PF/logs/research/fingerprint-2026-11-16.md, P2.8 "Revised first experiment"). No kernel change.
 *
 *   qseeprobe version            TZ_INFO_GET_FEATURE_VERSION(feature 10 = QSEE), as Linux qseecom_init_control.
 *                                Stock Android logs "qseecom.qsee_version = 0x1402000" (5.2.0).
 *   qseeprobe lookup [NAME...]   TZ_OS_APP_LOOKUP per name (default: keymaster64 cmnlib64 fpctzappfingerprint):
 *                                loaded or not, and the app ID. The name goes in a zeroed 4 KB PMEM_ALLOC page.
 *   qseeprobe load NAME DIR      ws-qsee-b: TZ_OS_APP_START of DIR/NAME.mdt + NAME.bNN in one contiguous buffer
 *   qseeprobe send-build-info ID ws-qsee-b: FPC g12/1 get_build_info to app ID (must be QSEE_APP's id)
 *   qseeprobe sensor-init ID     ws-qsee-b P4: FPC g10/0 then g10/5 (fpc_tee_sensor_init; QSEE_SENSOR_NO5=1: g10/0 only)
 *   qseeprobe sensor-cmd ID CMD  ws-qsee-b P4: one FPC g10/CMD (sensor_command), 0x58-byte buffer, 64-bit layout
 *   qseeprobe finger-detect ID [MS] ws-qsee-b P4: g10/2, wait for gpio112 high (kern.sweet_fpc_wait, 1190), g10/1
 *   qseeprobe fp-ids ID          P5: get_finger_ids + db id;  fp-delete ID FID: delete_template FID
 *   qseeprobe enrol-test ID [MS] P5: empty db, enrol, identify x5, delete (TA memory only, nothing stored)
 *   qseeprobe unload ID          ws-qsee-b: TZ_OS_APP_SHUTDOWN of app ID (must be QSEE_APP's id, never keymaster64)
 *   qseeprobe whitelist          the SMCs this tool may issue
 * A listener request (INCOMPLETE / BLOCKED_ON_LISTENER) is refused as Linux refuses a listener that does not exist,
 * its id is printed as "LISTENER REQUESTED", and the exit code is 4 (qsee_app.inc).
 * Environment:
 *   QSEE_DRYRUN=1      print every SMC, issue none (no /dev/pmem needed)
 *   QSEE_SHMBRIDGE=1   register every buffer handed to TZ (name page, image, sb, bulk, extra-arg page); lookup: the page with TZ_SHM_BRIDGE_CREATE first (VMID_HLOS, R|W, as Linux's
 *                      default bridge) and TZ_SHM_BRIDGE_DELETE it afterwards. Default: a plain PMEM_ALLOC page.
 *                      TZ_SHM_BRIDGE_ENABLE (one-way) is never issued.
 *   QSEE_APP=NAME      send/unload guard: the app the ID must belong to (default fpctzappfingerprint)
 *   QSEE_DATA_NULL=1   send-build-info smoke test: no data buffer, sb {0x418, 0}, empty sglistinfo
 *   QSEE_DATA64=1      send-build-info: data buffer anywhere, u64 pa at sb+4, sglistinfo 64-bit flag (0xc0000004)
 *   QSEE_SEND=wl|legacy  send-data form (default: as Linux, by TZ whitelist feature 15 >= 1.0.0)
 *   QSEE_CONSOLE=0     do not mirror the log lines to /dev/console (default: mirrored, so a hang leaves a klog line)
 *
 * Every SMC passes qsmc(): x0 and x1 must match an entry of qsee_whitelist[] (qsee_wl.c) or the tool exits before
 * the ioctl. No listener registration, services-image load, RPMB, key or bridge-enable calls exist in this tool.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../video/pmem.h"
#include "qsee_wl.h"

static int dryrun, shmbridge, con = -1;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int say_quiet;	/* finger-detect's loop: one line per round */
static void say(const char *fmt, ...)
{
	if (say_quiet)
		return;
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(line) - 2)
		n = (int)sizeof(line) - 2;
	line[n++] = '\n';
	fwrite(line, 1, (size_t)n, stdout);
	fflush(stdout);
	if (con >= 0 && (write(con, "qseeprobe: ", 11) < 0 || write(con, line, (size_t)n) < 0)) {
		/* console is a mirror only */
	}
}

/* the only path to PMEM_SMC; x[0..3] hold x0..x3 afterwards. Returns (int64_t)x0. */
static int64_t qsmc(uint64_t x[8])
{
	const struct qsee_wl_ent *e = qsee_allowed(x[0], x[1]);
	if (!e) {
		say("REFUSED smc x0 %#llx x1 %#llx: not on the whitelist", (unsigned long long)x[0],
		    (unsigned long long)x[1]);
		exit(3);
	}
	x[6] = x[7] = 0;	/* Linux passes x6 = 0 (scm.c __scm_call_armv8_64) */
	say("smc %s: x0 %#llx x1 %#llx x2 %#llx x3 %#llx x4 %#llx x5 %#llx%s", e->name, (unsigned long long)x[0],
	    (unsigned long long)x[1], (unsigned long long)x[2], (unsigned long long)x[3], (unsigned long long)x[4],
	    (unsigned long long)x[5], dryrun ? "  (dry run: not issued)" : "");
	if (dryrun) {
		x[0] = x[1] = x[2] = x[3] = 0;
		return 0;
	}
	uint64_t in[8];
	memcpy(in, x, sizeof(in));
	for (int tries = 0;; tries++) {	/* Linux __scm_call2: SCM_V2_EBUSY -> sleep 30 ms, retry (max 67) */
		memcpy(x, in, sizeof(in));
		pm_smc(x);	/* the kernel resumes SCM_INTERRUPTED itself (QCOM_A6 quirk) */
		if ((int64_t)x[0] != SCM_V2_EBUSY || tries >= 33)
			break;
		usleep(30000);
	}
	say("  -> x0 %#llx (%lld) x1 %#llx x2 %#llx x3 %#llx", (unsigned long long)x[0], (long long)x[0],
	    (unsigned long long)x[1], (unsigned long long)x[2], (unsigned long long)x[3]);
	return (int64_t)x[0];
}

static int cmd_version(void)
{
	uint64_t x[8] = { QSEE_SMC_FEAT_VERSION, QSEE_ARGS_FEAT_VERSION, QSEE_FEATURE_QSEE };
	int64_t rc = qsmc(x);
	if (rc) {
		say("version: smc failed (x0 %lld)", (long long)rc);
		return 1;
	}
	uint32_t v = (uint32_t)x[1];
	say("version: qsee_version %#x = %u.%u.%u (stock Android: 0x1402000)%s", v, v >> 22, (v >> 12) & 0x3ff,
	    v & 0xfff, dryrun ? "  (dry run)" : "");
	uint64_t y[8] = { QSEE_SMC_FEAT_VERSION, QSEE_ARGS_FEAT_VERSION, QSEE_FEATURE_WHITELIST };
	if (!qsmc(y))
		say("version: whitelist feature (15) %#x -> %s", (unsigned)y[1],
		    (uint32_t)y[1] >= QSEE_WHITELIST_V1 ? "Linux would use SEND_DATA_WITH_WHITELIST" : "plain SEND_DATA");
	return 0;
}

static uint64_t buf_pa;
static uint32_t lookup_app_id;	/* set by lookup_one() on LOADED */
static uint64_t bridge_handle;
static int bridged;

static int bridge_create(void)
{
	uint64_t x[8] = { QSEE_SMC_SHMB_CREATE, QSEE_ARGS_SHMB_CREATE,
			  buf_pa | SHMB_PERM_RW,				/* pfn_and_ns_perm_flags: pa | ns_perms */
			  buf_pa | SHMB_PERM_RW,				/* ipfn_and_s_perm_flags: pa | tz_perm */
			  4096 | (1ull << SHMB_NUM_VM_SHIFT),		/* size_and_flags: size | ns_vmid_num << 9 */
			  VMID_HLOS };					/* ns_vmids (args[3], x5) */
	int64_t rc = qsmc(x);
	if (rc || x[1]) {
		say("shmbridge: create failed (x0 %lld, ret0 %#llx%s)", (long long)rc, (unsigned long long)x[1],
		    x[1] == 4 ? ": SHMBRIDGE_E_NOT_SUPPORTED" : "");
		return -1;
	}
	bridge_handle = x[2];
	bridged = 1;
	say("shmbridge: page %#llx registered, handle %#llx", (unsigned long long)buf_pa,
	    (unsigned long long)bridge_handle);
	return 0;
}

static int bridge_delete(void)
{
	uint64_t x[8] = { QSEE_SMC_SHMB_DELETE, QSEE_ARGS_SHMB_DELETE, bridge_handle };
	int64_t rc = qsmc(x);
	if (rc || x[1]) {
		say("shmbridge: delete of handle %#llx failed (x0 %lld, ret0 %#llx)", (unsigned long long)bridge_handle,
		    (long long)rc, (unsigned long long)x[1]);
		return -1;
	}
	bridged = 0;
	say("shmbridge: handle %#llx deleted", (unsigned long long)bridge_handle);
	return 0;
}

/* 0 = loaded, 1 = not loaded, 2 = error, 3 = TZ wants something we won't do (stop) */
static int lookup_one(const char *name)
{
	size_t len = strlen(name);
	if (!len || len >= QSEE_APP_NAME_MAX) {
		say("lookup %s: name length %zu not in 1..%d", name, len, QSEE_APP_NAME_MAX - 1);
		return 2;
	}
	char page[QSEE_APP_NAME_MAX] = { 0 };
	memcpy(page, name, len);
	if (!dryrun && pm_wr(buf_pa, page, sizeof(page)))	/* kernel cleans the cache line to PoC */
		return 2;
	uint64_t x[8] = { QSEE_SMC_APP_LOOKUP, QSEE_ARGS_APP_LOOKUP, buf_pa, len };
	int64_t rc = qsmc(x);
	uint32_t result = (uint32_t)x[1], type = (uint32_t)x[2], data = (uint32_t)x[3];
	if (dryrun) {
		say("lookup %s: len %zu (dry run)", name, len);
		return 0;
	}
	if (rc) {
		say("lookup %s: smc failed (x0 %lld)", name, (long long)rc);
		return 2;
	}
	if (result == QSEOS_RESULT_FAILURE) {
		say("lookup %s: NOT loaded", name);
		return 1;
	}
	if (result == QSEOS_RESULT_INCOMPLETE || result == QSEOS_RESULT_BLOCKED) {
		say("lookup %s: result %u (%s): TZ wants a listener/continue call, not issued; stopping", name, result,
		    result == 1 ? "INCOMPLETE" : "BLOCKED_ON_LISTENER");
		return 3;
	}
	if (result == QSEOS_RESULT_SUCCESS && type == QSEOS_APP_ID) {
		say("lookup %s: LOADED, app id %u", name, data);
		lookup_app_id = data;
		return 0;
	}
	say("lookup %s: unexpected result %#x resp_type %#x data %#x", name, result, type, data);
	return 2;
}

static int cmd_lookup(int n, char **names)
{
	static char *def[] = { "keymaster64", "cmnlib64", "fpctzappfingerprint" };
	if (!n)
		n = 3, names = def;
	buf_pa = dryrun ? 0xdead0000ull : pm_alloc(4096, 4096);	/* zeroed, contiguous */
	if (!buf_pa)
		return 2;
	say("lookup: name page %#llx (%s)%s", (unsigned long long)buf_pa,
	    shmbridge ? "QSEE_SHMBRIDGE=1: registered first" : "plain PMEM_ALLOC, not bridged",
	    dryrun ? "  (dry-run address)" : "");
	int worst = 0;
	if (shmbridge && bridge_create()) {
		worst = 2;
		goto out;
	}
	for (int i = 0; i < n; i++) {
		int r = lookup_one(names[i]);
		if (r > worst)
			worst = r;
		if (r == 3)
			break;
	}
	if (bridged && bridge_delete()) {
		/* TZ may still think the page is shared: leak it rather than hand it back to XNU */
		say("lookup: page %#llx NOT freed (still bridged)", (unsigned long long)buf_pa);
		return 2;
	}
out:
	if (!dryrun && !bridged)
		pm_free(buf_pa);
	return worst == 1 ? 0 : worst;	/* "not loaded" is an answer, not an error */
}

#include "qsee_app.inc"
#include "qsee_fpc.inc"

int main(int argc, char **argv)
{
	const char *e;
	dryrun = (e = getenv("QSEE_DRYRUN")) && atoi(e);
	shmbridge = (e = getenv("QSEE_SHMBRIDGE")) && atoi(e);
	if (!(e = getenv("QSEE_CONSOLE")) || atoi(e))
		con = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (argc < 2) {
		fprintf(stderr, "usage: qseeprobe version | lookup [NAME...] | load NAME DIR | send-build-info APPID |\n"
				"       unload APPID | sensor-init APPID | sensor-cmd APPID CMD | finger-detect APPID [MS] |\n"
				"       fp-ids APPID | fp-delete APPID FID | enrol-test APPID [MS] | whitelist\n"
				"  env QSEE_DRYRUN=1 (print SMCs only), QSEE_SHMBRIDGE=1 (bridge every buffer),\n"
				"      QSEE_APP=NAME (send/unload guard), QSEE_SEND=wl|legacy\n");
		return 1;
	}
	if (!strcmp(argv[1], "whitelist")) {
		for (unsigned i = 0; i < qsee_whitelist_n; i++)
			printf("%#010llx arginfo %#04llx %s\n", (unsigned long long)qsee_whitelist[i].x0,
			       (unsigned long long)qsee_whitelist[i].arginfo, qsee_whitelist[i].name);
		return 0;
	}
	if (!dryrun)
		pm_init();
	say("start: %s%s%s", argv[1], dryrun ? " QSEE_DRYRUN=1" : "", shmbridge ? " QSEE_SHMBRIDGE=1" : "");
	if (!strcmp(argv[1], "version"))
		return cmd_version();
	if (!strcmp(argv[1], "lookup"))
		return cmd_lookup(argc - 2, argv + 2);
	if (!strcmp(argv[1], "load") && argc == 4)
		return cmd_load(argv[2], argv[3]);
	if (!strcmp(argv[1], "send-build-info") && argc == 3)
		return cmd_send_build_info((uint32_t)strtoul(argv[2], NULL, 0));
	if (!strcmp(argv[1], "sensor-init") && argc == 3)
		return cmd_sensor((uint32_t)strtoul(argv[2], NULL, 0), "sensor-init", 0);
	if (!strcmp(argv[1], "sensor-cmd") && argc == 4)
		return cmd_sensor((uint32_t)strtoul(argv[2], NULL, 0), "sensor-cmd", (uint32_t)strtoul(argv[3], NULL, 0));
	if (!strcmp(argv[1], "finger-detect") && (argc == 3 || argc == 4))
		return cmd_finger_detect((uint32_t)strtoul(argv[2], NULL, 0), argc == 4 ? atoi(argv[3]) : 60000);
	if (!strcmp(argv[1], "fp-ids") && argc == 3)
		return cmd_fp_ids((uint32_t)strtoul(argv[2], NULL, 0));
	if (!strcmp(argv[1], "fp-delete") && argc == 4)
		return cmd_fp_delete((uint32_t)strtoul(argv[2], NULL, 0), (uint32_t)strtoul(argv[3], NULL, 0));
	if (!strcmp(argv[1], "enrol-test") && (argc == 3 || argc == 4))
		return cmd_enrol_test((uint32_t)strtoul(argv[2], NULL, 0), argc == 4 ? atoi(argv[3]) : 180000);
	if (!strcmp(argv[1], "unload") && argc == 3)
		return cmd_unload((uint32_t)strtoul(argv[2], NULL, 0));
	fprintf(stderr, "qseeprobe: unknown command or arguments: %s\n", argv[1]);
	return 1;
}
