/*
 * qsee_lib.c (ws-fpcd-a): qseeprobe's QSEE + FPC TA code as a library (qsee_lib.h), shared by qseeprobe and sweetfpcd.
 * The code below is moved unchanged from qseeprobe.c, qsee_app.inc and qsee_fpc.inc (ws-qsee-b 5e7263b); new here:
 * say_tag, qsee_unload() (cmd_unload without the guard) and qsee_lock().
 * Every SMC passes qsmc(): x0 and x1 must match an entry of qsee_whitelist[] (qsee_wl.c) or the process exits.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include "../video/pmem.h"
#include "qsee_wl.h"
#include "qsee_lib.h"
int dryrun, shmbridge, con = -1;
const char *say_tag = "qseeprobe: ";

void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int say_quiet;	/* finger-detect's loop: one line per round */
void say(const char *fmt, ...)
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
	if (con >= 0 && (write(con, say_tag, strlen(say_tag)) < 0 || write(con, line, (size_t)n) < 0)) {
		/* console is a mirror only */
	}
}

/* the only path to PMEM_SMC; x[0..3] hold x0..x3 afterwards. Returns (int64_t)x0. */
int64_t qsmc(uint64_t x[8])
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

uint64_t buf_pa;
uint32_t lookup_app_id;	/* set by lookup_one() on LOADED */
uint64_t bridge_handle;
int bridged;

int bridge_create(void)
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

int bridge_delete(void)
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
int lookup_one(const char *name)
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
/*
 * ws-qsee-b: load / send / unload of one QSEE app, was qsee_app.inc (ws-qsee-b), now part of qsee_lib.c.
 * Sequence as Linux 4.19 redbull drivers/misc/qseecom.c (QSEE >= 4.0, 64-bit forms, reentrancy phase 2):
 *   load:   __qseecom_load_fw 4750-4888: one contiguous buffer = NAME.mdt followed by NAME.b00..b(e_phnum-1);
 *           TZ_OS_APP_START x2 mdt_len, x3 img_len (mdt + all segments), x4 pa. SUCCESS -> x3 = app id.
 *           cmnlib64 is not loaded first: the DT has qcom,commonlib64-loaded-by-uefi (qseecom.c 9459) and the app
 *           region notification is skipped because ABL passes androidboot.keymaster=1 (is_apps_region_protected).
 *   send:   __qseecom_send_cmd 3710-3876 + qseecom_scm_call2: x2 app_id, x3 req pa, x4 req len, x5 = pa of a page
 *           holding args[3..9] (scm.c allocate_extra_arg_buffer: rsp pa, rsp len[, sglistinfo pa, len]).
 *           With TZ whitelist feature >= 1.0.0 Linux uses SEND_DATA_WITH_WHITELIST and a 4-entry sglistinfo table
 *           naming the modfd offsets (here: one buffer pa patched at sb+4, as the FPC HAL's send_modified_cmd).
 *   unload: __qseecom_unload_app: TZ_OS_APP_SHUTDOWN x2 app_id.
 *   INCOMPLETE (x1 = 1): TZ wants listener x3. We have none, so we answer as Linux does for a listener that
 *           does not exist (__qseecom_process_incomplete_cmd 2117-2300: "Listener Svc %d does not exist"):
 *           TZ_OS_LISTENER_RESPONSE_HANDLER x2 listener id, x3 QSEOS_RESULT_FAILURE, and loop on the new result.
 *   BLOCKED_ON_LISTENER (x1 = 2): x3 listener, x2 session; Linux waits for the listener to be free, then
 *           TZ_OS_CONTINUE_BLOCKED_REQUEST_SMCINVOKE(session) (falls back to ..._REQUEST(app id)), which comes
 *           back INCOMPLETE and is answered as above.
 * Every listener id met is kept in seen_lstnr[] and printed, and the exit code is 4 when one was met.
 */
uint32_t seen_lstnr[8];
unsigned n_seen;

static const char *lstnr_name(uint32_t id)
{
	switch (id) {
	case RPMB_SERVICE: return "RPMB_SERVICE";
	case SSD_SERVICE: return "SSD_SERVICE";
	default: return "unknown to qseecom.c (qseecomd/HAL listener)";
	}
}

void note_lstnr(uint32_t id)
{
	for (unsigned i = 0; i < n_seen; i++)
		if (seen_lstnr[i] == id)
			return;
	if (n_seen < 8)
		seen_lstnr[n_seen++] = id;
}

