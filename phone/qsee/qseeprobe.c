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
 * ws-fpcd-a: the QSEE/TA code is in qsee_lib.c (shared with sweetfpcd); every command that issues SMCs takes the
 * QSEE lock first (qsee_lock(), /var/local/fpc/qsee.lock; QSEE_NOLOCK=1 skips it), so it refuses to run beside
 * sweetfpcd. Exit code 9 = lock held.
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
#include <sys/sysctl.h>
#include <time.h>
#include "qsee_wl.h"
#include "qsee_lib.h"

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




/*
 * FPC g12/1 get_build_info, laid out as the HAL (hal.dis fpc_tee_get_build_info @0xd59c, fpc_tac_transfer_platform
 * @0xfd1c): a 0x418-byte bulk buffer {u32 group 12, u32 id 1, u32 ta_status (zeroed), u32 result, u32 0,
 * u32 0x400 (size), char build[0x400]}; the 128-byte sb holds {u32 0x418, u32 pa(bulk)} (the pa patched at
 * cmd_buf_offset 4, 32-bit: QSEECom_send_modified_cmd), req = sb[0,0x40), resp = sb[0x40,0x80).
 */
static int cmd_send_build_info(uint32_t appid)
{
	if (check_appid(appid))
		return 2;
	/* QSEE_DATA_NULL=1 (smoke test): no bulk buffer; the sb carries {0x418, pa 0} and the sglistinfo table is empty,
	 * as Linux sends a command without modfd buffers; expect a TA error status, proves the send path only */
	const char *dn = getenv("QSEE_DATA_NULL");
	int datanull = dn && atoi(dn);
	/* QSEE_DATA64=1: bulk buffer in normal PMEM_ALLOC (may be above 4 GB), its full pa as a u64 at sb+4, sglistinfo
	 * {SGLIST_SET(1, 1, 4) = 0xc0000004, sizeOrCount = buffer length 0x1000} (qseecom.c 4327-4332: 64-bit flag,
	 * sizeOrCount = sg->length, the page-rounded ION allocation, for nents == 1) */
	const char *d64 = getenv("QSEE_DATA64");
	int data64 = d64 && atoi(d64) && !datanull;
	int wl = 1;
	const char *e = getenv("QSEE_SEND");
	if (e && !strcmp(e, "legacy"))
		wl = 0;
	else if (!(e && !strcmp(e, "wl")) && !dryrun) {
		uint64_t x[8] = { QSEE_SMC_FEAT_VERSION, QSEE_ARGS_FEAT_VERSION, QSEE_FEATURE_WHITELIST };
		wl = !qsmc(x) && (uint32_t)x[1] >= QSEE_WHITELIST_V1;
		say("send: whitelist feature version %#x -> %s", (unsigned)x[1], wl ? "SEND_DATA_WITH_WHITELIST" : "SEND_DATA");
	}
	struct pbuf sb, bulk, arg;
	if (pbuf_get(&sb, 4096, 4096, 0, 0xa0000000ull))
		return 2;
	if (datanull)
		memset(&bulk, 0, sizeof(bulk));
	else if (pbuf_get(&bulk, 4096, 4096, !data64, 0xa0001000ull)) {
		pbuf_put(&sb);
		return 2;
	}
	if (pbuf_get(&arg, 4096, 4096, 0, 0xa0002000ull)) {
		pbuf_put(&bulk);
		pbuf_put(&sb);
		return 2;
	}
	if (!data64 && bulk.pa >> 32) {
		say("send: bulk pa %#llx above 4 GB (the modfd patch is 32-bit)", (unsigned long long)bulk.pa);
		pbuf_put(&arg), pbuf_put(&bulk), pbuf_put(&sb);
		return 2;
	}
	uint8_t bb[0x418] = { 0 }, s[0x80] = { 0 }, a[0x100] = { 0 };
	put_u32(bb + 0, 12), put_u32(bb + 4, 1), put_u32(bb + 0x14, 0x400);
	put_u32(s + 0, sizeof(bb));
	if (data64)
		put_u64(s + 4, bulk.pa);
	else
		put_u32(s + 4, (uint32_t)bulk.pa);
	uint64_t req = sb.pa, rsp = sb.pa + 0x40, sgl = arg.pa + 0x80;
	put_u64(a + 0, rsp), put_u64(a + 8, 0x40);	/* args[3], args[4] */
	if (wl) {
		put_u64(a + 16, sgl), put_u64(a + 24, 8 * MAX_ION_FD);	/* args[5], args[6] */
		if (!datanull)	/* one contiguous buf; with QSEE_DATA_NULL the table stays all zero */
			put_u32(a + 0x80, SGLIST_SET(1, data64, 4)), put_u32(a + 0x84, (uint32_t)bulk.size);
	}
	if ((!datanull && wr_chunks(bulk.pa, bb, sizeof(bb))) || wr_chunks(sb.pa, s, sizeof(s)) ||
	    wr_chunks(arg.pa, a, sizeof(a))) {
		pbuf_put(&arg), pbuf_put(&bulk), pbuf_put(&sb);
		return 2;
	}
	say("send: app %u g12/1 get_build_info: sb %#llx (req 0x40, rsp +0x40 len 0x40), bulk %#llx len 0x418, "
	    "extra args %#llx%s", appid, (unsigned long long)sb.pa, (unsigned long long)bulk.pa, (unsigned long long)arg.pa,
	    !wl ? "" : datanull ? ", sglistinfo at +0x80 empty (QSEE_DATA_NULL=1: data pa 0)" :
	    data64 ? ", QSEE_DATA64=1: u64 pa at sb+4, sglistinfo at +0x80 {0xc0000004, 0x1000}" :
	    ", sglistinfo at +0x80 {0x80000004, 0x1000}");
	uint64_t x[8] = { wl ? QSEE_SMC_SEND_DATA_WL : QSEE_SMC_SEND_DATA, wl ? QSEE_ARGS_SEND_DATA_WL : QSEE_ARGS_SEND_DATA,
			  appid, req, 0x40, arg.pa };
	qsmc(x);
	say("send: raw x0 %#llx x1 %#llx x2 %#llx x3 %#llx", (unsigned long long)x[0], (unsigned long long)x[1],
	    (unsigned long long)x[2], (unsigned long long)x[3]);
	abort_listeners(x, appid, "send");
	int rc = 2;
	uint32_t result = (uint32_t)x[1];
	if ((int64_t)x[0]) {
		say("send: smc failed (x0 %lld)", (long long)x[0]);
	} else if (result != QSEOS_RESULT_SUCCESS) {
		say("send: result %#x resp_type %#llx data %#llx", result, (unsigned long long)x[2], (unsigned long long)x[3]);
		rc = n_seen ? 4 : 2;
	} else if (!dryrun && datanull && !pm_rd(sb.pa, s, sizeof(s))) {
		say("send: SUCCESS (TZ). QSEE_DATA_NULL: resp[0] (TA status, HAL requires 0) %#x (%d)", rd_u32(s + 0x40),
		    (int)rd_u32(s + 0x40));
		for (int o = 0; o < 0x80; o += 16)
			say("send: sb+%#04x: %08x %08x %08x %08x%s", o, rd_u32(s + o), rd_u32(s + o + 4), rd_u32(s + o + 8),
			    rd_u32(s + o + 12), o == 0x40 ? "  <- resp" : "");
		rc = rd_u32(s + 0x40) ? 5 : 0;
	} else if (!dryrun && !pm_rd(sb.pa, s, sizeof(s)) && !pm_rd(bulk.pa, bb, sizeof(bb))) {
		char build[0x401];
		memcpy(build, bb + 0x18, 0x400);
		build[0x400] = 0;
		for (char *c = build; *c; c++)
			if ((unsigned char)*c < 0x20 && *c != '\n')
				*c = '.';
		say("send: SUCCESS. sb resp[0] (QSEECom-level status) %#x; bulk: group %u id %u ta_status %#x result %#x "
		    "size %#x", rd_u32(s + 0x40), rd_u32(bb), rd_u32(bb + 4), rd_u32(bb + 8), rd_u32(bb + 0xc),
		    rd_u32(bb + 0x14));
		say("send: build info: \"%.400s\"", build);
		for (int o = 0; o < 0x80; o += 16)
			say("send: sb+%#04x: %08x %08x %08x %08x%s", o, rd_u32(s + o), rd_u32(s + o + 4), rd_u32(s + o + 8),
			    rd_u32(s + o + 12), o == 0x40 ? "  <- resp" : "");
		for (int o = 0; o < 0x30; o += 16)
			say("send: data+%#04x: %08x %08x %08x %08x", o, rd_u32(bb + o), rd_u32(bb + o + 4), rd_u32(bb + o + 8),
			    rd_u32(bb + o + 12));
		rc = rd_u32(s + 0x40) || rd_u32(bb + 8) || rd_u32(bb + 0xc) ? 5 : 0;
	} else if (dryrun) {
		rc = 0;
	}
	print_seen();
	pbuf_put(&arg), pbuf_put(&bulk), pbuf_put(&sb);
	return rc;
}

