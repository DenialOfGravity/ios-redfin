/*
 * redfin 1068 (P4 phase 1, research/cc/P4-design.md s.4.2/4.3/5): the QMUX <-> QRTR router in the kernel, behind the
 * AppleBasebandPCI stand-in's qmux1/qmux2 pipes (SweetABP.cpp), with boot-arg sweetbb & 2. A port of wland's
 * phone/wifi/abpbridge.c (ws-n-abp-router 53df921 = a84cf84 router + ssctl), mode 2 ("router") only, with the same
 * behaviour: CommCenter's frames reach it straight from SweetABP (sweet_abp_c2w), its answers go straight back
 * (SweetABPDeliver), and the modem is reached over the kernel's own QRTR link (sweet_bb.c). No /dev/abpbridge round trip.
 *
 * Per CommCenter session (one per user client of a pipe): CTL (svc 0) answered here (GetClientId/Release/Sync/
 * SetDataFormat/GetVersionInfo). Other requests go through the policy: FORWARD = to the modem's QMI server over QRTR,
 * each (pipe, session, svc, cid) from its own router port (0x4000..0x43ff, pooled per service when released), the SDU
 * unchanged; OK / ERR:<code> / RESP:<TLVs or template> = answered here; DROP = no answer.
 *
 * Policy = the built-in table (below; kern.sweet_qmi_rules picks the list) + override text with abpbridge.c's grammar:
 *     svc msg fwd|ok|err[:code]|drop|resp:<hex or template>      ('*' = every msg of the service)
 *     on svc msg ind <msg>|<svc>:<msg> [<hex or template>] [if <tlv> | if RQ..=RQ..]
 *     on svc msg replay <get> <ind> [map a=b,..] [if ..]
 *     on svc msg bbreset <ms> [msg|unpub] [if ..]
 *     strip svc msg tlv[,tlv..]
 *   templates: <id>{ICCID} <id>{IMSI} {RQ<tlv>:<off>:<len>[+n]}  (SIM identity read from the card at run time, kept in
 *   kernel memory only, never logged, never written anywhere; frame logs are redacted).
 * The override text comes from (first that exists): text written to kern.sweet_qmi_policy (empty string = forget it),
 * /var/local/abp-policy.conf (re-read when it changes, checked every 10 s, kern.sweet_qmi_policy_file=0 ignores it),
 * else the built-in default below = agent/stage/abp-policy.voice.conf (BSP 0xb00c activation + SIM identity indication,
 * 0xb001 chip info, 0xa000/0xa001 carrier-bundle segment acks, MFSE 0x2c + RFS sync (same token, reset-after-sync 0),
 * bbreset emulation of ABM's soft reset, PDS answers for locationd, NAS SysInfo/SigInfo replays + TLV 0x26 strip,
 * VOICE forwarded).
 *
 * Kept from abpbridge.c: per-(svc, msg) "NEW" decision lines, first ANSWER per (svc, msg), MALFORMED checks on every SDU
 * from the modem, NO ANSWER after 5 s + an unsolicited RESUME_TX ("kick"), client pooling, replays, synthesized
 * indications, strips, SIM identity snooping (CommCenter's own UIM 0x0020 EF_ICCID/EF_IMSI reads) and the router's own
 * reads (every 5 s while an indication waits for it), NAS indication hold until the BSP activation indication (+500 ms),
 * bbreset (reset-detected 100 ms after the DMS reset answer, alive <ms> later: SweetABPBasebandMessage, or un/re-publish).
 * Ready: the stand-ins are published (sweetabp & 8) once the modem's DMS, NAS and UIM servers are known.
 *
 * Safety (s.5): kern.sweet_qmi_filter 0 = pass (built-in protection list off: only the hard floor and the override text),
 * 1 = protect (default), 2 = protect + every frame in the kernel log (SIM identity redacted). kern.sweet_qmi_rules 0 = the
 * P2 list as abpbridge.c (default), 1 = the option-1 list (s.5.3 with the user's 2026-11-12 decisions: NAS/UIM/DMS state
 * changes and COEX forwarded, network scan 1/60 s, DMS reset still emulated, SIM PIN changes and SIM file writes blocked;
 * data services stay with wland in phase 1). The hard floor (never forwarded, whatever the text says): DMS 0x0038 0x003a
 * 0x0048 0x004a 0x0050 0x0052, DMS 0x002e with mode 6 (persistent LPM), UIM 0x0029, all of PDC 0x24, SAR 0x11, RFRPE 0x29,
 * SSCTL 0x2b; Apple-only services (>= 0xe0) and Apple message ids (>= 0x5500) are never forwarded either.
 * 1080: with sweetbb & 0x10 and kern.sweet_qmi_rules 1, CommCenter owns cellular data (WDS/WDA/QOS/DSD/DFS forwarded with
 *       translation, see the data section before handle()); kern.sweet_data_fwd (kill switch), _opts, _stats,
 *       kern.sweet_wds_profiles_bak / _ack / kern.sweet_wds_gate_ids (profile-write gate).
 * Tunables (RW): kern.sweet_qmux_err (0x5e), _cidfail (0), _reuse (1), _kick (1), _holdnas (1), _holddly (500);
 * kern.sweet_qmux_dump (write: the per-(svc, msg) table to the log). RD kern.sweet_bb_stats (sweet_bb.c).
 * 1074: kern.sweet_voice_auto (0 off default, 1 earpiece, 2 speaker): the voice path follows the modem's VOICE call states.
 * 1085: kern.sweet_qmi_plmnblip (0 off default, 1 on): _plmnblip_ms after a re-publish SysInfo shot, one SysInfo indication
 *       with the LTE PLMN's MNC changed to 9s, then the modem's real one 300 ms later (CommCenter re-publishes, SOS gone).
 * 1088: kern.sweet_data_opts & 0x4 (default on): the router's own WDA SetDataFormat (wland's X1 values) once per modem
 *       boot when data forwarding is on (CommCenter never sends one here); StartNetworkInterface held until it is answered.
 * 1087: kern.sweet_qmi_republish 4 = as 3, plus the PLMN withheld from CommCenter until the activation went (NAS
 *       indications held without the 5 s timeout, up to _withhold_s 600; PLMN TLVs stripped from NAS GET answers).
 * 1084: kern.sweet_qmi_republish 3 = BSP activation deferred until the modem has service (_wait_s 45), then the shot.
 * 1082: re-publish clients = NAS 0x0003 TLV 0x18/0x19 (not 0x12/0x13), fallbacks, choice logged; manual line timing fixed.
 * 1073: kern.sweet_qmi_republish (0 off default, 1 auto once per CommCenter session, write 2 = now), _republish_ms (20).
 * Log lines "SWEET QMUX:".
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>         /* rootvnode */
#include <sys/kauth.h>
#include <kern/locks.h>
#include <kern/kalloc.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "sweet_bb.h"
#include "sweet_ipa.h"
#include "sweet_pdp.h"

extern int SweetABPDeliver(unsigned pipe, uint32_t sess, const void *data, uint32_t len);
extern void SweetABPBasebandMessage(uint32_t code);
extern int SweetABPPublish(bool on, const char *why);
extern int sweet_voice_auto(int target, const char *why);      /* sweet_voice.c (1074) */

#define LOG(fmt, ...)           printf("SWEET QMUX: [%6llu] " fmt, (unsigned long long)sbb_ms(), ##__VA_ARGS__)
#define NPIPES                  2u
#define ABP_MAXFRAME            65536u
#define QMI_ERR_NOT_SUPPORTED   0x5e
#define QMI_ERR_INVALID_SERVICE_TYPE 0x1f
#define QMI_ERR_CLIENT_IDS_EXHAUSTED 0x05
#define QMI_ERR_DEVICE_IN_USE   0x17
#define POLICY_PATH             "/private/var/local/abp-policy.conf"
#define POLICY_MAX              (64u << 10)

static const char *pipe_name[NPIPES] = { "qmux1", "qmux2" };

/* tunables (abpbridge.c's cell.on words) */
static int qmi_filter = 1, qmi_rules = 0, err_code = QMI_ERR_NOT_SUPPORTED, cidfail = 0, reuse = 1, kickon = 1;
static int holdnas = 1, holddly = 500, policy_file = 1;
static int repub_mode = 0, repub_ms = 20;       /* 1073: kern.sweet_qmi_republish (0 off), kern.sweet_qmi_republish_ms */
static int wh_s = 600;                          /* 1087: kern.sweet_qmi_withhold_s (mode 4: longest PLMN withholding) */
static uint64_t wh_start;                       /* 1087: first withheld frame of this CommCenter session (0 none) */
static unsigned wh_held, wh_stripped, wh_logged;
static int repub_wait_s = 45;                   /* 1084: kern.sweet_qmi_republish_wait_s (mode 3: longest activation deferral) */
static int act_waiting;                         /* 1084: a BSP activation indication is being deferred */
static volatile int repub_now;                  /* 1073: kern.sweet_qmi_republish=2 written (manual shot) */
static int plmnblip = 0, plmnblip_ms = 1500;    /* 1085: kern.sweet_qmi_plmnblip, kern.sweet_qmi_plmnblip_ms */
static int voice_auto = 0;                      /* 1074: kern.sweet_voice_auto: 0 off, 1 earpiece, 2 speaker on a call */
/*
 * 1086: kern.sweet_voice_dtmflog (default 1; log lines only): every VOICE Burst/Start/Stop DTMF (0x0028/0x0029/0x002a)
 * request with its call id and digit(s) (DTMF digits only: no numbers are read), every answer with result and latency,
 * every VOICE DTMF indication, every Apple AUDIO (0xe1) request id, and the message of any request left without an
 * answer for 5 s. "SWEET QMUX: [...] DTMF ..." / "AUDIO(apple) req ..." / "NO ANSWER ...".
 */
static int dtmflog = 1;
static bool on;
static int ready = -1;

/* ---- the modem's QMI servers ---- */

#define NSRV    96
static struct { uint32_t svc, inst, node, port; int up; } srv[NSRV];

static int
srv_find(unsigned svc)
{
	int best = -1;
	for (int i = 0; i < NSRV; i++) {
		if (srv[i].up && srv[i].svc == svc && (best < 0 || srv[i].inst < srv[best].inst)) {
			best = i;
		}
	}
	return best;
}

/* ---- router clients ---- */

#define NCL     256
struct cl {
	int used;                       /* 0 free, 1 active, 2 idle (pooled) */
	uint32_t pipe, sess, port;
	uint8_t svc, cid;
	uint8_t dat;                    /* 1080: a CommCenter data client (used 3 = closing: StopNetworkInterface out) */
	unsigned fwd, resp, ind;
};
static struct cl cl[NCL];

/* 1080 (CommCenter owns cellular data): the data services and the hooks the code before the data section calls */
#define DSVC(s)                 ((s) == 0x01 || (s) == 0x04 || (s) == 0x1a || (s) == 0x2a || (s) == 0x30)
static bool data_active(void);
static bool d_port_reserved(uint32_t port);
static void d_cl_gone(struct cl *c);
static int d_release(struct cl *c, const char *why);
static uint32_t next_port_gen;
static unsigned late;

#define NMST    1024
struct mst {
	uint8_t svc, act;
	uint16_t msg, last_err;
	unsigned count, ok, err, noans, answered_logged;
};

#define TRG_MAXMAP      8
#define NPEND           512
struct pend {
	int used;
	uint32_t port;
	uint16_t txn, msg;
	uint8_t svc;
	uint64_t t;
	int replay;
	uint32_t rpipe, rsess;
	uint8_t rcid, nmap, map[TRG_MAXMAP][2];
	uint16_t ind_msg;
	uint8_t rqn, rq[96];
	uint8_t simread;
	uint16_t file;
	uint8_t repub;                  /* 1073: a re-publish GET (1 auto, 2 manual): answer -> indication, past the NAS hold */
	uint8_t probe;                  /* 1084: the router's own SysInfo poll while the activation waits: answer read, not sent */
};

#define NSESS   32
struct sess_st {
	uint32_t pipe, sess;
	uint8_t cids[256][32];
};

#define NHOLD   64
#define HOLD_MAXF 4096
struct hold {
	int used;
	uint32_t pipe, sess;
	uint8_t svc, cid;
	uint16_t msg;
	uint32_t n;
	uint64_t t;
	uint8_t f[HOLD_MAXF];
};

/* the big state: allocated only when the router is on */
static struct qbig {
	struct mst mst[NMST];
	struct pend pend[NPEND];
	struct sess_st st[NSESS];
	struct hold holdq[NHOLD];
	uint8_t ind_buf[16 + ABP_MAXFRAME];
	uint8_t ans_buf[6 + ABP_MAXFRAME];
	uint8_t rep_buf[ABP_MAXFRAME];
	uint8_t red_buf[6 + ABP_MAXFRAME];
	uint8_t loc_buf[32 + 4096];
	uint8_t trg_buf[4096];
	uint8_t tmp_buf[4096];
	uint8_t res_buf[4096];
	char line[12288];
} *B;
static int nmst, mst_dirty, nhold;

static uint32_t cur_pipe, cur_sess;
static unsigned nframes_in, nframes_out, nseen;
static struct { uint8_t svc; uint16_t msg; } seen[1024];
static struct {
	unsigned fwd, local_ok, local_err, dropped, resp, ind, noserver, sendfail, floor, ratelimited;
} cnt;
static struct { unsigned fwd, local, denied; } svc_cnt[256];
static uint16_t replay_txn = 0xff00;
static unsigned malformed, replays, synth_inds, stripped, kicks;

static struct mst *
mst_get(unsigned svc, unsigned msg)
{
	for (int i = 0; i < nmst; i++) {
		if (B->mst[i].svc == svc && B->mst[i].msg == msg) {
			return &B->mst[i];
		}
	}
	if (nmst >= NMST) {
		return NULL;
	}
	bzero(&B->mst[nmst], sizeof(B->mst[0]));
	B->mst[nmst].svc = (uint8_t)svc;
	B->mst[nmst].msg = (uint16_t)msg;
	return &B->mst[nmst++];
}

static const char *
svc_name(unsigned s)
{
	switch (s) {
	case 0x00: return "CTL"; case 0x01: return "WDS"; case 0x02: return "DMS"; case 0x03: return "NAS";
	case 0x04: return "QOS"; case 0x05: return "WMS"; case 0x06: return "PDS"; case 0x07: return "AUTH";
	case 0x08: return "AT"; case 0x09: return "VOICE"; case 0x0a: return "CAT"; case 0x0b: return "UIM";
	case 0x0c: return "PBM"; case 0x0f: return "TEST"; case 0x10: return "LOC"; case 0x11: return "SAR";
	case 0x12: return "IMSS"; case 0x1a: return "WDA"; case 0x22: return "COEX"; case 0x24: return "PDC";
	case 0x29: return "RFRPE"; case 0x2a: return "DSD"; case 0x2b: return "SSCTL"; case 0x2c: return "MFSE";
	case 0x2f: return "DPM"; case 0x30: return "DFS"; case 0x52: return "MS";
	case 0xe1: return "AUDIO(apple)"; case 0xe2: return "BSP(apple)"; case 0xe3: return "CIQ(apple)";
	case 0xe4: return "AWD(apple)"; case 0xe5: return "VINYL(apple)"; case 0xe6: return "MAVIMS(apple)";
	case 0xe7: return "ELQM(apple)"; case 0xe8: return "P2P(apple)"; case 0xe9: return "APPS(apple)";
	case 0xea: return "SFT(apple)";
	}
	return "?";
}

static const char *
ctl_name(unsigned m)
{
	switch (m) {
	case 0x20: return "SetInstanceId"; case 0x21: return "GetVersionInfo"; case 0x22: return "GetClientId";
	case 0x23: return "ReleaseClientId"; case 0x24: return "RevokeClientIdInd"; case 0x25: return "InvalidClientIdInd";
	case 0x26: return "SetDataFormat"; case 0x27: return "Sync"; case 0x28: return "RegPwrSaveMode";
	case 0x29: return "ConfigPwrSaveSettings"; case 0x2a: return "SetPwrSaveMode"; case 0x2b: return "GetPwrSaveMode";
	}
	return "?";
}

/* ---- small string helpers (no libc here) ---- */

static int
hexval(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

static bool
is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* a number, C style (0x.. hex, decimal, optional '-'), the whole string; 0 ok */
static int
parse_num(const char *s, int *v)
{
	long x = 0;
	int neg = 0, digits = 0;
	if (*s == '-') {
		neg = 1;
		s++;
	}
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		for (; *s && hexval(*s) >= 0; s++, digits++) {
			x = x * 16 + hexval(*s);
			if (x > 0x7fffffffL) {
				return -1;
			}
		}
	} else {
		for (; *s >= '0' && *s <= '9'; s++, digits++) {
			x = x * 10 + (*s - '0');
			if (x > 0x7fffffffL) {
				return -1;
			}
		}
	}
	if (!digits || *s) {
		return -1;
	}
	*v = (int)(neg ? -x : x);
	return 0;
}

/* leading decimal digits of s: value and how many characters; -1 if none */
static int
dec_prefix(const char *s, int *v, int *k)
{
	long x = 0;
	int n = 0;
	for (; s[n] >= '0' && s[n] <= '9'; n++) {
		x = x * 10 + (s[n] - '0');
		if (x > 0x7fffffffL) {
			return -1;
		}
	}
	if (!n) {
		return -1;
	}
	*v = (int)x;
	*k = n;
	return 0;
}

/* leading hex digits of s (no 0x) */
static int
hex_prefix(const char *s, unsigned *v, int *k)
{
	unsigned x = 0;
	int n = 0;
	for (; hexval(s[n]) >= 0 && n < 8; n++) {
		x = x * 16 + (unsigned)hexval(s[n]);
	}
	if (!n) {
		return -1;
	}
	*v = x;
	*k = n;
	return 0;
}

/* up to max whitespace-separated words of line, each cut to wl-1 chars; the count */
static int
split_words(const char *line, char (*w)[256], int max)
{
	int n = 0;
	const char *p = line;
	while (n < max) {
		while (*p && is_space(*p)) {
			p++;
		}
		if (!*p) {
			break;
		}
		int k = 0;
		while (*p && !is_space(*p)) {
			if (k < 255) {
				w[n][k++] = *p;
			}
			p++;
		}
		w[n][k] = 0;
		n++;
	}
	return n;
}

static const char *
find_str(const char *s, const char *what)
{
	return strnstr(s, what, strlen(s) + 1);
}

/* ---- policy ---- */

enum { ACT_FWD, ACT_OK, ACT_ERR, ACT_DROP, ACT_LOCAL, ACT_RESP };
static const char *act_name[] = { "fwd", "ok", "err", "drop", "local", "resp" };
#define POL_ANY         (-1)
#define POL_ABPERR      (-1)

struct pol {
	int svc, msg, act, err;
	const char *why;
	int rate_ms;                    /* rules 1: a forwarded message at most once per rate_ms (else err 0x17) */
};

struct fref { int tlv, off, len, add, hasadd; };
enum { TRG_IND, TRG_REPLAY, TRG_BBRESET };
#define NTRG    64
struct trg {
	int svc, msg, kind;
	uint16_t m2, m3;
	uint8_t map[TRG_MAXMAP][2];
	int nmap;
	const uint8_t *data;
	uint16_t dlen;
	int cond;
	const char *tmpl;
	int ceq;
	int tosvc;
	struct fref fa, fb;
};
static struct trg trg[NTRG];
static int ntrg;
static uint8_t pol_data[16384];
static uint32_t pol_dlen;
#define NSTRIP  32
static struct strip { int svc, msg; uint8_t ids[32]; int nids; } strips[NSTRIP];
static int nstrip;

static uint32_t
strip_tlvs(unsigned svc, unsigned msg, uint8_t *p, uint32_t n)
{
	const struct strip *s = NULL;
	for (int i = 0; i < nstrip && !s; i++) {
		if (strips[i].svc == (int)svc && strips[i].msg == (int)msg) {
			s = &strips[i];
		}
	}
	if (!s) {
		return n;
	}
	uint32_t o = 0;
	for (uint32_t k = 0; k + 3 <= n;) {
		uint32_t l = 3u + (uint32_t)(p[k + 1] | (p[k + 2] << 8));
		if (k + l > n) {
			break;
		}
		int drop = 0;
		for (int j = 0; j < s->nids; j++) {
			drop |= s->ids[j] == p[k];
		}
		if (drop) {
			stripped++;
		} else {
			memmove(p + o, p + k, l);
			o += l;
		}
		k += l;
	}
	return o;
}

static int
parse_strip(const char *line, int ln)
{
	char w[4][256];
	struct strip t;
	bzero(&t, sizeof(t));
	if (split_words(line, w, 4) != 4 || nstrip >= NSTRIP || parse_num(w[1], &t.svc) || parse_num(w[2], &t.msg)) {
		goto bad;
	}
	for (const char *m = w[3]; *m && t.nids < 32;) {
		unsigned x;
		int k;
		if (hex_prefix(m, &x, &k) || x > 255) {
			goto bad;
		}
		t.ids[t.nids++] = (uint8_t)x;
		m += k;
		if (*m == ',') {
			m++;
		} else if (*m) {
			goto bad;
		}
	}
	strips[nstrip++] = t;
	return 0;
bad:
	LOG("policy line %d: bad 'strip' line (strip <svc> <msg> <tlv>[,<tlv>..])\n", ln);
	return -1;
}

/*
 * The built-in protection lists. Exact (svc, msg) entries win over (svc, POL_ANY); a service with no entry is forwarded
 * when it is a standard one (< 0xe0) the modem offers, else err.
 */
static const struct pol rules_p2[] = {     /* kern.sweet_qmi_rules 0: abpbridge.c's abp_builtin, unchanged */
	{ 0x02, POL_ANY, ACT_FWD, 0, "DMS", 0 },
	{ 0x02, 0x0000, ACT_OK, 0, "DMS reset: protected", 0 },
	{ 0x02, 0x002e, ACT_OK, 0, "DMS set operating mode: protected (wland owns the mode)", 0 },
	{ 0x02, 0x0027, ACT_ERR, POL_ABPERR, "DMS UIM set PIN protection: protected", 0 },
	{ 0x02, 0x0029, ACT_ERR, POL_ABPERR, "DMS UIM unblock PIN: protected", 0 },
	{ 0x02, 0x002a, ACT_ERR, POL_ABPERR, "DMS UIM change PIN: protected", 0 },
	{ 0x02, 0x0032, ACT_ERR, POL_ABPERR, "DMS activate automatic: protected", 0 },
	{ 0x02, 0x0033, ACT_ERR, POL_ABPERR, "DMS activate manual: protected", 0 },
	{ 0x02, 0x0035, ACT_ERR, POL_ABPERR, "DMS set user lock state: protected", 0 },
	{ 0x02, 0x0036, ACT_ERR, POL_ABPERR, "DMS set user lock code: protected", 0 },
	{ 0x02, 0x0038, ACT_ERR, POL_ABPERR, "DMS write user data: protected", 0 },
	{ 0x02, 0x003a, ACT_ERR, POL_ABPERR, "DMS restore factory defaults: protected", 0 },
	{ 0x02, 0x0041, ACT_ERR, POL_ABPERR, "DMS UIM set CK protection: protected", 0 },
	{ 0x02, 0x0042, ACT_ERR, POL_ABPERR, "DMS UIM unblock CK: protected", 0 },
	{ 0x02, 0x0048, ACT_ERR, POL_ABPERR, "DMS set firmware preference: protected", 0 },
	{ 0x02, 0x004a, ACT_ERR, POL_ABPERR, "DMS delete stored image: protected", 0 },
	{ 0x02, 0x0050, ACT_ERR, POL_ABPERR, "DMS set boot image download mode: protected", 0 },
	{ 0x02, 0x0052, ACT_ERR, POL_ABPERR, "DMS set service programming code: protected", 0 },
	{ 0x03, POL_ANY, ACT_FWD, 0, "NAS", 0 },
	{ 0x03, 0x0000, ACT_OK, 0, "NAS reset: protected", 0 },
	{ 0x03, 0x0001, ACT_OK, 0, "NAS abort: protected", 0 },
	{ 0x03, 0x0021, ACT_ERR, POL_ABPERR, "NAS network scan: protected (drops service while it runs)", 0 },
	{ 0x03, 0x0022, ACT_OK, 0, "NAS initiate network register: protected", 0 },
	{ 0x03, 0x0023, ACT_OK, 0, "NAS initiate attach: protected", 0 },
	{ 0x03, 0x0027, ACT_ERR, POL_ABPERR, "NAS set preferred networks: protected", 0 },
	{ 0x03, 0x0029, ACT_ERR, POL_ABPERR, "NAS set forbidden networks: protected", 0 },
	{ 0x03, 0x002a, ACT_ERR, POL_ABPERR, "NAS set technology preference: protected", 0 },
	{ 0x03, 0x002d, ACT_ERR, POL_ABPERR, "NAS set ACCOLC: protected", 0 },
	{ 0x03, 0x0030, ACT_ERR, POL_ABPERR, "NAS set device config: protected", 0 },
	{ 0x03, 0x0033, ACT_OK, 0, "NAS set system selection preference: protected (wland's nolte/csonly own it)", 0 },
	{ 0x03, 0x0057, ACT_ERR, POL_ABPERR, "NAS update AKEY: protected", 0 },
	{ 0x0b, POL_ANY, ACT_FWD, 0, "UIM", 0 },
	{ 0x0b, 0x0000, ACT_OK, 0, "UIM reset: protected", 0 },
	{ 0x0b, 0x0022, ACT_ERR, POL_ABPERR, "UIM write transparent: protected", 0 },
	{ 0x0b, 0x0023, ACT_ERR, POL_ABPERR, "UIM write record: protected", 0 },
	{ 0x0b, 0x0025, ACT_ERR, POL_ABPERR, "UIM set PIN protection (enable/disable): protected", 0 },
	{ 0x0b, 0x0027, ACT_ERR, POL_ABPERR, "UIM unblock PIN: protected", 0 },
	{ 0x0b, 0x0028, ACT_ERR, POL_ABPERR, "UIM change PIN: protected", 0 },
	{ 0x0b, 0x0029, ACT_ERR, POL_ABPERR, "UIM depersonalization: protected", 0 },
	{ 0x0b, 0x0030, ACT_OK, 0, "UIM power down: protected", 0 },
	{ 0x0b, 0x0031, ACT_OK, 0, "UIM power up: protected", 0 },
	{ 0x0b, 0x0033, ACT_OK, 0, "UIM change provisioning session: protected", 0 },
	{ 0x0b, 0x0046, ACT_ERR, POL_ABPERR, "UIM switch slot: protected", 0 },
	{ 0x05, POL_ANY, ACT_FWD, 0, "WMS", 0 },
	{ 0x0a, POL_ANY, ACT_FWD, 0, "CAT", 0 },
	{ 0x0c, POL_ANY, ACT_FWD, 0, "PBM", 0 },
	{ 0x01, POL_ANY, ACT_ERR, POL_ABPERR, "WDS: data stays with wland (P4)", 0 },
	{ 0x04, POL_ANY, ACT_ERR, POL_ABPERR, "QOS: data stays with wland (P4)", 0 },
	{ 0x1a, POL_ANY, ACT_ERR, POL_ABPERR, "WDA: data stays with wland (P4)", 0 },
	{ 0x2a, POL_ANY, ACT_ERR, POL_ABPERR, "DSD: data stays with wland (P4)", 0 },
	{ 0x2f, POL_ANY, ACT_ERR, POL_ABPERR, "DPM: data stays with wland (P4)", 0 },
	{ 0x30, POL_ANY, ACT_ERR, POL_ABPERR, "DFS: data stays with wland (P4)", 0 },
	{ 0x09, POL_ANY, ACT_ERR, POL_ABPERR, "VOICE: not in P2", 0 },
	{ 0x06, POL_ANY, ACT_ERR, POL_ABPERR, "PDS: not in P2", 0 },
	{ 0x06, 0x0000, ACT_OK, 0, "PDS reset: answered here (an error makes locationd reset the baseband)", 0 },
	{ 0x10, POL_ANY, ACT_ERR, POL_ABPERR, "LOC: not in P2", 0 },
	{ 0x08, POL_ANY, ACT_ERR, POL_ABPERR, "AT: not in P2", 0 },
	{ 0x22, POL_ANY, ACT_OK, 0, "COEX: answered success here (would change LTE/Wi-Fi/BT coexistence under wland's Wi-Fi)", 0 },
	{ 0x24, POL_ANY, ACT_ERR, POL_ABPERR, "PDC: modem configuration (MBN) selection: protected", 0 },
	{ 0x2b, POL_ANY, ACT_ERR, POL_ABPERR, "SSCTL: subsystem restart: protected", 0 },
	{ 0x29, POL_ANY, ACT_ERR, POL_ABPERR, "RFRPE: RF settings: protected", 0 },
	{ 0x11, POL_ANY, ACT_ERR, POL_ABPERR, "SAR: RF power: protected", 0 },
};