/* x[] holds the result of an app-manager or send-data SMC; resolve INCOMPLETE/BLOCKED by refusing the listener. */
void abort_listeners(uint64_t x[8], uint32_t app_id, const char *what)
{
	for (int i = 0; i < MAX_ABORTS; i++) {
		if ((int64_t)x[0])
			return;
		uint32_t result = (uint32_t)x[1], type = (uint32_t)x[2], data = (uint32_t)x[3];
		if (result == QSEOS_RESULT_BLOCKED) {
			note_lstnr(data);
			say("%s: BLOCKED_ON_LISTENER: listener %#x (%s), session %#x: continue-blocked", what, data,
			    lstnr_name(data), type);
			uint64_t y[8] = { QSEE_SMC_CONT_BLOCKED_SI, QSEE_ARGS_CONT_BLOCKED, type };
			if (qsmc(y)) {
				uint64_t z[8] = { QSEE_SMC_CONT_BLOCKED, QSEE_ARGS_CONT_BLOCKED, app_id };
				qsmc(z);
				memcpy(x, z, 4 * sizeof(uint64_t));
			} else {
				memcpy(x, y, 4 * sizeof(uint64_t));
			}
			continue;
		}
		if (result == QSEOS_RESULT_INCOMPLETE) {
			note_lstnr(data);
			say("%s: INCOMPLETE: TZ requests listener %#x (%u, %s), resp_type %#x; no listener here: answering "
			    "QSEOS_RESULT_FAILURE", what, data, data, lstnr_name(data), type);
			uint64_t y[8] = { QSEE_SMC_LSTNR_RESP, QSEE_ARGS_LSTNR_RESP, data, QSEOS_RESULT_FAILURE };
			qsmc(y);
			memcpy(x, y, 4 * sizeof(uint64_t));
			continue;
		}
		return;
	}
	say("%s: still INCOMPLETE/BLOCKED after %d aborts, giving up", what, MAX_ABORTS);
}

void print_seen(void)
{
	for (unsigned i = 0; i < n_seen; i++)
		say("LISTENER REQUESTED: id %#x (%u) %s", seen_lstnr[i], seen_lstnr[i], lstnr_name(seen_lstnr[i]));
}

/* registration of one pmem buffer with the shmbridge (as qtee_shmbridge_register, VMID_HLOS R|W) */
int bridge_reg(uint64_t pa, uint64_t size, uint64_t *handle)
{
	size = (size + 4095) & ~4095ull;
	uint64_t x[8] = { QSEE_SMC_SHMB_CREATE, QSEE_ARGS_SHMB_CREATE, pa | SHMB_PERM_RW, pa | SHMB_PERM_RW,
			  size | (1ull << SHMB_NUM_VM_SHIFT), VMID_HLOS };
	int64_t rc = qsmc(x);
	if (rc || x[1]) {
		say("shmbridge: create %#llx+%#llx failed (x0 %lld, ret0 %#llx)", (unsigned long long)pa,
		    (unsigned long long)size, (long long)rc, (unsigned long long)x[1]);
		return -1;
	}
	*handle = x[2];
	say("shmbridge: %#llx+%#llx registered, handle %#llx", (unsigned long long)pa, (unsigned long long)size,
	    (unsigned long long)*handle);
	return 0;
}

int bridge_dereg(uint64_t handle)
{
	uint64_t x[8] = { QSEE_SMC_SHMB_DELETE, QSEE_ARGS_SHMB_DELETE, handle };
	int64_t rc = qsmc(x);
	if (rc || x[1]) {
		say("shmbridge: delete of handle %#llx failed (x0 %lld, ret0 %#llx)", (unsigned long long)handle,
		    (long long)rc, (unsigned long long)x[1]);
		return -1;
	}
	return 0;
}

/* a set of pmem buffers for one command; bridged when QSEE_SHMBRIDGE=1; freed (or leaked if a bridge sticks) */