static int cmd_unload(uint32_t appid)
{
	if (check_appid(appid))
		return 2;
	uint64_t x[8] = { QSEE_SMC_APP_SHUTDOWN, QSEE_ARGS_APP_SHUTDOWN, appid };
	qsmc(x);
	say("unload: raw x0 %#llx x1 %#llx x2 %#llx x3 %#llx", (unsigned long long)x[0], (unsigned long long)x[1],
	    (unsigned long long)x[2], (unsigned long long)x[3]);
	abort_listeners(x, appid, "unload");
	print_seen();
	if ((int64_t)x[0] || (uint32_t)x[1] != QSEOS_RESULT_SUCCESS) {
		say("unload %u: FAILED (x0 %lld result %#x)", appid, (long long)x[0], (unsigned)x[1]);
		return n_seen ? 4 : 2;
	}
	say("unload %u: SUCCESS", appid);
	return 0;
}

static int cmd_sensor(uint32_t appid, const char *which, uint32_t cmd)
{
	if (check_appid(appid))
		return 2;
	uint8_t d[FPC_SENSOR_LEN] = { 0 };
	int32_t st = 0;
	int rc;
	fpc_gpio_state("before");
	if (!strcmp(which, "sensor-init")) {
		/* fpc_tee_sensor_init: cmd 0, then (after fpc_irq_init) cmd 5; QSEE_SENSOR_NO5=1 stops after cmd 0 */
		const char *e = getenv("QSEE_SENSOR_NO5");
		rc = fpc_sensor_cmd(appid, d, 0, &st);
		fpc_dump("g10/0", d, FPC_SENSOR_LEN);
		fpc_gpio_state("after g10/0");
		if (!rc && !(e && atoi(e))) {
			rc = fpc_sensor_cmd(appid, d, 5, &st);
			fpc_gpio_state("after g10/5");
		}
	} else {
		rc = fpc_sensor_cmd(appid, d, cmd, &st);
		fpc_dump("g10", d, FPC_SENSOR_LEN);
		fpc_gpio_state("after");
	}
	say("%s %u: %s (rc %d, TA status %d)", which, appid, rc ? "FAILED" : "OK", rc, st);
	print_seen();
	return rc;
}