static const struct pol rules_o1[] = {     /* kern.sweet_qmi_rules 1: P4-design s.5.3 + the user's 2026-11-12 decisions */
	{ 0x02, POL_ANY, ACT_FWD, 0, "DMS", 0 },
	{ 0x02, 0x002e, ACT_FWD, 0, "DMS set operating mode: forwarded (reset mode 4 emulated, mode 6 hard floor)", 0 },
	{ 0x02, 0x0027, ACT_ERR, POL_ABPERR, "DMS UIM set PIN protection: blocked (user: SIM PIN change blocked for now)", 0 },
	{ 0x02, 0x0029, ACT_ERR, POL_ABPERR, "DMS UIM unblock PIN: protected (wrong PUK bricks the SIM)", 0 },
	{ 0x02, 0x002a, ACT_ERR, POL_ABPERR, "DMS UIM change PIN: blocked (user: SIM PIN change blocked for now)", 0 },
	{ 0x02, 0x0032, ACT_ERR, POL_ABPERR, "DMS activate automatic: protected", 0 },
	{ 0x02, 0x0033, ACT_ERR, POL_ABPERR, "DMS activate manual: protected", 0 },
	{ 0x02, 0x0035, ACT_ERR, POL_ABPERR, "DMS set user lock state: protected", 0 },
	{ 0x02, 0x0036, ACT_ERR, POL_ABPERR, "DMS set user lock code: protected", 0 },
	{ 0x02, 0x0041, ACT_ERR, POL_ABPERR, "DMS UIM set CK protection: protected", 0 },
	{ 0x02, 0x0042, ACT_ERR, POL_ABPERR, "DMS UIM unblock CK: protected", 0 },
	{ 0x03, POL_ANY, ACT_FWD, 0, "NAS", 0 },
	{ 0x03, 0x0021, ACT_FWD, 0, "NAS network scan: forwarded, once per 60 s", 60000 },
	{ 0x03, 0x0027, ACT_ERR, POL_ABPERR, "NAS set preferred networks: protected (persistent)", 0 },
	{ 0x03, 0x0029, ACT_ERR, POL_ABPERR, "NAS set forbidden networks: protected (persistent)", 0 },
	{ 0x03, 0x002a, ACT_ERR, POL_ABPERR, "NAS set technology preference: protected (persistent)", 0 },
	{ 0x03, 0x002d, ACT_ERR, POL_ABPERR, "NAS set ACCOLC: protected", 0 },
	{ 0x03, 0x0030, ACT_ERR, POL_ABPERR, "NAS set device config: protected", 0 },
	{ 0x03, 0x0057, ACT_ERR, POL_ABPERR, "NAS update AKEY: protected", 0 },
	{ 0x0b, POL_ANY, ACT_FWD, 0, "UIM", 0 },
	{ 0x0b, 0x0022, ACT_ERR, POL_ABPERR, "UIM write transparent: blocked (user: SIM file writes blocked for now)", 0 },
	{ 0x0b, 0x0023, ACT_ERR, POL_ABPERR, "UIM write record: blocked (user: SIM file writes blocked for now)", 0 },
	{ 0x0b, 0x0025, ACT_ERR, POL_ABPERR, "UIM set PIN protection: blocked (user: SIM PIN change blocked for now)", 0 },
	{ 0x0b, 0x0027, ACT_ERR, POL_ABPERR, "UIM unblock PIN: protected (wrong PUK bricks the SIM)", 0 },
	{ 0x0b, 0x0028, ACT_ERR, POL_ABPERR, "UIM change PIN: blocked (user: SIM PIN change blocked for now)", 0 },
	{ 0x0b, 0x0046, ACT_ERR, POL_ABPERR, "UIM switch slot: protected", 0 },
	{ 0x05, POL_ANY, ACT_FWD, 0, "WMS", 0 },
	{ 0x0a, POL_ANY, ACT_FWD, 0, "CAT", 0 },
	{ 0x0c, POL_ANY, ACT_FWD, 0, "PBM", 0 },
	{ 0x09, POL_ANY, ACT_FWD, 0, "VOICE", 0 },
	{ 0x09, 0x0040, ACT_OK, 0, "VOICE set config: answered here (modem NV)", 0 },
	{ 0x09, 0x0033, ACT_ERR, POL_ABPERR, "VOICE set SUPS: protected (network-side change)", 0 },
	{ 0x09, 0x0039, ACT_ERR, POL_ABPERR, "VOICE set barring password: protected", 0 },
	{ 0x22, POL_ANY, ACT_FWD, 0, "COEX: forwarded (user: Wi-Fi/LTE coexistence messages forwarded)", 0 },
	{ 0x01, POL_ANY, ACT_ERR, POL_ABPERR, "WDS: data stays with wland until phase 4", 0 },
	{ 0x04, POL_ANY, ACT_ERR, POL_ABPERR, "QOS: data stays with wland until phase 4", 0 },
	{ 0x1a, POL_ANY, ACT_ERR, POL_ABPERR, "WDA: data stays with wland until phase 4", 0 },
	{ 0x2a, POL_ANY, ACT_ERR, POL_ABPERR, "DSD: data stays with wland until phase 4", 0 },
	{ 0x2f, POL_ANY, ACT_ERR, POL_ABPERR, "DPM: data stays with wland until phase 4", 0 },
	{ 0x30, POL_ANY, ACT_ERR, POL_ABPERR, "DFS: data stays with wland until phase 4", 0 },
	{ 0x06, POL_ANY, ACT_ERR, POL_ABPERR, "PDS: answered by the policy text", 0 },
	{ 0x06, 0x0000, ACT_OK, 0, "PDS reset: answered here (an error makes locationd reset the baseband)", 0 },
	{ 0x10, POL_ANY, ACT_ERR, POL_ABPERR, "LOC: unsupported", 0 },
	{ 0x08, POL_ANY, ACT_ERR, POL_ABPERR, "AT: unsupported", 0 },
};

/* never forwarded, whatever the lists or the text say (s.5.2) */
static bool
hard_floor(unsigned svc, unsigned msg, const uint8_t *rq, uint32_t rqn, const char **why)
{
	if (svc == 0x02 && (msg == 0x0038 || msg == 0x003a || msg == 0x0048 || msg == 0x004a || msg == 0x0050 || msg == 0x0052)) {
		*why = "hard floor: DMS NV / firmware / factory / SPC";
		return true;
	}
	if (svc == 0x02 && msg == 0x002e && rq) {
		for (uint32_t k = 0; k + 3 <= rqn;) {
			uint32_t l = (uint32_t)(rq[k + 1] | (rq[k + 2] << 8));
			if (k + 3 + l > rqn) {
				break;
			}
			if (rq[k] == 0x01 && l >= 1 && rq[k + 3] == 6) {
				*why = "hard floor: DMS persistent LPM (mode 6)";
				return true;
			}
			k += 3 + l;
		}
	}
	if (svc == 0x0b && msg == 0x0029) {
		*why = "hard floor: UIM depersonalization";
		return true;
	}
	if (svc == 0x24 || svc == 0x11 || svc == 0x29 || svc == 0x2b) {
		*why = "hard floor: PDC / SAR / RFRPE / SSCTL";
		return true;
	}
	return false;
}

#define NOVR    256
static struct pol ovr[NOVR];
static struct { const uint8_t *data; uint16_t dlen; const char *tmpl; } ovr_data[NOVR];
static int novr;
static uint64_t rate_last[NOVR];        /* (rules_o1 index -> last forward time; only rate-limited entries use it) */

static const uint8_t *
parse_hex(const char *h, uint16_t *len)
{
	uint8_t *start = pol_data + pol_dlen;
	uint32_t n = 0;
	int hi = -1;
	for (; *h; h++) {
		int v;
		if (*h == '.' || *h == '_' || *h == ':') {
			continue;
		}
		v = hexval(*h);
		if (v < 0) {
			return NULL;
		}
		if (hi < 0) {
			hi = v;
			continue;
		}
		if (pol_dlen + n >= sizeof(pol_data)) {
			return NULL;
		}
		start[n++] = (uint8_t)(hi << 4 | v);
		hi = -1;
	}
	if (hi >= 0 || n > 4096) {
		return NULL;
	}
	pol_dlen += n;
	*len = (uint16_t)n;
	return start;
}

static int
tlvs_ok(const uint8_t *p, uint32_t n)
{
	while (n) {
		if (n < 3) {
			return 0;
		}
		uint32_t l = (uint32_t)(p[1] | (p[2] << 8));
		if (l > n - 3) {
			return 0;
		}
		p += 3 + l;
		n -= 3 + l;
	}
	return 1;
}

/* ---- SIM identity (n12b): kernel memory only, never logged, never written ---- */

#define SIM_ICCID_LEN   10
#define SIM_IMSI_LEN    8
static struct {
	uint8_t iccid[SIM_ICCID_LEN], imsi[SIM_IMSI_LEN];
	uint8_t iccid_ef[SIM_ICCID_LEN], imsi_ef[9];
	int have_iccid, have_imsi, refresh, tries;
	unsigned gen, reads;
	uint64_t next;
} simid;
static char pol_text[8192];
static uint32_t pol_tlen;

/* "RQ<tlv>:<off>:<len>[+<n>]" or a number; 0 ok */
static int
parse_fref(const char *s, struct fref *f)
{
	unsigned t;
	int off, len, add = 0, k;
	bzero(f, sizeof(*f));
	if (s[0] >= '0' && s[0] <= '9') {
		int v;
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
			unsigned x;
			if (hex_prefix(s + 2, &x, &k)) {
				return -1;
			}
			v = (int)x;
		} else if (dec_prefix(s, &v, &k)) {
			return -1;
		}
		f->tlv = -1;
		f->len = 1;
		f->add = v;
		return 0;
	}
	if ((s[0] != 'R' && s[0] != 'r') || (s[1] != 'Q' && s[1] != 'q')) {
		return -1;
	}
	s += 2;
	if (hex_prefix(s, &t, &k) || t > 255 || s[k] != ':') {
		return -1;
	}
	s += k + 1;
	if (dec_prefix(s, &off, &k) || s[k] != ':') {
		return -1;
	}
	s += k + 1;
	if (dec_prefix(s, &len, &k)) {
		return -1;
	}
	s += k;
	if (off > 4096 || len < 1 || len > 64) {
		return -1;
	}
	if (*s == '+') {
		if (dec_prefix(s + 1, &add, &k) || (len != 1 && len != 2 && len != 4)) {
			return -1;
		}
		f->hasadd = 1;
	}
	f->tlv = (int)t;
	f->off = off;
	f->len = len;
	f->add = add;
	return 0;
}

static const uint8_t *
tlv_find(const uint8_t *p, uint32_t n, unsigned id, uint32_t *len)
{
	for (uint32_t k = 0; k + 3 <= n;) {
		uint32_t l = (uint32_t)(p[k + 1] | (p[k + 2] << 8));
		if (k + 3 + l > n) {
			return NULL;
		}
		if (p[k] == id) {
			*len = l;
			return p + k + 3;
		}
		k += 3 + l;
	}
	return NULL;
}

static int
fref_get(const struct fref *f, const uint8_t *rq, uint32_t rqn, int dummy, uint8_t *out)
{
	uint32_t l = 0;
	const uint8_t *v = dummy ? NULL : tlv_find(rq, rqn, (unsigned)f->tlv, &l);
	if (dummy) {
		bzero(out, (size_t)f->len);
	} else {
		if (!v || (uint32_t)f->off + (uint32_t)f->len > l) {
			return -1;
		}
		memcpy(out, v + f->off, (size_t)f->len);
	}
	if (f->hasadd) {
		uint32_t x = 0;
		for (int i = 0; i < f->len; i++) {
			x |= (uint32_t)out[i] << (8 * i);
		}
		x += (uint32_t)f->add;
		for (int i = 0; i < f->len; i++) {
			out[i] = (uint8_t)(x >> (8 * i));
		}
	}
	return 0;
}

static int
fref_num(const struct fref *f, const uint8_t *rq, uint32_t rqn, uint32_t *x)
{
	uint8_t b[64];
	if (f->tlv < 0) {
		*x = (uint32_t)f->add;
		return 0;
	}
	if (f->len > 4 || fref_get(f, rq, rqn, 0, b)) {
		return -1;
	}
	*x = 0;
	for (int i = 0; i < f->len; i++) {
		*x |= (uint32_t)b[i] << (8 * i);
	}
	return 0;
}

/* template h -> bytes; -1 bad / no room, -2 a request field missing; *missing: an ICCID/IMSI TLV left out */
static int
tmpl_expand(const char *h, const uint8_t *rq, uint32_t rqn, int dummy, uint8_t *out, uint32_t room, int *missing)
{
	uint32_t o = 0;
	int hi = -1;
	*missing = 0;
	for (; *h; h++) {
		if (*h == '.' || *h == '_' || *h == ':') {
			continue;
		}
		if (*h == '{') {
			const char *e = strchr(h, '}');
			if (!e || hi >= 0) {
				return -1;
			}
			size_t nl = (size_t)(e - h - 1);
			if ((nl == 5 && !strncmp(h + 1, "ICCID", 5)) || (nl == 4 && !strncmp(h + 1, "IMSI", 4))) {
				int ic = nl == 5;
				uint32_t l = ic ? SIM_ICCID_LEN : SIM_IMSI_LEN;
				if (dummy || (ic ? simid.have_iccid : simid.have_imsi)) {
					if (o + 2 + l > room) {
						return -1;
					}
					out[o++] = (uint8_t)l;
					out[o++] = 0;
					if (dummy) {
						memset(out + o, 0xff, l);
					} else {
						memcpy(out + o, ic ? simid.iccid : simid.imsi, l);
					}
					o += l;
				} else {
					if (!o) {
						return -1;
					}
					o--;
					*missing = 1;
				}
			} else {
				struct fref f;
				if (parse_fref(h + 1, &f) || o + (uint32_t)f.len > room) {
					return -1;
				}
				if (fref_get(&f, rq, rqn, dummy, out + o)) {
					return -2;
				}
				o += (uint32_t)f.len;
			}
			h = e;
			continue;
		}
		int v = hexval(*h);
		if (v < 0) {
			return -1;
		}
		if (hi < 0) {
			hi = v;
			continue;
		}
		if (o >= room) {
			return -1;
		}
		out[o++] = (uint8_t)(hi << 4 | v);
		hi = -1;
	}
	return hi >= 0 ? -1 : (int)o;
}

static int
tmpl_sim(const char *h)
{
	return h && (find_str(h, "{ICCID}") || find_str(h, "{IMSI}"));
}

static const char *
parse_tmpl(const char *h)
{
	int miss, n = tmpl_expand(h, NULL, 0, 1, B->tmp_buf, sizeof(B->tmp_buf), &miss);
	size_t l = strlen(h);
	if (n < 0 || !tlvs_ok(B->tmp_buf, (uint32_t)n) || pol_tlen + l + 1 > sizeof(pol_text)) {
		return NULL;
	}
	char *t = pol_text + pol_tlen;
	memcpy(t, h, l + 1);
	pol_tlen += (uint32_t)l + 1;
	return t;
}

/* "on svc msg ind|replay|bbreset ..." (line is modified: the 'if' part is cut off) */
static int
parse_trigger(char *line, int ln)
{
	char w[7][256];
	struct trg t;
	bzero(&t, sizeof(t));
	t.cond = -1;
	char *iff = __DECONST(char *, find_str(line, " if "));
	if (iff) {
		char *c = iff + 4;
		while (*c == ' ') {
			c++;
		}
		char *eq = strchr(c, '=');
		if (eq) {
			if (parse_fref(c, &t.fa) || parse_fref(eq + 1, &t.fb) || t.fa.len > 4 || t.fb.len > 4) {
				goto bad;
			}
			t.ceq = 1;
		} else {
			unsigned x;
			int k;
			if (hex_prefix(c, &x, &k) || x > 255) {
				goto bad;
			}
			t.cond = (int)x;
		}
		*iff = 0;
	}
	int k = split_words(line, w, 7);
	int a, b;
	char *colon = k >= 5 && !strcmp(w[3], "ind") ? strchr(w[4], ':') : NULL;
	if (colon) {
		*colon = 0;
		if (parse_num(w[4], &t.tosvc) || t.tosvc < 1 || t.tosvc > 255) {
			goto bad;
		}
		memmove(w[4], colon + 1, strlen(colon + 1) + 1);
	}
	if (k < 5 || ntrg >= NTRG || parse_num(w[1], &t.svc) || t.svc < 1 || t.svc > 255 || parse_num(w[2], &t.msg) ||
	    parse_num(w[4], &a) || a < 0 || a > 0xffff) {
		goto bad;
	}
	t.m2 = (uint16_t)a;
	if (!strcmp(w[3], "ind")) {
		t.kind = TRG_IND;
		if (k >= 6 && strchr(w[5], '{')) {
			t.tmpl = parse_tmpl(w[5]);
			if (!t.tmpl) {
				goto bad;
			}
		} else if (k >= 6) {
			t.data = parse_hex(w[5], &t.dlen);
			if (!t.data || !tlvs_ok(t.data, t.dlen)) {
				goto bad;
			}
		}
	} else if (!strcmp(w[3], "bbreset")) {
		t.kind = TRG_BBRESET;
		if (k >= 6 && !strcmp(w[5], "unpub")) {
			t.m3 = 1;
		} else if (k >= 6 && strcmp(w[5], "msg")) {
			goto bad;
		}
	} else if (!strcmp(w[3], "replay")) {
		t.kind = TRG_REPLAY;
		if (k < 6 || parse_num(w[5], &b) || b < 0 || b > 0xffff) {
			goto bad;
		}
		t.m3 = (uint16_t)b;
		if (k >= 7 && strncmp(w[6], "map", 3)) {
			goto bad;
		}
		if (k >= 7) {
			const char *m = find_str(line, "map");
			m += 3;
			while (*m && t.nmap < TRG_MAXMAP) {
				unsigned x, y;
				int kx, ky;
				while (*m == ' ' || *m == '\t' || *m == ',') {
					m++;
				}
				if (!*m || *m == '\n' || *m == '\r') {
					break;
				}
				if (hex_prefix(m, &x, &kx) || m[kx] != '=' || hex_prefix(m + kx + 1, &y, &ky) || x > 255 || y > 255) {
					goto bad;
				}
				t.map[t.nmap][0] = (uint8_t)x;
				t.map[t.nmap][1] = (uint8_t)y;
				t.nmap++;
				m += kx + 1 + ky;
			}
		}
	} else {
		goto bad;
	}
	trg[ntrg++] = t;
	return 0;
bad:
	LOG("policy line %d: bad 'on' line\n", ln);
	return -1;
}

/*
 * The built-in default override text: agent/stage/abp-policy.voice.conf (the policy on the phone since n12w), the same
 * lines wland's router read from /var/local/abp-policy.conf. Used when no file and no pushed text.
 */
static const char policy_default[] =
    "0xe2 * err:0x5e\n"
    "0xe4 * ok\n"
    "0xe1 * ok\n"
    "0xe7 * ok\n"
    "0x06 0x0000 ok\n"
    "on 0x03 0x0003 replay 0x004d 0x004e if 18\n"
    "0x03 0x5557 ok\n"
    "on 0x03 0x0003 replay 0x004f 0x0051 if 19\n"
    "strip 0x03 0x004e 26\n"
    "strip 0x03 0x004d 26\n"
    "0x06 * ok\n"
    "0x06 0x0036 resp:0102000100\n"
    "0xe2 0xb00c ok\n"
    "on 0xe2 0xb00c ind 0xb00c 01010000.4f010002.50010002.56010001.57010001.42{ICCID}.43{IMSI}\n"
    "0xe2 0xb001 resp:010800e140950078563412\n"
    "0xe2 0xa000 resp:400200{RQ01:2:2+1}\n"
    "on 0xe2 0xa000 ind 0xa001 010200{RQ01:0:1}0010010001 if RQ01:2:2+1=RQ01:4:2\n"
    "0xe2 0xe00d ok\n"
    "0x03 0x555b ok\n"
    "on 0x02 0x002e bbreset 1500 msg if RQ01:0:1=4\n"
    "0x2c * ok\n"
    "on 0x02 0x002e ind 0x2c:0x0800 10010000 if RQ01:0:1=4\n"
    "0x2c 0x0020 resp:100400{RQ01:0:4}\n"
    "0x01 0x00c8 ok\n"
    "0x06 0x0020 resp:0102000101\n"
    "0x09 * fwd\n"
    "0x09 0x0040 ok\n"
    "0x09 0x0033 err:0x5e\n"
    "0x09 0x0039 err:0x5e\n";

/* the text's lines -> ovr / trg / strips (replaces the previous load) */
static void
policy_parse(const char *text, size_t len, const char *src)
{
	int n = 0, bad = 0, ln = 0;
	ntrg = 0;
	nstrip = 0;
	pol_dlen = 0;
	pol_tlen = 0;
	for (size_t at = 0; at < len && n < NOVR;) {
		size_t e = at;
		while (e < len && text[e] != '\n' && text[e] != 0) {
			e++;
		}
		size_t l = e - at < sizeof(B->line) - 1 ? e - at : sizeof(B->line) - 1;
		memcpy(B->line, text + at, l);
		B->line[l] = 0;
		at = e + 1;
		ln++;
		char *line = B->line;
		char *h = strchr(line, '#');
		if (h) {
			*h = 0;
		}
		char w[3][256];
		int k = split_words(line, w, 3);
		if (k >= 1 && !strcmp(w[0], "on")) {
			bad += parse_trigger(line, ln) != 0;
			continue;
		}
		if (k >= 1 && !strcmp(w[0], "strip")) {
			bad += parse_strip(line, ln) != 0;
			continue;
		}
		if (k != 3) {
			continue;
		}
		/* the action word may be longer than 255 (resp:<hex>): take it from the line itself */
		const char *c = line;
		for (int sk = 0; sk < 2; sk++) {
			while (*c && is_space(*c)) {
				c++;
			}
			while (*c && !is_space(*c)) {
				c++;
			}
		}
		while (*c && is_space(*c)) {
			c++;
		}
		char *ce = __DECONST(char *, c);
		while (*ce && !is_space(*ce)) {
			ce++;
		}
		*ce = 0;
		struct pol p = { 0, POL_ANY, ACT_ERR, POL_ABPERR, "conf", 0 };
		const uint8_t *pdata = NULL;
		const char *ptmpl = NULL;
		uint16_t pdlen = 0;
		if (parse_num(w[0], &p.svc) || p.svc < 1 || p.svc > 255 || (strcmp(w[1], "*") && parse_num(w[1], &p.msg))) {
			LOG("policy %s line %d: bad svc/msg\n", src, ln);
			bad++;
			continue;
		}
		if (!strcmp(c, "fwd")) {
			p.act = ACT_FWD;
		} else if (!strcmp(c, "ok")) {
			p.act = ACT_OK;
		} else if (!strcmp(c, "drop")) {
			p.act = ACT_DROP;
		} else if (!strncmp(c, "err:", 4) && !parse_num(c + 4, &p.err)) {
			p.act = ACT_ERR;
		} else if (!strcmp(c, "err")) {
			p.act = ACT_ERR;
		} else if (!strncmp(c, "resp:", 5) && strchr(c + 5, '{') && (ptmpl = parse_tmpl(c + 5)) != NULL) {
			p.act = ACT_RESP;
		} else if (!strncmp(c, "resp:", 5) && (pdata = parse_hex(c + 5, &pdlen)) != NULL && tlvs_ok(pdata, pdlen)) {
			p.act = ACT_RESP;
		} else {
			LOG("policy %s line %d: bad action (fwd | ok | err:<code> | drop | resp:<hex TLVs>)\n", src, ln);
			bad++;
			continue;
		}
		ovr_data[n].data = pdata;
		ovr_data[n].dlen = pdlen;
		ovr_data[n].tmpl = ptmpl;
		ovr[n++] = p;
	}
	novr = n;
	nseen = 0;
	LOG("policy %s: %d override line(s), %d trigger line(s), %d strip line(s)%s, decisions logged again\n", src, n, ntrg,
	    nstrip, bad ? " (some lines refused)" : "");
}

/* policy sources: pushed text (sysctl) > the file > the built-in default */
enum { SRC_DEFAULT, SRC_FILE, SRC_PUSHED };
static int pol_src = -1;
static int64_t file_mtime = -1, file_size = -1;
static LCK_GRP_DECLARE(qmux_grp, "sweet-qmux");
static LCK_MTX_DECLARE(push_lock, &qmux_grp);
static char *push_text;                 /* pending text written to kern.sweet_qmi_policy (NULL none) */
static size_t push_len;
static bool push_pending, push_clear;
static bool have_pushed;

/* read the policy file if it changed; 1 = loaded, 0 = unchanged, -1 = absent */
static int
policy_file_check(bool force)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	struct vnode_attr va;
	int left = 0;
	if (rootvnode == NULLVP || vnode_lookup(POLICY_PATH, 0, &vp, ctx) != 0) {
		return -1;
	}
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	VATTR_WANTED(&va, va_modify_time);
	if (vnode_getattr(vp, &va, ctx) != 0 || va.va_data_size > POLICY_MAX) {
		vnode_put(vp);
		return -1;
	}
	int64_t mt = (int64_t)va.va_modify_time.tv_sec, sz = (int64_t)va.va_data_size;
	if (!force && pol_src == SRC_FILE && mt == file_mtime && sz == file_size) {
		vnode_put(vp);
		return 0;
	}
	size_t n = (size_t)sz;
	char *buf = kalloc_data(n + 1, Z_WAITOK | Z_ZERO);
	if (buf == NULL) {
		vnode_put(vp);
		return -1;
	}
	int e = n ? vn_rdwr(UIO_READ, vp, (caddr_t)buf, (int)n, 0, UIO_SYSSPACE, 0, vfs_context_ucred(ctx), &left,
	    vfs_context_proc(ctx)) : 0;
	vnode_put(vp);
	if (e != 0 || left != 0) {
		kfree_data(buf, n + 1);
		LOG("policy %s: read error %d\n", POLICY_PATH, e);
		return -1;
	}
	file_mtime = mt;
	file_size = sz;
	pol_src = SRC_FILE;
	policy_parse(buf, n, "/var/local/abp-policy.conf");
	kfree_data(buf, n + 1);
	return 1;
}

static void
policy_poll(bool force)
{
	lck_mtx_lock(&push_lock);
	char *t = push_pending ? push_text : NULL;
	size_t tl = push_len;
	bool clear = push_clear;
	if (push_pending) {
		push_text = NULL;
		push_len = 0;
		push_pending = false;
		push_clear = false;
	}
	lck_mtx_unlock(&push_lock);
	if (t) {
		pol_src = SRC_PUSHED;
		have_pushed = true;
		policy_parse(t, tl, "kern.sweet_qmi_policy");
		kfree_data(t, tl + 1);
		return;
	}
	if (clear) {
		have_pushed = false;
		force = true;
		LOG("policy: pushed text forgotten\n");
	}
	if (have_pushed) {
		return;
	}
	int r = policy_file && rootvnode != NULLVP ? policy_file_check(force) : -1;
	if (r < 0 && (pol_src != SRC_DEFAULT || force)) {
		pol_src = SRC_DEFAULT;
		file_mtime = file_size = -1;
		policy_parse(policy_default, sizeof(policy_default) - 1, "built-in default (abp-policy.voice.conf)");
	}
}

static const uint8_t *dec_data;
static uint16_t dec_dlen;
static const char *dec_tmpl;
static int dec_rate;                    /* index into rules_o1 of a rate-limited forward, else -1 */

/* the action for (svc, msg) with the request's TLVs (rq may be NULL: replay GETs); *err the code, why for the log */
static int
decide(unsigned svc, unsigned msg, const uint8_t *rq, uint32_t rqn, int *err, char *whybuf, size_t wl)
{
	const struct pol *p = NULL;
	const struct pol *tab = qmi_rules == 1 ? rules_o1 : rules_p2;
	size_t ntab = qmi_rules == 1 ? sizeof(rules_o1) / sizeof(rules_o1[0]) : sizeof(rules_p2) / sizeof(rules_p2[0]);
	int a, from_conf = 0;
	*err = err_code;
	dec_rate = -1;
	for (int i = 0; i < novr && !p; i++) {
		if (ovr[i].svc == (int)svc && ovr[i].msg == (int)msg) {
			p = &ovr[i];
		}
	}
	for (int i = 0; i < novr && !p; i++) {
		if (ovr[i].svc == (int)svc && ovr[i].msg == POL_ANY) {
			p = &ovr[i];
		}
	}
	from_conf = p != NULL;
	static const struct pol data_pol = { 0, POL_ANY, ACT_FWD, 0, "data: CommCenter owns cellular data (1080, kern.sweet_data_fwd)", 0 };
	if (!p && DSVC(svc) && data_active()) {
		p = &data_pol;
	}
	if (qmi_filter != 0) {
		for (size_t i = 0; i < ntab && !p; i++) {
			if (tab[i].svc == (int)svc && tab[i].msg == (int)msg) {
				p = &tab[i];
			}
		}
		for (size_t i = 0; i < ntab && !p; i++) {
			if (tab[i].svc == (int)svc && tab[i].msg == POL_ANY) {
				p = &tab[i];
			}
		}
	}
	dec_data = NULL;
	dec_dlen = 0;
	dec_tmpl = NULL;
	if (p) {
		a = p->act;
		if (from_conf) {
			dec_data = ovr_data[p - ovr].data;
			dec_dlen = ovr_data[p - ovr].dlen;
			dec_tmpl = ovr_data[p - ovr].tmpl;
		} else if (p->rate_ms && tab == rules_o1) {
			dec_rate = (int)(p - rules_o1);
		}
		if (p->err != POL_ABPERR) {
			*err = p->err;
		}
		snprintf(whybuf, wl, "%s%s", from_conf ? "conf " : "", p->why);
	} else if (svc >= 0xe0) {
		a = ACT_ERR;
		snprintf(whybuf, wl, "Apple-only service");
	} else {
		a = ACT_FWD;
		snprintf(whybuf, wl, "standard service%s", qmi_filter == 0 ? " (filter 0: pass)" : "");
	}
	/* DMS operating mode with the option-1 list: reset (mode 4) stays emulated here (bbreset), others go to the modem */
	if (a == ACT_FWD && !from_conf && svc == 0x02 && msg == 0x002e && qmi_rules == 1) {
		uint32_t l = 0;
		const uint8_t *m = rq ? tlv_find(rq, rqn, 0x01, &l) : NULL;
		if (m && l >= 1 && m[0] == 4) {
			a = ACT_OK;
			snprintf(whybuf, wl, "DMS set operating mode reset: emulated here (radio-only soft reset)");
		}
	}
	const char *fw = NULL;
	if (a == ACT_FWD && svc >= 0xe0) {
		a = ACT_ERR;
		*err = err_code;
		snprintf(whybuf, wl, "Apple-only service: never forwarded");
	} else if (a == ACT_FWD && msg >= 0x5500) {
		a = ACT_ERR;
		*err = err_code;
		snprintf(whybuf, wl, "Apple message id: never forwarded");
	} else if (a == ACT_FWD && hard_floor(svc, msg, rq, rqn, &fw)) {
		a = ACT_ERR;
		*err = err_code;
		cnt.floor++;
		snprintf(whybuf, wl, "%s", fw);
	} else if (a == ACT_FWD && srv_find(svc) < 0) {
		a = ACT_ERR;
		*err = err_code;
		snprintf(whybuf, wl, "no modem server for svc 0x%02x", svc);
	}
	return a;
}