int pbuf_get(struct pbuf *b, uint64_t size, uint64_t align, int below4g, uint64_t fake)
{
	memset(b, 0, sizeof(*b));
	b->size = (size + 4095) & ~4095ull;
	if (dryrun) {
		b->pa = fake;
		return 0;
	}
	b->pa = below4g ? pm_alloc32(b->size, align) : pm_alloc(b->size, align);
	if (!b->pa && align > 4096)
		b->pa = below4g ? pm_alloc32(b->size, 4096) : pm_alloc(b->size, 4096);
	if (!b->pa)
		return -1;
	if (shmbridge) {
		if (bridge_reg(b->pa, b->size, &b->handle)) {
			pm_free(b->pa);
			b->pa = 0;
			return -1;
		}
		b->bridged = 1;
	}
	return 0;
}

void pbuf_put(struct pbuf *b)
{
	if (!b->pa)
		return;
	if (b->bridged && bridge_dereg(b->handle)) {
		say("pbuf: %#llx NOT freed (still bridged)", (unsigned long long)b->pa);
		return;
	}
	if (!dryrun)
		pm_free(b->pa);
	b->pa = 0;
}

int wr_chunks(uint64_t pa, const uint8_t *p, size_t len)
{
	if (dryrun)
		return 0;
	for (size_t o = 0; o < len; o += 65536) {
		size_t n = len - o < 65536 ? len - o : 65536;
		if (pm_wr(pa + o, p + o, n))
			return -1;
	}
	return 0;
}

uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		say("load: %s: %s", path, strerror(errno));
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *p = n > 0 ? malloc((size_t)n) : NULL;
	if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
		say("load: %s: read failed", path);
		free(p);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return p;
}

/* lookup in its own name page; 0 loaded (*id set), 1 not loaded, 2 error, 3 stop */
int lookup_name(const char *name, uint32_t *id)
{
	struct pbuf b;
	lookup_app_id = 0;
	if (pbuf_get(&b, 4096, 4096, 0, 0xdead0000ull))
		return 2;
	buf_pa = b.pa;
	int r = lookup_one(name);
	pbuf_put(&b);
	*id = lookup_app_id;
	return r;
}

/* the guard for send/unload: APPID must be what QSEE_APP (default fpctzappfingerprint) is loaded as */
int check_appid(uint32_t appid)
{
	const char *name = getenv("QSEE_APP");
	uint32_t id = 0, km = 0;
	if (!name || !*name)
		name = "fpctzappfingerprint";
	if (dryrun)
		return 0;
	if (!lookup_name("keymaster64", &km) && km == appid) {
		say("refused: app id %u is keymaster64", appid);
		return -1;
	}
	if (lookup_name(name, &id) || id != appid) {
		say("refused: %s is not loaded as app id %u (lookup id %u)", name, appid, id);
		return -1;
	}
	return 0;
}

