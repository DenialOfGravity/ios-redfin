/*
 * redfin 1195: device passcode for SweetKeyStore (AppleKeyStore) and SweetACM.
 * Design: logs/research/passcode-acm-design-2026-11-18.md.
 *
 * DESIGN LIMIT: the passcode is an authentication gate for the UI / ACM / Touch ID, never an encryption key.
 * Nothing is encrypted with it and no data is made unavailable: the kernel's file layer never sees a lock
 * (no cp_key_store_action), keychain wrap/unwrap ignore the lock state, and "unlocked since boot" is always
 * reported. "Locked" is only what the AKS lock-state selectors tell userland.
 *
 * kern.sweet_passcode (root, default 0): 0 = today's behaviour exactly (the stored verifier is not read);
 * 1 = passcode + UI lock state; 2 = passcode, never reported locked; 3 = like 1, locked at boot.
 * Boot-arg sweetpasscode=N sets it at boot; sweetpasscode=0 pins it off for the boot (writes refused).
 * kern.sweet_passcode_reset=1 (root) stores "no passcode". kern.sweet_passcode_state (RO).
 *
 * Verifier: /var/local/keystore/passcode.v1 (root 0600, folder 0700), one line
 *   "SWPC1 <iter> <salt hex> <PBKDF2-HMAC-SHA256 hex>\n" or "SWPC1 none\n".
 * The passcode itself is never stored and never logged (only its length).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <sys/random.h>
#include <sys/uio.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

extern int sweet_vfs_mkdir(const char *path, int mode);
extern void sweet_acm_publish(void);            /* SweetACM.cpp */
extern void sweet_aks_notify(void);             /* SweetKeyStore.cpp (1196) */
extern void sweet_mesa_lockstate_changed(void); /* SweetMesa.cpp (1225): 0xe3ff800a push, kern.sweet_mesa_lockout */

int sweet_pc_mode(void);
int sweet_pc_is_set(void);
void sweet_pc_aks_state(uint32_t *ss, uint32_t *sls, uint32_t *sfa, uint32_t *sb);
int sweet_pc_check_user(uint64_t uaddr, uint32_t len, int unlock);
int sweet_pc_check_kbuf(const uint8_t *pw, uint32_t len, int unlock);
int sweet_pc_change_user(uint64_t old_ua, uint32_t old_len, uint64_t new_ua, uint32_t new_len);
void sweet_pc_lock(void);
int sweet_pc_locked(void);

#define SAY(fmt, ...)   printf("SWEET PASSCODE: " fmt, ##__VA_ARGS__)
#define PC_DIR          "/var/local/keystore"
#define PC_FILE         PC_DIR "/passcode.v1"
#define PC_ITER         100000
#define PC_ITER_MIN     1000
#define PC_ITER_MAX     10000000
#define PC_SALT         16
#define PC_DK           32
#define PC_MAXLEN       128
#define PC_FREE_FAILS   5
#define PC_BACKOFF_MAX  900

enum { ST_UNKNOWN = 0, ST_NONE, ST_SET, ST_BAD };

static LCK_GRP_DECLARE(sweet_pc_grp, "sweet_passcode");
static LCK_MTX_DECLARE(sweet_pc_lck, &sweet_pc_grp);

static int pc_inited, pc_mode, pc_pinned;
static int pc_stored = ST_UNKNOWN;
static uint32_t pc_iter;
static uint8_t pc_salt[PC_SALT], pc_dk[PC_DK];
static int pc_locked, pc_lock_at_load = 1;      /* mode 3: the first load after boot starts locked */
static uint32_t pc_fails;
static clock_sec_t pc_last_try, pc_backoff_until;
static uint32_t pc_n_ok, pc_n_bad, pc_n_busy;
static int pc_unlocked_event;                 /* 1196: set under the lock, sent after it */
/* 1198: Touch ID unlock rules (as iOS): a passcode since boot, at most 48 h ago, fewer than 5 failed matches */
#define PC_BIO_TTL_S            (48 * 3600)
#define PC_BIO_MAX_FAILS        5
static clock_sec_t pc_pass_ok_at;
static uint32_t pc_bio_fails, pc_n_bio_unlock, pc_n_bio_refused;
/*
 * 1214: InBioUnlock. After a Touch ID keybag unlock real AKS reports the extended lock state 7 ("InBioUnlock" in
 * SpringBoardFoundation) until SpringBoard confirms it with aks_unlock_device() without a passcode
 * (SBFUserAuthenticationController _evaluateBiometricAttempt: -> _isInBioUnlockState -> _performNilPasscodeCheck ->
 * MKBUnlockDevice(nil)). SBUIBiometricResource _reallySetAuthenticated:keybagState: turns "authenticated + extended
 * lockState 7" into its "bio unlocked" event (SBUIBiometricEvent 4, "Identity match reported by the keybag -
 * BioUnlocked"). Event 4 is what the passcode pad needs (SBUIPasscodeLockViewBase -> autofill -> passcodeEntered
 * ViaMesa -> attemptUnlock) and what the lock screen's Mesa unlock behavior turns into bioUnlock + success feedback.
 * Without it the pad stays up after a match and no unlock feedback is played (touchid-passcodepad-2026-11-18.md).
 * kern.sweet_passcode_biostate: 0 = off (lock state 0 at once, as 1198), 1 = state 7 on aks_get_extended_device_state
 * (sel 35 only; sel 17 and aks_get_lock_state still say unlocked) until confirmed, locked, or PC_BIO_STATE_S.
 */