/* ---- logging ---- */

static void
hexline(char *out, size_t outlen, const uint8_t *p, uint32_t n)
{
	size_t o = 0;
	out[0] = 0;
	for (uint32_t i = 0; i < n && o + 4 < outlen; i++) {
		o += (size_t)snprintf(out + o, outlen - o, "%02x%s", p[i], (i + 1) % 4 == 0 && i + 1 < n ? " " : "");
	}
}

static void
note_seen(unsigned svc, unsigned msg, const char *act, const char *why)
{
	for (unsigned i = 0; i < nseen; i++) {
		if (seen[i].svc == svc && seen[i].msg == msg) {
			return;
		}
	}
	if (nseen < sizeof(seen) / sizeof(seen[0])) {
		seen[nseen].svc = (uint8_t)svc;
		seen[nseen].msg = (uint16_t)msg;
		nseen++;
	}
	LOG("NEW #%u %s/%u svc 0x%02x %s msg 0x%04x%s%s -> %s (%s)\n", nseen, pipe_name[cur_pipe], cur_sess, svc, svc_name(svc),
	    msg, svc == 0 ? " " : "", svc == 0 ? ctl_name(msg) : "", act, why);
}

/* a copy of frame f with the SIM identity overwritten by 0xee (for logs) */
static const uint8_t *
redact(const uint8_t *f, uint32_t n)
{
	uint8_t *rb = B->red_buf;
	if ((!simid.have_iccid && !simid.have_imsi) || n > sizeof(B->red_buf)) {
		return f;
	}
	memcpy(rb, f, n);
	const uint8_t *pat[4] = { simid.iccid, simid.iccid_ef, simid.imsi, simid.imsi_ef + 1 };
	const uint32_t pl[4] = { SIM_ICCID_LEN, SIM_ICCID_LEN, SIM_IMSI_LEN - 1, 8 };
	const int son[4] = { simid.have_iccid, simid.have_iccid, simid.have_imsi, simid.have_imsi };
	for (int k = 0; k < 4; k++) {
		for (uint32_t i = 0; son[k] && i + pl[k] <= n; i++) {
			if (!memcmp(rb + i, pat[k], pl[k])) {
				memset(rb + i, 0xee, pl[k]);
			}
		}
	}
	return rb;
}

/* kern.sweet_qmi_filter 2: every frame, first 64 bytes, redacted */
static void
log_frame(unsigned pipe, uint32_t sess, const char *dir, const uint8_t *f, uint32_t n)
{
	char h[3 * 64 + 8];
	if (qmi_filter < 2) {
		return;
	}
	f = redact(f, n);
	hexline(h, sizeof(h), f, n < 64 ? n : 64);
	LOG("%s/%u %s len %u: %s%s\n", pipe < NPIPES ? pipe_name[pipe] : "?", sess, dir, n, h, n > 64 ? ".." : "");
}

/* ---- to CommCenter ---- */

static int
send_to(unsigned pipe, uint32_t sess, const uint8_t *f, uint32_t n)
{
	if (n > ABP_MAXFRAME) {
		return -1;
	}
	log_frame(pipe, sess, "->CC", f, n);
	nframes_out++;
	int e = SweetABPDeliver(pipe, sess, f, n);
	if (e) {
		static unsigned told;
		if (told++ < 20) {
			LOG("deliver to %s/%u (%u bytes) failed: %d\n", pipe < NPIPES ? pipe_name[pipe] : "?", sess, n, e);
		}
		return -1;
	}
	return 0;
}

static int
send_frame(unsigned pipe, const uint8_t *f, uint32_t n)
{
	return send_to(pipe, cur_sess, f, n);
}

/* ---- NAS hold until BSP activation (n12z) ---- */

static int gate_set;
static uint64_t gate_at;
static uint32_t gate_pipe, gate_sess;
static unsigned held_cnt, held_rel_bsp, held_rel_to, held_drop;
static int hold_bypass;                 /* 1073: the re-publish indications go past the hold */

static int
sess_live(uint32_t pipe, uint32_t sess)
{
	for (unsigned i = 0; i < NSESS; i++) {
		if (B->st[i].sess && B->st[i].sess == sess && B->st[i].pipe == pipe) {
			return 1;
		}
	}
	return 0;
}

static int
hold_gate_open(void)
{
	if (gate_set && !sess_live(gate_pipe, gate_sess)) {
		gate_set = 0;
		gate_at = 0;
		wh_start = 0;   /* 1087 */
		LOG("holdnas: gate closed (session %s/%u ended): NAS indications held until the next BSP activation\n",
		    gate_pipe < NPIPES ? pipe_name[gate_pipe] : "?", gate_sess);
	}
	return gate_set && sbb_ms() >= gate_at;
}

static int cid_live(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid);

static int
hold_release(int k, const char *why)
{
	int sent = 0, gone = 0;
	if (k > nhold) {
		k = nhold;
	}
	for (int i = 0; i < k; i++) {
		struct hold *h = &B->holdq[i];
		if (cid_live(h->pipe, h->sess, h->svc, h->cid)) {
			send_to(h->pipe, h->sess, h->f, h->n);
			sent++;
		} else {
			gone++;
			held_drop++;
		}
	}
	memmove(B->holdq, B->holdq + k, sizeof(B->holdq[0]) * (size_t)(nhold - k));
	nhold -= k;
	if (sent || gone) {
		LOG("holdnas: released %d held NAS indication(s) (%s), %d client(s) gone\n", sent, why, gone);
	}
	return sent;
}

/*
 * ---- 1087: kern.sweet_qmi_republish = 4: the network identity withheld until the BSP activation went ----
 * research/cc/P5-sos2-result.md. SpringBoard shows SOS while CoreTelephony's display status is EmergencyOnly; CommCenter
 * publishes it on few events, one of them "Computed MCC/MNC changed" (sub_1003b0fac). Today CommCenter learns 206-01 long
 * before the activation (the 5 s hold timeout fires while the SIM PIN is not entered), publishes kEmergencyOnly at
 * kUnavailable -> kUnknown and never again. Mode 4 only orders and holds genuine modem data: NAS indications stay held
 * (newest of each kept) with no 5 s timeout until the activation went (+republish_ms, then the shot), and CommCenter's own
 * NAS GET answers in that window lose their PLMN-carrying optional TLVs (= "not reported"); the answers are not held,
 * because the window lasts as long as the PIN entry (240 s on 2026-11-13), far beyond a QMI request's wait.
 * Stripped (ids from CommCenter's own parsers, libCommCenterMCommandDrivers): GetSysInfo 0x004d: 0x15 CDMA, 0x16 HDR,
 * 0x17 GSM, 0x18 WCDMA, 0x19 LTE, 0x25 TD-SCDMA, 0x4b NR5G system info (network id / MCC-MNC), 0x2f CDMA derived MCC;
 * GetServingSystem 0x0024: 0x12 current PLMN, 0x27 MNC PCS digit; GetCellLocationInfo 0x0043: every TLV but the result.
 * Kept: service status TLVs (0x10-0x14, 0x24, 0x4a), LTE emergency bearer/access-barred (0x39, 0x3e), SIM reject info,
 * GetHomeNetwork 0x0025 (mandatory TLV: the SIM's home PLMN, which CommCenter reads from the SIM itself anyway).
 * Escape: kern.sweet_qmi_republish 3 (or 0) at run time ends the withholding (the 5 s timeout releases the queue).
 */
static int
wh_active(uint64_t now)
{
	if (repub_mode != 4 || hold_gate_open()) {
		return 0;
	}
	if (wh_start && now - wh_start >= (uint64_t)(wh_s > 0 ? wh_s : 0) * 1000) {
		if (wh_logged != 0xffffffffu) {
			wh_logged = 0xffffffffu;
			printf("SWEET QMUX: republish: PLMN withholding ended after %d s without an activation (%u held, %u stripped)\n",
			    wh_s, wh_held, wh_stripped);
		}
		return 0;
	}
	return 1;
}

static void
wh_note(uint64_t now)
{
	if (!wh_start) {
		wh_start = now ? now : 1;
		wh_held = wh_stripped = wh_logged = 0;
		printf("SWEET QMUX: republish: mode 4: PLMN withheld from CommCenter until the BSP activation (up to %d s)\n", wh_s);
	}
}

static const uint8_t wh_ids_4d[] = { 0x15, 0x16, 0x17, 0x18, 0x19, 0x25, 0x4b, 0x2f };
static const uint8_t wh_ids_24[] = { 0x12, 0x27 };

static uint32_t
wh_strip(unsigned msg, uint8_t *p, uint32_t n)
{
	const uint8_t *ids = NULL;
	int nids = 0, all = 0;
	if (msg == 0x004d) {
		ids = wh_ids_4d;
		nids = (int)sizeof(wh_ids_4d);
	} else if (msg == 0x0024) {
		ids = wh_ids_24;
		nids = (int)sizeof(wh_ids_24);
	} else if (msg == 0x0043) {
		all = 1;
	} else {
		return n;
	}
	uint32_t o = 0;
	int k0 = 0;
	char dropped[64];
	int dl = 0;
	dropped[0] = 0;
	for (uint32_t k = 0; k + 3 <= n;) {
		uint32_t l = 3u + (uint32_t)(p[k + 1] | (p[k + 2] << 8));
		if (k + l > n) {
			break;
		}
		int drop = all && p[k] != 0x02;
		for (int j = 0; j < nids; j++) {
			drop |= ids[j] == p[k];
		}
		if (drop) {
			k0++;
			if (dl < (int)sizeof(dropped) - 4) {
				dl += snprintf(dropped + dl, sizeof(dropped) - (size_t)dl, "%s%02x", dl ? "," : "", p[k]);
			}
		} else {
			memmove(p + o, p + k, l);
			o += l;
		}
		k += l;
	}
	if (k0) {
		wh_stripped++;
		if (wh_logged < 12) {
			wh_logged++;
			LOG("republish: mode 4: NAS answer 0x%04x to CommCenter without TLV(s) %s (PLMN withheld until activation)\n", msg,
			    dropped);
		}
	}
	return o;
}

static void
hold_open(uint32_t pipe, uint32_t sess, int dly, const char *why)
{
	if (!gate_set || gate_pipe != pipe || gate_sess != sess) {
		LOG("holdnas: gate open on %s/%u in %d ms (%s), %d held\n", pipe < NPIPES ? pipe_name[pipe] : "?", sess, dly, why, nhold);
	} else {
		dly = 0;
	}
	gate_set = 1;
	gate_pipe = pipe;
	gate_sess = sess;
	gate_at = sbb_ms() + (uint64_t)(dly > 0 ? dly : 0);
	if (nhold && !dly) {
		hold_release(nhold, why);
	}
}

static int
hold_frame(uint32_t pipe, uint32_t sess, const uint8_t *f, uint32_t n)
{
	if (hold_bypass || !holdnas || n < 13 || n > HOLD_MAXF || f[4] != 0x03 || f[6] != 0x04 || hold_gate_open()) {
		return 0;
	}
	uint64_t whn = sbb_ms();
	int wh = wh_active(whn);        /* 1087 */
	if (wh) {
		wh_note(whn);
		wh_held++;
	}
	if (act_waiting || wh) {        /* 1084/1087: while the activation waits keep only the newest of each indication */
		uint16_t m = (uint16_t)(f[9] | (f[10] << 8));
		for (int i = 0; i < nhold; i++) {
			struct hold *o = &B->holdq[i];
			if (o->pipe == pipe && o->sess == sess && o->cid == f[5] && o->msg == m) {
				o->n = n;
				memcpy(o->f, f, n);
				held_cnt++;
				return 1;
			}
		}
	}
	if (nhold == NHOLD) {
		hold_release(1, "queue full");
	}
	struct hold *h = &B->holdq[nhold++];
	h->used = 1;
	h->pipe = pipe;
	h->sess = sess;
	h->svc = f[4];
	h->cid = f[5];
	h->msg = (uint16_t)(f[9] | (f[10] << 8));
	h->n = n;
	h->t = sbb_ms();
	memcpy(h->f, f, n);
	held_cnt++;
	LOG("holdnas: NAS indication 0x%04x to %s/%u cid %u held (no BSP activation yet; %d held)\n", h->msg,
	    pipe < NPIPES ? pipe_name[pipe] : "?", sess, h->cid, nhold);
	return 1;
}

static void
hold_poll(uint64_t now)
{
	uint64_t to = holdnas >= 100 ? (uint64_t)holdnas : 5000;
	if (!nhold) {
		return;
	}
	if (!holdnas) {
		hold_release(nhold, "holdnas=0");
		return;
	}
	if (hold_gate_open()) {
		int k = hold_release(nhold, "gate open");
		if (repub_mode == 4 && wh_start) {      /* 1087 */
			printf("SWEET QMUX: republish: PLMN withheld until activation (%u indication(s) held, %u answer(s) stripped/held)\n",
			    wh_held, wh_stripped);
			printf("SWEET QMUX: republish: released after activation: %d indication(s) delivered\n", k);
			wh_start = 0;
		}
		return;
	}
	if (act_waiting || wh_active(now)) {
		return;         /* 1084/1087: no hold timeout while the activation is deferred / the PLMN is withheld */
	}
	if (now - B->holdq[0].t >= to + (gate_set ? (uint64_t)holddly : 0)) {
		held_rel_to++;
		hold_open(B->holdq[0].pipe, B->holdq[0].sess, 0, "timeout, no BSP activation");
	}
}

static void
put_tlv(uint8_t *b, uint32_t *o, unsigned t, const void *v, unsigned l)
{
	b[(*o)++] = (uint8_t)t;
	b[(*o)++] = (uint8_t)l;
	b[(*o)++] = (uint8_t)(l >> 8);
	memcpy(b + *o, v, l);
	*o += l;
}

static void
finish_qmux(uint8_t *b, uint32_t n, unsigned svc, unsigned cid)
{
	b[0] = 0x01;
	b[1] = (uint8_t)((n - 1) & 0xff);
	b[2] = (uint8_t)((n - 1) >> 8);
	b[3] = 0x80;
	b[4] = (uint8_t)svc;
	b[5] = (uint8_t)cid;
}

/* ---- router clients ---- */

static void
pend_drop_port(uint32_t port)
{
	for (int i = 0; i < NPEND; i++) {
		if (B->pend[i].used && B->pend[i].port == port) {
			B->pend[i].used = 0;
		}
	}
}

static void
cl_destroy(struct cl *c, const char *why)
{
	if (!c->used) {
		return;
	}
	d_cl_gone(c);                   /* 1080 */
	pend_drop_port(c->port);
	uint32_t pkt[5] = { SBB_QRTR_DEL_CLIENT, SBB_NODE_APPS, c->port, 0, 0 };
	sbb_qrtr_send(SBB_QRTR_DEL_CLIENT, c->port, SBB_NODE_BCAST, SBB_PORT_CTRL, pkt, sizeof(pkt));
	LOG("%s(0x%02x) port %#x given back to the modem (DEL_CLIENT; %s)\n", svc_name(c->svc), c->svc, c->port, why);
	bzero(c, sizeof(*c));
}

static void
cl_free(struct cl *c, const char *why)
{
	if (c->used != 1) {
		return;
	}
	if (d_release(c, why)) {        /* 1080: CommCenter's WDS/QOS data clients: calls stopped, never pooled */
		return;
	}
	if (!reuse) {
		LOG("%s/%u %s(0x%02x) cid %u port %#x released (%s; %u fwd, %u resp, %u ind)\n", pipe_name[c->pipe], c->sess,
		    svc_name(c->svc), c->svc, c->cid, c->port, why, c->fwd, c->resp, c->ind);
		cl_destroy(c, why);
		return;
	}
	pend_drop_port(c->port);
	c->used = 2;
	c->pipe = c->sess = 0;
	c->cid = 0;
}

static struct cl *
cl_get(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid, int create)
{
	struct cl *fr = NULL, *idle = NULL, *anyidle = NULL;
	for (int i = 0; i < NCL; i++) {
		if (cl[i].used == 1 && cl[i].pipe == pipe && cl[i].sess == sess && cl[i].svc == svc && cl[i].cid == cid) {
			return &cl[i];
		}
		if (!cl[i].used && !fr) {
			fr = &cl[i];
		}
		if (cl[i].used == 2 && cl[i].svc == svc && !idle) {
			idle = &cl[i];
		}
		if (cl[i].used == 2 && !anyidle) {
			anyidle = &cl[i];
		}
	}
	if (!create) {
		return NULL;
	}
	if (idle && reuse) {
		idle->used = 1;
		idle->pipe = pipe;
		idle->sess = sess;
		idle->cid = (uint8_t)cid;
		idle->fwd = idle->resp = idle->ind = 0;
		return idle;
	}
	if (!fr && anyidle) {
		cl_destroy(anyidle, "table full: oldest idle of another service");
		fr = anyidle;
	}
	if (!fr) {
		static unsigned told;
		if (told++ < 5) {
			LOG("all %d router clients in use: request refused\n", NCL);
		}
		return NULL;
	}
	uint32_t port = 0;
	for (unsigned k = 0; k < SBB_QMUX_NPORTS && !port; k++) {
		uint32_t pt = SBB_QMUX_PORT0 + (next_port_gen++ % SBB_QMUX_NPORTS);
		int busy = d_port_reserved(pt);
		for (int i = 0; i < NCL; i++) {
			busy |= cl[i].used && cl[i].port == pt;
		}
		if (!busy) {
			port = pt;
		}
	}
	if (!port) {
		return NULL;
	}
	pend_drop_port(port);
	bzero(fr, sizeof(*fr));
	fr->used = 1;
	fr->pipe = pipe;
	fr->sess = sess;
	fr->svc = (uint8_t)svc;
	fr->cid = (uint8_t)cid;
	fr->port = port;
	int s = srv_find(svc);
	LOG("%s/%u %s(0x%02x) cid %u -> QRTR port %#x -> modem %u:%x\n", pipe_name[pipe], sess, svc_name(svc), svc, cid, port,
	    s >= 0 ? srv[s].node : 0, s >= 0 ? srv[s].port : 0);
	return fr;
}

static void
cl_free_sess(uint32_t pipe, uint32_t sess, const char *why)
{
	unsigned n = 0;
	for (int i = 0; i < NCL; i++) {
		if (cl[i].used == 1 && cl[i].pipe == pipe && cl[i].sess == sess) {
			cl_free(&cl[i], why);
			n++;
		}
	}
	if (n && reuse) {
		LOG("%s/%u: %u router client(s) to the idle pool (%s)\n", pipe < NPIPES ? pipe_name[pipe] : "?", sess, n, why);
	}
}

/* ---- CTL ---- */

static uint8_t(*sess_cids(uint32_t pipe, uint32_t sess))[32]
{
	unsigned best = 0;
	for (unsigned i = 0; i < NSESS; i++) {
		if (B->st[i].sess == sess && B->st[i].pipe == pipe) {
			return B->st[i].cids;
		}
		if (B->st[i].sess < B->st[best].sess) {
			best = i;
		}
	}
	if (B->st[best].sess) {
		cl_free_sess(B->st[best].pipe, B->st[best].sess, "session slot reused");
	}
	bzero(&B->st[best], sizeof(B->st[best]));
	B->st[best].pipe = pipe;
	B->st[best].sess = sess;
	return B->st[best].cids;
}

static void
sess_end(uint32_t pipe, uint32_t sess)
{
	for (unsigned i = 0; i < NSESS; i++) {
		if (B->st[i].sess == sess && B->st[i].pipe == pipe) {
			bzero(&B->st[i], sizeof(B->st[i]));
		}
	}
	cl_free_sess(pipe, sess, "session closed");
}

static int
cid_alloc(unsigned svc)
{
	uint8_t(*cids)[32] = sess_cids(cur_pipe, cur_sess);
	for (unsigned c = 1; c < 255; c++) {
		if (!(cids[svc][c / 8] & (1u << (c % 8)))) {
			cids[svc][c / 8] |= (uint8_t)(1u << (c % 8));
			return (int)c;
		}
	}
	return -1;
}

static uint32_t
version_list(uint8_t *v, uint32_t max)
{
	static const uint8_t fixed[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x09, 0x0a, 0x0b, 0x0c, 0x1a, 0x22, 0x24, 0x2a, 0x2b, 0x30 };
	uint8_t list[128];
	unsigned n = 0;
	list[n++] = 0x00;
	for (unsigned s = 1; s < 0xe0 && n < sizeof(list); s++) {
		if (srv_find(s) >= 0) {
			list[n++] = (uint8_t)s;
		}
	}
	if (n <= 1) {
		memcpy(list, fixed, sizeof(fixed));
		n = sizeof(fixed);
	}
	if (1 + n * 5 > max) {
		n = (max - 1) / 5;
	}
	v[0] = (uint8_t)n;
	for (unsigned i = 0; i < n; i++) {
		v[1 + i * 5] = list[i];
		v[2 + i * 5] = 1;
		v[3 + i * 5] = 0;
		v[4 + i * 5] = 0x40;
		v[5 + i * 5] = 0;
	}
	return 1 + n * 5;
}

static void
ctl_answer(unsigned pipe, const uint8_t *s, uint32_t sn)
{
	unsigned tx = s[1], msg = s[2] | (s[3] << 8), ml = s[4] | (s[5] << 8);
	const uint8_t *tl = s + 6;
	uint32_t tn = sn - 6 < ml ? sn - 6 : ml;
	uint8_t b[1024];
	uint32_t o = 12;
	uint16_t res[2] = { 0, 0 };
	int req_svc = -1;
	note_seen(0, msg, "local", "CTL");
	for (uint32_t i = 0; i + 3 <= tn;) {
		unsigned t = tl[i], l = tl[i + 1] | (tl[i + 2] << 8);
		if (t == 0x01 && l >= 1 && i + 3 + l <= tn) {
			req_svc = tl[i + 3];
		}
		if (t == 0x01 && msg == 0x23 && l >= 2 && i + 3 + l <= tn) {
			unsigned sv = tl[i + 3], c = tl[i + 4];
			sess_cids(cur_pipe, cur_sess)[sv][c / 8] &= (uint8_t) ~(1u << (c % 8));
			struct cl *k = cl_get(cur_pipe, cur_sess, sv, c, 0);
			if (k) {
				cl_free(k, "ReleaseClientId");
			}
		}
		i += 3 + l;
	}
	if (msg == 0x22) {
		int c = -1, e = 0;
		int local_svc = 0;
		for (int i = 0; i < novr; i++) {
			if (ovr[i].svc == req_svc && ovr[i].msg == POL_ANY && ovr[i].act != ACT_FWD) {
				local_svc = 1;
			}
		}
		if (req_svc > 0 && req_svc < 0xe0 && cidfail && !local_svc && srv_find((unsigned)req_svc) < 0) {
			e = QMI_ERR_INVALID_SERVICE_TYPE;
		} else {
			c = req_svc >= 0 ? cid_alloc((unsigned)req_svc) : -1;
		}
		if (c < 0) {
			res[0] = 1;
			res[1] = (uint16_t)(e ? e : QMI_ERR_CLIENT_IDS_EXHAUSTED);
			put_tlv(b, &o, 0x02, res, 4);
			LOG("%s/%u GetClientId %s(0x%02x) refused (error 0x%x)\n", pipe_name[pipe], cur_sess, svc_name((unsigned)req_svc),
			    req_svc, res[1]);
		} else {
			put_tlv(b, &o, 0x02, res, 4);
			uint8_t v[2] = { (uint8_t)req_svc, (uint8_t)c };
			put_tlv(b, &o, 0x01, v, 2);
		}
	} else if (msg == 0x23) {
		put_tlv(b, &o, 0x02, res, 4);
		for (uint32_t i = 0; i + 3 <= tn;) {
			unsigned t = tl[i], l = tl[i + 1] | (tl[i + 2] << 8);
			if (t == 0x01 && l == 2 && i + 5 <= tn) {
				put_tlv(b, &o, 0x01, tl + i + 3, 2);
			}
			i += 3 + l;
		}
	} else if (msg == 0x21) {
		uint8_t v[1 + 128 * 5];
		uint32_t vl = version_list(v, (uint32_t)(sizeof(b) - 40 < sizeof(v) ? sizeof(b) - 40 : sizeof(v)));
		put_tlv(b, &o, 0x02, res, 4);
		put_tlv(b, &o, 0x01, v, vl);
	} else if (msg == 0x26) {
		put_tlv(b, &o, 0x02, res, 4);
		for (uint32_t i = 0; i + 3 <= tn;) {
			unsigned t = tl[i], l = tl[i + 1] | (tl[i + 2] << 8);
			if (t == 0x10 && i + 3 + l <= tn && l <= 4) {
				put_tlv(b, &o, 0x10, tl + i + 3, l);
			}
			i += 3 + l;
		}
	} else if (msg == 0x27) {
		put_tlv(b, &o, 0x02, res, 4);
		for (unsigned i = 0; i < NSESS; i++) {
			if (B->st[i].sess == cur_sess && B->st[i].pipe == cur_pipe) {
				bzero(B->st[i].cids, sizeof(B->st[i].cids));
			}
		}
		cl_free_sess(cur_pipe, cur_sess, "CTL Sync");
	} else {
		put_tlv(b, &o, 0x02, res, 4);
	}
	b[6] = 0x01;
	b[7] = (uint8_t)tx;
	b[8] = (uint8_t)msg;
	b[9] = (uint8_t)(msg >> 8);
	b[10] = (uint8_t)((o - 12) & 0xff);
	b[11] = (uint8_t)((o - 12) >> 8);
	finish_qmux(b, o, 0, 0);
	send_frame(pipe, b, o);
}

static void
svc_local_tlv(unsigned pipe, unsigned svc, unsigned cid, const uint8_t *s, int fail, int code, const uint8_t *tl, uint16_t tn)
{
	struct mst *m = mst_get(svc, (unsigned)(s[3] | (s[4] << 8)));
	if (m) {
		if (fail) {
			m->err++;
			m->last_err = (uint16_t)code;
		} else {
			m->ok++;
		}
		mst_dirty = 1;
	}
	if (fail) {
		svc_cnt[svc & 0xff].denied++;
	} else {
		svc_cnt[svc & 0xff].local++;
	}
	uint8_t *b = B->loc_buf;
	uint32_t o = 13;
	uint16_t res[2] = { (uint16_t)(fail ? 1 : 0), (uint16_t)(fail ? code : 0) };
	put_tlv(b, &o, 0x02, res, 4);
	if (!fail && tl && tn && o + tn <= sizeof(B->loc_buf)) {
		memcpy(b + o, tl, tn);
		o += tn;
	}
	b[6] = 0x02;
	b[7] = s[1];
	b[8] = s[2];
	b[9] = s[3];
	b[10] = s[4];
	b[11] = (uint8_t)((o - 13) & 0xff);
	b[12] = (uint8_t)((o - 13) >> 8);
	finish_qmux(b, o, svc, cid);
	send_frame(pipe, b, o);
}

static void
svc_local(unsigned pipe, unsigned svc, unsigned cid, const uint8_t *s, int fail, int code)
{
	svc_local_tlv(pipe, svc, cid, s, fail, code, NULL, 0);
}

static void repub_arm(uint32_t pipe, uint32_t sess);
static int act_defer(uint32_t pipe, uint32_t sess, unsigned cid, const uint8_t *tl, uint32_t tn);