int cmd_load(const char *name, const char *dir)
{
	char path[512];
	size_t mdt_len, total;
	uint32_t id = 0;
	if (strlen(name) >= QSEE_APP_NAME_MAX || strchr(name, '/')) {
		say("load: bad name %s", name);
		return 2;
	}
	snprintf(path, sizeof(path), "%s/%s.mdt", dir, name);
	uint8_t *mdt = slurp(path, &mdt_len);
	if (!mdt)
		return 2;
	if (mdt_len < 0x40 || memcmp(mdt, "\177ELF", 4)) {
		say("load: %s is not an ELF", path);
		return 2;
	}
	unsigned cls = mdt[4], phnum = cls == 2 ? (unsigned)(mdt[0x38] | mdt[0x39] << 8) : (unsigned)(mdt[0x2c] | mdt[0x2d] << 8);
	if (cls != 2) {
		say("load: %s is ELFCLASS%u: a 32-bit app needs cmnlib loaded first (not done by this tool)", path, cls);
		return 2;
	}
	/* image = mdt + b00..b(phnum-1), as __qseecom_get_fw_data */
	uint8_t *img = malloc(mdt_len);
	memcpy(img, mdt, mdt_len);
	total = mdt_len;
	for (unsigned i = 0; i < phnum; i++) {
		size_t sl;
		snprintf(path, sizeof(path), "%s/%s.b%02u", dir, name, i);
		uint8_t *seg = slurp(path, &sl);
		if (!seg)
			return 2;
		img = realloc(img, total + sl);
		memcpy(img + total, seg, sl);
		total += sl;
		free(seg);
	}
	say("load %s: ELF64, %u segments, mdt_len %#zx, img_len %#zx (%zu)", name, phnum, mdt_len, total, total);
	int lr = lookup_name(name, &id);
	if (lr == 0 && !dryrun) {
		say("load %s: already LOADED as app id %u, not loading again", name, id);
		return 0;
	}
	if (lr >= 2)
		return lr;
	struct pbuf b;
	if (pbuf_get(&b, total, 0x100000, 0, 0xbeef00000ull))
		return 2;
	say("load %s: image buffer pa %#llx size %#llx%s", name, (unsigned long long)b.pa, (unsigned long long)b.size,
	    b.bridged ? " (shmbridge registered)" : " (not bridged)");
	if (wr_chunks(b.pa, img, total)) {
		pbuf_put(&b);
		return 2;
	}
	uint64_t x[8] = { QSEE_SMC_APP_START, QSEE_ARGS_APP_START, mdt_len, total, b.pa };
	qsmc(x);
	say("load %s: APP_START raw x0 %#llx x1 %#llx x2 %#llx x3 %#llx", name, (unsigned long long)x[0],
	    (unsigned long long)x[1], (unsigned long long)x[2], (unsigned long long)x[3]);
	abort_listeners(x, 0, "load");
	pbuf_put(&b);	/* Linux frees the image right after APP_START */
	int rc;
	uint32_t result = (uint32_t)x[1];
	if ((int64_t)x[0]) {
		say("load %s: smc failed (x0 %lld)", name, (long long)x[0]);
		rc = 2;
	} else if (result == QSEOS_RESULT_SUCCESS && !n_seen) {
		say("load %s: SUCCESS, app id %u (resp_type %#llx)", name, (unsigned)x[3], (unsigned long long)x[2]);
		rc = 0;
	} else {
		say("load %s: result %#x resp_type %#llx data %#llx%s", name, result, (unsigned long long)x[2],
		    (unsigned long long)x[3], result == QSEOS_RESULT_FAILURE ? " (QSEOS_RESULT_FAILURE)" : "");
		rc = n_seen ? 4 : 2;
	}
	print_seen();
	if (n_seen && !dryrun && !lookup_name(name, &id)) {
		/* Linux unloads after a failed incomplete-cmd round; so do we */
		say("load %s: loaded as %u after a listener abort: shutting it down", name, id);
		uint64_t y[8] = { QSEE_SMC_APP_SHUTDOWN, QSEE_ARGS_APP_SHUTDOWN, id };
		qsmc(y);
		abort_listeners(y, id, "unload");
	}
	free(img);
	free(mdt);
	return rc;
}

uint32_t rd_u32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
void put_u32(uint8_t *p, uint32_t v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }
void put_u64(uint8_t *p, uint64_t v) { put_u32(p, (uint32_t)v), put_u32(p + 4, (uint32_t)(v >> 32)); }

/*
 * ws-qsee-b P4: FPC sensor commands (group 10), 64-bit layout as P3.5 (QSEE_DATA64): the data buffer in normal
 * PMEM_ALLOC, its u64 pa at sb+4, sglistinfo {0xc0000004, 0x1000}, SEND_DATA_WITH_WHITELIST. Was qsee_fpc.inc, now part of
 * qsee_lib.c.
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


/* one command in a 64-bit-layout data buffer; *data (len bytes) is sent and read back; 0 = TZ SUCCESS (TA status in data+8) */
int fpc_send64(uint32_t appid, uint8_t *data, size_t len, const char *what)
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

void fpc_dump(const char *what, const uint8_t *d, size_t len)
{
	for (size_t o = 0; o < len; o += 16)
		say("%s: data+%#04zx: %08x %08x %08x %08x", what, o, rd_u32(d + o), rd_u32(d + o + 4), rd_u32(d + o + 8),
		    rd_u32(d + o + 12));
}

/* sensor_command(cmd) as the HAL: group 10 at +0, cmd at +4, busy (-12) retried after 10 ms and 20 ms. Returns the
 * qseeprobe rc (0 ok, 3 TZ error, 4 listener, 5 TA status != 0); *status = TA status. */
int fpc_sensor_cmd(uint32_t appid, uint8_t *d, uint32_t cmd, int32_t *status)
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

uint32_t fpc_sysctl_u32(const char *name)
{
	uint32_t v = 0;
	size_t l = sizeof(v);
	if (sysctlbyname(name, &v, &l, NULL, 0))
		return 0xffffffffu;
	return v;
}