#define PC_BIO_STATE_S          30
/*
 * 1217: NOT native. iOS gives its own feedback for a Touch ID unlock (SBAuthenticationFeedback initForSuccess:
 * vibrates YES, played by SpringBoard after the "bio unlocked" event that 1214 enables). If that does not reach the
 * motor, kern.sweet_passcode_bio_buzz = ms | strength << 16 (ms 1..2000, strength 1..127; 0 = off, the default)
 * makes the kernel play one pulse itself (sweet_haptic.c, needs boot-arg sweethaptic) when Touch ID unlocks the keybag.
 */
extern int sweet_haptic_request(unsigned ms, unsigned strength);
static int pc_bio_buzz;
static int pc_biostate;
static clock_sec_t pc_bio_pending_at;
static uint32_t pc_n_bio_confirm, pc_n_bio_expired, pc_n_bio_nil_refused;

static clock_sec_t
pc_now(void)
{
	clock_sec_t s;
	clock_usec_t us;
	clock_get_system_microtime(&s, &us);
	return s;
}

/* 1196: tell MobileKeyBag clients (SpringBoard) that the keybag state changed. Never with the lock held. */
static uint32_t pc_n_notify;
static void
pc_notify(const char *why)
{
	pc_n_notify++;
	SAY("keybag status event (%s)\n", why);
	sweet_aks_notify();
	sweet_mesa_lockstate_changed();        /* 1225: no-op unless kern.sweet_mesa_lockout bit 3 */
}

static void
pc_init(void)
{
	if (pc_inited) {
		return;
	}
	int v = 0;
	if (PE_parse_boot_argn("sweetpasscode", &v, sizeof(v))) {
		if (v < 0 || v > 3) {
			v = 0;
		}
		pc_mode = v;
		pc_pinned = (v == 0);
	}
	pc_inited = 1;
	SAY("mode %d%s (boot)\n", pc_mode, pc_pinned ? " pinned by boot-arg sweetpasscode=0" : "");
}

/* ---- SHA-256 / HMAC / PBKDF2 (self-contained: no corecrypto on SWEET) ---- */
typedef struct {
	uint32_t h[8];
	uint64_t n;
	uint8_t b[64];
	uint32_t bl;
} pc_sha;