static void
send_ind(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid, unsigned msg, const uint8_t *tl, uint32_t tn)
{
	static uint16_t itx = 1;
	uint8_t *b = B->ind_buf;
	if (13 + tn > sizeof(B->ind_buf)) {
		return;
	}
	if (svc == 0xe2 && msg == 0xb00c && act_defer(pipe, sess, cid, tl, tn)) {
		return;         /* 1084 */
	}
	b[6] = 0x04;
	b[7] = (uint8_t)itx;
	b[8] = (uint8_t)(itx >> 8);
	itx = (uint16_t)(itx + 1 ? itx + 1 : 1);
	b[9] = (uint8_t)msg;
	b[10] = (uint8_t)(msg >> 8);
	b[11] = (uint8_t)(tn & 0xff);
	b[12] = (uint8_t)(tn >> 8);
	if (tn) {
		memmove(b + 13, tl, tn);
	}
	finish_qmux(b, 13 + tn, svc, cid);
	if (hold_frame(pipe, sess, b, 13 + tn)) {
		return;
	}
	send_to(pipe, sess, b, 13 + tn);
	if (svc == 0xe2 && msg == 0xb00c) {
		repub_arm(pipe, sess);
	}
	if (holdnas && svc == 0xe2 && msg == 0xb00c) {
		if (nhold) {
			held_rel_bsp++;
		}
		hold_open(pipe, sess, repub_mode == 4 ? repub_ms : holddly, "BSP activation 0xb00c sent");
	}
}

static void
pend_add(struct pend *x)
{
	int slot = -1;
	uint64_t oldest = ~0ull;
	for (int i = 0; i < NPEND; i++) {
		if (!B->pend[i].used) {
			slot = i;
			break;
		}
		if (B->pend[i].t < oldest) {
			oldest = B->pend[i].t;
			slot = i;
		}
	}
	B->pend[slot] = *x;
	B->pend[slot].used = 1;
	B->pend[slot].t = sbb_ms();
}

static int
tlv_byte(const uint8_t *p, uint32_t n, unsigned id)
{
	for (uint32_t k = 0; k + 3 <= n;) {
		unsigned l = (unsigned)(p[k + 1] | (p[k + 2] << 8));
		if (k + 3 + l > n) {
			return -1;
		}
		if (p[k] == id) {
			return l ? p[k + 3] : -1;
		}
		k += 3 + l;
	}
	return -1;
}

/* ---- SIM identity subscribers ---- */

#define NSUB    8
static struct sub {
	int used, pending;
	uint32_t pipe, sess;
	uint8_t svc, cid;
	uint16_t msg, m2;
	uint8_t rqn, rq[96];
	uint64_t t;
} subs[NSUB];

static int
cid_live(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid)
{
	for (unsigned i = 0; i < NSESS; i++) {
		if (B->st[i].sess && B->st[i].sess == sess && B->st[i].pipe == pipe) {
			return (B->st[i].cids[svc & 0xff][(cid & 0xff) / 8] >> (cid % 8)) & 1;
		}
	}
	return 0;
}

static void
sub_note(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid, unsigned msg, unsigned m2, const uint8_t *rq, uint32_t rqn, int pending)
{
	int k = -1;
	for (int i = 0; i < NSUB; i++) {
		if (subs[i].used && subs[i].pipe == pipe && subs[i].sess == sess && subs[i].svc == svc && subs[i].cid == cid &&
		    subs[i].msg == msg && subs[i].m2 == m2) {
			k = i;
		}
	}
	for (int i = 0; i < NSUB && k < 0; i++) {
		if (!subs[i].used || !cid_live(subs[i].pipe, subs[i].sess, subs[i].svc, subs[i].cid)) {
			k = i;
		}
	}
	if (k < 0) {
		k = 0;
		for (int i = 1; i < NSUB; i++) {
			if (subs[i].t < subs[k].t) {
				k = i;
			}
		}
	}
	struct sub *s = &subs[k];
	s->used = 1;
	s->pending = pending;
	s->pipe = pipe;
	s->sess = sess;
	s->svc = (uint8_t)svc;
	s->cid = (uint8_t)cid;
	s->msg = (uint16_t)msg;
	s->m2 = (uint16_t)m2;
	s->rqn = (uint8_t)(rqn < sizeof(s->rq) ? rqn : sizeof(s->rq));
	if (rq && s->rqn) {
		memcpy(s->rq, rq, s->rqn);
	}
	s->t = sbb_ms();
}

static int
trg_ind(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid, unsigned msg, const struct trg *t, const uint8_t *rq, uint32_t rqn,
    const char *why)
{
	uint8_t *xb = B->trg_buf;
	if (t->tosvc) {
		const uint8_t *tl = t->data;
		uint32_t tn = t->dlen;
		int miss = 0;
		if (t->tmpl) {
			int n = tmpl_expand(t->tmpl, rq, rqn, 0, xb, sizeof(B->trg_buf), &miss);
			if (n < 0 || miss || !tlvs_ok(xb, (uint32_t)n)) {
				return -1;
			}
			tl = xb;
			tn = (uint32_t)n;
		}
		unsigned sent = 0;
		for (unsigned i = 0; i < NSESS; i++) {
			if (!B->st[i].sess) {
				continue;
			}
			for (unsigned c = 1; c < 255; c++) {
				if ((B->st[i].cids[t->tosvc][c / 8] >> (c % 8)) & 1) {
					send_ind(B->st[i].pipe, B->st[i].sess, (unsigned)t->tosvc, c, t->m2, tl, tn);
					sent++;
				}
			}
		}
		synth_inds += sent;
		LOG("on %s(0x%02x) 0x%04x: indication %s(0x%02x) 0x%04x (%u TLV bytes) to %u client(s)\n", svc_name(svc), svc, msg,
		    svc_name((unsigned)t->tosvc), t->tosvc, t->m2, tn, sent);
		return sent ? 0 : -1;
	}
	if (!t->tmpl) {
		synth_inds++;
		LOG("on %s(0x%02x) 0x%04x: indication 0x%04x (%u TLV bytes) to %s/%u cid %u\n", svc_name(svc), svc, msg, t->m2, t->dlen,
		    pipe_name[pipe], sess, cid);
		send_ind(pipe, sess, svc, cid, t->m2, t->data, t->dlen);
		return 0;
	}
	int miss = 0, n = tmpl_expand(t->tmpl, rq, rqn, 0, xb, sizeof(B->trg_buf), &miss);
	if (n < 0 || !tlvs_ok(xb, (uint32_t)n)) {
		LOG("on %s(0x%02x) 0x%04x: indication 0x%04x not sent (%s)\n", svc_name(svc), svc, msg, t->m2,
		    n == -2 ? "the request lacks a template field" : "template does not expand to whole TLVs");
		return -1;
	}
	if (tmpl_sim(t->tmpl)) {
		sub_note(pipe, sess, svc, cid, msg, t->m2, rq, rqn, miss);
	}
	if (miss) {
		LOG("on %s(0x%02x) 0x%04x: indication 0x%04x to %s/%u cid %u deferred: SIM identity not read yet (ICCID %s, IMSI %s)\n",
		    svc_name(svc), svc, msg, t->m2, pipe_name[pipe], sess, cid, simid.have_iccid ? "known" : "unknown",
		    simid.have_imsi ? "known" : "unknown");
		return 1;
	}
	synth_inds++;
	LOG("on %s(0x%02x) 0x%04x: indication 0x%04x (%d TLV bytes, template%s%s) to %s/%u cid %u\n", svc_name(svc), svc, msg, t->m2, n,
	    tmpl_sim(t->tmpl) ? " with the SIM identity" : "", why, pipe_name[pipe], sess, cid);
	send_ind(pipe, sess, svc, cid, t->m2, xb, (uint32_t)n);
	return 0;
}

static void
simid_changed(void)
{
	for (int i = 0; i < NSUB; i++) {
		struct sub *s = &subs[i];
		if (!s->used) {
			continue;
		}
		if (!cid_live(s->pipe, s->sess, s->svc, s->cid)) {
			s->used = 0;
			continue;
		}
		for (int k = 0; k < ntrg; k++) {
			if (trg[k].kind == TRG_IND && trg[k].svc == s->svc && trg[k].msg == s->msg && trg[k].m2 == s->m2 && trg[k].tmpl) {
				trg_ind(s->pipe, s->sess, s->svc, s->cid, s->msg, &trg[k], s->rq, s->rqn, ", SIM identity changed");
				break;
			}
		}
	}
}

/* a successful UIM Read Transparent answer's TLVs for EF file: the identity (values never logged) */
static void
simid_from_read(unsigned file, const uint8_t *tl, uint32_t tn, const char *src)
{
	uint32_t l = 0;
	const uint8_t *v = tlv_find(tl, tn, 0x11, &l);
	if (!v || l < 2 || (uint32_t)(v[0] | (v[1] << 8)) + 2 > l) {
		return;
	}
	uint32_t clen = (uint32_t)(v[0] | (v[1] << 8));
	const uint8_t *ef = v + 2;
	uint8_t val[SIM_ICCID_LEN];
	int ic;
	if (file == 0x2fe2) {
		if (clen != SIM_ICCID_LEN) {
			return;
		}
		for (uint32_t i = 0; i < SIM_ICCID_LEN; i++) {
			val[i] = (uint8_t)(ef[i] << 4 | ef[i] >> 4);
		}
		ic = 1;
	} else if (file == 0x6f07) {
		uint8_t dg[16];
		uint32_t nd = 0;
		if (clen < 2 || !ef[0] || ef[0] > 8 || clen < 1u + ef[0]) {
			return;
		}
		for (uint32_t i = 1; i <= ef[0]; i++) {
			uint8_t lo = ef[i] & 0xf, hi = ef[i] >> 4;
			if (i > 1) {
				if (lo > 9) {
					break;
				}
				dg[nd++] = lo;
			}
			if (hi > 9) {
				break;
			}
			dg[nd++] = hi;
		}
		if (nd < 6 || nd > 15) {
			return;
		}
		for (uint32_t i = 0; i < SIM_IMSI_LEN; i++) {
			uint8_t a = 2 * i < nd ? dg[2 * i] : 0xf, b = 2 * i + 1 < nd ? dg[2 * i + 1] : 0xf;
			val[i] = (uint8_t)(a << 4 | b);
		}
		ic = 0;
	} else {
		return;
	}
	if (ic) {
		memcpy(simid.iccid_ef, ef, SIM_ICCID_LEN);
	} else {
		memcpy(simid.imsi_ef, ef, clen < 9 ? clen : 9);
	}
	uint8_t *have = ic ? simid.iccid : simid.imsi;
	int *known = ic ? &simid.have_iccid : &simid.have_imsi;
	uint32_t len = ic ? SIM_ICCID_LEN : SIM_IMSI_LEN;
	int changed = !*known || memcmp(have, val, len);
	if (changed) {
		LOG("SIM identity: %s %u bytes %s (from %s)\n", ic ? "ICCID" : "IMSI", len, *known ? "CHANGED" : "read", src);
		memcpy(have, val, len);
		*known = 1;
		simid.gen++;
	}
	bzero(val, sizeof(val));
	if (simid.have_iccid && simid.have_imsi) {
		simid.refresh = 0;
		simid.tries = 0;
	}
	if (changed) {
		simid_changed();
	}
}

static void
simid_read(struct cl *c, int si, unsigned file)
{
	static const uint8_t iccid[20] = { 0x01, 2, 0, 0x06, 0x00, 0x02, 5, 0, 0xe2, 0x2f, 2, 0x00, 0x3f, 0x03, 4, 0, 0, 0, 0, 0 };
	static const uint8_t imsi[22] = { 0x01, 2, 0, 0x00, 0x00, 0x02, 7, 0, 0x07, 0x6f, 4, 0x00, 0x3f, 0xff, 0x7f, 0x03, 4, 0, 0, 0, 0, 0 };
	const uint8_t *tl = file == 0x2fe2 ? iccid : imsi;
	uint32_t tn = file == 0x2fe2 ? sizeof(iccid) : sizeof(imsi);
	uint8_t q[7 + 22];
	if (++replay_txn == 0xffff) {
		replay_txn = 0xff00;
	}
	q[0] = 0x00;
	q[1] = (uint8_t)replay_txn;
	q[2] = (uint8_t)(replay_txn >> 8);
	q[3] = 0x20;
	q[4] = 0x00;
	q[5] = (uint8_t)tn;
	q[6] = 0;
	memcpy(q + 7, tl, tn);
	if (sbb_qrtr_send(SBB_QRTR_DATA, c->port, srv[si].node, srv[si].port, q, 7 + tn)) {
		cnt.sendfail++;
		return;
	}
	struct pend x;
	bzero(&x, sizeof(x));
	x.port = c->port;
	x.txn = replay_txn;
	x.msg = 0x20;
	x.svc = 0x0b;
	x.simread = 1;
	x.file = (uint16_t)file;
	pend_add(&x);
	simid.reads++;
}

static void
simid_poll(uint64_t now)
{
	int need = simid.refresh;
	for (int i = 0; i < NSUB && !need; i++) {
		if (subs[i].used && subs[i].pending && cid_live(subs[i].pipe, subs[i].sess, subs[i].svc, subs[i].cid)) {
			need = 1;
		}
	}
	if (!need || now < simid.next) {
		return;
	}
	simid.next = now + 5000;
	if (simid.refresh && ++simid.tries > 3) {
		simid.refresh = 0;
		simid.tries = 0;
		if (simid.have_iccid && simid.have_imsi) {
			return;
		}
	}
	int si = srv_find(0x0b), code;
	char wb[128];
	if (si < 0 || decide(0x0b, 0x0020, NULL, 0, &code, wb, sizeof(wb)) != ACT_FWD) {
		return;
	}
	struct cl *c = NULL;
	for (int i = 0; i < NCL && !c; i++) {
		if (cl[i].used == 1 && cl[i].svc == 0x0b) {
			c = &cl[i];
		}
	}
	for (int i = 0; i < NCL && !c; i++) {
		if (cl[i].used == 2 && cl[i].svc == 0x0b) {
			c = &cl[i];
		}
	}
	if (!c) {
		return;
	}
	LOG("SIM identity: reading EF_ICCID and EF_IMSI (UIM 0x0020 from port %#x)\n", c->port);
	simid_read(c, si, 0x2fe2);
	simid_read(c, si, 0x6f07);
}

/* ---- bbreset (emulated baseband reset for ABM's soft reset; the modem is not touched) ---- */

static uint64_t bbr_unpub_at, bbr_pub_at;
static unsigned bbresets;
static int bbr_unpub;

static void
bbr_publish(int up)
{
	if (bbr_unpub) {
		int e = SweetABPPublish(up != 0, up ? "bbreset: modem back" : "bbreset: modem down");
		LOG("bbreset: %s the stand-ins (%d, emulated reset #%u)\n", up ? "publish" : "un-publish", e, bbresets);
	} else {
		SweetABPBasebandMessage(up ? 0xe3ff8001u : 0xe3ff8000u);
		LOG("bbreset: AppleBaseband message %s (emulated reset #%u)\n", up ? "alive" : "reset detected", bbresets);
	}
}

static void
bbr_poll(uint64_t now)
{
	if (bbr_unpub_at && now >= bbr_unpub_at) {
		bbr_unpub_at = 0;
		bbr_publish(0);
	}
	if (bbr_pub_at && !bbr_unpub_at && now >= bbr_pub_at) {
		bbr_pub_at = 0;
		bbr_publish(1);
	}
}

static void
run_triggers(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid, unsigned msg, const uint8_t *rq, uint32_t rqn)
{
	for (int i = 0; i < ntrg; i++) {
		struct trg *t = &trg[i];
		if (t->svc != (int)svc || t->msg != (int)msg) {
			continue;
		}
		if (t->cond >= 0 && tlv_byte(rq, rqn, (unsigned)t->cond) <= 0) {
			continue;
		}
		if (t->ceq) {
			uint32_t a, b;
			if (fref_num(&t->fa, rq, rqn, &a) || fref_num(&t->fb, rq, rqn, &b) || a != b) {
				continue;
			}
		}
		if (t->kind == TRG_IND) {
			trg_ind(pipe, sess, svc, cid, msg, t, rq, rqn, "");
			continue;
		}
		if (t->kind == TRG_BBRESET) {
			if (!bbr_unpub_at && !bbr_pub_at) {
				bbresets++;
				bbr_unpub = t->m3 == 1;
				bbr_unpub_at = sbb_ms() + 100;
				bbr_pub_at = bbr_unpub_at + (t->m2 < 200 ? 200 : t->m2);
				LOG("on %s(0x%02x) 0x%04x: emulated baseband reset #%u (%s in 100 ms, for %u ms)\n", svc_name(svc), svc, msg,
				    bbresets, bbr_unpub ? "un-publish" : "reset-detect message", t->m2 < 200 ? 200 : t->m2);
			}
			continue;
		}
		int code;
		char wb[128];
		if (decide(svc, t->m2, NULL, 0, &code, wb, sizeof(wb)) != ACT_FWD || dec_rate >= 0) {
			LOG("on %s(0x%02x) 0x%04x: replay GET 0x%04x refused (policy: %s)\n", svc_name(svc), svc, msg, t->m2, wb);
			continue;
		}
		int si = srv_find(svc);
		struct cl *c = cl_get(pipe, sess, svc, cid, 1);
		if (si < 0 || !c) {
			continue;
		}
		uint8_t q[7] = { 0x00, 0, 0, (uint8_t)t->m2, (uint8_t)(t->m2 >> 8), 0, 0 };
		struct pend x;
		bzero(&x, sizeof(x));
		if (++replay_txn == 0xffff) {
			replay_txn = 0xff00;
		}
		q[1] = (uint8_t)replay_txn;
		q[2] = (uint8_t)(replay_txn >> 8);
		x.port = c->port;
		x.txn = replay_txn;
		x.msg = t->m2;
		x.svc = (uint8_t)svc;
		x.replay = 1;
		x.rpipe = pipe;
		x.rsess = sess;
		x.rcid = (uint8_t)cid;
		x.ind_msg = t->m3;
		x.nmap = (uint8_t)t->nmap;
		memcpy(x.map, t->map, sizeof(x.map));
		if (sbb_qrtr_send(SBB_QRTR_DATA, c->port, srv[si].node, srv[si].port, q, sizeof(q))) {
			cnt.sendfail++;
			continue;
		}
		pend_add(&x);
		replays++;
		LOG("on %s(0x%02x) 0x%04x: replay GET 0x%04x -> indication 0x%04x for %s/%u cid %u (txn 0x%04x)\n", svc_name(svc), svc, msg,
		    t->m2, t->m3, pipe_name[pipe], sess, cid, replay_txn);
	}
}

/*
 * ---- 1073: registration re-publish (kern.sweet_qmi_republish) ----
 * research/cc/P5-sos-result.md: CommCenter publishes the registration status (kRegistrationStatusNotification) only on a
 * provisioning change to a non-provisioned state, a call active/type change, the IMS registration callback or the 10 s
 * adjust timer; a plain registration change after kProvisioned does not publish (P5 run d; n12z: the held SysInfo
 * released 500 ms after activation and CommCenter's own Get Serving System answer both arrived after kProvisioned and
 * changed the adjusted status without a publish). The only post-provisioning publish seen without a call (P5 run b) came
 * ~40 ms after kProvisioned (kProvisioned is ~8 ms after the 0xb00c activation indication) while the registration was
 * already known, and n12z's hold keeps it unknown until +500 ms. So re-sending SysInfo "after provisioning" (the n12z
 * release) is not repeated here; instead the router fetches a FRESH SysInfo/SigInfo from the modem and delivers it to
 * CommCenter's registered NAS clients kern.sweet_qmi_republish_ms (default 20) after the activation indication, past the
 * hold: after the kUnavailable -> kUnknown publish (+0 ms, so no kEmergencyOnly) and before that ~+48 ms publish, which
 * then carries the registered status. The delay is the experiment's knob (sweep it, no rebuild).
 * Only modem answers are forwarded (the GET's response TLVs minus the result TLV, the policy's strip rules applied, as the
 * policy's replays); nothing is made up. 1 = one shot per CommCenter BSP session, 2 (write) = a shot now, 0 = off.
 */
#define NNASREG 16
static struct nasreg { int used; uint32_t pipe, sess; uint8_t cid, sys, sig; } nasreg[NNASREG];
static int rp_armed, rp_done;
static uint64_t rp_at, rp_act;
static uint32_t rp_pipe, rp_sess, rp_done_pipe, rp_done_sess;
static unsigned rp_shots;
static uint64_t rp_shot_act;            /* 1082: the activation time of the last auto shot (manual shots: none) */

/*
 * 1082: CommCenter's NAS 0x0003 (Register Indications). 1073 looked for TLV 0x12/0x13 (DDTM / serving system events), so
 * no client ever qualified (phone, ws-n-bb-int2: "nothing sent in 5 s"). The SysInfo / SigInfo switches are TLV 0x18 /
 * 0x19, as the policy's 'replay 0x004d 0x004e if 18' / '0x004f 0x0051 if 19' (hex) and P2-reg-result.md's capture
 * (cid A: 10=01 23=01 13=00 18=01 ...). Every client that sent 0x0003 is recorded (any pipe/session; dead ones drop out
 * through cid_live), with its 0x18/0x19 values (-1 = not in the request).
 */
static void
nasreg_note(uint32_t pipe, uint32_t sess, unsigned cid, const uint8_t *rq, uint32_t rqn)
{
	int sys = tlv_byte(rq, rqn, 0x18), sig = tlv_byte(rq, rqn, 0x19), k = -1;
	for (int i = 0; i < NNASREG && k < 0; i++) {
		if (nasreg[i].used && nasreg[i].pipe == pipe && nasreg[i].sess == sess && nasreg[i].cid == cid) {
			k = i;
		}
	}
	for (int i = 0; i < NNASREG && k < 0; i++) {
		if (!nasreg[i].used || !cid_live(nasreg[i].pipe, nasreg[i].sess, 0x03, nasreg[i].cid)) {
			bzero(&nasreg[i], sizeof(nasreg[i]));
			k = i;
		}
	}
	if (k < 0) {
		return;
	}
	struct nasreg *r = &nasreg[k];
	r->used = 1;
	r->pipe = pipe;
	r->sess = sess;
	r->cid = (uint8_t)cid;
	if (sys >= 0) {
		r->sys = sys == 1;
	}
	if (sig >= 0) {
		r->sig = sig == 1;
	}
	if (repub_mode) {
		char ids[64];
		int io = 0;
		ids[0] = 0;
		for (uint32_t q = 0; q + 3 <= rqn && io < (int)sizeof(ids) - 4;) {
			io += snprintf(ids + io, sizeof(ids) - (size_t)io, "%s%02x", q ? "," : "", rq[q]);
			q += 3 + (unsigned)(rq[q + 1] | (rq[q + 2] << 8));
		}
		LOG("re-publish: NAS client %s/%u cid %u registered indications (TLVs %s): sysinfo(0x18) %d, siginfo(0x19) %d\n",
		    pipe < NPIPES ? pipe_name[pipe] : "?", sess, cid, ids, sys, sig);
	}
}

static int
repub_get(const struct nasreg *r, unsigned get, unsigned ind, int kind)
{
	int si = srv_find(0x03);
	struct cl *c = si >= 0 ? cl_get(r->pipe, r->sess, 0x03, r->cid, 0) : NULL;
	if (!c) {
		return 0;
	}
	if (++replay_txn == 0xffff) {
		replay_txn = 0xff00;
	}
	uint8_t q[7] = { 0x00, (uint8_t)replay_txn, (uint8_t)(replay_txn >> 8), (uint8_t)get, (uint8_t)(get >> 8), 0, 0 };
	if (sbb_qrtr_send(SBB_QRTR_DATA, c->port, srv[si].node, srv[si].port, q, sizeof(q))) {
		cnt.sendfail++;
		return 0;
	}
	struct pend x;
	bzero(&x, sizeof(x));
	x.port = c->port;
	x.txn = replay_txn;
	x.msg = (uint16_t)get;
	x.svc = 0x03;
	x.replay = 1;
	x.repub = (uint8_t)kind;
	x.rpipe = r->pipe;
	x.rsess = r->sess;
	x.rcid = r->cid;
	x.ind_msg = (uint16_t)ind;
	pend_add(&x);
	return 1;
}

/*
 * 1082: which clients get the shot, best first: (1) live NAS clients that switched SysInfo / SigInfo on in 0x0003 (0x18 /
 * 0x19 = 1: GET 0x004d / 0x004f for what each switched on); (2) else every live client that sent 0x0003 at all (both);
 * (3) else every live NAS client of CommCenter (any pipe/session) that has a router client (both). The choice is logged.
 * fallback: 0 = tiers 1-2 only (the auto shot waits for a registration until 5 s), 1 = tier 3 allowed.
 */
static int
repub_send(int kind, int fallback)
{
	int n = 0, tier = 0, nc = 0;
	char who[384];
	int wo = 0;
	who[0] = 0;
	for (int t = 1; t <= 2 && !nc; t++) {
		for (int i = 0; i < NNASREG; i++) {
			struct nasreg *r = &nasreg[i];
			if (!r->used || !cid_live(r->pipe, r->sess, 0x03, r->cid) || (t == 1 && !r->sys && !r->sig)) {
				continue;
			}
			int a = 0, b = 0;
			if (t == 1) {
				a = r->sys ? repub_get(r, 0x004d, 0x004e, kind) : 0;
				b = r->sig ? repub_get(r, 0x004f, 0x0051, kind) : 0;
			} else {
				a = repub_get(r, 0x004d, 0x004e, kind);
				b = repub_get(r, 0x004f, 0x0051, kind);
			}
			if (a + b) {
				nc++;
				n += a + b;
				tier = t;
				if (wo < (int)sizeof(who) - 48) {
					wo += snprintf(who + wo, sizeof(who) - (size_t)wo, " %s/%u cid %u%s%s", pipe_name[r->pipe], r->sess, r->cid,
					    a ? " sys" : "", b ? " sig" : "");
				}
			}
		}
	}
	if (!nc && fallback) {
		for (unsigned i = 0; i < NSESS; i++) {
			if (!B->st[i].sess || B->st[i].pipe >= NPIPES) {
				continue;
			}
			for (unsigned c = 1; c < 255; c++) {
				if (!((B->st[i].cids[0x03][c / 8] >> (c % 8)) & 1)) {
					continue;
				}
				struct nasreg r;
				bzero(&r, sizeof(r));
				r.pipe = B->st[i].pipe;
				r.sess = B->st[i].sess;
				r.cid = (uint8_t)c;
				int a = repub_get(&r, 0x004d, 0x004e, kind), b = repub_get(&r, 0x004f, 0x0051, kind);
				if (a + b) {
					nc++;
					n += a + b;
					tier = 3;
					if (wo < (int)sizeof(who) - 48) {
						wo += snprintf(who + wo, sizeof(who) - (size_t)wo, " %s/%u cid %u sys sig", pipe_name[r.pipe], r.sess, c);
					}
				}
			}
		}
	}
	if (nc) {
		LOG("re-publish (%s): %d GET(s) for %d NAS client(s), %s:%s\n", kind == 2 ? "manual" : "auto", n, nc,
		    tier == 1 ? "registered for SysInfo/SigInfo (0x0003 TLV 0x18/0x19 = 1)" : tier == 2 ?
		    "fallback: sent 0x0003, none switched 0x18/0x19 on" : "fallback: every live NAS client (no 0x0003 seen)", who);
	}
	return n;
}

static void
repub_arm(uint32_t pipe, uint32_t sess)
{
	if ((repub_mode != 1 && repub_mode != 3 && repub_mode != 4) || (rp_done && rp_done_pipe == pipe && rp_done_sess == sess) ||
	    (rp_armed && rp_pipe == pipe && rp_sess == sess)) {
		return;
	}
	rp_armed = 1;
	rp_pipe = pipe;
	rp_sess = sess;
	rp_act = sbb_ms();
	rp_at = rp_act + (uint64_t)(repub_ms > 0 ? repub_ms : 0);
}

/*
 * ---- 1085: kern.sweet_qmi_plmnblip: make CommCenter re-publish its registration status after provisioning ----
 * research/cc/P5-sos2-result.md. SpringBoard's status bar (SystemStatusServer STTelephonyStatusDomainDataProvider) shows
 * SOS when CoreTelephony's registration display status is kCTRegistrationStatusEmergencyOnly, and re-reads it only on
 * CommCenter's kRegistrationDisplayStatusNotification. CommCenter 16.7 publishes (sub_1006e93e4) only on: a provisioned-
 * state change to a non-kProvisioned value (reported status then can never be registered: registered -> kEmergencyOnly),
 * the 10 s adjust timer, IMS registration, call active/type changes, and "Computed MCC/MNC changed" (sub_1003b0fac).
 * With 1084 the modem is registered when the BSP activation goes, so the kUnavailable -> kUnknown publish says
 * kEmergencyOnly, kUnknown -> kProvisioned does not publish, MCC/MNC are already known, and the bar stays SOS.
 * Mode 1: plmnblip_ms after the shot's SysInfo indication (so CommCenter is kProvisioned), one copy of it with the LTE
 * system info PLMN (TLV 0x19, network id at offset 19) MNC digits set to '9' (MCC kept: home-country logic unchanged),
 * then the unchanged copy 300 ms later. Each MCC/MNC change makes CommCenter publish the reported status, now
 * kRegisteredRoaming/Home. The copies use the shot's content; a real modem SysInfo in between restores the PLMN too.
 */
static struct {
	int step;                       /* 0 idle, 1 blip due, 2 restore due */
	uint64_t at;
	uint32_t pipe, sess, n;
	unsigned cid, svc, msg;
	uint8_t tl[ABP_MAXFRAME];
} blip;
static uint32_t blip_sent;