void fpc_gpio_state(const char *what)
{
	char s[256] = "";
	size_t l = sizeof(s) - 1;
	if (sysctlbyname("kern.sweet_fpc_state", s, &l, NULL, 0))
		snprintf(s, sizeof(s), "(kern.sweet_fpc_state: %s)", strerror(errno));
	say("%s: %s", what, s);
}

/* fpc_sensor_cmd without its per-command lines (TZ errors still give rc 3, listeners are kept for print_seen) */
int fd_cmd(uint32_t appid, uint8_t *d, uint32_t cmd, int32_t *status)
{
	say_quiet = 1;
	int r = fpc_sensor_cmd(appid, d, cmd, status);
	say_quiet = 0;
	return r;
}

long fpc_ms_since(const struct timespec *t0)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

/* kern.sweet_fpc_wait: 1 gpio112 high, 0 timeout, -1 error */
int fpc_wait_high(int ms)
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
int fpc_bio(uint32_t appid, uint8_t *d, uint32_t cmd, int32_t *status)
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

int fpc_get_ids(uint32_t appid, uint32_t out[5], uint32_t *count, int32_t *status)
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

int fpc_print_ids(uint32_t appid, const char *what, uint32_t *count)
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

int fpc_delete(uint32_t appid, uint32_t fid)
{
	uint8_t d[FPC_SENSOR_LEN] = { 0 };
	int32_t st;
	put_u32(d + 0xc, fid);
	int r = fpc_bio(appid, d, 8, &st);
	say("delete_template %#x: status %d", fid, st);
	return r ? r : st ? 5 : 0;
}

/* delete every finger the TA currently lists; returns 0 iff get_finger_ids ends up empty */
int fpc_cleanup_all(uint32_t appid, const char *what)
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
int fpc_capture(uint32_t appid, const struct timespec *t0, int ms, int32_t *st4, long *wake_ms)
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

const char *enrol_code(int32_t s)
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

/* TZ_OS_APP_SHUTDOWN of appid, listener requests refused as in cmd_unload; callers check_appid() first */
int qsee_unload(uint32_t appid)
{
	uint64_t x[8] = { QSEE_SMC_APP_SHUTDOWN, QSEE_ARGS_APP_SHUTDOWN, appid };
	qsmc(x);
	abort_listeners(x, appid, "unload");
	if ((int64_t)x[0] || (uint32_t)x[1] != QSEOS_RESULT_SUCCESS) {
		say("unload %u: FAILED (x0 %lld result %#x)", appid, (long long)x[0], (unsigned)x[1]);
		return n_seen ? 4 : 2;
	}
	say("unload %u: SUCCESS", appid);
	return 0;
}

/*
 * One QSEE/SMC user at a time: an exclusive flock on QSEE_LOCK_PATH (env QSEE_LOCK), held until the process exits
 * (the kernel drops it on any exit, so a stale file is harmless). The holder writes "pid N WHO" into the file.
 * QSEE_NOLOCK=1 skips the lock. If the file cannot be created (no /var/local/fpc), the lock is skipped with a line.
 */
static int lock_fd = -1;
int qsee_lock(const char *who)
{
	const char *e = getenv("QSEE_NOLOCK"), *path = getenv("QSEE_LOCK");
	if (!path || !*path)
		path = QSEE_LOCK_PATH;
	if (e && atoi(e)) {
		say("lock: QSEE_NOLOCK=1: %s not taken", path);
		return 0;
	}
	if (lock_fd >= 0)
		return 0;
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0) {
		say("lock: %s: %s: running without the lock", path, strerror(errno));
		return 0;
	}
	if (flock(fd, LOCK_EX | LOCK_NB)) {
		char held[96] = "";
		ssize_t n = pread(fd, held, sizeof(held) - 1, 0);
		held[n > 0 ? n : 0] = 0;
		char *nl = strchr(held, '\n');
		if (nl)
			*nl = 0;
		say("lock: %s is held (%s): another QSEE user runs; refusing (QSEE_NOLOCK=1 overrides)", path,
		    *held ? held : "holder unknown");
		close(fd);
		return -1;
	}
	char line[96];
	int n = snprintf(line, sizeof(line), "pid %d %s\n", (int)getpid(), who);
	if (ftruncate(fd, 0) || pwrite(fd, line, (size_t)n, 0) != n) {
		/* the lock is the flock, the text is information only */
	}
	lock_fd = fd;
	return 0;
}