static const uint32_t pc_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void
pc_sha_block(uint32_t *h, const uint8_t *p)
{
	uint32_t w[64], a, b, c, d, e, f, g, hh;
	for (int i = 0; i < 16; i++) {
		w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + pc_k[i] + w[i];
		uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void
pc_sha_init(pc_sha *s)
{
	static const uint32_t iv[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};
	memcpy(s->h, iv, sizeof(iv));
	s->n = 0;
	s->bl = 0;
}

static void
pc_sha_update(pc_sha *s, const uint8_t *p, size_t len)
{
	s->n += len;
	while (len > 0) {
		uint32_t k = 64 - s->bl;
		if (k > len) {
			k = (uint32_t)len;
		}
		memcpy(s->b + s->bl, p, k);
		s->bl += k;
		p += k;
		len -= k;
		if (s->bl == 64) {
			pc_sha_block(s->h, s->b);
			s->bl = 0;
		}
	}
}

static void
pc_sha_final(pc_sha *s, uint8_t out[32])
{
	uint64_t bits = s->n * 8;
	uint8_t pad = 0x80, z = 0, l[8];
	pc_sha_update(s, &pad, 1);
	while (s->bl != 56) {
		pc_sha_update(s, &z, 1);
	}
	for (int i = 0; i < 8; i++) {
		l[i] = (uint8_t)(bits >> (56 - 8 * i));
	}
	pc_sha_update(s, l, 8);
	for (int i = 0; i < 8; i++) {
		out[4 * i] = (uint8_t)(s->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)s->h[i];
	}
}

/* PBKDF2-HMAC-SHA256, one 32-byte block (dkLen 32). */
static void
pc_pbkdf2(const uint8_t *pw, uint32_t pwlen, const uint8_t *salt, uint32_t saltlen, uint32_t iter,
    uint8_t out[PC_DK])
{
	uint8_t key[64], ipad[64], opad[64], u[32], t[32];
	pc_sha in0, out0, s;
	memset(key, 0, sizeof(key));
	if (pwlen > 64) {
		pc_sha_init(&s);
		pc_sha_update(&s, pw, pwlen);
		pc_sha_final(&s, key);
	} else {
		memcpy(key, pw, pwlen);
	}
	for (int i = 0; i < 64; i++) {
		ipad[i] = key[i] ^ 0x36;
		opad[i] = key[i] ^ 0x5c;
	}
	pc_sha_init(&in0);
	pc_sha_update(&in0, ipad, 64);
	pc_sha_init(&out0);
	pc_sha_update(&out0, opad, 64);
	/* U1 = HMAC(P, salt || INT(1)) */
	static const uint8_t one[4] = { 0, 0, 0, 1 };
	s = in0;
	pc_sha_update(&s, salt, saltlen);
	pc_sha_update(&s, one, 4);
	pc_sha_final(&s, u);
	s = out0;
	pc_sha_update(&s, u, 32);
	pc_sha_final(&s, u);
	memcpy(t, u, 32);
	for (uint32_t i = 1; i < iter; i++) {
		s = in0;
		pc_sha_update(&s, u, 32);
		pc_sha_final(&s, u);
		s = out0;
		pc_sha_update(&s, u, 32);
		pc_sha_final(&s, u);
		for (int j = 0; j < 32; j++) {
			t[j] ^= u[j];
		}
	}
	memcpy(out, t, PC_DK);
	bzero(key, sizeof(key));
	bzero(ipad, sizeof(ipad));
	bzero(opad, sizeof(opad));
	bzero(u, sizeof(u));
	bzero(t, sizeof(t));
	bzero(&in0, sizeof(in0));
	bzero(&out0, sizeof(out0));
	bzero(&s, sizeof(s));
}

static int
pc_ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
	uint8_t d = 0;
	for (size_t i = 0; i < n; i++) {
		d |= a[i] ^ b[i];
	}
	return d == 0;
}

/* ---- verifier file ---- */
static void
pc_hex(char *o, const uint8_t *p, size_t n)
{
	static const char hx[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) {
		o[2 * i] = hx[p[i] >> 4];
		o[2 * i + 1] = hx[p[i] & 15];
	}
	o[2 * n] = 0;
}

static int
pc_unhex(const char *s, uint8_t *p, size_t n)
{
	for (size_t i = 0; i < 2 * n; i++) {
		char c = s[i];
		int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
		if (v < 0) {
			return -1;
		}
		if (i & 1) {
			p[i / 2] |= (uint8_t)v;
		} else {
			p[i / 2] = (uint8_t)(v << 4);
		}
	}
	return 0;
}

/* lock held */
static void
pc_load(void)
{
	if (pc_stored != ST_UNKNOWN) {
		return;
	}
	clock_sec_t now = pc_now();
	if (pc_last_try != 0 && now - pc_last_try < 2) {
		return;
	}
	pc_last_try = now;
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	char buf[192];
	int resid = 0;
	int err = vnode_open(PC_FILE, FREAD, 0, 0, &vp, ctx);
	if (err) {
		vnode_t dv = NULLVP;
		if (err == ENOENT && vnode_lookup("/var/local", 0, &dv, ctx) == 0) {
			vnode_put(dv);
			pc_stored = ST_NONE;
			SAY("no verifier file: no passcode\n");
		}
		return;         /* else the data volume is not there yet: unknown, retried */
	}
	bzero(buf, sizeof(buf));
	err = vn_rdwr(UIO_READ, vp, buf, sizeof(buf) - 1, 0, UIO_SYSSPACE, IO_UNIT, vfs_context_ucred(ctx), &resid,
	    vfs_context_proc(ctx));
	vnode_close(vp, FREAD, ctx);
	if (err) {
		SAY("verifier read error %d: treated as no passcode (BAD)\n", err);
		pc_stored = ST_BAD;
		return;
	}
	if (strncmp(buf, "SWPC1 none", 10) == 0) {
		pc_stored = ST_NONE;
		SAY("verifier file: no passcode\n");
		return;
	}
	/* SWPC1 <iter> <32 hex> <64 hex> */
	char *p = buf;
	if (strncmp(p, "SWPC1 ", 6) != 0) {
		goto bad;
	}
	p += 6;
	uint32_t it = 0;
	while (*p >= '0' && *p <= '9' && it < PC_ITER_MAX) {
		it = it * 10 + (uint32_t)(*p++ - '0');
	}
	if (*p++ != ' ' || it < PC_ITER_MIN || it > PC_ITER_MAX) {
		goto bad;
	}
	if (pc_unhex(p, pc_salt, PC_SALT) || p[2 * PC_SALT] != ' ') {
		goto bad;
	}
	p += 2 * PC_SALT + 1;
	if (pc_unhex(p, pc_dk, PC_DK)) {
		goto bad;
	}
	pc_iter = it;
	pc_stored = ST_SET;
	if (pc_lock_at_load && pc_mode == 3) {
		pc_locked = 1;
	}
	pc_lock_at_load = 0;
	SAY("verifier loaded (iter %u)%s\n", it, pc_locked ? ", locked at boot (mode 3)" : "");
	return;
bad:
	pc_stored = ST_BAD;
	SAY("verifier file unparsable: treated as no passcode (BAD; kern.sweet_passcode_reset=1 rewrites it)\n");
}

/* lock held. set = 0: store "none". */
static int
pc_store(int set)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	char line[160];
	if (set) {
		char sh[2 * PC_SALT + 1], dh[2 * PC_DK + 1];
		pc_hex(sh, pc_salt, PC_SALT);
		pc_hex(dh, pc_dk, PC_DK);
		snprintf(line, sizeof(line), "SWPC1 %u %s %s\n", pc_iter, sh, dh);
	} else {
		snprintf(line, sizeof(line), "SWPC1 none\n");
	}
	(void)sweet_vfs_mkdir(PC_DIR, 0700);
	int err = vnode_open(PC_FILE, O_CREAT | FWRITE, 0600, 0, &vp, ctx);
	if (!err) {
		err = vnode_setsize(vp, 0, 0, ctx);
	}
	if (!err) {
		int resid = 0;
		err = vn_rdwr(UIO_WRITE, vp, line, (int)strlen(line), 0, UIO_SYSSPACE, IO_UNIT | IO_SYNC,
		    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
		if (!err && resid) {
			err = EIO;
		}
	}
	if (vp != NULLVP) {
		struct vnode_attr va;
		VATTR_INIT(&va);
		VATTR_SET(&va, va_mode, 0600);
		VATTR_SET(&va, va_uid, 0);
		VATTR_SET(&va, va_gid, 0);
		(void)vnode_setattr(vp, &va, ctx);
		vnode_close(vp, FWRITE, ctx);
	}
	bzero(line, sizeof(line));
	return err;
}

/* ---- API for SweetKeyStore / SweetACM ---- */
int
sweet_pc_mode(void)
{
	if (!pc_inited) {
		lck_mtx_lock(&sweet_pc_lck);
		pc_init();
		lck_mtx_unlock(&sweet_pc_lck);
	}
	return pc_mode;
}

int
sweet_pc_is_set(void)
{
	if (sweet_pc_mode() == 0) {
		return 0;
	}
	lck_mtx_lock(&sweet_pc_lck);
	pc_load();
	int r = pc_stored == ST_SET;
	lck_mtx_unlock(&sweet_pc_lck);
	return r;
}

int
sweet_pc_locked(void)
{
	return sweet_pc_mode() != 0 && sweet_pc_mode() != 2 && sweet_pc_is_set() && pc_locked;
}

static uint32_t
pc_backoff_left(void)
{
	clock_sec_t now = pc_now();
	return pc_backoff_until > now ? (uint32_t)(pc_backoff_until - now) : 0;
}

/* keybag_state bits: 1 locked, 2 no_pin, 4 been_unlocked (always). sls: 0 unlocked, 1 locked, 3 disabled. */
void
sweet_pc_aks_state(uint32_t *ss, uint32_t *sls, uint32_t *sfa, uint32_t *sb)
{
	int set = sweet_pc_is_set(), locked = sweet_pc_locked();
	*ss = set ? (locked ? 0x5 : 0x4) : 0x6;
	*sls = set ? (locked ? 1 : 0) : 3;
	lck_mtx_lock(&sweet_pc_lck);
	*sfa = pc_fails;
	*sb = pc_backoff_left();
	lck_mtx_unlock(&sweet_pc_lck);
}

/* lock held. 0 ok, EAUTH wrong, EBUSY backoff. No passcode stored: ok for any input. */
static int
pc_check_locked(const uint8_t *pw, uint32_t len, int unlock)
{
	pc_load();
	if (pc_stored != ST_SET) {
		return 0;
	}
	if (pc_backoff_left() > 0) {
		pc_n_busy++;
		return EBUSY;
	}
	uint8_t dk[PC_DK];
	pc_pbkdf2(pw, len, pc_salt, PC_SALT, pc_iter, dk);
	int ok = pc_ct_equal(dk, pc_dk, PC_DK);
	bzero(dk, sizeof(dk));
	if (ok) {
		pc_fails = 0;
		pc_backoff_until = 0;
		pc_n_ok++;
		pc_pass_ok_at = pc_now();       /* 1198 */
		pc_bio_fails = 0;
		pc_bio_pending_at = 0;          /* 1214 */
		if (unlock && pc_locked) {
			pc_locked = 0;
			pc_unlocked_event = 1;
		}
		return 0;
	}
	pc_fails++;
	pc_n_bad++;
	if (pc_fails >= PC_FREE_FAILS) {
		uint32_t n = pc_fails - PC_FREE_FAILS, d = 60;
		while (n-- > 0 && d < PC_BACKOFF_MAX) {
			d *= 2;
		}
		if (d > PC_BACKOFF_MAX) {
			d = PC_BACKOFF_MAX;
		}
		pc_backoff_until = pc_now() + d;
	}
	return EAUTH;
}

int
sweet_pc_check_kbuf(const uint8_t *pw, uint32_t len, int unlock)
{
	if (sweet_pc_mode() == 0) {
		return 0;
	}
	if (len > PC_MAXLEN) {
		return EINVAL;
	}
	lck_mtx_lock(&sweet_pc_lck);
	int r = pc_check_locked(pw, len, unlock);
	uint32_t f = pc_fails;
	int ev = pc_unlocked_event;
	pc_unlocked_event = 0;
	lck_mtx_unlock(&sweet_pc_lck);
	SAY("check (len %u%s) -> %s, fails %u\n", len, unlock ? ", unlock" : "",
	    r == 0 ? "ok" : r == EBUSY ? "backoff" : "WRONG", f);
	if (ev) {
		pc_notify("unlocked");
	} else if (r == EAUTH) {
		pc_notify("failed attempt");    /* sfa / sb changed */
	}
	return r;
}

static int
pc_copyin(uint64_t ua, uint32_t len, uint8_t *buf)
{
	if (len > PC_MAXLEN) {
		return EINVAL;
	}
	if (len == 0) {
		return 0;
	}
	if (ua == 0) {
		return EFAULT;
	}
	return copyin((user_addr_t)ua, buf, len);
}

int
sweet_pc_check_user(uint64_t uaddr, uint32_t len, int unlock)
{
	uint8_t pw[PC_MAXLEN];
	int e = pc_copyin(uaddr, len, pw);
	if (e == 0) {
		e = sweet_pc_check_kbuf(pw, len, unlock);
	}
	bzero(pw, sizeof(pw));
	return e;
}

int
sweet_pc_change_user(uint64_t old_ua, uint32_t old_len, uint64_t new_ua, uint32_t new_len)
{
	uint8_t o[PC_MAXLEN], n[PC_MAXLEN];
	int e;
	if (sweet_pc_mode() == 0) {
		return EPERM;
	}
	if ((e = pc_copyin(old_ua, old_len, o)) != 0 || (e = pc_copyin(new_ua, new_len, n)) != 0) {
		goto out;
	}
	lck_mtx_lock(&sweet_pc_lck);
	e = pc_check_locked(o, old_len, 0);
	if (e == 0) {
		if (new_len == 0) {
			e = pc_store(0);
			if (e == 0) {
				pc_stored = ST_NONE;
				pc_locked = 0;
			}
		} else {
			uint8_t salt[PC_SALT], dk[PC_DK];
			read_random(salt, PC_SALT);
			pc_pbkdf2(n, new_len, salt, PC_SALT, PC_ITER, dk);
			uint8_t os[PC_SALT], od[PC_DK];
			uint32_t oi = pc_iter;
			memcpy(os, pc_salt, PC_SALT);
			memcpy(od, pc_dk, PC_DK);
			memcpy(pc_salt, salt, PC_SALT);
			memcpy(pc_dk, dk, PC_DK);
			pc_iter = PC_ITER;
			e = pc_store(1);
			if (e == 0) {
				pc_stored = ST_SET;
				pc_locked = 0;
			} else {                /* keep the old verifier in memory */
				memcpy(pc_salt, os, PC_SALT);
				memcpy(pc_dk, od, PC_DK);
				pc_iter = oi;
			}
			bzero(dk, sizeof(dk));
			bzero(od, sizeof(od));
		}
	}
	lck_mtx_unlock(&sweet_pc_lck);
	SAY("change (old len %u, new len %u) -> %d%s\n", old_len, new_len, e,
	    e == 0 ? (new_len ? ": verifier stored" : ": passcode removed") : e == EAUTH ? ": old passcode WRONG" : "");
	if (e == 0) {
		pc_notify(new_len ? "passcode set" : "passcode removed");
	}
out:
	bzero(o, sizeof(o));
	bzero(n, sizeof(n));
	return e;
}

void
sweet_pc_lock(void)
{
	int m = sweet_pc_mode();
	if (m == 0 || m == 2 || !sweet_pc_is_set()) {
		return;
	}
	int ev = 0;
	lck_mtx_lock(&sweet_pc_lck);
	if (!pc_locked) {
		pc_locked = 1;
		ev = 1;
		SAY("locked\n");
	}
	pc_bio_pending_at = 0;          /* 1214 */
	lck_mtx_unlock(&sweet_pc_lck);
	if (ev) {
		pc_notify("locked");
	}
}

/*
 * 1198: a Touch ID match (SweetMesa, kern.sweet_mesa_match=2) unlocks the keybag as the SEP would, under iOS's
 * rules. 0 = allowed (*did_unlock = 1 when it was locked and is now unlocked); else EPERM and *why.
 * No passcode (or mode 0 / 2): nothing is locked, allowed without unlocking.
 */
int sweet_pc_bio_unlock(int *did_unlock, const char **why);
void sweet_pc_bio_failed(void);
int sweet_pc_bio_allowed(const char **why);

int
sweet_pc_bio_unlock(int *did_unlock, const char **why)
{
	*did_unlock = 0;
	*why = "";
	int m = sweet_pc_mode();
	if (m == 0 || m == 2 || !sweet_pc_is_set()) {
		*why = "no passcode lock";
		return 0;
	}
	int e = 0, ev = 0;
	clock_sec_t now = pc_now();
	lck_mtx_lock(&sweet_pc_lck);
	if (pc_pass_ok_at == 0) {
		*why = "passcode required after restart";
		e = EPERM;
	} else if (now - pc_pass_ok_at > PC_BIO_TTL_S) {
		*why = "passcode required (48 h since the last passcode)";
		e = EPERM;
	} else if (pc_bio_fails >= PC_BIO_MAX_FAILS) {
		*why = "passcode required (5 failed Touch ID attempts)";
		e = EPERM;
	} else if (pc_backoff_left() > 0) {
		*why = "passcode backoff";
		e = EPERM;
	} else {
		pc_bio_fails = 0;
		if (pc_locked) {
			pc_locked = 0;
			ev = 1;
			*did_unlock = 1;
			if (pc_biostate) {
				pc_bio_pending_at = now ? now : 1;      /* 1214: InBioUnlock until confirmed */
			}
		}
	}
	if (e) {
		pc_n_bio_refused++;
	} else if (ev) {
		pc_n_bio_unlock++;
	}
	lck_mtx_unlock(&sweet_pc_lck);
	SAY("Touch ID %s%s%s\n", e ? "unlock REFUSED: " : ev ? "unlocked the keybag" : "match (already unlocked)",
	    e ? *why : "", ev && pc_biostate ? " (InBioUnlock: extended lock state 7 until SpringBoard confirms)" : "");
	if (ev) {
		pc_notify("unlocked (Touch ID)");
		int bz = pc_bio_buzz;
		if (bz != 0) {
			int he = sweet_haptic_request((unsigned)bz & 0xffffu, ((unsigned)bz >> 16) & 0xffu);
			SAY("Touch ID unlock buzz (non-native, kern.sweet_passcode_bio_buzz 0x%x) -> %d\n", bz, he);
		}
	}
	return e;
}

/*
 * 1202: the same rules as sweet_pc_bio_unlock without unlocking anything (SweetMesa 39 GetSKSLockState asks this
 * for the "no bio lockout" bit). No passcode lock at all also counts as allowed (nothing to enforce).
 */
int
sweet_pc_bio_allowed(const char **why)
{
	*why = "";
	int m = sweet_pc_mode();
	if (m == 0 || m == 2 || !sweet_pc_is_set()) {
		return 1;
	}
	int ok = 0;
	clock_sec_t now = pc_now();
	lck_mtx_lock(&sweet_pc_lck);
	if (pc_pass_ok_at == 0) {
		*why = "passcode required after restart";
	} else if (now - pc_pass_ok_at > PC_BIO_TTL_S) {
		*why = "passcode required (48 h since the last passcode)";
	} else if (pc_bio_fails >= PC_BIO_MAX_FAILS) {
		*why = "passcode required (5 failed Touch ID attempts)";
	} else if (pc_backoff_left() > 0) {
		*why = "passcode backoff";
	} else {
		ok = 1;
	}
	lck_mtx_unlock(&sweet_pc_lck);
	return ok;
}

/*
 * 1225: why Touch ID may not unlock, for SweetMesa's 39 GetSKSLockState bits (kern.sweet_mesa_lockout bit 0):
 * 0 allowed (or no passcode lock), 1 no passcode since boot, 2 48 h since the last passcode, 3 failed-match lockout
 * (5 no-matches, or a forced lockout: 73 / 57), 4 passcode backoff. Same order as sweet_pc_bio_allowed().
 */
int sweet_pc_bio_reason(void);
void sweet_pc_bio_lockout(const char *why);

int
sweet_pc_bio_reason(void)
{
	int m = sweet_pc_mode();
	if (m == 0 || m == 2 || !sweet_pc_is_set()) {
		return 0;
	}
	int r = 0;
	clock_sec_t now = pc_now();
	lck_mtx_lock(&sweet_pc_lck);
	if (pc_pass_ok_at == 0) {
		r = 1;
	} else if (now - pc_pass_ok_at > PC_BIO_TTL_S) {
		r = 2;
	} else if (pc_bio_fails >= PC_BIO_MAX_FAILS) {
		r = 3;
	} else if (pc_backoff_left() > 0) {
		r = 4;
	}
	lck_mtx_unlock(&sweet_pc_lck);
	return r;
}

/* 1225: 73 ForceBioLockout / 57 DropUnlockToken: Touch ID needs the passcode again (cleared by a passcode unlock) */
void
sweet_pc_bio_lockout(const char *why)
{
	if (sweet_pc_mode() == 0) {
		return;
	}
	lck_mtx_lock(&sweet_pc_lck);
	pc_bio_fails = PC_BIO_MAX_FAILS;
	pc_bio_pending_at = 0;
	lck_mtx_unlock(&sweet_pc_lck);
	SAY("Touch ID lockout forced (%s): passcode required\n", why);
}

void
sweet_pc_bio_failed(void)
{
	if (sweet_pc_mode() == 0) {
		return;
	}
	lck_mtx_lock(&sweet_pc_lck);
	uint32_t f = ++pc_bio_fails;
	lck_mtx_unlock(&sweet_pc_lck);
	SAY("Touch ID no-match %u/%d%s\n", f, PC_BIO_MAX_FAILS, f >= PC_BIO_MAX_FAILS ? ": passcode required" : "");
}

/*
 * 1214: 1 while a Touch ID unlock waits for SpringBoard's confirmation (SweetKeyStore reports extended lock state 7).
 * Dropped (no event) when locked, when the switch is turned off, or after PC_BIO_STATE_S without a confirmation.
 */
int sweet_pc_bio_pending(void);
int sweet_pc_bio_confirm(int *was_pending);

int
sweet_pc_bio_pending(void)
{
	int r = 0, expired = 0;
	lck_mtx_lock(&sweet_pc_lck);
	if (pc_bio_pending_at != 0) {
		if (!pc_biostate || pc_locked || pc_now() - pc_bio_pending_at > PC_BIO_STATE_S) {
			pc_bio_pending_at = 0;
			pc_n_bio_expired++;
			expired = 1;
		} else {
			r = 1;
		}
	}
	lck_mtx_unlock(&sweet_pc_lck);
	if (expired) {
		SAY("InBioUnlock dropped (not confirmed within %d s, locked, or kern.sweet_passcode_biostate off)\n",
		    PC_BIO_STATE_S);
	}
	return r;
}

/*
 * 1214: aks_unlock_device without a passcode (MKBUnlockDevice(nil), SpringBoard's _performNilPasscodeCheck).
 * ENOTSUP = switch off / no passcode lock: the caller keeps the old path. 0 = the keybag is unlocked (InBioUnlock
 * confirmed, or nothing pending and not locked: a late confirmation after the state expired). EAUTH = locked:
 * a missing passcode never unlocks, and it is not counted as a wrong passcode.
 */
int
sweet_pc_bio_confirm(int *was_pending)
{
	*was_pending = 0;
	int m = sweet_pc_mode();
	if (!pc_biostate || m == 0 || m == 2 || !sweet_pc_is_set()) {
		return ENOTSUP;
	}
	lck_mtx_lock(&sweet_pc_lck);
	*was_pending = pc_bio_pending_at != 0;
	pc_bio_pending_at = 0;
	int locked = pc_locked;
	if (*was_pending) {
		pc_n_bio_confirm++;
	} else if (locked) {
		pc_n_bio_nil_refused++;
	}
	lck_mtx_unlock(&sweet_pc_lck);
	SAY("aks_unlock_device without passcode: %s\n", *was_pending ? "InBioUnlock confirmed -> Unlocked" :
	    locked ? "REFUSED (locked, no Touch ID unlock pending; not counted)" : "already unlocked -> ok");
	if (*was_pending) {
		pc_notify("bio unlock confirmed");
	}
	return locked && !*was_pending ? EAUTH : 0;
}

static int
sysctl_sweet_passcode_bio_buzz(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int v = pc_bio_buzz, changed = 0;
	int e = sysctl_io_number(req, pc_bio_buzz, sizeof(pc_bio_buzz), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (v < 0 || (v & 0xffff) > 2000 || ((v >> 16) & 0xff) > 127 || (v >> 24) != 0) {
		return EINVAL;
	}
	pc_bio_buzz = v;
	SAY("kern.sweet_passcode_bio_buzz=0x%x (non-native Touch ID unlock pulse %s)\n", v, v ? "on" : "off");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_passcode_bio_buzz, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_passcode_bio_buzz, "I", "redfin 1217 (non-native): ms | strength << 16 = one vibration pulse when "
    "Touch ID unlocks the keybag; 0 = off (default)");

static int
sysctl_sweet_passcode_biostate(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int v = pc_biostate, changed = 0;
	int e = sysctl_io_number(req, pc_biostate, sizeof(pc_biostate), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (v < 0 || v > 1) {
		return EINVAL;
	}
	lck_mtx_lock(&sweet_pc_lck);
	pc_biostate = v;
	if (!v) {
		pc_bio_pending_at = 0;
	}
	lck_mtx_unlock(&sweet_pc_lck);
	SAY("kern.sweet_passcode_biostate=%d (InBioUnlock after a Touch ID unlock %s)\n", v, v ? "on" : "off");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_passcode_biostate, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_passcode_biostate, "I", "redfin 1214: 1 = extended lock state 7 (InBioUnlock) after a Touch ID "
    "unlock until SpringBoard confirms it (default 0)");

static int
sysctl_sweet_passcode_bio(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	char b[256];
	clock_sec_t now = pc_now();
	lck_mtx_lock(&sweet_pc_lck);
	snprintf(b, sizeof(b), "passcode_since_boot=%d age_s=%lu bio_fails=%u bio_unlocks=%u bio_refused=%u "
	    "biostate=%d in_bio_unlock=%d bio_confirms=%u bio_expired=%u nil_refused=%u",
	    pc_pass_ok_at != 0, pc_pass_ok_at ? (unsigned long)(now - pc_pass_ok_at) : 0UL, pc_bio_fails, pc_n_bio_unlock,
	    pc_n_bio_refused, pc_biostate, pc_bio_pending_at != 0, pc_n_bio_confirm, pc_n_bio_expired, pc_n_bio_nil_refused);
	lck_mtx_unlock(&sweet_pc_lck);
	return SYSCTL_OUT(req, b, strlen(b) + 1);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_passcode_bio, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_passcode_bio, "A", "redfin 1198: Touch ID unlock rule state");

/* ---- sysctls ---- */
static int
sysctl_sweet_passcode(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int v = sweet_pc_mode(), changed = 0;
	int e = sysctl_io_number(req, pc_mode, sizeof(pc_mode), &v, &changed);
	if (e || !changed) {
		return e;
	}
	char pn[32];
	proc_selfname(pn, (int)sizeof(pn));
	if (pc_pinned) {
		SAY("kern.sweet_passcode=%d by %s REFUSED: pinned off by boot-arg sweetpasscode=0\n", v, pn);
		return EPERM;
	}
	if (v < 0 || v > 3) {
		return EINVAL;
	}
	lck_mtx_lock(&sweet_pc_lck);
	int was = pc_mode;
	pc_mode = v;
	if (v == 0) {
		pc_locked = 0;
	}
	pc_bio_pending_at = 0;          /* 1214 */
	pc_stored = ST_UNKNOWN;         /* re-read the file when next needed */
	pc_last_try = 0;
	lck_mtx_unlock(&sweet_pc_lck);
	SAY("kern.sweet_passcode %d -> %d by %s\n", was, v, pn);
	if (v != 0) {
		sweet_acm_publish();
	}
	pc_notify("mode change");       /* SpringBoard re-reads: recovery to 0 drops its passcode state at once */
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_passcode, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_passcode, "I",
    "redfin 1195: 0 off (no passcode, as before), 1 passcode + lock state, 2 passcode never locked, 3 = 1 locked at boot");

static int
sysctl_sweet_passcode_reset(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int v = 0, changed = 0;
	int e = sysctl_io_number(req, 0, sizeof(int), &v, &changed);
	if (e || !changed || v != 1) {
		return e ? e : (changed ? EINVAL : 0);
	}
	char pn[32];
	proc_selfname(pn, (int)sizeof(pn));
	(void)sweet_pc_mode();
	lck_mtx_lock(&sweet_pc_lck);
	e = pc_store(0);
	pc_stored = e ? ST_UNKNOWN : ST_NONE;
	pc_locked = 0;
	pc_fails = 0;
	pc_backoff_until = 0;
	pc_bio_pending_at = 0;          /* 1214 */
	bzero(pc_dk, sizeof(pc_dk));
	lck_mtx_unlock(&sweet_pc_lck);
	SAY("RESET by %s: verifier -> none (%d)\n", pn, e);
	if (pc_mode != 0) {
		pc_notify("reset");
	}
	return e;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_passcode_reset, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_passcode_reset, "I", "redfin 1195: write 1 (root) to store 'no passcode' and clear lock/backoff");

/*
 * 1196: kern.sweet_passcode_notify=1 (root) sends a keybag status event by hand; =2 also locks (as
 * aks_lock_device would), for testing the lock screen without waiting for SpringBoard. Mode != 0 only.
 */
static int
sysctl_sweet_passcode_notify(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int v = 0, changed = 0;
	int e = sysctl_io_number(req, 0, sizeof(int), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (sweet_pc_mode() == 0) {
		return EPERM;
	}
	if (v == 2) {
		sweet_pc_lock();        /* notifies when it locks */
		return 0;
	}
	if (v != 1) {
		return EINVAL;
	}
	pc_notify("sysctl");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_passcode_notify, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_passcode_notify, "I", "redfin 1196: 1 = send a keybag status event, 2 = lock now (mode != 0)");

static int
sysctl_sweet_passcode_state(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	static const char *const st[] = { "unknown", "none", "set", "BAD" };
	char b[256];
	int m = sweet_pc_mode();
	if (m != 0) {
		(void)sweet_pc_is_set();
	}
	lck_mtx_lock(&sweet_pc_lck);
	snprintf(b, sizeof(b), "mode=%d pinned=%d stored=%s%s locked=%d fails=%u backoff=%u iter=%u ok=%u bad=%u busy=%u events=%u",
	    m, pc_pinned, st[pc_stored], m == 0 ? "(ignored)" : "", m != 0 && m != 2 && pc_locked, pc_fails,
	    pc_backoff_left(), pc_iter, pc_n_ok, pc_n_bad, pc_n_busy, pc_n_notify);
	lck_mtx_unlock(&sweet_pc_lck);
	return SYSCTL_OUT(req, b, strlen(b) + 1);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_passcode_state, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_passcode_state, "A", "redfin 1195: passcode state");
#endif /* ARM64_BOARD_CONFIG_SWEET */
