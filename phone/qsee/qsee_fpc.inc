/*
 * ws-qsee-b P4: FPC sensor commands (group 10), 64-bit layout as P3.5 (QSEE_DATA64): the data buffer in normal
 * PMEM_ALLOC, its u64 pa at sb+4, sglistinfo {0xc0000004, 0x1000}, SEND_DATA_WITH_WHITELIST. Included by qseeprobe.c
 * after qsee_app.inc.
 *
 * From the HAL (hal.dis): fpc_tee_init allocates one 0x58-byte shared buffer (fpc_tac_alloc_shared @0xd204) that every
 * sensor_command (@0xea84) reuses: it writes u32 group 10 at +0 and u32 cmd at +4, sends it (fpc_tac_transfer), and
 * reads the TA status at +8; status -12 is retried after 10 ms, then after 20 ms, then the call gives -9. Other fields
 * written by the HAL: u8 wait_for_finger at +0xc (fpc_tee_capture_image @0xec08), result u32 at +0x10 (read back after
 * cmd 4). Commands (who calls them):
 *   0  fpc_tee_sensor_init, first           5  sensor_init (after fpc_irq_init), sensor_release, capture error path
 *   2  capture_image with wait_for_finger: arm finger detect, then fpc_irq_wait(level 1)
 *   1  right after that wait (finger present)
 *   3  arm capture, then fpc_irq_wait(1)     4  read the capture; returns 3 = retry (loop)
 * fpc_irq_wait reads the sysfs "irq" attribute (the gpio112 level) and polls it until it is high: here
 * kern.sweet_fpc_wait (1190).
 */
#include <sys/sysctl.h>
#include <time.h>

#define FPC_SENSOR_LEN 0x58
#define FPC_TA_BUSY (-12)

/* one command in a 64-bit-layout data buffer; *data (len bytes) is sent and read back; 0 = TZ SUCCESS (TA status in data+8) */
static int fpc_send64(uint32_t appid, uint8_t *data, size_t len, const char *what)
{
	struct pbuf sb, bulk, arg;
	if (pbuf_get(&sb, 4096, 4096, 0, 0xa0000000ull))
		return 2;
	if (pbuf_get(&bulk, 4096, 4096, 0, 0xa0001000ull)) {
		pbuf_put(&sb);
		return 2;
	}
	if (pbuf_get(&arg, 4096, 4096, 0, 0xa0002000ull)) {
		pbuf_put(&bulk), pbuf_put(&sb);
		return 2;
	}
	uint8_t s[0x80] = { 0 }, a[0x100] = { 0 };
	put_u32(s + 0, (uint32_t)len);
	put_u64(s + 4, bulk.pa);
	put_u64(a + 0, sb.pa + 0x40), put_u64(a + 8, 0x40);
	put_u64(a + 16, arg.pa + 0x80), put_u64(a + 24, 8 * MAX_ION_FD);
	put_u32(a + 0x80, SGLIST_SET(1, 1, 4)), put_u32(a + 0x84, (uint32_t)bulk.size);
	int rc = 2;
	if (wr_chunks(bulk.pa, data, len) || wr_chunks(sb.pa, s, sizeof(s)) || wr_chunks(arg.pa, a, sizeof(a)))
		goto out;
	uint64_t x[8] = { QSEE_SMC_SEND_DATA_WL, QSEE_ARGS_SEND_DATA_WL, appid, sb.pa, 0x40, arg.pa };
	qsmc(x);
	say("%s: send raw x0 %#llx x1 %#llx x2 %#llx x3 %#llx", what, (unsigned long long)x[0], (unsigned long long)x[1],
	    (unsigned long long)x[2], (unsigned long long)x[3]);
	unsigned before = n_seen;
	abort_listeners(x, appid, what);
	if (n_seen != before) {
		rc = 4;
		goto out;
	}
	if ((int64_t)x[0] || (uint32_t)x[1] != QSEOS_RESULT_SUCCESS) {
		say("%s: TZ error (x0 %lld result %#x)", what, (long long)x[0], (unsigned)x[1]);
		rc = 3;
		goto out;
	}
	if (dryrun) {
		rc = 0;
		goto out;
	}
	if (pm_rd(sb.pa, s, sizeof(s)) || pm_rd(bulk.pa, data, len))
		goto out;
	if (rd_u32(s + 0x40))
		say("%s: QSEECom-level resp[0] %#x (HAL requires 0)", what, rd_u32(s + 0x40));
	rc = 0;
out:
	if (!dryrun && bulk.pa) {	/* no key blob / HAT left in freed PMEM */
		static const uint8_t zero[4096];
		wr_chunks(bulk.pa, zero, sizeof(zero));
	}
	pbuf_put(&arg), pbuf_put(&bulk), pbuf_put(&sb);
	return rc;
}