/*
 * finger-detect APPID [TIMEOUT_MS]: the HAL's capture loop (fpc_tee_capture_image @0xebac with wait_for_finger, called
 * from do_enroll @0xae50 / authenticate @0xa630 until it gives a usable image):
 *   +0xc = 1; cmd 2 (arm); fpc_irq_wait(gpio112 high); cmd 1 (status NOT checked by the HAL);
 *   up to 4x: cmd 3 (arm capture); fpc_irq_wait; cmd 4 -> status 3 = retry capture, else done;
 *   cmd 4 status (capture_image's return, do_enroll's jump table @0x5304): 0 = good image (finger qualified),
 *   1 and 4 = acquired TOO_FAST (5), 3 = INSUFFICIENT (2), 2 = logged as "capture_status" error; on any non-zero
 *   result capture_image sends cmd 5 and the caller starts over at cmd 2. Negative = FPC_ERROR_* (fpc_error_str).
 * Loops until cmd 4 gives 0 (exit 0), the overall timeout (default 60000 ms, exit 6), a TA/TZ error (exit 5/3/4)
 * or QSEE_FD_MAXITER rounds (default 300). QSEE_FD_NOSTOP=1 keeps going after status 0. One line per round: wake time
 * after arming, cmd 1 status + data words +8..+0x14, cmd 4 status / tries / words, irqs since start.
 * hal.dis: sensor_command's callers are only sensor_init (0, 5), sensor_release (5) and capture_image (2, 1, 3, 4, 5);
 * cmd 1's return is dropped (bl at 0xec4c, w0 unused), and only cmd 4's +0x10 is kept (ctx+0x48, never read again).
 */