static void
blip_arm(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid, unsigned msg, const uint8_t *tl, uint32_t n)
{
	if (!plmnblip || svc != 0x03 || msg != 0x004e || n > sizeof(blip.tl)) {
		return;
	}
	blip.pipe = pipe;
	blip.sess = sess;
	blip.svc = svc;
	blip.cid = cid;
	blip.msg = msg;
	blip.n = n;
	memcpy(blip.tl, tl, n);
	blip.at = sbb_ms() + (uint64_t)(plmnblip_ms > 0 ? plmnblip_ms : 0);
	blip.step = 1;
	printf("SWEET QMUX: plmnblip: armed for %s/%u cid %u, %d ms\n", pipe_name[pipe], sess, cid, plmnblip_ms);
}

static void
blip_poll(uint64_t now)
{
	if (!blip.step || now < blip.at) {
		return;
	}
	if (!plmnblip || !cl_get(blip.pipe, blip.sess, blip.svc, blip.cid, 0)) {
		printf("SWEET QMUX: plmnblip: %s, nothing sent\n", plmnblip ? "client gone" : "switched off");
		blip.step = 0;
		return;
	}
	if (blip.step == 2) {
		hold_bypass = 1;
		send_ind(blip.pipe, blip.sess, blip.svc, blip.cid, blip.msg, blip.tl, blip.n);
		hold_bypass = 0;
		blip_sent++;
		printf("SWEET QMUX: plmnblip: real SysInfo 0x%04x again (%u TLV bytes) to %s/%u cid %u [t=%llu ms]\n", blip.msg, blip.n,
		    pipe_name[blip.pipe], blip.sess, blip.cid, (unsigned long long)now);
		blip.step = 0;
		return;
	}
	uint32_t ll = 0;
	const uint8_t *lte = tlv_find(blip.tl, blip.n, 0x19, &ll);
	if (lte == NULL || ll < 26 || lte[19] != 1 || lte[20] < '0' || lte[20] > '9' || lte[23] < '0' || lte[23] > '9') {
		printf("SWEET QMUX: plmnblip: no LTE PLMN in TLV 0x19 (len %u, valid %d), nothing sent\n", lte ? ll : 0,
		    lte && ll >= 20 ? lte[19] : -1);
		blip.step = 0;
		return;
	}
	uint8_t m[3];
	uint8_t *w = (uint8_t *)(uintptr_t)lte;
	memcpy(m, w + 23, 3);
	for (int i = 0; i < 3; i++) {
		if (w[23 + i] >= '0' && w[23 + i] <= '9') {
			w[23 + i] = (w[23 + i] == '9') ? '8' : '9';     /* never the real MNC */
		}
	}
	hold_bypass = 1;
	send_ind(blip.pipe, blip.sess, blip.svc, blip.cid, blip.msg, blip.tl, blip.n);
	hold_bypass = 0;
	blip_sent++;
	printf("SWEET QMUX: plmnblip: SysInfo 0x%04x with MNC %c%c%c instead of the modem's to %s/%u cid %u [t=%llu ms]\n", blip.msg,
	    w[23], w[24], w[25] >= '0' && w[25] <= '9' ? w[25] : ' ', pipe_name[blip.pipe], blip.sess, blip.cid,
	    (unsigned long long)now);
	memcpy(w + 23, m, 3);
	blip.step = 2;
	blip.at = now + 300;
}

static void
repub_poll(uint64_t now)
{
	blip_poll(now);
	if (repub_now) {
		repub_now = 0;
		if (!repub_send(2, 1)) {
			printf("SWEET QMUX: republish (manual): nothing sent (no live NAS client of CommCenter with a router client, "
			    "or no modem NAS server)\n");
		}
	}
	if (!rp_armed || now < rp_at) {
		return;
	}
	if ((repub_mode != 1 && repub_mode != 3 && repub_mode != 4) || !sess_live(rp_pipe, rp_sess)) {
		rp_armed = 0;
		return;
	}
	/* 1082: registered clients first; after 1 s more without one, every live NAS client */
	int n = repub_send(1, now - rp_at >= 1000);
	if (n) {
		rp_armed = 0;
		rp_done = 1;
		rp_done_pipe = rp_pipe;
		rp_done_sess = rp_sess;
		LOG("re-publish (auto): %d GET(s) to the modem, %llu ms after the BSP activation indication on %s/%u\n", n,
		    (unsigned long long)(now - rp_act), pipe_name[rp_pipe], rp_sess);
		rp_shot_act = rp_act;
	} else if (now - rp_act > 5000) {
		rp_armed = 0;
		printf("SWEET QMUX: republish (auto): nothing sent in 5 s after the BSP activation on %s/%u (no live NAS client of "
		    "CommCenter with a router client)\n", pipe_name[rp_pipe], rp_sess);
	}
}

/*
 * ---- 1084: kern.sweet_qmi_republish = 3: BSP activation deferred until the modem has service ----
 * Cold boot (phone, ws-n-bb-int4): the SIM PIN is entered late, the activation indication goes out as soon as the SIM
 * identity is known, the modem is still searching (SysInfo: LTE srv none) and the one re-publish shot carries no service,
 * so the bar stays SOS. Mode 3 holds the 0xb00c indication (content as built, SIM identity in kernel memory only) until the
 * router sees usable service from the modem, then delivers it, which opens the NAS hold and arms the +republish_ms shot.
 * Safety (research/cc/P2-bsp-result.md C, run base2: no activation indication at all for 90 s -> CommCenter keeps running,
 * 1 reset, 0 crashes; and today's SIM-identity defer already holds the activation for as long as the PIN takes, 139 s on
 * the cold boot above, without resets): CommCenter only stays kUnavailable (no carrier-bundle push, no publish) meanwhile.
 * "Usable" = the newest modem SysInfo (0x004d answers, 0x004e indications, our polls) has srv_status 2 (service) on any
 * RAT TLV (0x10-0x15, NR5G 0x4a), or a Serving System (0x0024) says registered. Poll: GET 0x004d every 2 s on a router
 * NAS port. kern.sweet_qmi_republish_wait_s (45): then the activation goes anyway. The timer starts when the indication is
 * first deferred, i.e. once the SIM identity is known (PIN entered). While deferred, NAS indications stay held (no
 * timeout, newest per message kept).
 */
static struct { int set; uint32_t pipe, sess; uint8_t cid; uint32_t tn; uint8_t tl[512]; } actq;
static int act_bypass, act_rel_set, srv_state = -1, srv_usable;
static uint32_t act_rel_pipe, act_rel_sess;
static uint64_t act_since, act_next_probe;
static unsigned act_probes;

static const char *
srv_name(int s)
{
	return s == 2 ? "full" : s == 1 ? "limited" : s == 0 ? "none" : "unknown";
}

/* the modem's SysInfo TLVs (answer or indication, result TLV allowed) */
static void
nas_srv_note(const uint8_t *p, uint32_t n)
{
	static const uint8_t ids[] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x4a };
	int best = -1;
	for (unsigned i = 0; i < sizeof(ids); i++) {
		uint32_t l = 0;
		const uint8_t *v = tlv_find(p, n, ids[i], &l);
		if (v && l >= 1) {
			int s = v[0] == 2 ? 2 : (v[0] == 1 || v[0] == 3) ? 1 : 0;
			best = s > best ? s : best;
		}
	}
	if (best >= 0) {
		srv_state = best;
		srv_usable = best == 2;
	}
}

/* Serving System (0x0024 answer / indication) TLV 0x01: registration state 1 = registered */
static void
nas_ss_note(const uint8_t *p, uint32_t n)
{
	uint32_t l = 0;
	const uint8_t *v = tlv_find(p, n, 0x01, &l);
	if (v && l >= 1 && v[0] == 1) {
		srv_state = 2;
		srv_usable = 1;
	}
}

static int
act_defer(uint32_t pipe, uint32_t sess, unsigned cid, const uint8_t *tl, uint32_t tn)
{
	if (act_bypass || (repub_mode != 3 && repub_mode != 4) || srv_usable || tn > sizeof(actq.tl) ||
	    (act_rel_set && act_rel_pipe == pipe && act_rel_sess == sess)) {
		return 0;
	}
	actq.set = 1;
	actq.pipe = pipe;
	actq.sess = sess;
	actq.cid = (uint8_t)cid;
	actq.tn = tn;
	memcpy(actq.tl, tl, tn);
	if (!act_waiting) {
		act_waiting = 1;
		act_since = sbb_ms();
		act_next_probe = act_since;
		printf("SWEET QMUX: republish: activation deferred (modem srv %s), waiting up to %d s [%s/%u]\n", srv_name(srv_state),
		    repub_wait_s, pipe < NPIPES ? pipe_name[pipe] : "?", sess);
	}
	return 1;
}

static void
act_probe(void)
{
	int si = srv_find(0x03);
	struct cl *c = NULL;
	for (int i = 0; i < NCL && !c && si >= 0; i++) {
		if (cl[i].used == 1 && cl[i].svc == 0x03) {
			c = &cl[i];
		}
	}
	if (!c) {
		return;
	}
	if (++replay_txn == 0xffff) {
		replay_txn = 0xff00;
	}
	uint8_t q[7] = { 0x00, (uint8_t)replay_txn, (uint8_t)(replay_txn >> 8), 0x4d, 0x00, 0, 0 };
	if (sbb_qrtr_send(SBB_QRTR_DATA, c->port, srv[si].node, srv[si].port, q, sizeof(q))) {
		cnt.sendfail++;
		return;
	}
	struct pend x;
	bzero(&x, sizeof(x));
	x.port = c->port;
	x.txn = replay_txn;
	x.msg = 0x004d;
	x.svc = 0x03;
	x.replay = 1;
	x.probe = 1;
	pend_add(&x);
	act_probes++;
}

static void
act_poll(uint64_t now)
{
	if (!act_waiting) {
		return;
	}
	if (!actq.set || !sess_live(actq.pipe, actq.sess)) {
		LOG("republish: deferred activation dropped (CommCenter session %s/%u ended)\n",
		    actq.pipe < NPIPES ? pipe_name[actq.pipe] : "?", actq.sess);
		act_waiting = 0;
		actq.set = 0;
		return;
	}
	int go = 0;
	if (srv_usable) {
		printf("SWEET QMUX: republish: modem srv full after %llu ms: activation delivered (%u poll(s))\n",
		    (unsigned long long)(now - act_since), act_probes);
		go = 1;
	} else if (repub_mode != 3 && repub_mode != 4) {
		printf("SWEET QMUX: republish: mode %d now: deferred activation delivered after %llu ms\n", repub_mode,
		    (unsigned long long)(now - act_since));
		go = 1;
	} else if (now - act_since >= (uint64_t)(repub_wait_s > 0 ? repub_wait_s : 0) * 1000) {
		printf("SWEET QMUX: republish: no usable service in %d s (modem srv %s, %u poll(s)): activation delivered anyway\n",
		    repub_wait_s, srv_name(srv_state), act_probes);
		go = 1;
	} else if (now >= act_next_probe) {
		act_next_probe = now + 2000;
		act_probe();
	}
	if (!go) {
		return;
	}
	act_waiting = 0;
	act_rel_set = 1;
	act_rel_pipe = actq.pipe;
	act_rel_sess = actq.sess;
	act_probes = 0;
	act_bypass = 1;
	send_ind(actq.pipe, actq.sess, 0xe2, actq.cid, 0xb00c, actq.tl, actq.tn);
	act_bypass = 0;
	actq.set = 0;
	bzero(actq.tl, sizeof(actq.tl));
}

static void
svc_forward(unsigned pipe, unsigned svc, unsigned cid, const uint8_t *s, uint32_t sn)
{
	int si = srv_find(svc);
	struct cl *c = si >= 0 ? cl_get(pipe, cur_sess, svc, cid, 1) : NULL;
	unsigned ml = s[5] | (s[6] << 8);
	uint32_t len = 7 + (ml < sn - 7 ? ml : sn - 7);
	if (!c) {
		cnt.noserver++;
		svc_local(pipe, svc, cid, s, 1, si < 0 ? QMI_ERR_NOT_SUPPORTED : QMI_ERR_CLIENT_IDS_EXHAUSTED);
		return;
	}
	if (sbb_qrtr_send(SBB_QRTR_DATA, c->port, srv[si].node, srv[si].port, s, len)) {
		cnt.sendfail++;
		svc_local(pipe, svc, cid, s, 1, 0x03);
		return;
	}
	c->fwd++;
	cnt.fwd++;
	svc_cnt[svc & 0xff].fwd++;
	struct pend x;
	bzero(&x, sizeof(x));
	x.port = c->port;
	x.txn = (uint16_t)(s[1] | (s[2] << 8));
	x.msg = (uint16_t)(s[3] | (s[4] << 8));
	x.svc = (uint8_t)svc;
	uint32_t tn = len - 7 < sizeof(x.rq) ? len - 7 : (uint32_t)sizeof(x.rq);
	x.rqn = (uint8_t)tn;
	memcpy(x.rq, s + 7, tn);
	pend_add(&x);
}

static void result_of(const uint8_t *d, uint32_t n, unsigned *res, unsigned *err);
/*
 * ---- 1080 (P4 M3c): CommCenter owns cellular data ----
 * Active only when sweetbb & 0x10 (SBB_DATA: 1075 pdp_ip + 1076 kernel IPA) AND kern.sweet_qmi_rules = 1 AND
 * kern.sweet_data_fwd = 1 (default 1; write 0 = data services answered err 0x5e again, no reboot: live calls stopped,
 * data clients given back). Otherwise nothing below runs and the router behaves exactly as before.
 *   - WDS 0x01, WDA 0x1a, QOS 0x04, DSD 0x2a, DFS 0x30 forwarded per client (the policy text's exact/wildcard lines still
 *     win); DPM stays err (the kernel IPA opens the DPM port itself). WDS/QOS clients are never pooled: released (CTL
 *     Release / Sync / session close) -> StopNetworkInterface for every packet data handle learned from its
 *     StartNetworkInterface answers (own txn from the client's port, answers not forwarded), DEL_CLIENT after the last
 *     answer or 5 s.
 *   - WDA SetDataFormat 0x0020: link layer / aggregation / endpoint TLVs rewritten to what sweet_ipa.c implements (wland's
 *     proven X1 request with qmapv5=1: 0x11 raw IP 2, 0x12/0x13 QMAPv5 9, 0x15 DL 16 datagrams, 0x16 DL 0x3800 bytes,
 *     0x17 endpoint {4 embedded, 1}; 0x10 QoS header and 0x19 flow control forced 0 when sent; missing ones added); the
 *     answer (and GetDataFormat 0x0021's) shows CommCenter its own values for the ids it sent (0x10-0x16, same length).
 *   - WDS BindMuxDataPort 0x00a2 / QOS BindDataPort 0x002b: endpoint TLV 0x10 = {4, 1} (added when absent); mux TLV 0x11
 *     recorded per client; a mux missing from kern.sweet_pdp_mux is given a free pdp_ip unit (logged).
 *   - WDS 0x5564 (Apple-only) answered success here.
 *   - WDS StartNetworkInterface 0x0020 held until sweet_ipa_ready() (15 s, then QMI error 0x0e CALL_FAILED); at most 6
 *     per minute per mux (else 0x0e). Success -> pdp_ip link up + sweet_ipa_call(mux, 1, gw); the IPv4 gateway comes
 *     from CommCenter's own GetRuntimeSettings answer, else after 2 s from the router's own 0x002d on that client's port
 *     (answer not forwarded). StopNetworkInterface answer / PktSrvcStatus ind "disconnected" / client gone -> when the
 *     mux has no live handle left: sweet_ipa_call(mux, 0, 0) + pdp_ip link down.
 *   - Profile-write gate (user decision D2): WDS Create/Modify/Delete profile (0x0027/0x0028/0x0029) and the ids in
 *     kern.sweet_wds_gate_ids (default 0x004a set default profile, 0x0093 set LTE attach PDN list) are held until the
 *     modem's 3GPP profile list + every profile's settings (0x002a/0x002b, read-only, router port 0x43ff, credential TLVs
 *     0x1b/0x1c/0x9b/0x9c removed) were exposed once by RO root-only kern.sweet_wds_profiles_bak and userland wrote 1
 *     to kern.sweet_wds_profiles_ack (agent/stage/wds-profile-backup.sh); 60 s max, then QMI error 0x34. The backup is
 *     started at CommCenter's first WDS request, before any write. After the ack each write passes, logged with the
 *     profile and TLV ids only.
 *   - kern.sweet_data_stats (RO), kern.sweet_data_fwd (kill switch), kern.sweet_data_opts (1 = drop WDA request TLVs
 *     outside 0x10-0x19, 2 = add WDS bind client type 0x13 = 1 tethered as wland did; default 0).
 * Logs "SWEET QMUX: data: ..." and "SWEET QMUX: profile backup ...".
 */
#define QMI_ERR_CALL_FAILED     0x0e
#define QMI_ERR_DEVICE_NOT_READY 0x34
#define DOPT_WDA_STRIP          0x1
#define DOPT_BIND_TETH          0x2
#define DOPT_WDA_OWN            0x4     /* 1088: the router sends its own WDA SetDataFormat (default on) */
#define D_HELD_START_MS         15000u
#define D_HELD_PROF_MS          60000u
#define D_GW_WAIT_MS            2000u
#define D_OWN_MS                5000u
#define D_CLOSE_MS              5000u
#define D_RATE_N                6
#define D_RATE_MS               60000u
#define D_PROF_PORT             (SBB_QMUX_PORT0 + SBB_QMUX_NPORTS - 1u)
#define D_WDA_PORT              (SBB_QMUX_PORT0 + SBB_QMUX_NPORTS - 2u)        /* 1088: the router's WDA client */
#define D_WDA_RETRY_MS          10000u
#define D_WDA_TRIES             3
#define D_NH                    4
#define D_NHOLD                 16
#define D_HOLD_MAXF             2048u
#define D_NOWN                  32
#define D_NPROF                 24
#define D_BAK_MAX               (48u << 10)
#define D_NGATE                 8

static int data_fwd = 1, data_opts = DOPT_WDA_OWN;  /* 1088: bit 0x4 on by default */
static int data_was;                    /* data_active() as the thread saw it last (logs + kill switch) */
static uint16_t gate_ids[D_NGATE] = { 0x0027, 0x0028, 0x0029, 0x004a, 0x0093 };     /* [0..2] always */
static int ngate = 5;

struct dcl {                            /* per router client, same index as cl[] */
	uint8_t mux, nh, closing, gw_state;     /* gw_state: 0 -, 1 waiting for CommCenter's 0x002d, 2 own 0x002d out, 3 done */
	uint32_t h[D_NH];
	uint64_t at;                            /* closing deadline / end of the gateway wait */
};
enum { DH_START = 1, DH_PROF };
struct dhold {
	int used, kind;
	uint32_t pipe, sess, n;
	uint8_t svc, cid;
	uint16_t msg;
	uint64_t t;
	uint8_t f[D_HOLD_MAXF];
};
enum { DO_STOP = 1, DO_GW, DO_PLIST, DO_PSET, DO_WDA };
struct down {
	int used, kind, ci;
	uint32_t port, hd;
	uint16_t txn, msg;
	uint8_t idx;
	uint64_t t;
};
static struct qdata {
	struct dcl d[NCL];
	struct dhold hold[D_NHOLD];
	struct down own[D_NOWN];
	struct { uint8_t up; uint32_t gw; uint64_t st[D_RATE_N]; } mx[256];
	uint8_t wda_req[512];                   /* CommCenter's last WDA SetDataFormat TLVs, as it sent them */
	uint32_t wda_n;
	uint8_t tbuf[D_HOLD_MAXF + 64];         /* a translated request SDU */
	uint8_t pidx[D_NPROF];
	int nprof, pnext;
	char bak[D_BAK_MAX];
	uint32_t bakn;
} *D;
static uint16_t d_txn = 0xfe00;
static struct {
	unsigned held_start, rel_start, to_start, rate, held_prof, rel_prof, to_prof, prof_writes, wda_tr, wda_ans, bind_tr,
	    gw_own, gw_cc, stops_sent, starts_ok, starts_err, local5564, muxmap, ups, downs, closes;
} dst;
static int pb_state;                    /* 0 none, 1 list out, 2 settings out, 3 ready (exposed), 4 acked */
static int pb_tries, pb_n;
static volatile int pb_ack;
/*
 * 1088: the router's own WDA SetDataFormat. On an iPhone ABM's DataModule sets the data format once per baseband boot;
 * here ABM's boot is answered by the router, so CommCenter never even allocates a WDA client (2026-11-13: no CTL
 * GetClientId for 0x1a, kern.sweet_data_stats "fwd WDA 0") and the modem's IPA endpoint keeps its default format.
 * With kern.sweet_data_opts & 0x4 (default on) and data forwarding active, once sweet_ipa_ready(): WDA 0x0020 from the
 * router's own port D_WDA_PORT with wland's proven X1 values (0x11 raw IP 2, 0x12/0x13 QMAPv5 9, 0x15 DL 16 datagrams,
 * 0x16 DL 0x3800 bytes, 0x17 endpoint {4 embedded, 1}); answer logged; 3 tries 10 s apart; sent again when the WDA
 * server goes or moves (modem restart). WDS StartNetworkInterface stays held (the 15 s hold) until it is answered.
 * Bit clear = no router WDA request (StartNetworkInterface waits for the IPA only, as 1080 did).
 */
static int wda_st;                      /* 0 not sent, 1 out, 2 modem ok, 3 error/no answer */
static int wda_tries;
static uint32_t wda_node, wda_port;
static uint64_t wda_next;
static unsigned wda_res = 0xffff, wda_err = 0xffff;
static void wda_own_answer(const uint8_t *tl, uint32_t tn, unsigned res, unsigned er);
static LCK_MTX_DECLARE(bak_lock, &qmux_grp);

static bool
data_active(void)
{
	return D != NULL && (sweet_bb & SBB_DATA) && qmi_rules == 1 && data_fwd;
}

static struct dcl *
d_of(struct cl *c)
{
	return &D->d[c - cl];
}

