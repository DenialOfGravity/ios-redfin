/*
 * ws-qsee-b, gatekeeper phase 2 (READ-ONLY): QSEE listeners (RPMB 0x2000, file system 10, time 11) served from this process,
 * and the keymaster64 commands KM GET_VERSION 0x200, GET_AUTH_TOKEN_KEY 0x205 and a dummy gatekeeper VERIFY 0x1002 for the
 * private uid 0x7e000001. Included by qseeprobe.c after qsee_app.inc (uses say(), qsmc(), pbuf_*, lookup_name(), rd_u32,
 * put_u32/64). Plan: PF/logs/research/fingerprint-gatekeeper-2026-11-18.md sections 2, 3, 5, 6, 8.
 *
 * Read-only rules enforced here (independently of the kernel's kern.sweet_rpmb filter):
 *   RPMB 0x103 WRITE requests are never relayed: answered with status -1, logged, counted as a write attempt.
 *   File system: only open (without O_CREAT/O_TRUNC/O_APPEND; served O_RDONLY), read, lseek, fstat, lstat, close of our
 *   own fds. mkdir, unlink(at), creat, write, link/rename/copy are answered -1, logged, counted as write attempts.
 *   Keymaster: only 0x200, 0x205, 0x1002 (uid 0x7e000001) are ever built; ENROLL 0x1001, DELETE_USER 0x1003,
 *   DELETE_ALL_USERS 0x1004 and every CBOR 0x2xxxx command (0x21003, 0x21004, ...) are refused by km_cmd_allowed().
 *   A write attempt ends the run after the current TA command: no further command, listeners deregistered, exit 6.
 * Everything is logged to stdout, /dev/console and /var/local/tz/log/lsnr.log. One SMC user at a time:
 * /var/local/tz/qsee.lock (flock) is taken by qseeprobe's main() for every command.
 */
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>


#define TZ_DIR		"/var/local/tz"
#define LSNR_STATE	TZ_DIR "/listeners.state"
#define FS_ROOT		TZ_DIR "/persist"	/* "/persist/..." -> FS_ROOT "/..." (read-only copy of stock persist) */
#define FS_DATA		TZ_DIR "/data"		/* "/data/..." -> FS_DATA "/..." (empty in phase 2) */
#define GK_UID		0x7e000001u		/* the only uid this tool ever names */

static FILE *lsnr_log;
static unsigned write_attempts, rw_opens;
static char write_what[8][160];