static int cmd_finger_detect(uint32_t appid, int ms)
{
	if (check_appid(appid))
		return 2;
	const char *e = getenv("QSEE_FD_MAXITER");
	int maxiter = e && atoi(e) > 0 ? atoi(e) : 300;
	const char *ns = getenv("QSEE_FD_NOSTOP");	/* 1: keep looping after a good image (compare finger on/off) */
	int nostop = ns && atoi(ns);
	uint8_t d[FPC_SENSOR_LEN] = { 0 };
	int32_t st = 0;
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	fpc_gpio_state("before");
	uint32_t irq_start = fpc_sysctl_u32("kern.sweet_fpc_irq");
	say("finger-detect: HAL capture loop, up to %d ms / %d rounds; touch the sensor now", ms, maxiter);
	int rc = 6, round;
	for (round = 1; round <= maxiter; round++) {
		long t_round = fpc_ms_since(&t0);
		if (t_round >= ms)
			break;
		d[0xc] = 1;
		int r = fd_cmd(appid, d, 2, &st);
		if (r) {
			say("fd %d: arm (g10/2) failed rc %d status %d", round, r, st);
			rc = r;
			break;
		}
		int hit = fpc_wait_high(ms - (int)fpc_ms_since(&t0));
		long t_wake = fpc_ms_since(&t0) - t_round;
		if (hit < 0) {
			rc = 2;
			break;
		}
		if (!hit) {
			say("fd %d: armed, no irq before the timeout", round);
			break;
		}
		int32_t st1 = 0;
		r = fd_cmd(appid, d, 1, &st1);	/* the HAL ignores this status and never reads its data */
		if (r && r != 5) {
			rc = r;
			break;
		}
		uint32_t w1[4] = { rd_u32(d + 8), rd_u32(d + 0xc), rd_u32(d + 0x10), rd_u32(d + 0x14) };
		int32_t st4 = 0;
		int tries = 0, timeout = 0;
		for (tries = 1; tries <= 4; tries++) {
			r = fd_cmd(appid, d, 3, &st);
			if (r) {
				say("fd %d: capture arm (g10/3) failed rc %d status %d", round, r, st);
				break;
			}
			hit = fpc_wait_high(ms - (int)fpc_ms_since(&t0));
			if (hit <= 0) {
				timeout = 1;
				break;
			}
			r = fd_cmd(appid, d, 4, &st4);
			if (st4 != 3)
				break;
		}
		say("fd %d: wake %ld ms | cmd1 %d [+8..+0x14 %x %x %x %x] | cmd4 %d after %d tr%s [%x %x %x %x] | irqs %u",
		    round, t_wake, st1, w1[0], w1[1], w1[2], w1[3], st4, tries > 4 ? 4 : tries, timeout ? " TIMEOUT" : "",
		    rd_u32(d + 8), rd_u32(d + 0xc), rd_u32(d + 0x10), rd_u32(d + 0x14), fpc_sysctl_u32("kern.sweet_fpc_irq") - irq_start);
		if (timeout)
			break;
		if (r && r != 5) {
			rc = r;
			break;
		}
		if (st4 == 0) {
			rc = 0;
			if (!nostop)
				break;
		}
		if (st4 < 0) {
			say("fd %d: cmd 4 FPC error %d", round, st4);
			rc = 5;
			break;
		}
		int32_t st5 = 0;
		fd_cmd(appid, d, 5, &st5);	/* capture_image's error path */
	}
	fpc_dump("last", d, 0x20);
	fpc_gpio_state("after");
	say("finger-detect %u: %s after %d round(s), %ld ms, irqs %u", appid,
	    rc == 0 ? "FINGER QUALIFIED (cmd 4 status 0, good image)" : rc == 6 ? "TIMEOUT / no qualified finger" : "ERROR",
	    round > maxiter ? maxiter : round, fpc_ms_since(&t0), fpc_sysctl_u32("kern.sweet_fpc_irq") - irq_start);
	print_seen();
	return rc;
}