static void fpc_dump(const char *what, const uint8_t *d, size_t len)
{
	for (size_t o = 0; o < len; o += 16)
		say("%s: data+%#04zx: %08x %08x %08x %08x", what, o, rd_u32(d + o), rd_u32(d + o + 4), rd_u32(d + o + 8),
		    rd_u32(d + o + 12));
}

/* sensor_command(cmd) as the HAL: group 10 at +0, cmd at +4, busy (-12) retried after 10 ms and 20 ms. Returns the
 * qseeprobe rc (0 ok, 3 TZ error, 4 listener, 5 TA status != 0); *status = TA status. */
static int fpc_sensor_cmd(uint32_t appid, uint8_t *d, uint32_t cmd, int32_t *status)
{
	char what[32];
	snprintf(what, sizeof(what), "g10/%u", cmd);
	static const unsigned retry_us[] = { 10000, 20000 };
	for (int t = 0;; t++) {
		put_u32(d + 0, 10), put_u32(d + 4, cmd);
		put_u32(d + 8, 0);
		int rc = fpc_send64(appid, d, FPC_SENSOR_LEN, what);
		if (rc)
			return rc;
		*status = (int32_t)rd_u32(d + 8);
		say("%s: TA status %d (%#x), +0xc %#x, result(+0x10) %#x", what, *status, (unsigned)*status, rd_u32(d + 0xc),
		    rd_u32(d + 0x10));
		if (*status != FPC_TA_BUSY || t == 2)
			return *status ? 5 : 0;
		say("%s: busy (-12), retry in %u ms", what, retry_us[t] / 1000);
		usleep(retry_us[t]);
	}
}

static uint32_t fpc_sysctl_u32(const char *name)
{
	uint32_t v = 0;
	size_t l = sizeof(v);
	if (sysctlbyname(name, &v, &l, NULL, 0))
		return 0xffffffffu;
	return v;
}