static void lg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void lg(const char *fmt, ...)
{
	char line[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	say("%s", line);
	if (lsnr_log) {
		fprintf(lsnr_log, "%ld %s\n", (long)time(NULL), line);
		fflush(lsnr_log);
	}
}

static void write_attempt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void write_attempt(const char *fmt, ...)
{
	char what[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(what, sizeof(what), fmt, ap);
	va_end(ap);
	if (write_attempts < 8)
		snprintf(write_what[write_attempts], sizeof(write_what[0]), "%s", what);
	write_attempts++;
	lg("WRITE ATTEMPT #%u REFUSED: %s", write_attempts, what);
}

/* ---- request log (every listener request, summarised) ---- */
struct req_note { uint32_t lid, cmd, a, b; };
static struct req_note notes[256];
static unsigned n_notes;
static void note(uint32_t lid, uint32_t cmd, uint32_t a, uint32_t b)
{
	if (n_notes < 256)
		notes[n_notes] = (struct req_note){ lid, cmd, a, b };
	n_notes++;
}

/* ---- listeners ---- */
struct lsnr {
	uint32_t id, sb_len;
	const char *name;
	struct pbuf sb;
	int registered, smcinvoke;
	unsigned served;
};
static struct lsnr lsnrs[] = {
	{ .id = 0x2000, .sb_len = 0x6400, .name = "RPMB" },
	{ .id = 10, .sb_len = 0x5000, .name = "file system" },
	{ .id = 11, .sb_len = 0x5000, .name = "time" },
};
#define N_LSNR (sizeof(lsnrs) / sizeof(lsnrs[0]))
static struct pbuf sgl;		/* zeroed 32-byte sglistinfo table for the whitelist response */
static uint8_t sbuf[0x8000];

static void state_save(void)
{
	FILE *f = fopen(LSNR_STATE, "w");
	if (!f)
		return;
	for (unsigned i = 0; i < N_LSNR; i++)
		if (lsnrs[i].registered)
			fprintf(f, "%#x %#llx\n", lsnrs[i].id, (unsigned long long)lsnrs[i].sb.pa);
	fclose(f);
}

static int dereg_id(uint32_t id)
{
	uint64_t x[8] = { QSEE_SMC_LSTNR_DEREG, QSEE_ARGS_LSTNR_DEREG, id };
	int64_t rc = qsmc(x);
	lg("listener %#x deregister: x0 %lld result %#x", id, (long long)rc, (unsigned)x[1]);
	return rc || (uint32_t)x[1] ? -1 : 0;
}

/* a previous run that died: deregister its ids (its PMEM stays allocated: leaked, harmless) */
static void state_cleanup(void)
{
	FILE *f = fopen(LSNR_STATE, "r");
	if (!f)
		return;
	unsigned id;
	unsigned long long pa;
	while (fscanf(f, "%x %llx", &id, &pa) == 2) {
		lg("stale listener %#x (sb %#llx) from an earlier run: deregistering", id, pa);
		dereg_id(id);
	}
	fclose(f);
	unlink(LSNR_STATE);
}

static int lsnr_register(struct lsnr *l)
{
	if (pbuf_get(&l->sb, l->sb_len, 4096, 0, 0xc0000000ull + l->id * 0x10000ull))
		return -1;
	/* Linux registers the whole ION buffer: sb_len = the dmabuf size, page-rounded (0x6400 -> 0x7000) */
	l->sb_len = (uint32_t)l->sb.size;
	uint64_t x[8] = { QSEE_SMC_LSTNR_REG_SI, QSEE_ARGS_LSTNR_REG, l->id, l->sb.pa, l->sb_len };
	int64_t rc = qsmc(x);
	l->smcinvoke = 1;
	if (rc) {
		lg("listener %#x: smcinvoke register x0 %lld: trying the legacy register", l->id, (long long)rc);
		uint64_t y[8] = { QSEE_SMC_LSTNR_REG, QSEE_ARGS_LSTNR_REG, l->id, l->sb.pa, l->sb_len };
		rc = qsmc(y);
		memcpy(x, y, sizeof(x));
		l->smcinvoke = 0;
	}
	if (rc || (uint32_t)x[1]) {
		lg("listener %#x (%s): register FAILED x0 %lld result %#x", l->id, l->name, (long long)rc, (unsigned)x[1]);
		pbuf_put(&l->sb);
		return -1;
	}
	l->registered = 1;
	state_save();
	lg("listener %#x (%s): registered (%s), sb %#llx len %#x", l->id, l->name, l->smcinvoke ? "smcinvoke" : "legacy",
	   (unsigned long long)l->sb.pa, l->sb_len);
	return 0;
}

static void lsnr_all_down(void)
{
	for (unsigned i = 0; i < N_LSNR; i++) {
		struct lsnr *l = &lsnrs[i];
		if (!l->registered)
			continue;
		if (dereg_id(l->id) == 0) {
			l->registered = 0;
			pbuf_put(&l->sb);
		} else {
			lg("listener %#x: deregister failed: its sb %#llx is NOT freed", l->id, (unsigned long long)l->sb.pa);
		}
	}
	state_save();
	int any = 0;
	for (unsigned i = 0; i < N_LSNR; i++)
		any |= lsnrs[i].registered;
	if (!any)
		unlink(LSNR_STATE);
	pbuf_put(&sgl);
}

static int lsnr_all_up(void)
{
	state_cleanup();
	if (pbuf_get(&sgl, 4096, 4096, 0, 0xc1000000ull))
		return -1;
	const char *e = getenv("QSEE_LSNR_CONT");	/* diagnosis: try every listener even after a failure */
	int cont = e && atoi(e), bad = 0;
	for (unsigned i = 0; i < N_LSNR; i++)
		if (lsnr_register(&lsnrs[i])) {
			bad = 1;
			if (!cont)
				break;
		}
	if (bad) {
		lsnr_all_down();
		return -1;
	}
	return 0;
}

static struct lsnr *lsnr_find(uint32_t id)
{
	for (unsigned i = 0; i < N_LSNR; i++)
		if (lsnrs[i].id == id && lsnrs[i].registered)
			return &lsnrs[i];
	return NULL;
}

static uint32_t g32(const uint8_t *p) { return rd_u32(p); }

/* ---- RPMB (librpmb rpmb_start @0x5bbc) ---- */
static int sysctl_int(const char *name)
{
	int v = -1;
	size_t len = sizeof(v);
	if (sysctlbyname(name, &v, &len, NULL, 0))
		return -1;
	return v;
}

struct rpmb_res { int32_t err; uint32_t scsi_status; uint8_t sense[18]; uint16_t pad; uint32_t nframes; };

/* kern.sweet_rpmb_xfer op 1 (read): 1 request frame out, nframes back. 0 or errno; *res filled */
static int rpmb_xfer_read(const uint8_t *req_frame, uint32_t nframes, uint8_t *out, struct rpmb_res *res)
{
	if (nframes < 1 || nframes > 64)
		return EINVAL;
	uint8_t in[12 + 512];
	put_u32(in, 1), put_u32(in + 4, nframes), put_u32(in + 8, 0);
	memcpy(in + 12, req_frame, 512);
	size_t olen = sizeof(struct rpmb_res) + (size_t)nframes * 512;
	uint8_t *o = calloc(1, olen);
	if (!o)
		return ENOMEM;
	if (sysctlbyname("kern.sweet_rpmb_xfer", o, &olen, in, sizeof(in))) {
		int e = errno;
		free(o);
		memset(res, 0, sizeof(*res));
		res->err = e;
		return e;
	}
	memcpy(res, o, sizeof(*res));
	if (res->nframes)
		memcpy(out, o + sizeof(*res), (size_t)res->nframes * 512);
	free(o);
	return res->err;
}

static uint32_t rpmb_size_half, rpmb_rwc;	/* INIT answer, from kern.sweet_rpmb_info */

static void rpmb_info_load(void)
{
	char s[400] = "";
	size_t len = sizeof(s) - 1;
	if (sysctlbyname("kern.sweet_rpmb_info", s, &len, NULL, 0)) {
		lg("rpmb: kern.sweet_rpmb_info missing (%s): INIT will report failure", strerror(errno));
		return;
	}
	const char *p = strstr(s, " blocks=");
	const char *q = strstr(s, " rw_size=");
	unsigned long long blocks = p ? strtoull(p + 8, NULL, 10) : 0;
	rpmb_rwc = q ? (uint32_t)strtoul(q + 9, NULL, 10) : 0;
	rpmb_size_half = (uint32_t)blocks >> 1;	/* librpmb rpmb_ufs_init: low 32 bits of qLogicalBlockCount >> 1 */
	lg("rpmb: %s -> INIT size %u rel_wr_count %u dev_type 9", s, rpmb_size_half, rpmb_rwc);
}

/*
 * Phase 3 (owner's go, 2026-11-18): authenticated writes relayed through kern.sweet_rpmb_xfer op 2 (kern.sweet_rpmb must
 * be 2), only while a gatekeeper enroll/verify runs, and only when every frame is type 3 and its address range lies in the
 * GK partition blocks read in phase 2 (1642..1689). Anything else: refused, counted as a write attempt (halt).
 * librpmb rpmb_ufs_write: n / rel_wr_count chunks, each OUT rwc frames + OUT result request + IN 1; the result frame of
 * the last chunk goes back at sb+0x14.
 */
#define GK_BLK_LO 1642u
#define GK_BLK_HI 1690u		/* exclusive */
static int gk_writes_allowed;
static unsigned gk_writes_done;

static int32_t rpmb_relay_write(uint8_t *b, uint32_t sb_len, uint32_t n, uint32_t off, uint32_t rwc, uint32_t *rlen)
{
	if (!rwc)
		rwc = n;
	if (n < 1 || n % rwc || rwc > 64 || off + n * 512 > sb_len) {
		write_attempt("RPMB WRITE bad layout n %u rwc %u off %#x", n, rwc, off);
		return -1;
	}
	for (uint32_t i = 0; i < n; i++) {
		const uint8_t *f = b + off + i * 512;
		uint16_t ft = (uint16_t)(f[510] << 8 | f[511]);
		uint32_t addr = (uint32_t)(f[504] << 8 | f[505]), cnt = (uint32_t)(f[506] << 8 | f[507]);
		if (ft != 3 || addr < GK_BLK_LO || addr + (cnt ? cnt : 1) > GK_BLK_HI) {
			write_attempt("RPMB WRITE outside the rule: frame %u type %#06x addr %u count %u (GK range %u..%u)", i, ft,
				      addr, cnt, GK_BLK_LO, GK_BLK_HI - 1);
			return -1;
		}
	}
	if (sysctl_int("kern.sweet_rpmb") != 2) {
		write_attempt("RPMB WRITE while kern.sweet_rpmb != 2");
		return -1;
	}
	uint8_t res_frame[512] = { 0 };
	for (uint32_t c = 0; c < n / rwc; c++) {
		const uint8_t *fr = b + off + c * rwc * 512;
		uint32_t addr = (uint32_t)(fr[504] << 8 | fr[505]), cnt = (uint32_t)(fr[506] << 8 | fr[507]);
		size_t inl = 12 + (size_t)rwc * 512, olen = sizeof(struct rpmb_res) + 512;
		uint8_t *in = malloc(inl), o[sizeof(struct rpmb_res) + 512];
		put_u32(in, 2), put_u32(in + 4, rwc), put_u32(in + 8, rwc);
		memcpy(in + 12, fr, (size_t)rwc * 512);
		int r = sysctlbyname("kern.sweet_rpmb_xfer", o, &olen, in, inl);
		int e = r ? errno : (int)rd_u32(o);
		free(in);
		struct rpmb_res res;
		memcpy(&res, o, sizeof(res));
		uint16_t rt = 0, rr = 0;
		uint32_t wc = 0;
		if (!e && res.nframes == 1) {
			memcpy(res_frame, o + sizeof(res), 512);
			rt = (uint16_t)(res_frame[510] << 8 | res_frame[511]);
			rr = (uint16_t)(res_frame[508] << 8 | res_frame[509]);
			wc = (uint32_t)res_frame[500] << 24 | res_frame[501] << 16 | res_frame[502] << 8 | res_frame[503];
		}
		gk_writes_done++;
		lg("RPMB WRITE #%u chunk %u/%u: %u frame(s) addr %u count %u -> kernel err %d scsi %#x; result frame type %#06x "
		   "result %#06x write counter %u", gk_writes_done, c + 1, n / rwc, rwc, addr, cnt, e, r ? 0 : res.scsi_status,
		   rt, rr, wc);
		if (e)
			return -1;
	}
	memcpy(b + 0x14, res_frame, 512);
	*rlen = 512;
	return 0;
}

static uint32_t serve_rpmb(uint8_t *b, uint32_t sb_len)
{
	uint32_t cmd = g32(b);
	if (cmd == 0x101) {	/* INIT {cmd, version} */
		uint32_t ver = g32(b + 4);
		int32_t st = rpmb_size_half ? 0 : -1;
		note(0x2000, cmd, ver, rpmb_size_half);
		lg("RPMB req INIT version %u -> status %d size %u rel_wr_count %u", ver, st, rpmb_size_half, rpmb_rwc);
		memset(b, 0, 0x28);
		put_u32(b, cmd);
		if (ver == 0) {	/* librpmb's default answer */
			put_u32(b + 4, 1), put_u32(b + 8, 0xffffffffu);
			return 0x14;
		}
		put_u32(b + 4, ver == 1 ? 1 : 2), put_u32(b + 8, (uint32_t)st), put_u32(b + 0xc, rpmb_size_half);
		put_u32(b + 0x10, rpmb_rwc);
		if (ver == 1)
			return 0x14;
		put_u32(b + 0x14, 9);	/* dev_type UFS (rpmb_ufs_init stores 9) */
		return 0x28;
	}
	if (cmd == 0x104) {	/* partition config: no rpmb_parti_cfg file -> librpmb's {0x104, -1, 0, 0x10} */
		note(0x2000, cmd, g32(b + 4), 0);
		lg("RPMB req PARTITION-CONFIG version %#x -> -1 (no config file, as librpmb without one)", g32(b + 4));
		memset(b, 0, 0x10);
		put_u32(b, 0x104), put_u32(b + 4, 0xffffffffu), put_u32(b + 0xc, 0x10);
		return 0x10;
	}
	if (cmd == 0x102 || cmd == 0x103) {
		uint32_t n = g32(b + 4), blen = g32(b + 8), off = g32(b + 0xc), rwc = g32(b + 0x14);
		uint16_t t = off + 512 <= sb_len ? (uint16_t)(b[off + 510] << 8 | b[off + 511]) : 0xffff;
		note(0x2000, cmd, n, t);
		int32_t st = -1;
		uint32_t rlen = 0;
		if (cmd == 0x103 && gk_writes_allowed) {
			st = rpmb_relay_write(b, sb_len, n, off, rwc, &rlen);
		} else if (cmd == 0x103) {
			write_attempt("RPMB WRITE request: %u frame(s), first frame type %#06x, rel_wr_count %u, req_len %u (not relayed)",
				      n, t, rwc, blen);
		} else if (off + 512 > sb_len || n < 1 || n > 64 || 0x14 + n * 512 > sb_len) {
			lg("RPMB req READ: bad layout n %u len %u off %#x -> -1", n, blen, off);
		} else {
			/* librpmb rpmb_ufs_read: ceil(n / rel_wr_count) chunks; TZ puts one request frame per chunk at off,
			 * off+512, ...; each chunk = OUT its request frame + IN min(left, rel_wr_count) frames */
			uint32_t per = rpmb_rwc ? rpmb_rwc : n, chunks = (n + per - 1) / per, left = n, got = 0;
			uint8_t *out = malloc((size_t)n * 512);
			int e = out ? 0 : ENOMEM;
			if (off + chunks * 512 > sb_len)
				e = EINVAL;
			for (uint32_t c = 0; !e && c < chunks; c++) {
				uint32_t k = left < per ? left : per;
				uint8_t frame[512];
				struct rpmb_res res;
				memcpy(frame, b + off + c * 512, 512);
				uint16_t ft = (uint16_t)(frame[510] << 8 | frame[511]);
				e = rpmb_xfer_read(frame, k, out + (size_t)got * 512, &res);
				if (!e && res.nframes != k)
					e = EIO;
				const uint8_t *last = out + (size_t)(got + k - 1) * 512;
				lg("RPMB req READ chunk %u/%u type %#06x (%s) frames %u addr %u -> kernel err %d scsi %#x sense key %#x "
				   "asc %#x; last resp type %#06x result %#06x counter %u", c + 1, chunks, ft,
				   ft == 2 ? "counter" : ft == 4 ? "auth-read" : "?", k, (unsigned)(frame[504] << 8 | frame[505]), e,
				   res.scsi_status, res.sense[2] & 0xf, res.sense[12], e ? 0 : (unsigned)(last[510] << 8 | last[511]),
				   e ? 0 : (unsigned)(last[508] << 8 | last[509]),
				   e ? 0 : (unsigned)last[500] << 24 | last[501] << 16 | last[502] << 8 | last[503]);
				got += e ? 0 : k;
				left -= k;
			}
			if (!e) {
				memcpy(b + 0x14, out, (size_t)n * 512);
				st = 0;
				rlen = n * 512;
			}
			lg("RPMB req READ type %#06x n %u in %u chunk(s) -> status %d", t, n, chunks, st);
			free(out);
		}
		put_u32(b + 4, (uint32_t)st), put_u32(b + 8, rlen), put_u32(b + 0xc, 0x14);
		return 0x14 + rlen;	/* librpmb sends 0x14 but its frames are already in the sb; ours go back with the header */
	}
	note(0x2000, cmd, 0, 0);
	lg("RPMB req unknown cmd %#x -> -7 (as librpmb)", cmd);
	put_u32(b + 4, 0xfffffff9u), put_u32(b + 8, 0), put_u32(b + 0xc, 0x14);
	return 0x14;
}

/* ---- file system (libdrmfs fs_start @0x43b8; read-only) ---- */
static int fs_fds[32];
static unsigned n_fs_fds;

static int map_path(const char *tz, char *out, size_t len)
{
	if (strstr(tz, ".."))
		return -1;
	if (!strncmp(tz, "/persist/", 9))
		snprintf(out, len, "%s/%s", FS_ROOT, tz + 9);
	else if (!strncmp(tz, "/data/", 6))
		snprintf(out, len, "%s/%s", FS_DATA, tz + 6);
	else
		return -1;
	return 0;
}

static int own_fd(int fd)
{
	for (unsigned i = 0; i < n_fs_fds; i++)
		if (fs_fds[i] == fd)
			return 1;
	return 0;
}

/* struct stat -> libdrmfs's 0x68-byte answer (fs_start 0x47c8) */
static uint32_t fs_stat_resp(uint8_t *b, uint32_t cmd, int r, const struct stat *st)
{
	memset(b, 0, 0x68);
	put_u32(b, cmd);
	if (r) {
		put_u32(b + 0x64, 0xffffffffu);
		return 0x68;
	}
	put_u64(b + 0x04, (uint64_t)st->st_dev);
	put_u32(b + 0x14, st->st_mode), put_u32(b + 0x18, st->st_nlink), put_u32(b + 0x1c, st->st_uid);
	put_u32(b + 0x20, st->st_gid);
	put_u64(b + 0x24, (uint64_t)st->st_rdev);
	put_u64(b + 0x30, (uint64_t)st->st_size);
	put_u32(b + 0x38, (uint32_t)st->st_blksize);
	put_u64(b + 0x3c, (uint64_t)st->st_blocks);
	put_u32(b + 0x44, (uint32_t)st->st_atimespec.tv_sec), put_u32(b + 0x48, (uint32_t)st->st_atimespec.tv_nsec);
	put_u32(b + 0x4c, (uint32_t)st->st_mtimespec.tv_sec), put_u32(b + 0x50, (uint32_t)st->st_mtimespec.tv_nsec);
	put_u32(b + 0x54, (uint32_t)st->st_ctimespec.tv_sec), put_u32(b + 0x58, (uint32_t)st->st_ctimespec.tv_nsec);
	put_u64(b + 0x5c, (uint64_t)st->st_ino);
	return 0x68;
}

static uint32_t fs_ret(uint8_t *b, uint32_t cmd, int32_t ret)
{
	put_u32(b, cmd), put_u32(b + 4, (uint32_t)ret);
	return 8;
}

static uint32_t serve_fs(uint8_t *b, uint32_t sb_len)
{
	uint32_t cmd = g32(b);
	char tzp[0x101], path[512];
	switch (cmd) {
	case 0x203: {	/* open {cmd, dirfd, path[0x100] @+8, flags @+0x108} */
		uint32_t flags = g32(b + 0x108);
		memcpy(tzp, b + 8, 0x100), tzp[0x100] = 0;
		note(10, cmd, flags, 0);
		if (flags & (0x40 | 0x200 | 0x400)) {	/* Linux O_CREAT O_TRUNC O_APPEND */
			write_attempt("FS open \"%s\" flags %#x (create/truncate/append)", tzp, flags);
			return fs_ret(b, cmd, -1);
		}
		int fd = -1;
		if (map_path(tzp, path, sizeof(path)) == 0 && n_fs_fds < 32)
			fd = open(path, O_RDONLY);
		if (flags & 3)
			rw_opens++;
		if (fd >= 0)
			fs_fds[n_fs_fds++] = fd;
		lg("FS req OPEN \"%s\" flags %#x%s -> %s fd %d", tzp, flags, flags & 3 ? " (rw: served read-only)" : "", path, fd);
		return fs_ret(b, cmd, fd);
	}
	case 0x207: {	/* read {cmd, fd, count} -> {cmd, data[0x4e20] @+4, ret @+0x4e24} */
		int fd = (int)g32(b + 4);
		uint32_t cnt = g32(b + 8);
		note(10, cmd, (uint32_t)fd, cnt);
		ssize_t r = -1;
		if (own_fd(fd) && cnt <= 0x4e20 && 0x4e28 <= sb_len) {
			uint8_t tmp[0x4e20];
			r = read(fd, tmp, cnt);
			memset(b + 4, 0, 0x4e20);
			if (r > 0)
				memcpy(b + 4, tmp, (size_t)r);
		}
		lg("FS req READ fd %d count %u -> %zd", fd, cnt, r);
		put_u32(b, cmd), put_u32(b + 0x4e24, (uint32_t)(int32_t)r);
		return 0x4e28;
	}
	case 0x209: {	/* close {cmd, fd} */
		int fd = (int)g32(b + 4), r = -1;
		note(10, cmd, (uint32_t)fd, 0);
		for (unsigned i = 0; i < n_fs_fds; i++)
			if (fs_fds[i] == fd) {
				r = close(fd);
				fs_fds[i] = fs_fds[--n_fs_fds];
				break;
			}
		lg("FS req CLOSE fd %d -> %d", fd, r);
		return fs_ret(b, cmd, r);
	}
	case 0x20a: {	/* lseek {cmd, fd, s32 off, whence} */
		int fd = (int)g32(b + 4), wh = (int)g32(b + 0xc);
		int32_t off = (int32_t)g32(b + 8);
		note(10, cmd, (uint32_t)fd, (uint32_t)off);
		off_t r = own_fd(fd) && wh >= 0 && wh <= 2 ? lseek(fd, off, wh) : -1;
		lg("FS req LSEEK fd %d off %d whence %d -> %lld", fd, off, wh, (long long)r);
		return fs_ret(b, cmd, (int32_t)r);
	}
	case 0x20e: {	/* fstat {cmd, fd} */
		int fd = (int)g32(b + 4);
		struct stat st;
		note(10, cmd, (uint32_t)fd, 0);
		int r = own_fd(fd) ? fstat(fd, &st) : -1;
		lg("FS req FSTAT fd %d -> %d size %lld", fd, r, r ? -1LL : (long long)st.st_size);
		return fs_stat_resp(b, cmd, r, &st);
	}
	case 0x20f: {	/* lstat {cmd, path @+4} */
		struct stat st;
		memcpy(tzp, b + 4, 0x100), tzp[0x100] = 0;
		note(10, cmd, 0, 0);
		int r = map_path(tzp, path, sizeof(path)) ? -1 : lstat(path, &st);
		lg("FS req LSTAT \"%s\" -> %d", tzp, r);
		return fs_stat_resp(b, cmd, r, &st);
	}
	case 0x216:	/* fsync: nothing was written */
		note(10, cmd, g32(b + 4), 0);
		lg("FS req FSYNC fd %d -> 0", (int)g32(b + 4));
		return fs_ret(b, cmd, 0);
	case 0x219:	/* opendir {cmd, path @+4} -> {cmd, u64 dir, ret}: not served */
		memcpy(tzp, b + 4, 0x100), tzp[0x100] = 0;
		note(10, cmd, 0, 0);
		lg("FS req OPENDIR \"%s\" -> -1 (not served)", tzp);
		memset(b, 0, 0x10);
		put_u32(b, cmd), put_u32(b + 0xc, 0xffffffffu);
		return 0x10;
	case 0x202: case 0x204: case 0x206: case 0x208: case 0x20b: case 0x20c: case 0x210: case 0x211: {
		static const char *nm[] = { [0] = "mkdir", [2] = "unlinkat", [4] = "creat", [6] = "write", [9] = "link/rename",
					    [10] = "unlink", [14] = "link/copy", [15] = "link/copy" };
		note(10, cmd, g32(b + 4), 0);
		memcpy(tzp, b + 4, 0x100), tzp[0x100] = 0;
		for (int i = 0; i < 0x100; i++)
			if (tzp[i] && ((unsigned char)tzp[i] < 0x20 || (unsigned char)tzp[i] > 0x7e))
				tzp[i] = '.';
		write_attempt("FS %s (cmd %#x) word1 %#x \"%.60s\"", nm[cmd - 0x202] ? nm[cmd - 0x202] : "?", cmd, g32(b + 4), tzp);
		return fs_ret(b, cmd, -1);
	}
	default:
		note(10, cmd, g32(b + 4), 0);
		lg("FS req cmd %#x word1 %#x -> -1 (not served)", cmd, g32(b + 4));
		return fs_ret(b, cmd, -1);
	}
}

static uint32_t serve_time(uint8_t *b)
{
	uint32_t cmd = g32(b);
	note(11, cmd, g32(b + 4), 0);
	lg("TIME req cmd %#x word1 %#x -> -1 (logged only)", cmd, g32(b + 4));
	memset(b, 0, 0x10);
	put_u32(b, cmd), put_u32(b + 4, 0xffffffffu);
	return 0x10;
}

/* one INCOMPLETE round: serve listener id from its sb, answer TZ; x[] = the next result */
static void serve_one(uint64_t x[8], uint32_t id)
{
	struct lsnr *l = lsnr_find(id);
	if (!l) {
		note(id, 0xffffffffu, 0, 0);
		lg("TZ requests listener %#x (%u): not registered here: QSEOS_RESULT_FAILURE", id, id);
		uint64_t y[8] = { QSEE_SMC_LSTNR_RESP, QSEE_ARGS_LSTNR_RESP, id, QSEOS_RESULT_FAILURE };
		qsmc(y);
		memcpy(x, y, 4 * sizeof(uint64_t));
		return;
	}
	uint32_t st = QSEOS_RESULT_SUCCESS;
	if (pm_rd(l->sb.pa, sbuf, l->sb_len)) {
		lg("listener %#x: sb read failed", id);
		st = QSEOS_RESULT_FAILURE;
	} else {
		uint32_t n = id == 0x2000 ? serve_rpmb(sbuf, l->sb_len) : id == 10 ? serve_fs(sbuf, l->sb_len) : serve_time(sbuf);
		if (pm_wr(l->sb.pa, sbuf, n)) {
			lg("listener %#x: sb write failed", id);
			st = QSEOS_RESULT_FAILURE;
		}
	}
	l->served++;
	uint64_t y[8] = { QSEE_SMC_LSTNR_RESP_WL, QSEE_ARGS_LSTNR_RESP_WL, id, st, sgl.pa, 32 };
	qsmc(y);
	memcpy(x, y, 4 * sizeof(uint64_t));
}

/* ---- keymaster64 ---- */
static int km_cmd_allowed(uint32_t cmd, uint32_t uid)
{
	if (cmd == 0x200 || cmd == 0x205)
		return 1;
	if (cmd == 0x1002 && uid == GK_UID)
		return 1;
	if (cmd == 0x207 && uid == 0x207)	/* SET_VERSION: only the stock KeymasterUtils payload (cmd_km_setversion) */
		return 1;
	if (cmd == 0x1001 && uid == GK_UID && gk_writes_allowed)	/* phase 3: enroll of the private uid only */
		return 1;
	return 0;	/* 0x1001 other uids, 0x1003 delete_user, 0x1004 / 0x21004 delete_all, all 0x2xxxx CBOR: never */
}

/*
 * One keymaster64 command: req at sb+0 (req_len), rsp at sb+rsp_off (rsp_len), send-data (whitelist form when TZ has it),
 * listener requests served until SUCCESS/FAILURE. Returns 0 if TZ returned SUCCESS; sb read back into resp (rsp_len max).
 */
static int km_send(uint32_t appid, const uint8_t *req, uint32_t req_len, uint32_t sb_len, uint32_t rsp_off, uint32_t tz_rsp_len,
		   uint8_t *resp, uint32_t rsp_len, uint32_t uid)
{
	uint32_t cmd = rd_u32(req);
	if (!km_cmd_allowed(cmd, uid)) {
		lg("REFUSED: keymaster command %#x (uid %#x) is not allowed in this tool", cmd, uid);
		return -2;
	}
	uint64_t f[8] = { QSEE_SMC_FEAT_VERSION, QSEE_ARGS_FEAT_VERSION, QSEE_FEATURE_WHITELIST };
	int wl = !dryrun && !qsmc(f) && (uint32_t)f[1] >= QSEE_WHITELIST_V1;
	struct pbuf sb, arg;
	if (pbuf_get(&sb, sb_len, 4096, 0, 0xa0000000ull))
		return -1;
	if (pbuf_get(&arg, 4096, 4096, 0, 0xa0100000ull)) {
		pbuf_put(&sb);
		return -1;
	}
	uint8_t a[0x100] = { 0 };
	put_u64(a, sb.pa + rsp_off), put_u64(a + 8, tz_rsp_len);
	if (wl)
		put_u64(a + 16, arg.pa + 0x80), put_u64(a + 24, 8 * MAX_ION_FD);	/* empty sglistinfo table */
	uint8_t *z = calloc(1, sb_len);
	memcpy(z, req, req_len);
	int rc = -1;
	if (wr_chunks(sb.pa, z, sb_len) || wr_chunks(arg.pa, a, sizeof(a)))
		goto out;
	lg("km: app %u cmd %#x req %u bytes, rsp at +%#x len %#x, %s", appid, cmd, req_len, rsp_off, tz_rsp_len,
	   wl ? "SEND_DATA_WITH_WHITELIST" : "SEND_DATA");
	uint64_t x[8] = { wl ? QSEE_SMC_SEND_DATA_WL : QSEE_SMC_SEND_DATA, wl ? QSEE_ARGS_SEND_DATA_WL : QSEE_ARGS_SEND_DATA,
			  appid, sb.pa, req_len, arg.pa };
	qsmc(x);
	for (int round = 0; round < 4000; round++) {
		if ((int64_t)x[0]) {
			lg("km: smc error x0 %lld", (long long)x[0]);
			break;
		}
		uint32_t result = (uint32_t)x[1], type = (uint32_t)x[2], data = (uint32_t)x[3];
		if (result == QSEOS_RESULT_INCOMPLETE) {
			serve_one(x, data);
			continue;
		}
		if (result == QSEOS_RESULT_BLOCKED) {
			lg("km: BLOCKED_ON_LISTENER %#x session %#x: continue-blocked", data, type);
			uint64_t y[8] = { QSEE_SMC_CONT_BLOCKED_SI, QSEE_ARGS_CONT_BLOCKED, type };
			if (qsmc(y)) {
				uint64_t w[8] = { QSEE_SMC_CONT_BLOCKED, QSEE_ARGS_CONT_BLOCKED, appid };
				qsmc(w);
				memcpy(x, w, 4 * sizeof(uint64_t));
			} else {
				memcpy(x, y, 4 * sizeof(uint64_t));
			}
			continue;
		}
		if (result == QSEOS_RESULT_SUCCESS) {
			rc = 0;
		} else {
			lg("km: TZ result %#x resp_type %#x data %#x", result, type, data);
			rc = 1;
		}
		break;
	}
	if (rc == 0 && !dryrun && pm_rd(sb.pa + rsp_off, resp, rsp_len))
		rc = -1;
out:
	memset(z, 0, sb_len);
	wr_chunks(sb.pa, z, sb_len);	/* do not leave key material in freed PMEM */
	free(z);
	pbuf_put(&arg), pbuf_put(&sb);
	return rc;
}

static int km_appid(uint32_t *id)
{
	int r = lookup_name("keymaster64", id);
	if (r) {
		lg("keymaster64 is not loaded (lookup %d)", r);
		return -1;
	}
	lg("keymaster64: app id %u", *id);
	return 0;
}

static void print_notes(void)
{
	lg("listener requests seen: %u", n_notes);
	for (unsigned i = 0; i < n_notes && i < 256; i++)
		lg("  req %u: listener %#x cmd %#x a %#x b %#x", i, notes[i].lid, notes[i].cmd, notes[i].a, notes[i].b);
	for (unsigned i = 0; i < N_LSNR; i++)
		lg("  listener %#x (%s) served %u", lsnrs[i].id, lsnrs[i].name, lsnrs[i].served);
	lg("write attempts: %u%s; read-write opens served read-only: %u", write_attempts,
	   write_attempts ? " (RUN HALTED: see WRITE ATTEMPT lines)" : "", rw_opens);
	for (unsigned i = 0; i < write_attempts && i < 8; i++)
		lg("  write attempt %u: %s", i + 1, write_what[i]);
}

static int cmd_km_version(void)
{
	uint32_t km;
	if (km_appid(&km))
		return 2;
	uint8_t req[4], rsp[0x20] = { 0 };
	put_u32(req, 0x200);
	int rc = km_send(km, req, 4, 0xa000, 4, 0x9ffc, rsp, sizeof(rsp), 0);	/* as KeymasterUtils(): req sb+0 len 4, rsp sb+4 */
	lg("km GET_VERSION: rc %d status %d km %u.%u ta %u.%u -> %s", rc, (int)g32(rsp), g32(rsp + 4), g32(rsp + 8),
	   g32(rsp + 0xc), g32(rsp + 0x10),
	   rc ? "?" : (g32(rsp + 4) <= 4 && g32(rsp + 8) == 0) ? "LEGACY gatekeeper structs" : "CBOR gatekeeper");
	print_notes();
	return rc ? 2 : (int)g32(rsp) ? 5 : 0;
}

static int cmd_lsnr_test(void)
{
	int rc = 0;
	for (int pass = 1; pass <= 2; pass++) {
		lg("lsnr-test pass %d: register RPMB/FS/TIME, then deregister", pass);
		if (lsnr_all_up())
			rc = 2;
		lsnr_all_down();
		if (rc)
			break;
	}
	lg("lsnr-test: %s", rc ? "FAILED" : "ok (both passes)");
	return rc;
}

/* counter read through the kernel relay (no TZ involved): type 2 frame, random nonce */
static int cmd_rpmb_counter(void)
{
	uint8_t f[512] = { 0 }, out[512] = { 0 };
	arc4random_buf(f + 484, 16);
	f[511] = 0x02;
	struct rpmb_res res;
	int e = rpmb_xfer_read(f, 1, out, &res);
	uint16_t rt = (uint16_t)(out[510] << 8 | out[511]), rr = (uint16_t)(out[508] << 8 | out[509]);
	uint32_t wc = (uint32_t)out[500] << 24 | out[501] << 16 | out[502] << 8 | out[503];
	int nonce_ok = !memcmp(out + 484, f + 484, 16);
	lg("rpmb-counter: err %d (%s) scsi %#x sense key %#x asc %#x ascq %#x frames %u; resp type %#06x result %#06x "
	   "write counter %u nonce echoed %s", e, e ? strerror(e) : "ok", res.scsi_status, res.sense[2] & 0xf, res.sense[12],
	   res.sense[13], res.nframes, rt, rr, wc, nonce_ok ? "yes" : "no");
	return e || rt != 0x0200 || (rr & 0x7f) ? 2 : 0;
}

/* the kernel must refuse an authenticated write in mode 1 (garbage MAC: a device would reject it anyway) */
static int cmd_rpmb_refuse_test(void)
{
	int mode = sysctl_int("kern.sweet_rpmb");
	if (mode != 1) {
		lg("rpmb-refuse-test: kern.sweet_rpmb is %d, runs only at 1 (read-only)", mode);
		return 2;
	}
	uint8_t in[12 + 512] = { 0 }, o[64 + 512];
	put_u32(in, 2), put_u32(in + 4, 1), put_u32(in + 8, 1);
	in[12 + 511] = 0x03;	/* type 3 authenticated write, zero MAC, zero counter */
	size_t olen = sizeof(o);
	int r = sysctlbyname("kern.sweet_rpmb_xfer", o, &olen, in, sizeof(in));
	int e = r ? errno : 0;
	lg("rpmb-refuse-test: write op type 3 -> %s (%s)", r ? "refused" : "ACCEPTED", r ? strerror(e) : "!");
	uint8_t rin[12 + 512] = { 0 };
	put_u32(rin, 1), put_u32(rin + 4, 1);
	rin[12 + 511] = 0x03;	/* the same frame dressed as a read op */
	olen = sizeof(o);
	int r2 = sysctlbyname("kern.sweet_rpmb_xfer", o, &olen, rin, sizeof(rin));
	int e2 = r2 ? errno : 0;
	lg("rpmb-refuse-test: read op with a type-3 frame -> %s (%s)", r2 ? "refused" : "ACCEPTED", r2 ? strerror(e2) : "!");
	return (r && e == EPERM && r2 && e2 == EPERM) ? 0 : 2;
}

static int listeners_cmd_begin(void)
{
	int mode = sysctl_int("kern.sweet_rpmb");
	if (mode == 2) {
		lg("REFUSED: kern.sweet_rpmb is 2 (read/write); this phase runs only at 0/1");
		return -1;
	}
	lg("kern.sweet_rpmb = %d", mode);
	rpmb_info_load();
	return lsnr_all_up();
}

static int listeners_cmd_begin_rw(void)
{
	lg("kern.sweet_rpmb = %d (phase 3: GK-range writes relayed)", sysctl_int("kern.sweet_rpmb"));
	rpmb_info_load();
	return lsnr_all_up();
}

static int cmd_km_authkey(void)
{
	uint32_t km;
	if (km_appid(&km) || listeners_cmd_begin())
		return 2;
	uint8_t req[0x40] = { 0 }, rsp[0x3c0];
	put_u32(req, 0x205), put_u32(req + 4, 2);	/* fpc_tee_init_hw_auth: {0x205, 2}, req 0x40, rsp +0x40 len 0x3c0 */
	int rc = km_send(km, req, 0x40, 0x1000, 0x40, 0x3c0, rsp, sizeof(rsp), 0);
	/* the key blob is never printed or stored: status and length only */
	int32_t kst = (int32_t)g32(rsp);
	lg("km GET_AUTH_TOKEN_KEY: rc %d status %d, blob length field %u (resp+8), resp+4 %#x", rc, kst, g32(rsp + 8),
	   g32(rsp + 4));
	memset(rsp, 0, sizeof(rsp));
	lsnr_all_down();
	print_notes();
	return write_attempts ? 6 : rc ? 2 : kst ? 5 : 0;
}

static int cmd_gk_verify_dummy(void)
{
	uint32_t km;
	int mode = sysctl_int("kern.sweet_rpmb");
	if (mode != 1) {
		lg("REFUSED: gk-verify-dummy runs only with kern.sweet_rpmb = 1 (now %d)", mode);
		return 2;
	}
	if (km_appid(&km) || listeners_cmd_begin())
		return 2;
	/* password_handle_t (58 bytes, packed): version 2, user_id, flags 0, salt, signature[32] zero, hardware_backed 1 */
	uint8_t handle[58] = { 0 };
	handle[0] = 2;
	put_u64(handle + 1, 0x7e0000017e000001ull);
	put_u64(handle + 17, 0x5a5a5a5a5a5a5a5aull);
	handle[57] = 1;
	static const char pwd[] = "redfin-gk-dummy-x";	/* 17 bytes, no NUL; never a real password */
	uint32_t plen = (uint32_t)strlen(pwd), hlen = sizeof(handle);
	uint8_t req[0x20 + 58 + 32] = { 0 };
	put_u32(req, 0x1002), put_u32(req + 4, GK_UID), put_u64(req + 8, 0);	/* challenge 0 */
	put_u32(req + 0x10, 0x20), put_u32(req + 0x14, hlen);
	put_u32(req + 0x18, 0x20 + hlen), put_u32(req + 0x1c, plen);
	memcpy(req + 0x20, handle, hlen);
	memcpy(req + 0x20 + hlen, pwd, plen);
	uint32_t req_len = 0x20 + hlen + plen;
	uint32_t rsp_off = (req_len + 3) & ~3u;
	uint8_t rsp[0x40] = { 0 };
	lg("gk verify (legacy 0x1002): uid %#x challenge 0, dummy handle %u bytes, password %u bytes; req %u, rsp at +%#x", GK_UID,
	   hlen, plen, req_len, rsp_off);
	int rc = km_send(km, req, req_len, 0xa000, rsp_off, 0xa000 - rsp_off, rsp, sizeof(rsp), GK_UID);
	lg("gk verify: rc %d status %d hat_off %u hat_len %u word3 %#x word4 %#x (HAT never printed)", rc, (int)g32(rsp),
	   g32(rsp + 4), g32(rsp + 8), g32(rsp + 0xc), g32(rsp + 0x10));
	memset(rsp, 0, sizeof(rsp));
	lsnr_all_down();
	uint32_t km2 = 0;
	lookup_name("keymaster64", &km2);
	lg("after verify: keymaster64 app id %u (%s)", km2, km2 == km ? "unchanged" : "CHANGED");
	print_notes();
	return write_attempts ? 6 : rc ? 2 : 0;
}

/* ---- phase 3 ---- */
static int rpmb_counter_now(uint32_t *wc)
{
	uint8_t f[512] = { 0 }, out[512] = { 0 };
	arc4random_buf(f + 484, 16);
	f[511] = 0x02;
	struct rpmb_res res;
	if (rpmb_xfer_read(f, 1, out, &res) || memcmp(out + 484, f + 484, 16) || (out[508] << 8 | out[509]) & 0x7f)
		return -1;
	*wc = (uint32_t)out[500] << 24 | out[501] << 16 | out[502] << 8 | out[503];
	return 0;
}

static void counter_log(const char *when)
{
	uint32_t wc;
	if (rpmb_counter_now(&wc))
		lg("RPMB write counter %s: read failed", when);
	else
		lg("RPMB write counter %s: %u", when, wc);
}

/* KeymasterUtils::KeymasterUtils: SET_VERSION {0x207, 4, 5, 4, 5, 0}, req 0x18, rsp sb+0x18 len 0x9fe8 (kmdu 0x2524) */
static int cmd_km_setversion(void)
{
	uint32_t km;
	int mode = sysctl_int("kern.sweet_rpmb");
	if (mode != 1) {
		lg("REFUSED: km-setversion runs with kern.sweet_rpmb = 1 (writes refused), now %d", mode);
		return 2;
	}
	if (km_appid(&km) || listeners_cmd_begin())
		return 2;
	counter_log("before SET_VERSION");
	uint8_t req[0x18] = { 0 }, rsp[0x20] = { 0 };
	put_u32(req, 0x207), put_u32(req + 4, 4), put_u32(req + 8, 5), put_u32(req + 0xc, 4), put_u32(req + 0x10, 5);
	int rc = km_send(km, req, 0x18, 0xa000, 0x18, 0x9fe8, rsp, sizeof(rsp), 0x207);
	lg("km SET_VERSION {0x207, 4, 5, 4, 5}: rc %d status %d words %#x %#x", rc, (int)g32(rsp), g32(rsp + 4), g32(rsp + 8));
	lsnr_all_down();
	counter_log("after SET_VERSION");
	print_notes();
	return write_attempts ? 6 : rc ? 2 : (int)g32(rsp) ? 5 : 0;
}

#define GK_DIR TZ_DIR "/gk"
#define GK_HANDLE GK_DIR "/handle-7e000001"

static int read_pw(char *pw, size_t cap)
{
	if (!fgets(pw, (int)cap, stdin))
		return -1;
	pw[strcspn(pw, "\r\n")] = 0;
	size_t n = strlen(pw);
	return n >= 8 && n < 64 ? (int)n : -1;
}

static int gk_phase3_begin(uint32_t *km)
{
	int mode = sysctl_int("kern.sweet_rpmb");
	if (mode != 2) {
		lg("REFUSED: enroll/verify need kern.sweet_rpmb = 2 (now %d)", mode);
		return -1;
	}
	if (km_appid(km) || listeners_cmd_begin_rw())
		return -1;
	gk_writes_allowed = 1;
	return 0;
}

static void gk_phase3_end(void)
{
	gk_writes_allowed = 0;
	lsnr_all_down();
}

/* password on stdin; the handle goes to GK_HANDLE (0600) */
static int cmd_gk_enroll(void)
{
	char pw[80];
	int plen = read_pw(pw, sizeof(pw));
	if (plen < 0) {
		lg("gk-enroll: password (8..63 chars) expected on stdin");
		return 2;
	}
	struct stat st;
	if (!stat(GK_HANDLE, &st)) {
		lg("gk-enroll: %s exists: refusing a second enroll", GK_HANDLE);
		return 2;
	}
	uint32_t km;
	if (gk_phase3_begin(&km))
		return 2;
	counter_log("before ENROLL");
	uint8_t req[0x20 + 64] = { 0 }, rsp[0x200] = { 0 };
	put_u32(req, 0x1001), put_u32(req + 4, GK_UID);	/* no current handle / password */
	put_u32(req + 0x18, 0x20), put_u32(req + 0x1c, (uint32_t)plen);
	memcpy(req + 0x20, pw, (size_t)plen);
	uint32_t req_len = 0x20 + (uint32_t)plen, rsp_off = (req_len + 3) & ~3u;
	lg("gk ENROLL (legacy 0x1001): uid %#x, new password %d bytes (not shown), req %u, rsp +%#x", GK_UID, plen, req_len,
	   rsp_off);
	int rc = km_send(km, req, req_len, 0xa000, rsp_off, 0xa000 - rsp_off, rsp, sizeof(rsp), GK_UID);
	memset(req, 0, sizeof(req)), memset(pw, 0, sizeof(pw));
	int32_t kst = (int32_t)g32(rsp);
	uint32_t hoff = g32(rsp + 4), hlen = g32(rsp + 8);
	lg("gk ENROLL: rc %d status %d handle_off %u handle_len %u retry %#x", rc, kst, hoff, hlen, g32(rsp + 0xc));
	gk_phase3_end();
	counter_log("after ENROLL");
	int ok = !rc && !kst && hlen && hlen <= 0x100 && hoff + hlen <= sizeof(rsp);
	if (ok) {
		const uint8_t *h = rsp + hoff;
		uint64_t sid = 0;
		for (int i = 8; i >= 1; i--)
			sid = sid << 8 | h[i];	/* password_handle_t: u8 version, u64 user_id (LE) */
		mkdir(GK_DIR, 0700);
		int fd = open(GK_HANDLE, O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd < 0 || write(fd, h, hlen) != (ssize_t)hlen)
			ok = 0;
		if (fd >= 0)
			close(fd);
		lg("gk ENROLL: handle %u bytes, version %u, secure user id (SID) %#llx, hw-backed %u -> %s%s", hlen, h[0],
		   (unsigned long long)sid, hlen >= 58 ? h[57] : 0, GK_HANDLE, ok ? "" : " (SAVE FAILED)");
	}
	print_notes();
	return write_attempts ? 6 : ok ? 0 : 5;
}

/* verify with the saved handle; hat (69 bytes) returned to the caller only, never printed except id fields */
static int gk_verify_real(uint32_t km, uint64_t challenge, const char *pw, uint8_t hat[69], uint32_t *hat_len)
{
	uint8_t handle[0x100];
	int fd = open(GK_HANDLE, O_RDONLY);
	ssize_t hl = fd >= 0 ? read(fd, handle, sizeof(handle)) : -1;
	if (fd >= 0)
		close(fd);
	if (hl <= 0) {
		lg("gk verify: no saved handle %s", GK_HANDLE);
		return -1;
	}
	uint32_t plen = (uint32_t)strlen(pw);
	uint8_t req[0x20 + 0x100 + 64] = { 0 }, rsp[0x200] = { 0 };
	put_u32(req, 0x1002), put_u32(req + 4, GK_UID), put_u64(req + 8, challenge);
	put_u32(req + 0x10, 0x20), put_u32(req + 0x14, (uint32_t)hl);
	put_u32(req + 0x18, 0x20 + (uint32_t)hl), put_u32(req + 0x1c, plen);
	memcpy(req + 0x20, handle, (size_t)hl);
	memcpy(req + 0x20 + hl, pw, plen);
	uint32_t req_len = 0x20 + (uint32_t)hl + plen, rsp_off = (req_len + 3) & ~3u;
	counter_log("before VERIFY");
	lg("gk VERIFY (legacy 0x1002): uid %#x challenge %#llx, handle %zd bytes, password %u bytes (not shown)", GK_UID,
	   (unsigned long long)challenge, hl, plen);
	int rc = km_send(km, req, req_len, 0xa000, rsp_off, 0xa000 - rsp_off, rsp, sizeof(rsp), GK_UID);
	memset(req, 0, sizeof(req));
	int32_t kst = (int32_t)g32(rsp);
	uint32_t hoff = g32(rsp + 4), hlen = g32(rsp + 8);
	lg("gk VERIFY: rc %d status %d hat_off %u hat_len %u retry_timeout %#x", rc, kst, hoff, hlen, g32(rsp + 0xc));
	*hat_len = 0;
	if (!rc && !kst && hlen == 69 && hoff + hlen <= sizeof(rsp)) {
		const uint8_t *t = rsp + hoff;
		memcpy(hat, t, 69);
		*hat_len = 69;
		uint64_t ch = 0, uid = 0, aid = 0;
		for (int i = 8; i >= 1; i--)
			ch = ch << 8 | t[i];
		for (int i = 16; i >= 9; i--)
			uid = uid << 8 | t[i];
		for (int i = 24; i >= 17; i--)
			aid = aid << 8 | t[i];
		uint32_t atype = (uint32_t)t[25] << 24 | t[26] << 16 | t[27] << 8 | t[28];
		lg("gk VERIFY: HAT version %u challenge %#llx%s user_id(SID) %#llx authenticator_id %#llx authenticator_type %u%s "
		   "(HMAC not shown)", t[0], (unsigned long long)ch, ch == challenge ? " (echoed)" : " (NOT the challenge)",
		   (unsigned long long)uid, (unsigned long long)aid, atype, atype == 1 ? " PASSWORD" : "");
	}
	memset(rsp, 0, sizeof(rsp));
	counter_log("after VERIFY");
	return rc ? -1 : kst;
}

static int cmd_gk_verify(uint64_t challenge)
{
	char pw[80];
	if (read_pw(pw, sizeof(pw)) < 0) {
		lg("gk-verify: password expected on stdin");
		return 2;
	}
	uint32_t km, hl;
	uint8_t hat[69];
	if (gk_phase3_begin(&km))
		return 2;
	int r = gk_verify_real(km, challenge, pw, hat, &hl);
	memset(pw, 0, sizeof(pw)), memset(hat, 0, sizeof(hat));
	gk_phase3_end();
	print_notes();
	return write_attempts ? 6 : r ? 5 : hl == 69 ? 0 : 5;
}

/*
 * FPC hw-auth chain, no finger (fpc HAL: fpc_tee_init_hw_auth @0xfee8, fpc_tee_get_enrol_challenge @0xf800,
 * fpc_tee_authorize_enrol @0xf84c): KM 0x205 key blob (memory only) -> FPC {3,5, status, len, blob} ->
 * FPC {3,2} challenge (u64 at +0x10) -> GK verify(challenge) -> FPC {3,3, status, len, HAT} authorize_enrol.
 * Password on stdin. kern.sweet_rpmb must be 2 (verify writes the GK failure record).
 */
static int cmd_fpc_hwauth(uint32_t appid)
{
	char pw[80];
	if (read_pw(pw, sizeof(pw)) < 0) {
		lg("fpc-hwauth: password expected on stdin");
		return 2;
	}
	if (check_appid(appid))
		return 2;
	uint32_t km, hl = 0;
	if (gk_phase3_begin(&km))
		return 2;
	int rc = 5;
	uint8_t rsp[0x3c0] = { 0 }, d[0x400] = { 0 }, hat[69] = { 0 };
	uint8_t req[0x40] = { 0 };
	put_u32(req, 0x205), put_u32(req + 4, 2);
	int r = km_send(km, req, 0x40, 0x1000, 0x40, 0x3c0, rsp, sizeof(rsp), 0);
	int32_t kst = (int32_t)g32(rsp);
	uint32_t koff = g32(rsp + 4), klen = g32(rsp + 8);
	lg("fpc-hwauth: KM GET_AUTH_TOKEN_KEY rc %d status %d key blob length %u (not shown)", r, kst, klen);
	if (r || kst || !klen || klen > sizeof(d) - 0x10 || koff + klen > sizeof(rsp))
		goto out;
	put_u32(d, 3), put_u32(d + 4, 5), put_u32(d + 0xc, klen);
	memcpy(d + 0x10, rsp + koff, klen);
	memset(rsp, 0, sizeof(rsp));
	r = fpc_send64(appid, d, 0x10 + klen, "fpc init_hw_auth (3,5)");
	int32_t ist = (int32_t)g32(d + 8);
	lg("fpc-hwauth: init_hw_auth (3,5): send rc %d TA status %d", r, ist);
	memset(d, 0, sizeof(d));
	if (r || ist)
		goto out;
	put_u32(d, 3), put_u32(d + 4, 2);
	r = fpc_send64(appid, d, FPC_SENSOR_LEN, "fpc get_enrol_challenge (3,2)");	/* the HAL's shared 0x58 buffer */
	uint64_t ch = (uint64_t)g32(d + 0x10) | (uint64_t)g32(d + 0x14) << 32;
	int32_t cst = (int32_t)g32(d + 8);
	lg("fpc-hwauth: get_enrol_challenge (3,2): send rc %d TA status %d challenge %#llx", r, cst, (unsigned long long)ch);
	if (r || cst || !ch)
		goto out;
	int v = gk_verify_real(km, ch, pw, hat, &hl);
	if (v || hl != 69)
		goto out;
	memset(d, 0, sizeof(d));
	put_u32(d, 3), put_u32(d + 4, 3), put_u32(d + 0xc, 69);
	memcpy(d + 0x10, hat, 69);
	r = fpc_send64(appid, d, 0x10 + 69, "fpc authorize_enrol (3,3)");
	lg("fpc-hwauth: authorize_enrol (3,3) with the HAT: send rc %d TA status %d", r, (int)g32(d + 8));
	rc = r || g32(d + 8) ? 5 : 0;
out:
	memset(pw, 0, sizeof(pw)), memset(hat, 0, sizeof(hat)), memset(d, 0, sizeof(d)), memset(rsp, 0, sizeof(rsp));
	gk_phase3_end();
	print_notes();
	lg("fpc-hwauth: %s", write_attempts ? "HALTED (write attempt)" : rc ? "FAILED (see statuses)" : "ALL PASSED: enrol-test can run with a finger");
	return write_attempts ? 6 : rc;
}

static int lock_fd = -1;
static int qsee_lock(void)
{
	mkdir(TZ_DIR, 0700);
	mkdir(TZ_DIR "/log", 0700);
	lock_fd = open(TZ_DIR "/qsee.lock", O_RDWR | O_CREAT, 0600);
	if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB)) {
		fprintf(stderr, "qseeprobe: %s/qsee.lock is held by another SMC user\n", TZ_DIR);
		return -1;
	}
	lsnr_log = fopen(TZ_DIR "/log/lsnr.log", "a");
	return 0;
}