static int cmd_fp_ids(uint32_t appid)
{
	if (check_appid(appid))
		return 2;
	uint32_t n;
	return fpc_print_ids(appid, "fp-ids", &n);
}

static int cmd_fp_delete(uint32_t appid, uint32_t fid)
{
	if (check_appid(appid))
		return 2;
	int r = fpc_delete(appid, fid);
	uint32_t n;
	fpc_print_ids(appid, "fp-delete after", &n);
	return r;
}

/*
 * enrol-test APPID [MS]: load_empty_db, set_gid 0, ids; begin_enrol; capture+enrol until done; end_enrol; ids;
 * up to 5 identify captures; delete the new finger; ids must be empty. The DB stays in TA memory (no store).
 */
static int cmd_enrol_test(uint32_t appid, int ms)
{
	if (check_appid(appid))
		return 2;
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	uint8_t d[FPC_SENSOR_LEN] = { 0 };
	int32_t st;
	uint32_t n;
	int rc = 0;
	uint32_t fid = 0;
	int have_fid = 0;
	fpc_gpio_state("start");
	if (fpc_bio(appid, d, 6, &st))
		return 3;
	say("1 load_empty_db: status %d", st);
	memset(d, 0, sizeof(d));
	put_u32(d + 0xc, 0);
	if (fpc_bio(appid, d, 9, &st))
		return 3;
	say("2 set_gid 0: status %d", st);
	fpc_print_ids(appid, "3 before", &n);
	memset(d, 0, sizeof(d));
	if (fpc_bio(appid, d, 0, &st))
		return 3;
	say("4 begin_enrol (no authorize_enrol / HAT sent): status %d%s", st,
	    st ? "  -> REFUSED: stopping (the TA may want authorize_enrol / init_hw_auth)" : "");
	if (st)
		return 7;
	say("5 enrol: touch the sensor, LIFT fully after each 'sample accepted', touch again (slightly moved)");
	int sample = 0, attempts = 0, done = 0, quiet_caps = 0;
	while (fpc_ms_since(&t0) < ms) {
		int32_t st4 = 0;
		long wake = 0;
		attempts++;
		int r = fpc_capture(appid, &t0, ms, &st4, &wake);
		if (r == 6)
			break;
		if (r) {
			say("5 capture %d: rc %d", attempts, r);
			rc = r;
			break;
		}
		if (st4) {
			if (st4 == 1 || st4 == 4) {	/* too fast / no finger: the idle loop, counted, not printed */
				quiet_caps++;
				continue;
			}
			say("5 capture %d: wake %ld ms, image status %d (%s): touch again", attempts, wake, st4,
			    st4 == 3 ? "insufficient" : "error");
			continue;
		}
		memset(d, 0, sizeof(d));
		if (fpc_bio(appid, d, 1, &st)) {
			rc = 3;
			break;
		}
		uint32_t rem = rd_u32(d + 0xc);
		if (st == 1 || st == 5 || st == 0)
			sample++;
		say("5 capture %d (+%d too-fast/no-finger): wake %ld ms, enrol status %d (%s), samples remaining %u, accepted %d%s",
		    attempts, quiet_caps, wake, st, enrol_code(st), rem, sample, st == 0 ? "" : st == 1 || st == 5 ? "  -> LIFT, then touch again" : "");
		if (st == 0) {
			done = 1;
			break;
		}
		if (st == 2 || st == 3 || st < 0) {
			rc = 5;
			break;
		}
		usleep(300000);
	}
	if (!done) {
		say("5 enrol NOT finished (%s) after %d captures (%d too-fast/no-finger), %d accepted", rc ? "error" : "timeout",
		    attempts, quiet_caps, sample);
		fpc_cleanup_all(appid, "cleanup");
		return rc ? rc : 6;
	}
	memset(d, 0, sizeof(d));
	if (fpc_bio(appid, d, 2, &st))
		return 3;
	fid = rd_u32(d + 0xc);
	have_fid = !st;
	say("6 end_enrol: status %d, finger id %#x", st, fid);
	uint32_t ids7[5] = { 0 }, n7 = 0;
	int32_t st7;
	fpc_print_ids(appid, "7 after enrol", &n);
	fpc_get_ids(appid, ids7, &n7, &st7);
	if (!have_fid) {
		/* end_enrol failed (e.g. -1 = HMAC auth key not initialized / no HAT). If the TA still listed a
		 * finger, carry on with its id so identify + delete are still exercised; else clean up and stop. */
		if (!st7 && n7 > 0) {
			fid = ids7[0];
			have_fid = 1;
			say("6b end_enrol failed but get_finger_ids shows %u finger(s); using id %#x for identify/delete", n7, fid);
		} else {
			say("6b end_enrol failed and no finger stored; cleaning up and stopping");
			fpc_cleanup_all(appid, "cleanup");
			return 5;
		}
	}
	say("8 identify: touch with the SAME finger up to 5 times (lift between)");
	int matches = 0, tries = 0;
	while (tries < 5 && fpc_ms_since(&t0) < ms) {
		int32_t st4 = 0;
		long wake = 0;
		int r = fpc_capture(appid, &t0, ms, &st4, &wake);
		if (r)
			break;
		if (st4) {
			if (st4 != 1 && st4 != 4)
				say("8 capture: image status %d: touch again", st4);
			continue;
		}
		tries++;
		memset(d, 0, sizeof(d));
		if (fpc_bio(appid, d, 3, &st))
			break;
		uint32_t id = rd_u32(d + 0xc);
		int m = st == 0 && id == fid;
		matches += m;
		say("8 identify %d: %s status %d id %#x score %d coverage %d quality %d result %d", tries,
		    m ? "MATCH" : st == 0 ? "match? (id differs)" : "no match", st, id, (int)rd_u32(d + 0x20),
		    (int)rd_u32(d + 0x10), (int)rd_u32(d + 0x14), (int)rd_u32(d + 0x1c));
		usleep(300000);
	}
	say("8 identify: %d match(es) of %d", matches, tries);
	int clean = fpc_cleanup_all(appid, "9 after delete");	/* delete every id the TA lists, not just fid */
	int ok = clean == 0;
	say("enrol-test %u: enrolled %#x with %d samples, identify %d/%d, cleanup %s (%ld ms)", appid, fid, sample, matches,
	    tries, ok ? "OK, DB empty" : "FAILED", fpc_ms_since(&t0));
	print_seen();
	return !ok ? 5 : matches ? 0 : 8;
}

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
	if (!dryrun) {
		pm_init();
		if (qsee_lock("qseeprobe"))
			return 9;
	}
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