static void fpc_gpio_state(const char *what)
{
	char s[256] = "";
	size_t l = sizeof(s) - 1;
	if (sysctlbyname("kern.sweet_fpc_state", s, &l, NULL, 0))
		snprintf(s, sizeof(s), "(kern.sweet_fpc_state: %s)", strerror(errno));
	say("%s: %s", what, s);
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

/* fpc_sensor_cmd without its per-command lines (TZ errors still give rc 3, listeners are kept for print_seen) */
static int fd_cmd(uint32_t appid, uint8_t *d, uint32_t cmd, int32_t *status)
{
	say_quiet = 1;
	int r = fpc_sensor_cmd(appid, d, cmd, status);
	say_quiet = 0;
	return r;
}

static long fpc_ms_since(const struct timespec *t0)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

/* kern.sweet_fpc_wait: 1 gpio112 high, 0 timeout, -1 error */
static int fpc_wait_high(int ms)
{
	int hit = 0;
	size_t l = sizeof(hit);
	if (ms < 1)
		return 0;
	if (ms > 60000)
		ms = 60000;
	if (dryrun)
		return 0;
	if (sysctlbyname("kern.sweet_fpc_wait", &hit, &l, &ms, sizeof(ms))) {
		say("finger-detect: kern.sweet_fpc_wait: %s", strerror(errno));
		return -1;
	}
	return hit;
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

/*
 * P5: FPC g11 (bio) commands, same 0x58-byte buffer and 64-bit layout as the sensor commands (the HAL uses one shared
 * buffer for both, fpc_tee_init). hal.dis: header {u32 11, u32 cmd} from .rodata, TA status at +8.
 *   (11,6) load_empty_db                       (11,9) set_gid: in +0xc gid
 *   (11,0) begin_enrol                          (11,1) enrol: status = enrol code, out +0xc samples remaining
 *   (11,2) end_enrol: out +0xc finger id        (11,3) identify: status 0 = match, out +0xc finger id,
 *          +0x10 coverage, +0x14 quality, +0x1c result, +0x20 score; status 100 = partial, 101/102 = insufficient
 *   (11,4) update_template: out +0xc updated    (11,7) get_finger_ids: in +0xc capacity (HAL max 5), out +0xc count,
 *          u32 ids from +0x10                   (11,8) delete_template: in +0xc finger id
 *   (11,10) get_template_db_id: out u64 +0x10
 * do_enroll's enrol codes (table @0x5309): 0 done (-> check_enrol_timeout, end_enrol, store), 1 progress (acquired
 * GOOD + remaining), 2 error UNABLE_TO_PROCESS (stop), 3 vendor error 1000 (stop), 4 partial, 5 progress + vendor
 * acquired 1000, 6 imager dirty. The HAL calls authorize_enrol (g3, the HAT) before begin_enrol; we do not.
 */
static int fpc_bio(uint32_t appid, uint8_t *d, uint32_t cmd, int32_t *status)
{
	char what[32];
	snprintf(what, sizeof(what), "g11/%u", cmd);
	put_u32(d + 0, 11), put_u32(d + 4, cmd), put_u32(d + 8, 0);
	say_quiet = 1;
	int r = fpc_send64(appid, d, FPC_SENSOR_LEN, what);
	say_quiet = 0;
	*status = (int32_t)rd_u32(d + 8);
	if (r)
		say("%s: transport rc %d", what, r);
	return r;
}

static int fpc_get_ids(uint32_t appid, uint32_t out[5], uint32_t *count, int32_t *status)
{
	uint8_t d[FPC_SENSOR_LEN] = { 0 };
	put_u32(d + 0xc, 5);
	int r = fpc_bio(appid, d, 7, status);
	if (r)
		return r;
	uint32_t n = rd_u32(d + 0xc);
	if (n > 5)
		n = 5;
	for (uint32_t i = 0; i < n; i++)
		out[i] = rd_u32(d + 0x10 + 4 * i);
	*count = n;
	return 0;
}

static int fpc_print_ids(uint32_t appid, const char *what, uint32_t *count)
{
	uint32_t list[5] = { 0 };
	uint32_t n = 0;
	int32_t st;
	int r = fpc_get_ids(appid, list, &n, &st);
	if (r)
		return r;
	char ids[96] = "";
	for (uint32_t i = 0; i < n; i++)
		snprintf(ids + strlen(ids), sizeof(ids) - strlen(ids), " %#x", list[i]);
	*count = st ? 0xffffffffu : n;
	uint8_t e[FPC_SENSOR_LEN] = { 0 };
	int32_t st2;
	r = fpc_bio(appid, e, 10, &st2);
	say("%s: get_finger_ids status %d count %u ids:%s | db id status %d %#llx", what, st, n, n ? ids : " (none)", st2,
	    (unsigned long long)rd_u32(e + 0x10) | (unsigned long long)rd_u32(e + 0x14) << 32);
	return st ? 5 : r;
}

static int cmd_fp_ids(uint32_t appid)
{
	if (check_appid(appid))
		return 2;
	uint32_t n;
	return fpc_print_ids(appid, "fp-ids", &n);
}

static int fpc_delete(uint32_t appid, uint32_t fid)
{
	uint8_t d[FPC_SENSOR_LEN] = { 0 };
	int32_t st;
	put_u32(d + 0xc, fid);
	int r = fpc_bio(appid, d, 8, &st);
	say("delete_template %#x: status %d", fid, st);
	return r ? r : st ? 5 : 0;
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

/* delete every finger the TA currently lists; returns 0 iff get_finger_ids ends up empty */
static int fpc_cleanup_all(uint32_t appid, const char *what)
{
	uint32_t list[5] = { 0 }, n = 0;
	int32_t st;
	if (fpc_get_ids(appid, list, &n, &st) || st) {
		say("%s: get_finger_ids failed (status %d), cannot clean up", what, st);
		return -1;
	}
	if (n == 0) {
		say("%s: DB already empty", what);
		return 0;
	}
	say("%s: deleting %u leftover finger(s)", what, n);
	for (uint32_t i = 0; i < n; i++)
		fpc_delete(appid, list[i]);
	uint32_t left = 0xffffffffu;
	fpc_print_ids(appid, what, &left);
	return left == 0 ? 0 : -1;
}

/* one capture as capture_image(wait_for_finger): 0 + *st4 = cmd 4 status; 6 = timeout; else transport rc */
static int fpc_capture(uint32_t appid, const struct timespec *t0, int ms, int32_t *st4, long *wake_ms)
{
	uint8_t d[FPC_SENSOR_LEN] = { 0 };
	int32_t st;
	long t = fpc_ms_since(t0);
	d[0xc] = 1;
	int r = fd_cmd(appid, d, 2, &st);
	if (r)
		return r;
	int hit = fpc_wait_high(ms - (int)fpc_ms_since(t0));
	*wake_ms = fpc_ms_since(t0) - t;
	if (hit <= 0)
		return hit < 0 ? 2 : 6;
	r = fd_cmd(appid, d, 1, &st);
	if (r && r != 5)
		return r;
	for (int tries = 1; tries <= 4; tries++) {
		r = fd_cmd(appid, d, 3, &st);
		if (r && r != 5)
			return r;
		hit = fpc_wait_high(ms - (int)fpc_ms_since(t0));
		if (hit <= 0)
			return hit < 0 ? 2 : 6;
		fd_cmd(appid, d, 4, st4);
		if (*st4 != 3)
			break;
	}
	if (*st4)
		fd_cmd(appid, d, 5, &st);	/* capture_image's error path */
	return 0;
}

static const char *enrol_code(int32_t s)
{
	switch (s) {
	case 0: return "DONE";
	case 1: return "progress";
	case 2: return "ERROR unable to process";
	case 3: return "ERROR vendor 1000";
	case 4: return "partial";
	case 5: return "progress + vendor 1000 (too similar / same spot?)";
	case 6: return "imager dirty";
	default: return s < 0 ? "FPC error" : "unknown";
	}
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
