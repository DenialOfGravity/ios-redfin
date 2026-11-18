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