static void
ip4s(char *o, size_t ol, uint32_t be)
{
	const uint8_t *b = (const uint8_t *)&be;
	snprintf(o, ol, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/* TLV ids of tl as "01,10,11" */
static void
tlv_ids(char *o, size_t ol, const uint8_t *tl, uint32_t tn)
{
	size_t k = 0;
	o[0] = 0;
	for (uint32_t i = 0; i + 3 <= tn && k + 4 < ol;) {
		k += (size_t)snprintf(o + k, ol - k, "%s%02x", i ? "," : "", tl[i]);
		i += 3 + (uint32_t)(tl[i + 1] | (tl[i + 2] << 8));
	}
}

static int
tlv_u32(const uint8_t *tl, uint32_t tn, unsigned id, uint32_t *v)
{
	uint32_t l = 0;
	const uint8_t *p = tlv_find(tl, tn, id, &l);
	if (!p || l < 4) {
		return -1;
	}
	*v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
	return 0;
}

/* live packet data handles on mux (all data clients) */
static int
d_mux_handles(unsigned mux)
{
	int n = 0;
	for (int i = 0; i < NCL; i++) {
		if (cl[i].used && cl[i].dat && D->d[i].mux == mux) {
			n += D->d[i].nh;
		}
	}
	return n;
}

/* a request of the router's own from port to the modem's svc server; 0 sent */
static int
d_own_send(int kind, int ci, uint32_t port, unsigned svc, unsigned msg, const uint8_t *tl, uint32_t tn, uint32_t hd, uint8_t idx)
{
	int si = srv_find(svc), k = -1;
	uint8_t b[7 + 64];
	if (si < 0 || tn > 64) {
		return -1;
	}
	for (int i = 0; i < D_NOWN && k < 0; i++) {
		if (!D->own[i].used) {
			k = i;
		}
	}
	if (k < 0) {
		return -1;
	}
	d_txn = d_txn >= 0xfeff ? 0xfe00 : (uint16_t)(d_txn + 1);
	b[0] = 0x00;
	b[1] = (uint8_t)d_txn;
	b[2] = (uint8_t)(d_txn >> 8);
	b[3] = (uint8_t)msg;
	b[4] = (uint8_t)(msg >> 8);
	b[5] = (uint8_t)tn;
	b[6] = 0;
	if (tn) {
		memcpy(b + 7, tl, tn);
	}
	if (sbb_qrtr_send(SBB_QRTR_DATA, port, srv[si].node, srv[si].port, b, 7 + tn)) {
		return -1;
	}
	struct down *o = &D->own[k];
	o->used = 1;
	o->kind = kind;
	o->ci = ci;
	o->port = port;
	o->hd = hd;
	o->txn = d_txn;
	o->msg = (uint16_t)msg;
	o->idx = idx;
	o->t = sbb_ms();
	return 0;
}

/* ---- calls: pdp_ip links + the kernel IPA ---- */

static void
d_call_down(unsigned mux, const char *why)
{
	if (!D->mx[mux].up || d_mux_handles(mux)) {
		return;
	}
	int u = sweet_pdp_mux_unit(mux);
	D->mx[mux].up = 0;
	D->mx[mux].gw = 0;
	dst.downs++;
	sweet_ipa_call(mux, 0, 0);
	if (u >= 0) {
		(void)sweet_pdp_link((uint32_t)u, 0);
	}
	LOG("data: call down: mux %u, no live handle (%s): sweet_ipa_call(%u, 0), pdp_ip%d link down\n", mux, why, mux, u);
}

static void
d_call_up(struct cl *c, uint32_t hd)
{
	struct dcl *x = d_of(c);
	unsigned mux = x->mux;
	if (!mux) {
		LOG("data: call up on %s/%u WDS cid %u handle 0x%x, but the client never bound a mux: no pdp_ip mapping\n",
		    pipe_name[c->pipe], c->sess, c->cid, hd);
		return;
	}
	int u = sweet_pdp_mux_unit(mux);
	D->mx[mux].up = 1;
	dst.ups++;
	if (u >= 0) {
		(void)sweet_pdp_link((uint32_t)u, 1);
	}
	sweet_ipa_call(mux, 1, D->mx[mux].gw);
	LOG("data: call up: %s/%u WDS cid %u handle 0x%x mux %u -> pdp_ip%d link up, sweet_ipa_call(%u, 1)\n", pipe_name[c->pipe],
	    c->sess, c->cid, hd, mux, u, mux);
	if (x->gw_state != 3 || !D->mx[mux].gw) {
		x->gw_state = 1;
		x->at = sbb_ms() + D_GW_WAIT_MS;
	}
}

static void
d_gw_known(struct cl *c, uint32_t gw_be, const char *src)
{
	struct dcl *x = d_of(c);
	char a[20];
	x->gw_state = 3;
	if (!x->mux || !gw_be) {
		return;
	}
	ip4s(a, sizeof(a), gw_be);
	if (D->mx[x->mux].gw != gw_be) {
		LOG("data: mux %u IPv4 gateway %s (%s)\n", x->mux, a, src);
	}
	D->mx[x->mux].gw = gw_be;
	if (D->mx[x->mux].up) {
		sweet_ipa_call(x->mux, 1, gw_be);
	}
}

static void
d_handle_add(struct cl *c, uint32_t hd)
{
	struct dcl *x = d_of(c);
	for (int i = 0; i < x->nh; i++) {
		if (x->h[i] == hd) {
			return;
		}
	}
	if (x->nh < D_NH) {
		x->h[x->nh++] = hd;
	} else {
		LOG("data: %s/%u WDS cid %u: more than %d live handles, 0x%x not tracked\n", pipe_name[c->pipe], c->sess, c->cid, D_NH, hd);
	}
}

static int
d_handle_del(struct dcl *x, uint32_t hd)
{
	for (int i = 0; i < x->nh; i++) {
		if (x->h[i] == hd) {
			x->h[i] = x->h[--x->nh];
			return 1;
		}
	}
	return 0;
}

/* a mux CommCenter bound: give it a pdp_ip unit when kern.sweet_pdp_mux has none */
static void
d_mux_map(unsigned mux)
{
	if (!mux || sweet_pdp_mux_unit(mux) >= 0) {
		return;
	}
	for (uint32_t u = 0; u < SWEET_PDP_NUNITS; u++) {
		int busy = 0;
		for (int i = 0; i < NCL; i++) {
			busy |= cl[i].used && cl[i].dat && D->d[i].mux && sweet_pdp_mux_unit(D->d[i].mux) == (int)u;
		}
		if (!busy) {
			dst.muxmap++;
			LOG("data: CommCenter bound mux %u, not in kern.sweet_pdp_mux: pdp_ip%u carries it from now on\n", mux, u);
			(void)sweet_pdp_set_mux(u, mux);
			return;
		}
	}
	LOG("data: CommCenter bound mux %u, not in kern.sweet_pdp_mux and every pdp_ip unit is in use: its packets are dropped\n", mux);
}

/* ---- client lifetime (cl_free / cl_destroy / server gone) ---- */

static void
d_cl_gone(struct cl *c)
{
	if (!D || !c->dat) {
		return;
	}
	struct dcl *x = d_of(c);
	unsigned mux = x->mux;
	int had = x->nh;
	for (int i = 0; i < D_NOWN; i++) {
		if (D->own[i].used && D->own[i].ci == (int)(c - cl)) {
			D->own[i].used = 0;
		}
	}
	bzero(x, sizeof(*x));
	if (had && mux) {
		d_call_down(mux, "client gone");
	}
}

/* cl_free of a data client: 1 = handled here (WDS/QOS: never pooled) */
static int
d_release(struct cl *c, const char *why)
{
	if (!D || !c->dat) {
		return 0;
	}
	if (c->svc != 0x01 && c->svc != 0x04) {     /* WDA/DSD/DFS: pooled as before, as a plain client again */
		bzero(d_of(c), sizeof(struct dcl));
		c->dat = 0;
		return 0;
	}
	struct dcl *x = d_of(c);
	int sent = 0;
	dst.closes++;
	for (int i = 0; i < x->nh; i++) {
		uint8_t t[7] = { 0x01, 4, 0, (uint8_t)x->h[i], (uint8_t)(x->h[i] >> 8), (uint8_t)(x->h[i] >> 16), (uint8_t)(x->h[i] >> 24) };
		if (d_own_send(DO_STOP, (int)(c - cl), c->port, 0x01, 0x0021, t, sizeof(t), x->h[i], 0) == 0) {
			sent++;
			dst.stops_sent++;
		}
	}
	LOG("data: %s/%u %s(0x%02x) cid %u port %#x released (%s): %d live handle(s), %d StopNetworkInterface sent, not pooled\n",
	    pipe_name[c->pipe], c->sess, svc_name(c->svc), c->svc, c->cid, c->port, why, x->nh, sent);
	if (!sent) {
		cl_destroy(c, why);
		return 1;
	}
	c->used = 3;                            /* closing: matches no CommCenter frame, keeps its port until the answers */
	x->closing = 1;
	x->at = sbb_ms() + D_CLOSE_MS;
	return 1;
}

/* ---- profile backup (read-only) ---- */

static void
bak_add(const char *fmt, ...) __printflike(1, 2);
static void
bak_add(const char *fmt, ...)
{
	va_list ap;
	lck_mtx_lock(&bak_lock);
	if (D->bakn < D_BAK_MAX - 1) {
		va_start(ap, fmt);
		int k = vsnprintf(D->bak + D->bakn, D_BAK_MAX - D->bakn, fmt, ap);
		va_end(ap);
		if (k > 0) {
			D->bakn = D->bakn + (uint32_t)k < D_BAK_MAX - 1 ? D->bakn + (uint32_t)k : D_BAK_MAX - 1;
		}
	}
	lck_mtx_unlock(&bak_lock);
}

static void
bak_hex(const uint8_t *p, uint32_t n)
{
	lck_mtx_lock(&bak_lock);
	for (uint32_t i = 0; i < n && D->bakn + 3 < D_BAK_MAX; i++) {
		static const char hx[] = "0123456789abcdef";
		D->bak[D->bakn++] = hx[p[i] >> 4];
		D->bak[D->bakn++] = hx[p[i] & 15];
	}
	D->bak[D->bakn] = 0;
	lck_mtx_unlock(&bak_lock);
}

static void
pb_start(const char *why)
{
	uint8_t t[4] = { 0x10, 1, 0, 0 };       /* profile type 0 = 3GPP */
	if (pb_state != 0 || pb_tries >= 3 || srv_find(0x01) < 0) {
		return;
	}
	pb_tries++;
	lck_mtx_lock(&bak_lock);
	D->bakn = 0;
	D->bak[0] = 0;
	lck_mtx_unlock(&bak_lock);
	D->nprof = D->pnext = 0;
	if (d_own_send(DO_PLIST, -1, D_PROF_PORT, 0x01, 0x002a, t, sizeof(t), 0, 0)) {
		LOG("profile backup: cannot send Get Profile List (try %d)\n", pb_tries);
		return;
	}
	pb_state = 1;
	LOG("profile backup: reading the modem's 3GPP profile list (read-only, router port %#x, %s, try %d)\n", D_PROF_PORT, why, pb_tries);
}

static void
pb_fail(const char *why)
{
	LOG("profile backup FAILED (%s, try %d of 3): profile writes stay held (then QMI error 0x%x)\n", why, pb_tries,
	    QMI_ERR_DEVICE_NOT_READY);
	pb_state = 0;
	uint32_t pkt[5] = { SBB_QRTR_DEL_CLIENT, SBB_NODE_APPS, D_PROF_PORT, 0, 0 };
	sbb_qrtr_send(SBB_QRTR_DEL_CLIENT, D_PROF_PORT, SBB_NODE_BCAST, SBB_PORT_CTRL, pkt, sizeof(pkt));
}

static void
pb_next(void)
{
	if (D->pnext >= D->nprof) {
		bak_add("END %d\n", D->nprof);
		pb_state = 3;
		pb_n = D->nprof;
		uint32_t pkt[5] = { SBB_QRTR_DEL_CLIENT, SBB_NODE_APPS, D_PROF_PORT, 0, 0 };
		sbb_qrtr_send(SBB_QRTR_DEL_CLIENT, D_PROF_PORT, SBB_NODE_BCAST, SBB_PORT_CTRL, pkt, sizeof(pkt));
		printf("SWEET QMUX: profile backup ready (%d profiles): read kern.sweet_wds_profiles_bak, save it, then write 1 to "
		    "kern.sweet_wds_profiles_ack\n", D->nprof);
		return;
	}
	uint8_t t[5] = { 0x01, 2, 0, 0, D->pidx[D->pnext] };
	if (d_own_send(DO_PSET, -1, D_PROF_PORT, 0x01, 0x002b, t, sizeof(t), 0, D->pidx[D->pnext])) {
		pb_fail("cannot send Get Profile Settings");
		return;
	}
	pb_state = 2;
}

static void
pb_answer(struct down *o, const uint8_t *d, uint32_t ml, unsigned res, unsigned er)
{
	const uint8_t *tl = d + 7;
	if (o->kind == DO_PLIST) {
		uint32_t l = 0;
		const uint8_t *v = res == 0 ? tlv_find(tl, ml, 0x01, &l) : NULL;
		if (!v || l < 1) {
			char why[48];
			snprintf(why, sizeof(why), "Get Profile List error 0x%x", res == 0 ? 0xffff : er);
			pb_fail(why);
			return;
		}
		bak_add("SWEET-WDS-PROFILES v1 (redfin 1080; modem 3GPP profiles; TLVs 0x1b/0x1c/0x9b/0x9c removed)\nlist ");
		bak_hex(v, l);
		bak_add("\n");
		unsigned n = v[0];
		uint32_t k = 1;
		for (unsigned i = 0; i < n && k + 3 <= l; i++) {
			if (D->nprof < D_NPROF) {
				D->pidx[D->nprof++] = v[k + 1];
			}
			k += 3 + v[k + 2];
		}
		LOG("profile backup: %u profile(s) listed, reading %d\n", n, D->nprof);
		pb_next();
		return;
	}
	/* DO_PSET: every TLV but the result and the credentials */
	uint8_t *t = B->tmp_buf;
	uint32_t o2 = 0, dropped = 0;
	for (uint32_t k = 0; k + 3 <= ml;) {
		uint32_t l = 3u + (uint32_t)(tl[k + 1] | (tl[k + 2] << 8));
		unsigned id = tl[k];
		if (id == 0x1b || id == 0x1c || id == 0x9b || id == 0x9c) {
			dropped++;
		} else if (id != 0x02 && o2 + l <= sizeof(B->tmp_buf)) {
			memcpy(t + o2, tl + k, l);
			o2 += l;
		}
		k += l;
	}
	bak_add("profile 0 %u %s 0x%x ", o->idx, res == 0 ? "ok" : "err", res == 0 ? 0 : er);
	bak_hex(t, o2);
	bak_add("\n");
	LOG("profile backup: profile %u: %s, %u TLV bytes kept, %u credential TLV(s) removed\n", o->idx, res == 0 ? "read" :
	    "error", o2, dropped);
	D->pnext++;
	pb_next();
}

/* ---- the router's own answers (first look at every modem response) ---- */

static int
d_own_answer(const struct sbb_qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	if (!D || n < 7 || d[0] != 0x02) {
		return 0;
	}
	uint16_t txn = (uint16_t)(d[1] | (d[2] << 8));
	struct down *o = NULL;
	for (int i = 0; i < D_NOWN && !o; i++) {
		if (D->own[i].used && D->own[i].port == h->dst_port && D->own[i].txn == txn) {
			o = &D->own[i];
		}
	}
	if (!o) {
		return 0;
	}
	o->used = 0;
	uint32_t ml = (uint32_t)(d[5] | (d[6] << 8));
	unsigned res = 0xffff, er = 0xffff;
	if (ml > n - 7 || !tlvs_ok(d + 7, ml)) {
		malformed++;
		LOG("data: MALFORMED answer to the router's own msg 0x%04x: dropped\n", o->msg);
		return 1;
	}
	result_of(d, n, &res, &er);
	struct cl *c = o->ci >= 0 && o->ci < NCL && cl[o->ci].used && cl[o->ci].dat ? &cl[o->ci] : NULL;
	switch (o->kind) {
	case DO_STOP:
		LOG("data: own StopNetworkInterface handle 0x%x (port %#x): %s 0x%x\n", o->hd, o->port, res == 0 ? "ok" : "error",
		    res == 0 ? 0 : er);
		if (c) {
			struct dcl *x = d_of(c);
			unsigned mux = x->mux;
			d_handle_del(x, o->hd);
			if (!x->nh) {
				d_call_down(mux, "router's StopNetworkInterface");
				if (x->closing) {
					cl_destroy(c, "released after StopNetworkInterface");
				}
			}
		}
		break;
	case DO_GW: {
		uint32_t v;
		if (c && res == 0 && tlv_u32(d + 7, ml, 0x20, &v) == 0) {
			dst.gw_own++;
			d_gw_known(c, __builtin_bswap32(v), "router's own GetRuntimeSettings");
		} else {
			if (c) {
				d_of(c)->gw_state = 3;
			}
			LOG("data: own GetRuntimeSettings: %s (no IPv4 gateway: keep-alive uses kern.sweet_pdp_gw)\n", res == 0 ?
			    "no TLV 0x20" : "error");
		}
		break;
	}
	case DO_WDA:
		wda_own_answer(d + 7, ml, res, er);     /* 1088 */
		break;
	default:
		pb_answer(o, d, ml, res, er);
		break;
	}
	return 1;
}

/* ---- CommCenter's answers / indications on data clients (in place, same lengths) ---- */

static void
wda_show_own(uint8_t *tl, uint32_t tn, int *rew)
{
	for (uint32_t k = 0; k + 3 <= tn;) {
		uint32_t l = (uint32_t)(tl[k + 1] | (tl[k + 2] << 8)), cl2 = 0;
		unsigned id = tl[k];
		const uint8_t *cv = id >= 0x10 && id <= 0x16 ? tlv_find(D->wda_req, D->wda_n, id, &cl2) : NULL;
		if (cv && cl2 == l && memcmp(tl + k + 3, cv, l)) {
			memcpy(tl + k + 3, cv, l);
			(*rew)++;
		}
		k += 3 + l;
	}
}

static void
d_answer(struct cl *c, uint8_t *sdu, uint32_t len, const uint8_t *rq, uint32_t rqn)
{
	unsigned type = sdu[0], msg = (unsigned)(sdu[3] | (sdu[4] << 8)), res = 0xffff, er = 0xffff;
	uint8_t *tl = sdu + 7;
	uint32_t tn = len - 7, v;
	struct dcl *x = d_of(c);
	if (type == 0x02) {
		result_of(sdu, len, &res, &er);
	}
	if (c->svc == 0x1a && type == 0x02 && (msg == 0x0020 || msg == 0x0021)) {
		char m[160];
		uint32_t a[7] = { 0, 0, 0, 0, 0, 0, 0 };
		static const uint8_t ids[7] = { 0x11, 0x12, 0x13, 0x15, 0x16, 0x17, 0x18 };
		for (int i = 0; i < 7; i++) {
			(void)tlv_u32(tl, tn, ids[i], &a[i]);
		}
		snprintf(m, sizeof(m), "link %u ul %u dl %u dl max %u/%#x ul max %u/%#x", a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
		int rew = 0;
		if (res == 0 && D->wda_n) {
			wda_show_own(tl, tn, &rew);
		}
		dst.wda_ans++;
		LOG("data: WDA %s answer: %s 0x%x, modem: %s; %d TLV(s) rewritten to CommCenter's own values\n", msg == 0x0020 ?
		    "SetDataFormat" : "GetDataFormat", res == 0 ? "success" : "FAILURE", res == 0 ? 0 : er, m, rew);
		return;
	}
	if (c->svc == 0x04 && type == 0x02 && msg == 0x002b) {
		int mx = tlv_byte(rq, rqn, 0x11);
		LOG("data: QOS BindDataPort %s/%u cid %u mux %d: %s 0x%x\n", pipe_name[c->pipe], c->sess, c->cid, mx, res == 0 ?
		    "ok" : "error", res == 0 ? 0 : er);
		if (res == 0 && mx > 0) {
			x->mux = (uint8_t)mx;
		}
		return;
	}
	if (c->svc != 0x01) {
		return;
	}
	if (type == 0x02 && msg == 0x00a2) {
		int mx = tlv_byte(rq, rqn, 0x11);
		LOG("data: WDS BindMuxDataPort %s/%u cid %u mux %d: %s 0x%x\n", pipe_name[c->pipe], c->sess, c->cid, mx, res == 0 ?
		    "ok" : "error", res == 0 ? 0 : er);
		if (res == 0 && mx > 0) {
			x->mux = (uint8_t)mx;
			d_mux_map((unsigned)mx);
		}
	} else if (type == 0x02 && msg == 0x0020) {
		if (res == 0 && tlv_u32(tl, tn, 0x01, &v) == 0) {
			dst.starts_ok++;
			d_handle_add(c, v);
			d_call_up(c, v);
		} else {
			uint32_t l1 = 0, l2 = 0;
			const uint8_t *ce = tlv_find(tl, tn, 0x10, &l1), *vb = tlv_find(tl, tn, 0x11, &l2);
			ce = ce && l1 >= 2 ? ce : NULL;
			vb = vb && l2 >= 4 ? vb : NULL;
			dst.starts_err++;
			LOG("data: StartNetworkInterface %s/%u cid %u mux %u: error 0x%x, end reason %d, verbose %d:%d\n",
			    pipe_name[c->pipe], c->sess, c->cid, x->mux, er, ce ? (int)(ce[0] | (ce[1] << 8)) : -1,
			    vb ? (int)(vb[0] | (vb[1] << 8)) : -1, vb ? (int)(int16_t)(vb[2] | (vb[3] << 8)) : 0);
		}
	} else if (type == 0x02 && msg == 0x0021) {
		uint32_t hd;
		if (tlv_u32(rq, rqn, 0x01, &hd) == 0) {
			int had = d_handle_del(x, hd);
			LOG("data: StopNetworkInterface %s/%u cid %u handle 0x%x mux %u: %s 0x%x%s\n", pipe_name[c->pipe], c->sess, c->cid,
			    hd, x->mux, res == 0 ? "ok" : "error", res == 0 ? 0 : er, had ? "" : " (handle not tracked)");
			d_call_down(x->mux, "StopNetworkInterface");
		}
	} else if (type == 0x02 && msg == 0x002d && res == 0) {
		if (tlv_u32(tl, tn, 0x20, &v) == 0) {
			dst.gw_cc++;
			d_gw_known(c, __builtin_bswap32(v), "CommCenter's GetRuntimeSettings");
		}
	} else if (type == 0x04 && msg == 0x0022) {
		int st = tlv_byte(tl, tn, 0x01);
		LOG("data: PktSrvcStatus %s/%u cid %u mux %u: %s\n", pipe_name[c->pipe], c->sess, c->cid, x->mux, st == 1 ?
		    "disconnected" : st == 2 ? "connected" : st == 3 ? "suspended" : st == 4 ? "authenticating" : "unknown");
		if (st == 1 && x->nh) {
			x->nh = 0;
			d_call_down(x->mux, "PktSrvcStatus disconnected");
		}
	}
}

/* ---- CommCenter's data requests ---- */

static void
d_fwd(unsigned pipe, unsigned svc, unsigned cid, const uint8_t *s, uint32_t sn)
{
	svc_forward(pipe, svc, cid, s, sn);
	struct cl *c = cl_get(pipe, cur_sess, svc, cid, 0);
	if (c && !c->dat) {
		c->dat = 1;
		bzero(d_of(c), sizeof(struct dcl));
	}
}

/* copy s's header to tbuf; returns the TLV write offset (7) */
static uint32_t
tb_begin(const uint8_t *s)
{
	memcpy(D->tbuf, s, 7);
	return 7;
}

static uint32_t
tb_end(uint32_t o)
{
	D->tbuf[5] = (uint8_t)(o - 7);
	D->tbuf[6] = (uint8_t)((o - 7) >> 8);
	return o;
}

static void
tb_u32(uint32_t *o, unsigned id, uint32_t v)
{
	put_tlv(D->tbuf, o, id, &v, 4);
}

/* WDA SetDataFormat -> what sweet_ipa.c implements */
static uint32_t
wda_translate(const uint8_t *s, const uint8_t *tl, uint32_t tn, char *log, size_t ll)
{
	static const uint8_t need[6] = { 0x11, 0x12, 0x13, 0x15, 0x16, 0x17 };
	uint32_t o = tb_begin(s), have = 0;
	size_t lo = 0;
	log[0] = 0;
	for (uint32_t k = 0; k + 3 <= tn;) {
		uint32_t l = (uint32_t)(tl[k + 1] | (tl[k + 2] << 8)), nv = 0;
		unsigned id = tl[k];
		if (k + 3 + l > tn) {
			break;
		}
		const uint8_t *v = tl + k + 3;
		int known = 1;
		uint32_t cv = l == 4 ? (uint32_t)(v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24) : (l == 1 ? v[0] : 0xffffffffu);
		switch (id) {
		case 0x11: nv = 2; break;
		case 0x12: case 0x13: nv = 9; break;
		case 0x15: nv = 16; break;
		case 0x16: nv = 0x3800; break;
		case 0x10: case 0x19: nv = 0; break;
		case 0x17: nv = 1; break;
		default: known = 0; break;
		}
		if (o + 3 + l + 8 > sizeof(D->tbuf)) {
			break;
		}
		if (lo + 24 < ll) {
			lo += (size_t)snprintf(log + lo, ll - lo, " %02x", id);
			if (l == 1 || l == 4) {
				lo += (size_t)snprintf(log + lo, ll - lo, "=%u", cv);
			} else if (id == 0x17 && l == 8) {
				lo += (size_t)snprintf(log + lo, ll - lo, "={%u,%u}", v[0] | v[1] << 8, v[4] | v[5] << 8);
			} else {
				lo += (size_t)snprintf(log + lo, ll - lo, "(%u B)", l);
			}
		}
		if (known && id == 0x17 && l == 8) {
			uint32_t ep[2] = { 4, 1 };
			put_tlv(D->tbuf, &o, id, ep, 8);
		} else if (known && (id == 0x10 || id == 0x19) && l == 1) {
			uint8_t z = 0;
			put_tlv(D->tbuf, &o, id, &z, 1);
		} else if (known && id != 0x10 && id != 0x19 && id != 0x17 && l == 4) {
			tb_u32(&o, id, nv);
		} else if (!known && (data_opts & DOPT_WDA_STRIP) && (id < 0x10 || id > 0x19)) {
			if (lo + 4 < ll) {
				lo += (size_t)snprintf(log + lo, ll - lo, "[dropped]");
			}
		} else {
			memcpy(D->tbuf + o, tl + k, 3 + l);     /* unknown id or unexpected length: untouched */
			o += 3 + l;
		}
		for (int i = 0; i < 6; i++) {
			have |= need[i] == id ? 1u << i : 0;
		}
		k += 3 + l;
	}
	for (int i = 0; i < 6; i++) {
		if (!(have & (1u << i))) {
			if (need[i] == 0x17) {
				uint32_t ep[2] = { 4, 1 };
				put_tlv(D->tbuf, &o, 0x17, ep, 8);
			} else {
				tb_u32(&o, need[i], need[i] == 0x11 ? 2 : need[i] == 0x15 ? 16 : need[i] == 0x16 ? 0x3800 : 9);
			}
		}
	}
	return tb_end(o);
}

/* WDS BindMuxDataPort / QOS BindDataPort: endpoint TLV 0x10 = {4, 1}, the rest untouched */
static uint32_t
bind_translate(const uint8_t *s, unsigned svc, const uint8_t *tl, uint32_t tn)
{
	uint32_t o = tb_begin(s), ep[2] = { 4, 1 };
	int had_ep = 0, had_ct = 0;
	for (uint32_t k = 0; k + 3 <= tn;) {
		uint32_t l = (uint32_t)(tl[k + 1] | (tl[k + 2] << 8));
		if (k + 3 + l > tn || o + 3 + l + 22 > sizeof(D->tbuf)) {
			break;
		}
		if (tl[k] == 0x10) {
			had_ep = 1;
			put_tlv(D->tbuf, &o, 0x10, ep, 8);
		} else {
			had_ct |= svc == 0x01 && tl[k] == 0x13;
			memcpy(D->tbuf + o, tl + k, 3 + l);
			o += 3 + l;
		}
		k += 3 + l;
	}
	if (!had_ep) {
		put_tlv(D->tbuf, &o, 0x10, ep, 8);
	}
	if (svc == 0x01 && !had_ct && (data_opts & DOPT_BIND_TETH)) {
		tb_u32(&o, 0x13, 1);
	}
	return tb_end(o);
}

static int
d_is_gated(unsigned msg)
{
	for (int i = 0; i < ngate; i++) {
		if (gate_ids[i] == msg) {
			return 1;
		}
	}
	return 0;
}

static void
d_prof_log(unsigned pipe, unsigned cid, unsigned msg, const uint8_t *tl, uint32_t tn, const char *what)
{
	char ids[96];
	uint32_t l = 0;
	const uint8_t *p = tlv_find(tl, tn, 0x01, &l);
	tlv_ids(ids, sizeof(ids), tl, tn);
	dst.prof_writes++;
	LOG("data: WDS profile write 0x%04x from %s/%u cid %u profile %s%d (type %d), TLVs %s: %s\n", msg, pipe_name[pipe], cur_sess,
	    cid, p && l >= 2 ? "" : "-", p && l >= 2 ? p[1] : 0, p && l >= 1 ? p[0] : -1, ids, what);
}

static int
d_hold(int kind, unsigned pipe, unsigned svc, unsigned cid, unsigned msg, const uint8_t *s, uint32_t sn)
{
	for (int i = 0; i < D_NHOLD; i++) {
		struct dhold *h = &D->hold[i];
		if (!h->used && sn <= D_HOLD_MAXF) {
			h->used = 1;
			h->kind = kind;
			h->pipe = pipe;
			h->sess = cur_sess;
			h->svc = (uint8_t)svc;
			h->cid = (uint8_t)cid;
			h->msg = (uint16_t)msg;
			h->t = sbb_ms();
			h->n = sn;
			memcpy(h->f, s, sn);
			return 0;
		}
	}
	return -1;
}

/* StartNetworkInterface to the modem (the IPA is ready): per-mux rate limit */
static void
d_start(unsigned pipe, unsigned cid, const uint8_t *s, uint32_t sn)
{
	struct cl *c = cl_get(pipe, cur_sess, 0x01, cid, 0);
	unsigned mux = c && c->dat ? d_of(c)->mux : 0;
	uint64_t now = sbb_ms(), *st = D->mx[mux].st;
	int n = 0, old = 0;
	for (int i = 0; i < D_RATE_N; i++) {
		n += st[i] && now - st[i] < D_RATE_MS;
		if (st[i] < st[old]) {
			old = i;
		}
	}
	if (n >= D_RATE_N) {
		dst.rate++;
		LOG("data: StartNetworkInterface %s/%u cid %u mux %u refused: rate limit (%d per minute per mux): error 0x%x\n",
		    pipe_name[pipe], cur_sess, cid, mux, D_RATE_N, QMI_ERR_CALL_FAILED);
		svc_local(pipe, 0x01, cid, s, 1, QMI_ERR_CALL_FAILED);
		return;
	}
	st[old] = now;
	LOG("data: StartNetworkInterface %s/%u cid %u mux %u -> modem\n", pipe_name[pipe], cur_sess, cid, mux);
	d_fwd(pipe, 0x01, cid, s, sn);
}

/* 1088: StartNetworkInterface may go: the IPA is up and the router's WDA SetDataFormat (if enabled) was answered or gave up */
static bool
d_start_ok(void)
{
	return sweet_ipa_ready() && (!(data_opts & DOPT_WDA_OWN) || wda_st == 2 || (wda_st == 3 && wda_tries >= D_WDA_TRIES));
}

static void
wda_own_poll(uint64_t now)
{
	int si = srv_find(0x1a);
	if (wda_st && (si < 0 || srv[si].node != wda_node || srv[si].port != wda_port)) {
		LOG("data: WDA server %s: the router's WDA SetDataFormat is due again (1088)\n", si < 0 ? "gone" : "moved (modem restart)");
		wda_st = 0;
		wda_tries = 0;
		wda_next = 0;
	}
	if (!(data_opts & DOPT_WDA_OWN) || !data_active() || si < 0 || wda_st == 1 || wda_st == 2 || now < wda_next ||
	    wda_tries >= D_WDA_TRIES || !sweet_ipa_ready()) {
		return;
	}
	uint8_t t[64];
	uint32_t o = 0, v, ep[2] = { 4, 1 };
	static const uint8_t ids[5] = { 0x11, 0x12, 0x13, 0x15, 0x16 };
	static const uint32_t val[5] = { 2, 9, 9, 16, 0x3800 };
	for (int i = 0; i < 5; i++) {
		v = val[i];
		put_tlv(t, &o, ids[i], &v, 4);
	}
	put_tlv(t, &o, 0x17, ep, 8);
	if (d_own_send(DO_WDA, -1, D_WDA_PORT, 0x1a, 0x0020, t, o, 0, 0)) {
		wda_next = now + D_WDA_RETRY_MS;
		LOG("data: the router's WDA SetDataFormat could not be sent (no slot / send failed): retry in %u s\n", D_WDA_RETRY_MS / 1000);
		return;
	}
	wda_tries++;
	wda_st = 1;
	wda_node = srv[si].node;
	wda_port = srv[si].port;
	LOG("data: the router's own WDA SetDataFormat -> modem %u:%#x from port %#x (1088, kern.sweet_data_opts & 0x4, try %d of %d): "
	    "link 2 raw-ip, ul/dl 9 QMAPv5, dl max 16/0x3800, endpoint {4,1}\n", wda_node, wda_port, D_WDA_PORT, wda_tries, D_WDA_TRIES);
}

static void
wda_own_answer(const uint8_t *tl, uint32_t tn, unsigned res, unsigned er)
{
	char b[160];
	size_t k = 0;
	static const uint8_t ids[6] = { 0x11, 0x12, 0x13, 0x15, 0x16, 0x17 };
	b[0] = 0;
	for (int i = 0; i < 6; i++) {
		uint32_t l = 0;
		const uint8_t *p = tlv_find(tl, tn, ids[i], &l);
		if (!p || k + 24 >= sizeof(b)) {
			continue;
		}
		if (l == 4) {
			k += (size_t)snprintf(b + k, sizeof(b) - k, " %02x=%u", ids[i], (unsigned)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24));
		} else if (l == 8) {
			k += (size_t)snprintf(b + k, sizeof(b) - k, " %02x={%u,%u}", ids[i], (unsigned)(p[0] | p[1] << 8), (unsigned)(p[4] | p[5] << 8));
		} else {
			k += (size_t)snprintf(b + k, sizeof(b) - k, " %02x(%u B)", ids[i], l);
		}
	}
	wda_res = res;
	wda_err = er;
	wda_st = res == 0 ? 2 : 3;
	wda_next = sbb_ms() + D_WDA_RETRY_MS;
	LOG("data: the router's WDA SetDataFormat answer: %s 0x%x (try %d of %d); modem now:%s%s\n", res == 0 ? "success" : "error",
	    res == 0 ? 0 : er, wda_tries, D_WDA_TRIES, b[0] ? b : " (no format TLVs)", res == 0 ? "" : wda_tries >= D_WDA_TRIES ?
	    " - giving up: StartNetworkInterface no longer waits for it" : " - retry in 10 s");
}

static void
d_request(unsigned pipe, unsigned svc, unsigned cid, const uint8_t *s, uint32_t sn, uint32_t rl)
{
	unsigned msg = (unsigned)(s[3] | (s[4] << 8));
	const uint8_t *tl = s + 7;
	uint32_t len = 7 + rl, n;
	char lg[200];
	if (svc == 0x01 && pb_state == 0) {
		pb_start("CommCenter's first WDS request");
	}
	if (svc == 0x01 && msg == 0x0020) {
		if (!d_start_ok()) {           /* 1088: IPA ready and the router's WDA SetDataFormat answered */
			if (d_hold(DH_START, pipe, svc, cid, msg, s, len)) {
				LOG("data: StartNetworkInterface %s/%u cid %u: IPA not ready and the hold queue is full: error 0x%x\n",
				    pipe_name[pipe], cur_sess, cid, QMI_ERR_CALL_FAILED);
				svc_local(pipe, svc, cid, s, 1, QMI_ERR_CALL_FAILED);
				return;
			}
			dst.held_start++;
			LOG("data: StartNetworkInterface %s/%u cid %u held: %s (%u s max)\n", pipe_name[pipe], cur_sess, cid,
			    !sweet_ipa_ready() ? "IPA data plane not ready (kern.sweet_ipa_state != 3)" : "the router's WDA SetDataFormat "
			    "not answered yet (1088)", D_HELD_START_MS / 1000);
			return;
		}
		d_start(pipe, cid, s, len);
		return;
	}
	if (svc == 0x01 && d_is_gated(msg)) {
		if (pb_state != 4) {
			if (d_hold(DH_PROF, pipe, svc, cid, msg, s, len)) {
				svc_local(pipe, svc, cid, s, 1, QMI_ERR_DEVICE_NOT_READY);
				LOG("data: WDS profile write 0x%04x: hold queue full: error 0x%x\n", msg, QMI_ERR_DEVICE_NOT_READY);
				return;
			}
			dst.held_prof++;
			d_prof_log(pipe, cid, msg, tl, rl, "held until the modem's profiles are backed up and acknowledged");
			if (pb_state == 0) {
				pb_start("a profile write");
			}
			return;
		}
		d_prof_log(pipe, cid, msg, tl, rl, "forwarded (backup acknowledged)");
		d_fwd(pipe, svc, cid, s, len);
		return;
	}
	if (svc == 0x1a && msg == 0x0020) {
		D->wda_n = rl <= sizeof(D->wda_req) ? rl : 0;
		memcpy(D->wda_req, tl, D->wda_n);
		n = wda_translate(s, tl, rl, lg, sizeof(lg));
		dst.wda_tr++;
		LOG("data: WDA SetDataFormat from %s/%u cid %u: CommCenter%s -> modem: link 2 raw-ip, ul/dl 9 QMAPv5, dl max 16/0x3800, "
		    "endpoint {4,1}%s\n", pipe_name[pipe], cur_sess, cid, lg, D->wda_n ? "" : " (request too long to echo back)");
		d_fwd(pipe, svc, cid, D->tbuf, n);
		return;
	}
	if ((svc == 0x01 && msg == 0x00a2) || (svc == 0x04 && msg == 0x002b)) {
		tlv_ids(lg, sizeof(lg), tl, rl);
		n = bind_translate(s, svc, tl, rl);
		dst.bind_tr++;
		LOG("data: %s %s from %s/%u cid %u mux %d (TLVs %s): endpoint 0x10 -> {4,1}\n", svc_name(svc), svc == 0x01 ?
		    "BindMuxDataPort" : "BindDataPort", pipe_name[pipe], cur_sess, cid, tlv_byte(tl, rl, 0x11), lg);
		d_fwd(pipe, svc, cid, D->tbuf, n);
		return;
	}
	d_fwd(pipe, svc, cid, s, len);
}

/* ---- the thread's round ---- */

static void
d_hold_answer(struct dhold *h, int code, const char *why)
{
	cur_pipe = h->pipe;
	cur_sess = h->sess;
	if (cid_live(h->pipe, h->sess, h->svc, h->cid)) {
		svc_local(h->pipe, h->svc, h->cid, h->f, 1, code);
	}
	LOG("data: held %s 0x%04x of %s/%u cid %u answered error 0x%x (%s)\n", svc_name(h->svc), h->msg, pipe_name[h->pipe], h->sess,
	    h->cid, code, why);
	h->used = 0;
}

static void
data_poll(uint64_t now)
{
	if (!D) {
		return;
	}
	int act = data_active();
	if (act != data_was) {
		LOG("data: %s (sweetbb 0x%x, kern.sweet_qmi_rules %d, kern.sweet_data_fwd %d)\n", act ? "ON: WDS/WDA/QOS/DSD/DFS "
		    "forwarded for CommCenter (1080)" : "off: data services answered err again", sweet_bb, qmi_rules, data_fwd);
		if (!act) {
			for (int i = 0; i < NCL; i++) {
				if (cl[i].used == 1 && cl[i].dat) {
					cl_free(&cl[i], "data mode off");
				}
			}
			for (int i = 0; i < D_NHOLD; i++) {
				if (D->hold[i].used) {
					d_hold_answer(&D->hold[i], err_code, "data mode off");
				}
			}
		}
		data_was = act;
	}
	if (pb_ack) {
		pb_ack = 0;
		if (pb_state == 3) {
			pb_state = 4;
			lck_mtx_lock(&bak_lock);
			D->bakn = 0;
			D->bak[0] = 0;
			lck_mtx_unlock(&bak_lock);
			printf("SWEET QMUX: profile backup acknowledged by userland (%d profiles saved): WDS profile writes pass, each logged\n",
			    pb_n);
		}
	}
	wda_own_poll(now);              /* 1088 */
	for (int i = 0; i < D_NHOLD; i++) {
		struct dhold *h = &D->hold[i];
		if (!h->used) {
			continue;
		}
		if (h->kind == DH_START && act && d_start_ok()) {
			cur_pipe = h->pipe;
			cur_sess = h->sess;
			h->used = 0;
			if (!cid_live(h->pipe, h->sess, h->svc, h->cid)) {
				LOG("data: held StartNetworkInterface of %s/%u cid %u: client gone, dropped\n", pipe_name[h->pipe], h->sess, h->cid);
				continue;
			}
			dst.rel_start++;
			LOG("data: held StartNetworkInterface of %s/%u cid %u released after %llu ms (IPA ready, WDA format %s)\n",
			    pipe_name[h->pipe], h->sess, h->cid, (unsigned long long)(now - h->t), !(data_opts & DOPT_WDA_OWN) ? "not the "
			    "router's (opts & 0x4 clear)" : wda_st == 2 ? "set" : "gave up");
			d_start(h->pipe, h->cid, h->f, h->n);
		} else if (h->kind == DH_START && now - h->t >= D_HELD_START_MS) {
			dst.to_start++;
			d_hold_answer(h, QMI_ERR_CALL_FAILED, sweet_ipa_ready() ? "the router's WDA SetDataFormat not answered in 15 s" :
			    "IPA data plane not ready in 15 s");
		} else if (h->kind == DH_PROF && act && pb_state == 4) {
			cur_pipe = h->pipe;
			cur_sess = h->sess;
			h->used = 0;
			if (!cid_live(h->pipe, h->sess, h->svc, h->cid)) {
				continue;
			}
			dst.rel_prof++;
			d_prof_log(h->pipe, h->cid, h->msg, h->f + 7, h->n - 7, "released (backup acknowledged)");
			d_fwd(h->pipe, h->svc, h->cid, h->f, h->n);
		} else if (h->kind == DH_PROF && now - h->t >= D_HELD_PROF_MS) {
			dst.to_prof++;
			d_hold_answer(h, QMI_ERR_DEVICE_NOT_READY, "no acknowledged profile backup in 60 s");
		} else if (h->kind == DH_PROF && pb_state == 0) {
			pb_start("a held profile write");
		}
	}
	for (int i = 0; i < D_NOWN; i++) {
		struct down *o = &D->own[i];
		if (!o->used || now - o->t < D_OWN_MS) {
			continue;
		}
		o->used = 0;
		LOG("data: the router's own msg 0x%04x (port %#x) got no answer in %u s\n", o->msg, o->port, D_OWN_MS / 1000);
		if (o->kind == DO_GW && o->ci >= 0 && cl[o->ci].dat) {
			D->d[o->ci].gw_state = 3;
		} else if (o->kind == DO_PLIST || o->kind == DO_PSET) {
			pb_fail("no answer");
		} else if (o->kind == DO_WDA) {
			wda_st = 3;
			wda_next = now + D_WDA_RETRY_MS;
			LOG("data: the router's WDA SetDataFormat: no answer (try %d of %d)\n", wda_tries, D_WDA_TRIES);
		}
	}
	for (int i = 0; i < NCL; i++) {
		struct dcl *x = &D->d[i];
		if (!cl[i].used || !cl[i].dat) {
			continue;
		}
		if (x->closing && now >= x->at) {
			unsigned mux = x->mux;
			LOG("data: closing client port %#x: %d StopNetworkInterface answer(s) missing after %u s: given back anyway\n",
			    cl[i].port, x->nh, D_CLOSE_MS / 1000);
			x->nh = 0;
			d_call_down(mux, "client released");
			cl_destroy(&cl[i], "released (stop answers missing)");
		} else if (cl[i].used == 1 && x->gw_state == 1 && now >= x->at) {
			if (D->mx[x->mux].gw) {
				x->gw_state = 3;
			} else if (d_own_send(DO_GW, i, cl[i].port, 0x01, 0x002d,
			    (const uint8_t[]){ 0x10, 4, 0, 0x14, 0xa3, 0, 0 }, 7, 0, 0) == 0) {
				x->gw_state = 2;
				LOG("data: no gateway from CommCenter in %u s: the router's own GetRuntimeSettings on port %#x (answer not "
				    "forwarded)\n", D_GW_WAIT_MS / 1000, cl[i].port);
			} else {
				x->gw_state = 3;
			}
		}
	}
}

static int
data_stats(char *buf, size_t len)
{
	int o;
	if (!D) {
		return snprintf(buf, len, "data module off (sweetbb & 0x10 not set or router off)");
	}
	o = snprintf(buf, len, "data %s (sweetbb 0x%x rules %d fwd %d opts %d) | fwd WDS %u WDA %u QOS %u DSD %u DFS %u | starts ok %u "
	    "err %u held %u released %u timeout %u rate-limited %u | translations WDA %u (answers %u) bind %u | gw cc %u own %u | "
	    "stops sent %u closes %u | ups %u downs %u muxmap %u | 5564 local %u | profiles: backup %s (%d) writes %u held %u "
	    "released %u timeout %u | handles per mux:", data_active() ? "ON" : "off", sweet_bb, qmi_rules, data_fwd, data_opts,
	    svc_cnt[0x01].fwd, svc_cnt[0x1a].fwd, svc_cnt[0x04].fwd, svc_cnt[0x2a].fwd, svc_cnt[0x30].fwd, dst.starts_ok,
	    dst.starts_err, dst.held_start, dst.rel_start, dst.to_start, dst.rate, dst.wda_tr, dst.wda_ans, dst.bind_tr, dst.gw_cc,
	    dst.gw_own, dst.stops_sent, dst.closes, dst.ups, dst.downs, dst.muxmap, dst.local5564, pb_state == 0 ? "none" :
	    pb_state == 1 ? "listing" : pb_state == 2 ? "reading" : pb_state == 3 ? "ready" : "acked", pb_n, dst.prof_writes,
	    dst.held_prof, dst.rel_prof, dst.to_prof);
	for (unsigned m = 1; m < 256 && o < (int)len - 40; m++) {
		int h = d_mux_handles(m);
		if (h || D->mx[m].up) {
			o += snprintf(buf + o, len - (size_t)o, " %u:%d%s", m, h, D->mx[m].up ? "(up)" : "");
		}
	}
	int held = 0;
	for (int i = 0; i < D_NHOLD; i++) {
		held += D->hold[i].used;
	}
	if (o < (int)len - 24) {
		o += snprintf(buf + o, len - (size_t)o, " | held now %d", held);
	}
	if (o < (int)len - 64) {        /* 1088 */
		o += snprintf(buf + o, len - (size_t)o, " | router WDA format %s (tries %d, result %u/0x%x)", !(data_opts & DOPT_WDA_OWN) ?
		    "off" : wda_st == 0 ? "not sent" : wda_st == 1 ? "out" : wda_st == 2 ? "set" : "error", wda_tries, wda_res, wda_err);
	}
	return o < (int)len ? o : (int)len - 1;
}

static bool
d_port_reserved(uint32_t port)
{
	return D != NULL && (port == D_PROF_PORT || port == D_WDA_PORT);
}

/* one record from SweetABP (CommCenter wrote a frame / opened, closed, started, aborted a pipe) */
static void
handle(unsigned pipe, uint32_t sess, unsigned type, const uint8_t *d, uint32_t n)
{
	if (pipe >= NPIPES) {
		return;
	}
	cur_pipe = pipe;
	cur_sess = sess;
	if (type != 0) {
		static const char *ev[] = { "data", "OPEN", "CLOSE", "START", "ABORT" };
		unsigned long long v = 0;
		if (n >= 8) {
			memcpy(&v, d, 8);
		}
		LOG("%s/%u %s (arg 0x%llx)\n", pipe_name[pipe], sess, type < 5 ? ev[type] : "?", v);
		if (type == 2) {
			sess_end(pipe, sess);
		}
		return;
	}
	nframes_in++;
	log_frame(pipe, sess, "CC->", d, n);
	if (n < 6 || d[0] != 0x01) {
		return;
	}
	unsigned svc = d[4], cid = d[5];
	const uint8_t *s = d + 6;
	uint32_t sn = n - 6;
	if (svc == 0) {
		if (sn >= 6 && s[0] == 0x00) {
			ctl_answer(pipe, s, sn);
		}
		return;
	}
	if (sn < 7 || s[0] != 0x00) {
		return;
	}
	unsigned msg = s[3] | (s[4] << 8);
	uint32_t rl = sn - 7 < (uint32_t)(s[5] | (s[6] << 8)) ? sn - 7 : (uint32_t)(s[5] | (s[6] << 8));
	int code;
	char wb[160];
	if (svc == 0x03 && msg == 0x0003) {
		nasreg_note(pipe, sess, cid, s + 7, rl);        /* 1073 */
	}
	if (svc == 0x01 && msg == 0x5564 && data_active()) {     /* 1080: Apple-only WDS message, answered here */
		note_seen(svc, msg, "ok", "data: Apple-only WDS 0x5564 answered success here (1080)");
		cnt.local_ok++;
		dst.local5564++;
		svc_local(pipe, svc, cid, s, 0, 0);
		return;
	}
	int a = decide(svc, msg, s + 7, rl, &code, wb, sizeof(wb));
	if (a == ACT_FWD && dec_rate >= 0) {
		uint64_t now = sbb_ms();
		uint64_t *last = &rate_last[dec_rate];
		if (*last && now - *last < (uint64_t)rules_o1[dec_rate].rate_ms) {
			a = ACT_ERR;
			code = QMI_ERR_DEVICE_IN_USE;
			cnt.ratelimited++;
			snprintf(wb, sizeof(wb), "rate limited (once per %d ms)", rules_o1[dec_rate].rate_ms);
		} else {
			*last = now;
		}
	}
	char an[24];
	if (a == ACT_ERR) {
		snprintf(an, sizeof(an), "err:0x%x", code);
	} else {
		snprintf(an, sizeof(an), "%s", act_name[a]);
	}
	note_seen(svc, msg, an, wb);
	struct mst *ms = mst_get(svc, msg);
	if (ms) {
		ms->count++;
		ms->act = (uint8_t)a;
		mst_dirty = 1;
	}
	if (dtmflog && svc == 0x09 && msg >= 0x0028 && msg <= 0x002a) {     /* 1086 */
		uint32_t tl = 0;
		const uint8_t *t = tlv_find(s + 7, rl, 0x01, &tl);
		char dg[24] = "";

		if (t && msg == 0x0029 && tl >= 2) {
			snprintf(dg, sizeof(dg), " digit '%c'", t[1] >= 0x20 && t[1] < 0x7f ? t[1] : '?');
		} else if (t && msg == 0x0028 && tl >= 2) {
			unsigned k = 0;
			for (; k < t[1] && k + 2 < tl && k < 16; k++) {
				dg[k] = t[k + 2] >= 0x20 && t[k + 2] < 0x7f ? (char)t[k + 2] : '?';
			}
			dg[k] = 0;
		}
		LOG("DTMF CC-> VOICE 0x%04x %s txn %u cid %u: call id %d%s%s (%u TLV bytes) -> %s\n", msg, msg == 0x0028 ? "BURST" :
		    msg == 0x0029 ? "START" : "STOP", (unsigned)(s[1] | (s[2] << 8)), cid, t && tl >= 1 ? t[0] : -1,
		    msg == 0x0028 ? " digits " : "", dg, rl, an);
	} else if (dtmflog && svc == 0xe1) {
		char ids[64];
		int io = 0;

		ids[0] = 0;
		for (uint32_t k = 0; k + 3 <= rl && io < (int)sizeof(ids) - 12;) {
			unsigned l = (unsigned)(s[7 + k + 1] | (s[7 + k + 2] << 8));
			io += snprintf(ids + io, sizeof(ids) - (size_t)io, "%s%02x/%u", k ? "," : "", s[7 + k], l);
			k += 3 + l;
		}
		LOG("AUDIO(apple) req 0x%04x cid %u, TLVs %s -> %s\n", msg, cid, ids[0] ? ids : "none", an);
	}
	switch (a) {
	case ACT_FWD:
		if (DSVC(svc) && data_active()) {
			d_request(pipe, svc, cid, s, sn, rl);   /* 1080: translations, holds, the profile gate */
		} else {
			svc_forward(pipe, svc, cid, s, sn);
		}
		break;
	case ACT_OK:
		cnt.local_ok++;
		svc_local(pipe, svc, cid, s, 0, 0);
		run_triggers(pipe, cur_sess, svc, cid, msg, s + 7, rl);
		break;
	case ACT_RESP:
		if (dec_tmpl) {
			int miss = 0, tn = tmpl_expand(dec_tmpl, s + 7, rl, 0, B->res_buf, sizeof(B->res_buf), &miss);
			if (tn < 0 || !tlvs_ok(B->res_buf, (uint32_t)tn)) {
				cnt.local_err++;
				svc_local(pipe, svc, cid, s, 1, 0x11);
				break;
			}
			cnt.local_ok++;
			svc_local_tlv(pipe, svc, cid, s, 0, 0, B->res_buf, (uint16_t)tn);
			run_triggers(pipe, cur_sess, svc, cid, msg, s + 7, rl);
			break;
		}
		cnt.local_ok++;
		svc_local_tlv(pipe, svc, cid, s, 0, 0, dec_data, dec_dlen);
		run_triggers(pipe, cur_sess, svc, cid, msg, s + 7, rl);
		break;
	case ACT_ERR:
		cnt.local_err++;
		svc_local(pipe, svc, cid, s, 1, code);
		break;
	default:
		cnt.dropped++;
		svc_cnt[svc & 0xff].denied++;
		break;
	}
}

/*
 * ---- 1074: automatic call audio (kern.sweet_voice_auto) ----
 * research/cc/P3-audio-result.md s.4 B: the modem's VOICE (0x09) All Call Status indication 0x002e (TLV 0x01) and the
 * Get All Call Info answer 0x002f (TLV 0x10) that go to CommCenter carry every call as 7 bytes: id, state, type,
 * direction, mode, multiparty, ALS. A call ORIGINATING (1), CONVERSATION (3, also an answered incoming call) or
 * ALERTING (5) -> sweet_voice.c starts voice in the configured mode (1 earpiece, 2 speaker) if nothing runs; INCOMING
 * (2, ringing), WAITING, HOLD, SETUP, CC_IN_PROGRESS keep what runs; no call left (all END 9 / DISCONNECTING 8, or none)
 * -> voice stopped, only if this trigger started it (a kern.sweet_voice set by hand is never touched). sweet_voice.c
 * sleeps, so it is called from a thread call, never from this (sweetbb) thread. Numbers (TLV 0x10+ of the indication)
 * are never read or logged. Log: "SWEET VOICE AUTO: call N state S -> start|stop|keep".
 */
#define NVCALL  8
static struct { uint8_t used, id, state; } vcall[NVCALL];
static thread_call_t vauto_tc;
static volatile int vauto_req;          /* last asked of sweet_voice.c: 0 stop, 1/2 start */

static const char *
call_state_name(unsigned s)
{
	static const char *nm[] = { "?", "ORIGINATING", "INCOMING", "CONVERSATION", "CC_IN_PROGRESS", "ALERTING", "HOLD", "WAITING",
		                    "DISCONNECTING", "END", "SETUP" };
	return s < sizeof(nm) / sizeof(nm[0]) ? nm[s] : "?";
}

static void
vauto_call(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	int t = vauto_req;
	(void)sweet_voice_auto(t, t ? "call up (kern.sweet_voice_auto)" : "no call left (kern.sweet_voice_auto)");
}

static void
vauto_ask(int t)
{
	vauto_req = t;
	if (vauto_tc) {
		thread_call_enter(vauto_tc);
	}
}

static void
vauto_parse(const uint8_t *p, uint32_t n, unsigned id, const char *src)
{
	uint32_t l = 0;
	const uint8_t *v = voice_auto ? tlv_find(p, n, id, &l) : NULL;
	if (!v || l < 1 || 1u + 7u * v[0] > l) {
		return;
	}
	unsigned nc = v[0];
	int active = 0, present = 0;
	for (unsigned i = 0; i < nc; i++) {
		unsigned st = v[2 + 7 * i];
		active |= st == 1 || st == 3 || st == 5;
		present |= st != 8 && st != 9;
	}
	int act = active ? voice_auto : !present ? 0 : -1;
	const char *an = act < 0 ? "keep" : act ? "start" : "stop";
	/* calls whose state changed */
	for (unsigned i = 0; i < nc; i++) {
		unsigned cidn = v[1 + 7 * i], st = v[2 + 7 * i];
		int k = -1, fr = -1;
		for (int j = 0; j < NVCALL; j++) {
			if (vcall[j].used && vcall[j].id == cidn) {
				k = j;
			}
			if (!vcall[j].used && fr < 0) {
				fr = j;
			}
		}
		if (k >= 0 && vcall[k].state == st) {
			continue;
		}
		if (k < 0) {
			k = fr >= 0 ? fr : 0;
		}
		vcall[k].used = 1;
		vcall[k].id = (uint8_t)cidn;
		vcall[k].state = (uint8_t)st;
		printf("SWEET VOICE AUTO: call %u state %s -> %s (%s, %u call(s))\n", cidn, call_state_name(st), an, src, nc);
	}
	/* calls no longer listed */
	for (int j = 0; j < NVCALL; j++) {
		int listed = 0;
		for (unsigned i = 0; i < nc && vcall[j].used; i++) {
			listed |= v[1 + 7 * i] == vcall[j].id;
		}
		if (vcall[j].used && !listed) {
			printf("SWEET VOICE AUTO: call %u state gone -> %s (%s, %u call(s))\n", vcall[j].id, an, src, nc);
			vcall[j].used = 0;
		}
	}
	if (act >= 0 && act != vauto_req) {
		vauto_ask(act);
	}
}

/* ---- from the link (sweetbb thread) ---- */

void
sweet_qmux_server(uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
	int fr = -1;
	if (!on) {
		return;
	}
	for (int i = 0; i < NSRV; i++) {
		if (srv[i].up && srv[i].svc == service && srv[i].inst == instance && srv[i].node == node && srv[i].port == port) {
			return;
		}
		if (!srv[i].up && fr < 0) {
			fr = i;
		}
	}
	if (fr < 0) {
		return;
	}
	srv[fr].svc = service;
	srv[fr].inst = instance;
	srv[fr].node = node;
	srv[fr].port = port;
	srv[fr].up = 1;
}

void
sweet_qmux_server_gone(uint32_t node, uint32_t port)
{
	if (!on) {
		return;
	}
	for (int i = 0; i < NSRV; i++) {
		if (srv[i].up && srv[i].node == node && (port == 0xffffffffu || srv[i].port == port)) {
			srv[i].up = 0;
			LOG("modem server svc 0x%02x (%s) gone\n", srv[i].svc, svc_name(srv[i].svc));
			for (int k = 0; k < NCL; k++) {
				if (cl[k].used && cl[k].svc == srv[i].svc && srv_find(srv[i].svc) < 0) {
					d_cl_gone(&cl[k]);      /* 1080 */
					pend_drop_port(cl[k].port);
					bzero(&cl[k], sizeof(cl[k]));
				}
			}
		}
	}
}

/* the 7-byte SDU header's result TLV of a response: res/err (0xffff = none) */
static void
result_of(const uint8_t *d, uint32_t n, unsigned *res, unsigned *err)
{
	uint32_t ml = (uint32_t)(d[5] | (d[6] << 8)), rl = 0;
	const uint8_t *r = ml <= n - 7 ? tlv_find(d + 7, ml, 0x02, &rl) : NULL;
	*res = r && rl >= 4 ? (unsigned)(r[0] | (r[1] << 8)) : 0xffff;
	*err = r && rl >= 4 ? (unsigned)(r[2] | (r[3] << 8)) : 0xffff;
}

void
sweet_qmux_qrtr(const struct sbb_qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	if (!on) {
		return;
	}
	if (d_own_answer(h, d, n)) {    /* 1080: an answer to the router's own data request */
		return;
	}
	if (n >= 7 && d[0] == 0x02) {
		uint16_t txn = (uint16_t)(d[1] | (d[2] << 8));
		for (int i = 0; i < NPEND; i++) {
			struct pend *x = &B->pend[i];
			if (!x->used || !(x->replay || x->simread) || x->port != h->dst_port || x->txn != txn) {
				continue;
			}
			x->used = 0;
			if (x->simread) {
				uint32_t sl = (uint32_t)(d[5] | (d[6] << 8));
				unsigned res, er;
				if (sl > n - 7 || !tlvs_ok(d + 7, sl)) {
					return;
				}
				result_of(d, n, &res, &er);
				if (res != 0) {
					LOG("SIM identity: EF %04x read failed (error 0x%x)\n", x->file, er);
					return;
				}
				simid_from_read(x->file, d + 7, sl, "the router's read");
				return;
			}
			uint32_t ml = (uint32_t)(d[5] | (d[6] << 8));
			if (ml > n - 7 || !tlvs_ok(d + 7, ml)) {
				malformed++;
				LOG("MALFORMED replay answer %s(0x%02x) msg 0x%04x (%u bytes): dropped\n", svc_name(x->svc), x->svc, x->msg, n);
				return;
			}
			unsigned res, er;
			result_of(d, n, &res, &er);
			if (res != 0) {
				if (!x->probe) {
					LOG("replay %s(0x%02x) GET 0x%04x: modem error 0x%x, no indication\n", svc_name(x->svc), x->svc, x->msg, er);
				}
				return;
			}
			if (x->svc == 0x03 && x->msg == 0x004d) {
				nas_srv_note(d + 7, ml);        /* 1084 */
			}
			if (x->probe) {
				return;
			}
			uint8_t *t = B->rep_buf;
			uint32_t o = 0;
			const uint8_t *p = d + 7;
			for (uint32_t k = 0; k + 3 <= ml;) {
				unsigned id = p[k], l = (unsigned)(p[k + 1] | (p[k + 2] << 8));
				if (id != 0x02) {
					unsigned nid = id;
					for (int m = 0; m < x->nmap; m++) {
						if (x->map[m][0] == id) {
							nid = x->map[m][1];
						}
					}
					t[o++] = (uint8_t)nid;
					memcpy(t + o, p + k + 1, 2 + l);
					o += 2 + l;
				}
				k += 3 + l;
			}
			o = strip_tlvs(x->svc, x->ind_msg, t, o);
			synth_inds++;
			if (x->repub) {
				/* 1073: TLV ids only (no values); 0x14 = LTE service status (srv, true srv, pref data path) */
				char ids[96];
				int io = 0;
				uint32_t ll = 0;
				const uint8_t *lte = tlv_find(t, o, 0x14, &ll);
				ids[0] = 0;
				for (uint32_t k = 0; k + 3 <= o && io < (int)sizeof(ids) - 4;) {
					io += snprintf(ids + io, sizeof(ids) - (size_t)io, "%s%02x", k ? "," : "", t[k]);
					k += 3 + (unsigned)(t[k + 1] | (t[k + 2] << 8));
				}
				int live = cl_get(x->rpipe, x->rsess, x->svc, x->rcid, 0) != NULL;
				rp_shots++;
				printf("SWEET QMUX: republish (%s) NAS GET 0x%04x -> indication 0x%04x (%u TLV bytes, modem's answer, TLVs %s%s%s) "
				    "to %s/%u cid %u%s [t=%llu ms, %s %llu ms]\n", x->repub == 2 ? "manual" : "auto", x->msg,
				    x->ind_msg, o, ids, lte && ll >= 2 ? ", LTE srv " : "", lte && ll >= 2 ? (lte[0] == 2 ? "full" : lte[0] == 1 ?
				    "limited" : lte[0] == 0 ? "none" : "other") : "", pipe_name[x->rpipe], x->rsess, x->rcid, live ? "" :
				    " (client gone: not sent)", (unsigned long long)sbb_ms(), x->repub == 1 && rp_shot_act ?
				    "after BSP activation +" : rp_act ? "since the last BSP activation" : "no BSP activation seen,",
				    (unsigned long long)(x->repub == 1 && rp_shot_act ? sbb_ms() - rp_shot_act : rp_act ? sbb_ms() - rp_act : 0));
				if (live) {
					hold_bypass = 1;
					send_ind(x->rpipe, x->rsess, x->svc, x->rcid, x->ind_msg, t, o);
					hold_bypass = 0;
					blip_arm(x->rpipe, x->rsess, x->svc, x->rcid, x->ind_msg, t, o);        /* 1085 */
				}
				return;
			}
			LOG("replay %s(0x%02x) GET 0x%04x -> indication 0x%04x (%u TLV bytes) to %s/%u cid %u\n", svc_name(x->svc), x->svc,
			    x->msg, x->ind_msg, o, pipe_name[x->rpipe], x->rsess, x->rcid);
			if (cl_get(x->rpipe, x->rsess, x->svc, x->rcid, 0)) {
				send_ind(x->rpipe, x->rsess, x->svc, x->rcid, x->ind_msg, t, o);
			}
			return;
		}
	}
	struct cl *c = NULL;
	for (int i = 0; i < NCL && !c; i++) {
		if (cl[i].used == 1 && cl[i].port == h->dst_port) {
			c = &cl[i];
		}
	}
	if (!c || n < 7) {
		late++;
		if (n >= 7 && d[0] == 0x02) {
			uint16_t txn = (uint16_t)(d[1] | (d[2] << 8));
			for (int i = 0; i < NPEND; i++) {
				if (B->pend[i].used && B->pend[i].port == h->dst_port && B->pend[i].txn == txn) {
					B->pend[i].used = 0;
				}
			}
		}
		return;
	}
	unsigned ml = d[5] | (d[6] << 8);
	if (ml > n - 7 || !tlvs_ok(d + 7, ml)) {
		char hx[3 * 48 + 8];
		malformed++;
		hexline(hx, sizeof(hx), redact(d, n), n < 40 ? n : 40);
		LOG("MALFORMED %s from the modem for %s(0x%02x) cid %u (%u bytes, msg len %u): dropped: %s\n",
		    d[0] == 0x04 ? "indication" : d[0] == 0x02 ? "response" : "frame", svc_name(c->svc), c->svc, c->cid, n, ml, hx);
		return;
	}
	unsigned res = 0xffff, rmsg = (unsigned)(d[3] | (d[4] << 8));
	uint8_t rq[96];
	uint32_t rqn = 0;
	uint64_t sent_at = 0;   /* 1086 */
	if (d[0] == 0x02) {
		uint16_t txn = (uint16_t)(d[1] | (d[2] << 8));
		unsigned er;
		for (int i = 0; i < NPEND; i++) {
			if (B->pend[i].used && B->pend[i].port == h->dst_port && B->pend[i].txn == txn) {
				B->pend[i].used = 0;
				sent_at = B->pend[i].t;
				rqn = B->pend[i].rqn;
				memcpy(rq, B->pend[i].rq, rqn);
				break;
			}
		}
		result_of(d, n, &res, &er);
		struct mst *m = mst_get(c->svc, rmsg);
		if (m) {
			if (res == 0) {
				m->ok++;
			} else {
				m->err++;
				m->last_err = (uint16_t)er;
			}
			mst_dirty = 1;
			if (!m->answered_logged) {
				m->answered_logged = 1;
				LOG("ANSWER %s(0x%02x) msg 0x%04x: %s error 0x%x (%u bytes)\n", svc_name(c->svc), c->svc, rmsg,
				    res == 0 ? "success" : res == 0xffff ? "no result TLV" : "FAILURE", res == 0 ? 0 : er, n);
			}
		}
	}
	if (d[0] == 0x02 && res == 0 && c->svc == 0x0b && rmsg == 0x0020 && rqn) {
		uint32_t fl = 0;
		const uint8_t *fv = tlv_find(rq, rqn, 0x02, &fl);
		if (fv && fl >= 3) {
			simid_from_read((unsigned)(fv[0] | (fv[1] << 8)), d + 7, ml, "CommCenter's read");
		}
	}
	if (d[0] == 0x04 && c->svc == 0x0b && rmsg == 0x0032 && !simid.refresh) {
		simid.refresh = 1;
		simid.tries = 0;
		simid.next = sbb_ms() + 2000;
	}
	if (c->svc == 0x03 && ((d[0] == 0x04 && rmsg == 0x004e) || (d[0] == 0x02 && rmsg == 0x004d && res == 0))) {
		nas_srv_note(d + 7, ml);        /* 1084 */
	} else if (c->svc == 0x03 && rmsg == 0x0024 && (d[0] == 0x04 || res == 0)) {
		nas_ss_note(d + 7, ml);
	}
	if (dtmflog && c->svc == 0x09 && rmsg >= 0x0028 && rmsg <= 0x002a) {        /* 1086 */
		uint32_t tl = 0;
		const uint8_t *t = tlv_find(d + 7, ml, d[0] == 0x04 ? 0x01 : 0x10, &tl);

		if (d[0] == 0x02) {
			unsigned r2, e2;
			result_of(d, n, &r2, &e2);
			LOG("DTMF <-modem VOICE 0x%04x answer txn %u: %s error 0x%x, call id %d, after %llu ms\n", rmsg,
			    (unsigned)(d[1] | (d[2] << 8)), r2 == 0 ? "success" : "FAILURE", r2 == 0 ? 0 : e2, t && tl >= 1 ? t[0] : -1,
			    sent_at ? (unsigned long long)(sbb_ms() - sent_at) : 0ull);
		} else {
			LOG("DTMF <-modem VOICE ind 0x%04x: call id %d, event %d, digit '%c' (%u TLV bytes)\n", rmsg, t && tl >= 1 ? t[0] : -1,
			    t && tl >= 2 ? t[1] : -1, t && tl >= 3 && t[2] >= 0x20 && t[2] < 0x7f ? t[2] : '-', ml);
		}
	}
	if (c->svc == 0x09 && voice_auto) {     /* 1074 */
		if (d[0] == 0x04 && rmsg == 0x002e) {
			vauto_parse(d + 7, ml, 0x01, "All Call Status ind");
		} else if (d[0] == 0x02 && rmsg == 0x002f && res == 0) {
			vauto_parse(d + 7, ml, 0x10, "Get All Call Info");
		}
	}
	uint32_t len = 7 + ml;
	uint8_t *b = B->ans_buf;
	if (6 + len > sizeof(B->ans_buf)) {
		return;
	}
	memcpy(b + 6, d, len);
	if (d[0] == 0x02 && c->svc == 0x03 && (rmsg == 0x004d || rmsg == 0x0024 || rmsg == 0x0043) && wh_active(sbb_ms())) {
		wh_note(sbb_ms());      /* 1087 */
		uint32_t nl = wh_strip(rmsg, b + 13, ml);
		if (nl != ml) {
			b[11] = (uint8_t)nl;
			b[12] = (uint8_t)(nl >> 8);
			len = 7 + nl;
			ml = nl;
		}
	}
	if (nstrip) {
		uint32_t nl = strip_tlvs(c->svc, rmsg, b + 13, ml);
		if (nl != ml) {
			b[11] = (uint8_t)nl;
			b[12] = (uint8_t)(nl >> 8);
			len = 7 + nl;
		}
	}
	if (c->dat && D) {
		d_answer(c, b + 6, len, rq, rqn);       /* 1080: learn handles/mux/gateway; WDA answers show CommCenter its values */
	}
	finish_qmux(b, 6 + len, c->svc, c->cid);
	if (d[0] == 0x04) {
		c->ind++;
		cnt.ind++;
	} else {
		c->resp++;
		cnt.resp++;
	}
	uint32_t cpipe = c->pipe, csess = c->sess;
	unsigned csvc = c->svc, ccid = c->cid;
	if (d[0] == 0x04 && hold_frame(c->pipe, c->sess, b, 6 + len)) {
		return;
	}
	send_to(cpipe, csess, b, 6 + len);
	if (d[0] == 0x02 && res == 0) {
		run_triggers(cpipe, csess, csvc, ccid, rmsg, rq, rqn);
	}
}

/* ---- CommCenter's records (from SweetABP, any thread) -> the sweetbb thread ---- */

struct c2w {
	struct c2w *next;
	uint32_t pipe, sess, type, len;
	uint8_t d[];
};
static LCK_MTX_DECLARE(c2w_lock, &qmux_grp);
static struct c2w *c2w_first, *c2w_last;
static uint32_t c2w_bytes, c2w_count;
static unsigned c2w_dropped;

void
sweet_qmux_c2w(unsigned pipe, uint32_t sess, unsigned type, const void *data, uint32_t len)
{
	if (!on || len > ABP_MAXFRAME) {
		return;
	}
	struct c2w *r = kalloc_data(sizeof(*r) + len, Z_WAITOK);
	if (r == NULL) {
		c2w_dropped++;
		return;
	}
	r->next = NULL;
	r->pipe = pipe;
	r->sess = sess;
	r->type = type;
	r->len = len;
	if (len) {
		memcpy(r->d, data, len);
	}
	lck_mtx_lock(&c2w_lock);
	if (c2w_bytes + len > (4u << 20) || c2w_count >= 4096) {
		lck_mtx_unlock(&c2w_lock);
		kfree_data(r, sizeof(*r) + len);
		if (c2w_dropped++ < 20) {
			printf("SWEET QMUX: CommCenter's queue full: a %u-byte record of pipe %u dropped\n", len, pipe);
		}
		return;
	}
	if (c2w_last) {
		c2w_last->next = r;
	} else {
		c2w_first = r;
	}
	c2w_last = r;
	c2w_bytes += len;
	c2w_count++;
	lck_mtx_unlock(&c2w_lock);
	sbb_wake();
}

/* ---- timers, ready, stats ---- */

static uint64_t next_check, next_stats, last_kick[SBB_QMUX_NPORTS];
static int dump_req;

static void
kick(uint32_t port, unsigned svc, uint64_t now)
{
	int si = srv_find(svc);
	if (!kickon || si < 0 || port < SBB_QMUX_PORT0 || port >= SBB_QMUX_PORT0 + SBB_QMUX_NPORTS) {
		return;
	}
	if (last_kick[port - SBB_QMUX_PORT0] && now - last_kick[port - SBB_QMUX_PORT0] < 5000) {
		return;
	}
	last_kick[port - SBB_QMUX_PORT0] = now;
	kicks++;
	LOG("kick: RESUME_TX port %#x -> %s server %u:%x (a request got no answer in 5 s)\n", port, svc_name(svc), srv[si].node,
	    srv[si].port);
	uint32_t pkt[5] = { SBB_QRTR_RESUME_TX, SBB_NODE_APPS, port, 0, 0 };
	sbb_qrtr_send(SBB_QRTR_RESUME_TX, port, srv[si].node, srv[si].port, pkt, sizeof(pkt));
}

static void
mst_table(const char *why)
{
	LOG("table (%s, %d rows, late %u): svc msg action count ok err[code] noans\n", why, nmst, late);
	for (int i = 0; i < nmst; i++) {
		struct mst *m = &B->mst[i];
		LOG(" %02x %-12s %04x %-5s %u %u %u[0x%x] %u\n", m->svc, svc_name(m->svc), m->msg,
		    m->count || m->act ? act_name[m->act < 6 ? m->act : 4] : "-", m->count, m->ok, m->err, m->last_err, m->noans);
	}
	mst_dirty = 0;
}

static void
update_ready(void)
{
	int ok = srv_find(0x02) >= 0 && srv_find(0x03) >= 0 && srv_find(0x0b) >= 0;
	if (ok == ready) {
		return;
	}
	ready = ok;
	sweet_abp_set_ready(ok != 0, ok ? "kernel router (modem DMS, NAS and UIM servers known)" :
	    "kernel router (a modem DMS/NAS/UIM server missing)");
}

void
sweet_qmux_poll(void)
{
	if (!on) {
		return;
	}
	uint64_t now = sbb_ms();
	/* CommCenter's records first, in order */
	for (unsigned k = 0; k < 256; k++) {
		lck_mtx_lock(&c2w_lock);
		struct c2w *r = c2w_first;
		if (r) {
			c2w_first = r->next;
			if (!c2w_first) {
				c2w_last = NULL;
			}
			c2w_bytes -= r->len;
			c2w_count--;
		}
		lck_mtx_unlock(&c2w_lock);
		if (!r) {
			break;
		}
		handle(r->pipe, r->sess, r->type, r->d, r->len);
		kfree_data(r, sizeof(*r) + r->len);
	}
	if (now >= next_check) {
		next_check = now + 10000;
		policy_poll(false);
	} else if (push_pending || push_clear) {
		policy_poll(false);
	}
	update_ready();
	for (int i = 0; i < NPEND; i++) {
		struct pend *x = &B->pend[i];
		if (x->used && now - x->t > 5000) {
			x->used = 0;
			if (dtmflog) {  /* 1086: which request it was */
				LOG("NO ANSWER in 5 s: %s(0x%02x) msg 0x%04x txn %u port %#x\n", svc_name(x->svc), x->svc, x->msg, x->txn, x->port);
			}
			kick(x->port, x->svc, now);
			struct mst *m = mst_get(x->svc, x->msg);
			if (m) {
				m->noans++;
				mst_dirty = 1;
				if (!m->answered_logged) {
					m->answered_logged = 1;
					LOG("ANSWER %s(0x%02x) msg 0x%04x: NO ANSWER after 5 s\n", svc_name(x->svc), x->svc, x->msg);
				}
			}
		}
	}
	simid_poll(now);
	bbr_poll(now);
	hold_poll(now);
	act_poll(now);
	repub_poll(now);
	if (dump_req) {
		dump_req = 0;
		mst_table("kern.sweet_qmux_dump");
	}
	data_poll(now);                 /* 1080 */
	if (now >= next_stats) {
		if (next_stats) {
			int ncl = 0, nidle = 0;
			for (int i = 0; i < NCL; i++) {
				ncl += cl[i].used == 1;
				nidle += cl[i].used == 2;
			}
			LOG("stats: %u frames in, %u out; %u fwd, %u modem resp, %u ind; local ok %u, err %u, drop %u; %d router clients "
			    "(%d idle); no server %u, send fail %u, late %u; malformed %u, replays %u, synth ind %u; kicks %u, stripped %u, "
			    "floor %u, rate %u; holdnas=%d held %u (rel bsp %u, timeout %u, gone %u)\n", nframes_in, nframes_out, cnt.fwd,
			    cnt.resp, cnt.ind, cnt.local_ok, cnt.local_err, cnt.dropped, ncl, nidle, cnt.noserver, cnt.sendfail, late,
			    malformed, replays, synth_inds, kicks, stripped, cnt.floor, cnt.ratelimited, holdnas, held_cnt, held_rel_bsp,
			    held_rel_to, held_drop);
		}
		next_stats = now + 60000;
	}
}

bool
sweet_qmux_on(void)
{
	return on;
}

void
sweet_qmux_start(void)
{
	B = kalloc_data(sizeof(*B), Z_WAITOK | Z_ZERO);
	if (B == NULL) {
		printf("SWEET QMUX: no memory for the router (%lu bytes): off\n", (unsigned long)sizeof(*B));
		return;
	}
	vauto_tc = thread_call_allocate_with_options(vauto_call, NULL, THREAD_CALL_PRIORITY_KERNEL, 0);  /* 1074 */
	if (sweet_bb & SBB_DATA) {      /* 1080: the data section's state (active only with kern.sweet_qmi_rules 1) */
		D = kalloc_data(sizeof(*D), Z_WAITOK | Z_ZERO);
		printf("SWEET QMUX: data section %s (%lu bytes): CommCenter owns data when kern.sweet_qmi_rules = 1 and "
		    "kern.sweet_data_fwd = 1\n", D ? "ready" : "NO MEMORY: off", (unsigned long)sizeof(*D));
	}
	PE_parse_boot_argn("sweetqmifilter", &qmi_filter, sizeof(qmi_filter));
	PE_parse_boot_argn("sweetqmirules", &qmi_rules, sizeof(qmi_rules));
	on = true;
	policy_poll(true);      /* (the built-in default: the root is not mounted yet) */
	LOG("router on (%lu bytes of state), filter %d, rules %d (%s), err 0x%x, reuse %d, kick %d, holdnas %d/%d ms\n",
	    (unsigned long)sizeof(*B), qmi_filter, qmi_rules, qmi_rules == 1 ? "option-1 list" : "P2 list", err_code, reuse, kickon,
	    holdnas, holddly);
}

int
sweet_qmux_stats(char *buf, size_t len)
{
	int o, ncl = 0, nidle = 0;
	if (!on) {
		return snprintf(buf, len, "router off (sweetbb & 2 not set)");
	}
	for (int i = 0; i < NCL; i++) {
		ncl += cl[i].used == 1;
		nidle += cl[i].used == 2;
	}
	o = snprintf(buf, len, "router on, ready %d, filter %d, rules %d, policy %s (%d ovr, %d trg, %d strip) | frames in %u out %u | fwd %u "
	    "resp %u ind %u | local ok %u err %u drop %u | clients %d (+%d idle) | noserver %u sendfail %u late %u malformed %u "
	    "replays %u synth %u kicks %u floor %u rate %u | held %u (bsp %u, to %u, gone %u) | SIM id %s/%s gen %u reads %u | "
	    "bbresets %u | CC queue %u (%u B, dropped %u) | per svc fwd/local/denied/live:",
	    ready, qmi_filter, qmi_rules, pol_src == SRC_PUSHED ? "pushed" : pol_src == SRC_FILE ? "file" : "built-in default", novr,
	    ntrg, nstrip, nframes_in, nframes_out, cnt.fwd, cnt.resp, cnt.ind, cnt.local_ok, cnt.local_err, cnt.dropped, ncl, nidle,
	    cnt.noserver, cnt.sendfail, late, malformed, replays, synth_inds, kicks, cnt.floor, cnt.ratelimited, held_cnt,
	    held_rel_bsp, held_rel_to, held_drop, simid.have_iccid ? "known" : "unknown", simid.have_imsi ? "known" : "unknown",
	    simid.gen, simid.reads, bbresets, c2w_count, c2w_bytes, c2w_dropped);
	for (unsigned s = 0; s < 256 && o < (int)len - 48; s++) {
		int live = 0;
		for (int i = 0; i < NCL; i++) {
			live += cl[i].used == 1 && cl[i].svc == s;
		}
		if (svc_cnt[s].fwd || svc_cnt[s].local || svc_cnt[s].denied || live) {
			o += snprintf(buf + o, len - (size_t)o, " %s(%02x) %u/%u/%u/%d", svc_name(s), s, svc_cnt[s].fwd, svc_cnt[s].local,
			    svc_cnt[s].denied, live);
		}
	}
	return o < (int)len ? o : (int)len - 1;
}

/* ---- sysctls ---- */

static int
sysctl_qint SYSCTL_HANDLER_ARGS
{
#pragma unused(arg2)
	int *p = (int *)arg1, v = *p, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);
	if (e == 0 && changed) {
		if (proc_suser(req->p) != 0) {
			return EPERM;
		}
		*p = v;
		printf("SWEET QMUX: kern.%s = %d\n", oidp->oid_name, v);
	}
	return e;
}
#define QINT(name, var, desc) \
	SYSCTL_PROC(_kern, OID_AUTO, name, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, &var, 0, sysctl_qint, "I", desc)
QINT(sweet_qmi_filter, qmi_filter, "redfin 1068: 0 pass (hard floor + policy text only), 1 protect (default), 2 protect + frame log");
QINT(sweet_qmi_rules, qmi_rules, "redfin 1068: built-in list 0 = P2 (abpbridge.c, default), 1 = option-1 (P4-design s.5.3 + user decisions)");
QINT(sweet_qmux_err, err_code, "redfin 1068: QMI error of the built-in err entries (abperr, default 0x5e)");
QINT(sweet_qmux_cidfail, cidfail, "redfin 1068: 1 = GetClientId fails for a service the modem lacks (abpcidfail)");
QINT(sweet_qmux_reuse, reuse, "redfin 1068: 1 = released router clients pooled per service (abpreuse, default 1)");
QINT(sweet_voice_dtmflog, dtmflog, "redfin 1086: 1 = log VOICE DTMF requests/answers/indications, Apple AUDIO request ids, unanswered requests (default 1)");
QINT(sweet_qmux_kick, kickon, "redfin 1068: 1 = RESUME_TX kick after 5 s without an answer (abpkick, default 1)");
QINT(sweet_qmux_holdnas, holdnas, "redfin 1068: NAS indications held until BSP activation (abpholdnas: 0 off, 1 = 5 s, >= 100 ms)");
QINT(sweet_qmux_holddly, holddly, "redfin 1068: ms after the BSP activation indication before held NAS indications go (abpholddly, 500)");
QINT(sweet_qmi_republish_ms, repub_ms, "redfin 1073: ms after the BSP activation indication before the auto re-publish GETs (20)");

static int
sysctl_repub SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int v = repub_mode, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (proc_suser(req->p) != 0) {
		return EPERM;
	}
	if (v < 0 || v > 4) {
		return EINVAL;
	}
	if (v == 2) {
		repub_now = 1;
		printf("SWEET QMUX: kern.sweet_qmi_republish = 2: a re-publish now (mode stays %d)\n", repub_mode);
		sbb_wake();
		return 0;
	}
	repub_mode = v;
	printf("SWEET QMUX: kern.sweet_qmi_republish = %d\n", v);
	return 0;
}
static int
sysctl_vauto SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int v = voice_auto, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (proc_suser(req->p) != 0) {
		return EPERM;
	}
	if (v < 0 || v > 2) {
		return EINVAL;
	}
	printf("SWEET VOICE AUTO: kern.sweet_voice_auto = %d (%s; was %d)\n", v, v == 0 ? "off" : v == 1 ? "earpiece" : "speaker",
	    voice_auto);
	voice_auto = v;
	if (v == 0) {
		bzero(vcall, sizeof(vcall));
		if (vauto_req) {
			vauto_ask(0);   /* stops only a session the trigger started */
		}
	} else if (vauto_req) {
		vauto_ask(v);   /* a call is up: the new route */
	}
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_voice_auto, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_vauto, "I",
    "redfin 1074: in-call audio follows the modem's call states: 0 off (default), 1 earpiece, 2 speaker");

SYSCTL_PROC(_kern, OID_AUTO, sweet_qmi_republish, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_repub, "I",
    "redfin 1073: 0 off (default), 1 = once per CommCenter session the modem's fresh NAS SysInfo/SigInfo as indications "
    "kern.sweet_qmi_republish_ms after BSP activation; write 2 = send them now; 3 = as 1, the BSP activation deferred until the "
    "modem has service (1084); 4 = as 3, the PLMN withheld from CommCenter until the activation (1087)");
QINT(sweet_qmi_withhold_s, wh_s, "redfin 1087: mode 4: longest PLMN withholding in s per CommCenter session (600), then the old 5 s hold timeout");
QINT(sweet_qmi_plmnblip, plmnblip, "redfin 1085: 1 = after a re-publish SysInfo shot, a SysInfo with another MNC then the real one (CommCenter re-publishes its registration status: no SOS); 0 off (default)");
QINT(sweet_qmi_plmnblip_ms, plmnblip_ms, "redfin 1085: delay in ms from the shot to the PLMN blip (1500)");
QINT(sweet_qmi_republish_wait_s, repub_wait_s, "redfin 1084: mode 3: longest wait in s for modem service before the BSP activation goes anyway (45)");
QINT(sweet_qmi_policy_file, policy_file, "redfin 1068: 1 = /var/local/abp-policy.conf overrides the built-in default text (checked every 10 s)");

static int
sysctl_dump SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int v = 0, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);
	if (e == 0 && changed && v) {
		dump_req = 1;
		sbb_wake();
	}
	return e;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_qmux_dump, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_dump, "I",
    "redfin 1068: write 1 = the per-(svc, msg) table to the kernel log");

static int
sysctl_policy SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char sum[160];
	snprintf(sum, sizeof(sum), "%s: %d override, %d trigger, %d strip line(s)", pol_src == SRC_PUSHED ? "pushed" :
	    pol_src == SRC_FILE ? "/var/local/abp-policy.conf" : "built-in default", novr, ntrg, nstrip);
	int e = SYSCTL_OUT(req, sum, strlen(sum) + 1);
	if (e || req->newptr == USER_ADDR_NULL) {
		return e;
	}
	if (proc_suser(req->p) != 0) {
		return EPERM;
	}
	size_t n0 = req->newlen - req->newidx, n = n0;
	if (n > POLICY_MAX) {
		return E2BIG;
	}
	char *t = kalloc_data(n0 + 1, Z_WAITOK | Z_ZERO);
	if (t == NULL) {
		return ENOMEM;
	}
	e = SYSCTL_IN(req, t, n0);
	if (e) {
		kfree_data(t, n0 + 1);
		return e;
	}
	while (n && (t[n - 1] == 0 || is_space(t[n - 1]))) {
		n--;
	}
	char *x = NULL;
	if (n && !(n == 1 && t[0] == '-')) {
		x = kalloc_data(n + 1, Z_WAITOK | Z_ZERO);
		if (x == NULL) {
			kfree_data(t, n0 + 1);
			return ENOMEM;
		}
		memcpy(x, t, n);
	}
	kfree_data(t, n0 + 1);
	lck_mtx_lock(&push_lock);
	if (push_text) {
		kfree_data(push_text, push_len + 1);
		push_text = NULL;
		push_len = 0;
	}
	if (x == NULL) {
		push_clear = true;      /* back to the file / the default at the next round */
		push_pending = false;
		next_check = 0;
	} else {
		push_text = x;
		push_len = n;
		push_pending = true;
		push_clear = false;
	}
	lck_mtx_unlock(&push_lock);
	sbb_wake();
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_qmi_policy, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_policy, "A",
    "redfin 1068: reads the policy source; write = load this policy text (abp-policy.conf grammar), '-' or empty = back to the file/default");
/* ---- 1080 sysctls ---- */

static int
sysctl_data_fwd SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int v = data_fwd, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (proc_suser(req->p) != 0) {
		return EPERM;
	}
	if (v != 0 && v != 1) {
		return EINVAL;
	}
	data_fwd = v;
	printf("SWEET QMUX: kern.sweet_data_fwd = %d\n", v);
	sbb_wake();
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_data_fwd, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_data_fwd, "I",
    "redfin 1080: 1 (default) = with sweetbb & 0x10 and kern.sweet_qmi_rules 1 CommCenter's WDS/WDA/QOS/DSD/DFS go to the modem; "
    "0 = answered err 0x5e (live calls stopped)");
QINT(sweet_data_opts, data_opts, "redfin 1080: 1 = drop WDA SetDataFormat TLVs outside 0x10-0x19, 2 = add WDS bind client type 0x13 = 1; "
    "1088: 4 (default on) = the router sends its own WDA SetDataFormat once per modem boot, StartNetworkInterface waits for it");

static int
sysctl_data_stats SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char *b = kalloc_data(2048, Z_WAITOK | Z_ZERO);
	if (b == NULL) {
		return ENOMEM;
	}
	int n = data_stats(b, 2048);
	int e = SYSCTL_OUT(req, b, (size_t)n + 1);
	kfree_data(b, 2048);
	return e;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_data_stats, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_data_stats, "A",
    "redfin 1080: CommCenter data forwarding: per service forwards, starts/holds/rate, translations, handles per mux, profile gate");

static int
sysctl_wds_bak SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	if (req->newptr != USER_ADDR_NULL || proc_suser(req->p) != 0) {
		return EPERM;
	}
	if (D == NULL) {
		return SYSCTL_OUT(req, "off", 4);
	}
	char *b = kalloc_data(D_BAK_MAX, Z_WAITOK | Z_ZERO);
	if (b == NULL) {
		return ENOMEM;
	}
	size_t n;
	lck_mtx_lock(&bak_lock);
	if (pb_state == 3) {
		memcpy(b, D->bak, D->bakn);
		n = D->bakn;
	} else {
		n = (size_t)snprintf(b, D_BAK_MAX, "state %d (%s)", pb_state, pb_state == 0 ? "none yet" : pb_state == 4 ?
		    "acknowledged: exposed once, cleared" : "reading");
	}
	lck_mtx_unlock(&bak_lock);
	b[n] = 0;
	int e = SYSCTL_OUT(req, b, n + 1);
	kfree_data(b, D_BAK_MAX);
	return e;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_wds_profiles_bak, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_wds_bak, "A",
    "redfin 1080: root only: the modem's WDS profile backup (hex TLVs, no credentials) until kern.sweet_wds_profiles_ack");

static int
sysctl_wds_ack SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int v = pb_state == 4, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (proc_suser(req->p) != 0) {
		return EPERM;
	}
	if (v != 1) {
		return EINVAL;
	}
	if (pb_state != 3) {
		return pb_state == 4 ? 0 : EBUSY;
	}
	pb_ack = 1;
	sbb_wake();
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_wds_profiles_ack, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_wds_ack, "I",
    "redfin 1080: write 1 after saving kern.sweet_wds_profiles_bak: CommCenter's WDS profile writes then pass (EBUSY = not ready)");

static int
sysctl_gate_ids SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char s[96];
	int o = 0;
	for (int i = 0; i < ngate; i++) {
		o += snprintf(s + o, sizeof(s) - (size_t)o, "%s0x%04x", i ? " " : "", gate_ids[i]);
	}
	int e = SYSCTL_OUT(req, s, strlen(s) + 1);
	if (e || req->newptr == USER_ADDR_NULL) {
		return e;
	}
	if (proc_suser(req->p) != 0) {
		return EPERM;
	}
	char t[96];
	size_t n = req->newlen - req->newidx;
	if (n >= sizeof(t)) {
		return E2BIG;
	}
	bzero(t, sizeof(t));
	e = SYSCTL_IN(req, t, n);
	if (e) {
		return e;
	}
	uint16_t ids[D_NGATE] = { 0x0027, 0x0028, 0x0029 };
	int k = 3;
	char w[D_NGATE + 4][256];
	int nw = split_words(t, w, D_NGATE + 4);
	for (int i = 0; i < nw; i++) {
		int v;
		if (parse_num(w[i], &v) || v < 1 || v >= 0x5500) {
			return EINVAL;
		}
		if (v == 0x27 || v == 0x28 || v == 0x29) {
			continue;
		}
		if (k >= D_NGATE) {
			return E2BIG;
		}
		ids[k++] = (uint16_t)v;
	}
	memcpy(gate_ids, ids, sizeof(ids));
	ngate = k;
	printf("SWEET QMUX: kern.sweet_wds_gate_ids: %d WDS message ids held until the profile backup is acknowledged\n", k);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_wds_gate_ids, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_gate_ids, "A",
    "redfin 1080: WDS message ids held until the profile backup is acknowledged (0x0027-0x0029 always; write the extra ids)");
#endif /* ARM64_BOARD_CONFIG_SWEET */
