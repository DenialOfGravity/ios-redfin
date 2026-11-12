/*
 * abpbridge: the userland end of kernel 1061/1063's /dev/abpbridge (Claude 2026-11-09).
 * Apple's CommCenter (its in-process AppleBasebandManager) talks QMI to "the baseband" over two AppleBasebandPCI
 * pipes, qmux1 and qmux2, each one whole QMUX stream (01 | len16 | flags | svc | cid | SDU). The kernel stand-in
 * hands every frame CommCenter writes to us (tagged with pipe + session, one session per user client) and turns
 * our frames into completions of that session's reads.
 *
 * Word abpbridge=N in /var/local/cell.on (re-read every 10 s):
 *   0 / absent  off: the device is closed (baseline behaviour).
 *   1           P1 observe: nothing goes to the modem. CTL (service 0) is answered here, every other request gets
 *               QMI error abperr (default 0x5e NOT_SUPPORTED); every frame is logged.
 *   2           P2 router: CTL stays local (per session), other requests go through the policy table below:
 *               FORWARD = sent to the modem's QMI service over QRTR, each (pipe, session, service, client id) from
 *               its own QRTR port (ABP_PORT0..), so the modem's responses and indications come back unambiguously
 *               and are returned to that session with the QMUX header (svc, cid); OK = answered here with success
 *               and no TLVs; ERR:<code> = answered here with a QMI failure; DROP = no answer.
 * Run 2 (2026-11-09): router clients are POOLED per service (abpreuse=1, default in mode 2): a released client's
 * QRTR port (and so its modem-side client) goes idle and is given to the next CommCenter client of that service
 * instead of a DEL_CLIENT + new port, so CommCenter's session restarts cannot pile up modem clients (run 1: NAS stopped
 * answering everyone, wland's cell.c too, after 3 CommCenter restarts leaked ~20 NAS clients; the modem kept sending
 * PBM indications to released ports, so it did not act on our DEL_CLIENT). abpreuse=0 = run-1 behaviour. Packets to a
 * router port with no active client (idle, released) are dropped silently and counted as "late".
 * Diagnostics (mode 2): every forwarded request is tracked (port + txn) for 5 s; the first answer per (svc, msg) is
 * logged once with the modem's result TLV ("abp: ANSWER ..." / "NO ANSWER after 5 s"); per-(svc, msg) counters
 * (action, count, ok, err [last code], no answer, late) print as a table every 60 s when they changed; abpdump=N
 * (any new non-zero value) prints the table at once.
 * Other words: abplog=1 (P1-style frame log: every frame in wland.log + full hex in ABP_LOG; always on in mode 1),
 * abperr=N (the error code of the built-in "err" entries, default 0x5e), abpcidfail=1 (GetClientId fails with
 * 0x1f INVALID_SERVICE_TYPE for a forwarded service the modem has no server for; default: the client id is given
 * and its requests get 0x5e).
 *
 * The SDU of a non-CTL QMUX frame (u8 flags 0 req/2 resp/4 ind | u16 txn | u16 msg | u16 len | TLVs) is byte for
 * byte the QMI-over-QRTR message (qmi.h), checked against the P1 frames; only the 6-byte QMUX header is added or
 * removed. QMUX "broadcast" indications (cid 0xff) do not exist on QRTR: the modem sends each registered client its
 * own copy, which goes back with that client's cid.
 *
 * Policy: built-in table (abp_builtin below), overridden by /var/local/abp-policy.conf (re-read when it changes):
 *     # svc msg action          svc/msg: hex (0x..) or decimal, msg '*' = every message of the service
 *     0x02 0x002e ok            action: fwd | ok | err:<code> | drop
 *     0xe2 * err:0x5e
 * Never FORWARDED whatever the file says: Apple-only services (svc >= 0xe0) and Apple message ids (>= 0x5500); ok,
 * err:<code>, resp:<hex> and drop lines for them do apply (a fwd line for them becomes err abperr).
 * r3 (2026-11-09) more actions and trigger lines, all re-read with the file (no rebuild):
 *     0xe2 0xe010 resp:1001000102...   success answered here WITH these TLVs (hex of t|len16|value ..., the result TLV
 *                                      0x02 is added in front); spaces not allowed inside the hex, '.' or '_' may separate
 *     on 0x0b 0x002e ind 0x0032 <hex>  after the answer to (svc, msg) went to CommCenter (a modem success or a local
 *                                      ok/resp), send that client indication <msg> of the same service with TLVs <hex>
 *     on 0x03 0x0003 replay 0x004d 0x004e [map 01=14,..]
 *                                      after a SUCCESS answer to (svc, msg), the router itself sends GET <msg 0x004d> to
 *                                      the modem (from that client's QRTR port; only if the policy forwards it, so
 *                                      protected messages can never be sent this way) and returns the modem's answer to
 *                                      that client as indication <0x004e> (result TLV dropped, TLV ids remapped by
 *                                      'map a=b'): "replay the current state" for a modem that sent its indications
 *                                      long before CommCenter registered. One line per (svc, msg, kind) is used (all
 *                                      matching 'on' lines run, in file order).
 * P2 reg (2026-11-09): an 'on' line may end in "if <tlv>": it runs only when the request carried TLV <tlv> with a non-zero
 * first byte (e.g. "on 0x03 0x0003 replay 0x004d 0x004e if 18": sys info only to a client that enabled sys info inds).
 * "strip <svc> <msg> <tlv>[,..]" lines remove TLVs from that (svc, msg) on the way to CommCenter (responses, indications,
 * replays): Apple's NAS SysInfo handler asserts (boost::optional get) on the modem's 1-byte TLV 0x26.
 * A forwarded request unanswered for 5 s sends an unsolicited RESUME_TX from its port to the server (abpkick=0: off).
 * Every SDU from the modem is checked (msg len and TLVs must add up) before it goes to CommCenter: a malformed one is
 * dropped and logged ("abp: MALFORMED"), as one truncated PBM indication ABM-ASSERTed CommCenter in P2 run1.
 * Every new (svc, msg) is logged once with the action and why ("abp: NEW ..."); a policy reload logs them again.
 *
 * READY: the kernel (1063, sweetabp & 8) publishes the baseband services only after ABPB_IOC_READY 1. Mode 1 says
 * ready right after opening; mode 2 once the modem's DMS, NAS and UIM servers are known on QRTR, and not ready
 * when one of them goes away.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include "abpbridge.h"
#include "cell.h"
#include "link.h"
#include "qmi.h"

#define ABP_DEV		"/dev/abpbridge"
#define ABP_LOG		"/var/local/abpbridge.log"
#define ABP_POLICY	"/var/local/abp-policy.conf"
#define ABPB_MAGIC	0xAB51
#define ABPB_IOC_READY	_IOW('B', 0x51, uint32_t)	/* kernel 1063 */
#define ABP_MAXFRAME	65536
#define NPIPES		2
#define QMI_ERR_NOT_SUPPORTED		0x5e
#define QMI_ERR_INVALID_SERVICE_TYPE	0x1f
#define QMI_ERR_CLIENT_IDS_EXHAUSTED	0x05

struct abpb_hdr {
	uint16_t magic;
	uint8_t pipe;
	uint8_t type;
	uint32_t sess;		/* one per CommCenter user client on the pipe (ABM opens qmux2 several times) */
	uint32_t len;
} __attribute__((packed));

static const char *pipe_name[NPIPES] = { "qmux1", "qmux2" };
static int fd = -1, mode, err_code = QMI_ERR_NOT_SUPPORTED, logframes, cidfail, ready = -1, ioctl_bad;
static uint64_t next_check, t0, next_stats;
static FILE *lf;
static uint8_t rbuf[sizeof(struct abpb_hdr) * 8 + ABP_MAXFRAME * 2];
#define NSESS	32
static struct { uint32_t pipe, sess; uint8_t cids[256][32]; } st[NSESS];	/* per session: [svc][cid] in use */
static uint32_t cur_pipe, cur_sess;	/* the session of the frame being handled */
static unsigned nframes_in, nframes_out, nseen;
static struct { uint8_t svc; uint16_t msg; } seen[1024];
static struct {
	unsigned fwd, local_ok, local_err, dropped, resp, ind, noport, noserver, sendfail;
} cnt;

/* ---- the modem's QMI servers on QRTR (every NEW_SERVER wland sees) ---- */

#define NSRV	96
static struct { uint32_t svc, inst, node, port; int up; } srv[NSRV];

static int srv_find(unsigned svc)
{
	int best = -1;
	for (int i = 0; i < NSRV; i++)
		if (srv[i].up && srv[i].svc == svc && (best < 0 || srv[i].inst < srv[best].inst))
			best = i;	/* several instances (e.g. 0x33): the lowest */
	return best;
}

/* ---- router clients: one QRTR port per (pipe, session, service, client id) ---- */

#define NCL	256
static struct cl {
	int used;			/* 0 free, 1 active (a CommCenter client), 2 idle (pooled: port + modem client kept) */
	uint32_t pipe, sess, port;
	uint8_t svc, cid;
	unsigned fwd, resp, ind;
} cl[NCL];
static uint32_t next_port_gen;
static int reuse = 1, dumpword, dumpword_old;
static unsigned late;

/* ---- per-(svc, msg) counters and the pending forwarded requests ---- */

#define NMST	1024
static struct mst {
	uint8_t svc, act;
	uint16_t msg, last_err;
	unsigned count, ok, err, noans, answered_logged;
} mst[NMST];
static int nmst, mst_dirty;

static struct mst *mst_get(unsigned svc, unsigned msg)
{
	for (int i = 0; i < nmst; i++)
		if (mst[i].svc == svc && mst[i].msg == msg)
			return &mst[i];
	if (nmst >= NMST)
		return NULL;
	memset(&mst[nmst], 0, sizeof(mst[0]));
	mst[nmst].svc = (uint8_t)svc;
	mst[nmst].msg = (uint16_t)msg;
	return &mst[nmst++];
}

#define TRG_MAXMAP	8	/* r3 replay: TLV id remaps per 'on' line */
#define NPEND	512
static struct pend {
	int used;
	uint32_t port;
	uint16_t txn, msg;
	uint8_t svc;
	uint64_t t;
	/* r3 replay: a GET the router sent itself; its answer becomes indication ind_msg to (pipe, sess, svc, cid) */
	int replay;
	uint32_t rpipe, rsess;
	uint8_t rcid, nmap, map[TRG_MAXMAP][2];
	uint16_t ind_msg;
	/* P2 reg: the request's TLVs (first 96 bytes), for 'if <tlv>' triggers on its answer */
	uint8_t rqn, rq[96];
	/* n12b: the router's own EF read (SIM identity); its answer never reaches CommCenter */
	uint8_t simread;
	uint16_t file;
} pend[NPEND];
static uint16_t replay_txn = 0xff00;	/* router-made GETs use txn 0xff00..0xfffe */

static const char *svc_name(unsigned s)
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

static const char *ctl_name(unsigned m)
{
	switch (m) {
	case 0x20: return "SetInstanceId"; case 0x21: return "GetVersionInfo"; case 0x22: return "GetClientId";
	case 0x23: return "ReleaseClientId"; case 0x24: return "RevokeClientIdInd"; case 0x25: return "InvalidClientIdInd";
	case 0x26: return "SetDataFormat"; case 0x27: return "Sync"; case 0x28: return "RegPwrSaveMode";
	case 0x29: return "ConfigPwrSaveSettings"; case 0x2a: return "SetPwrSaveMode"; case 0x2b: return "GetPwrSaveMode";
	}
	return "?";
}

/* ---- policy ---- */

enum { ACT_FWD, ACT_OK, ACT_ERR, ACT_DROP, ACT_LOCAL, ACT_RESP };
static const char *act_name[] = { "fwd", "ok", "err", "drop", "local", "resp" };
#define POL_ANY		(-1)
#define POL_ABPERR	(-1)	/* err code: abperr (default 0x5e) */

struct pol {
	int svc, msg, act, err;
	const char *why;
};

struct fref { int tlv, off, len, add, hasadd; };	/* n12b: a request field "RQ<tlv>:<off>:<len>[+<n>]" */
/* r3 trigger lines ("on svc msg ind|replay ...") */
enum { TRG_IND, TRG_REPLAY };
#define NTRG	64
static struct trg {
	int svc, msg, kind;
	uint16_t m2, m3;		/* ind: indication msg; replay: GET msg, indication msg */
	uint8_t map[TRG_MAXMAP][2];	/* replay: TLV id a -> b */
	int nmap;
	const uint8_t *data;		/* ind: TLVs */
	uint16_t dlen;
	int cond;			/* P2 reg 'if <tlv>': only when the request had TLV <tlv> with a non-zero first byte; -1 always */
	const char *tmpl;		/* n12b: ind data with placeholders (expanded at send time) */
	int ceq;			/* n12b 'if RQ..=RQ..': only when both request fields are equal */
	struct fref fa, fb;
} trg[NTRG];
static int ntrg;
static uint8_t pol_data[16384];	/* hex data of resp/ind lines (one policy load) */
static uint32_t pol_dlen;
static unsigned malformed, replays, synth_inds, badrec;
static int kickon = 1;		/* abpkick=0 turns the lost-RESUME_TX recovery off */
/* P2 reg: "strip <svc> <msg> <tlv>[,<tlv>..]": those TLVs are removed from every modem message (response, indication,
 * replay) of (svc, msg) before it reaches CommCenter (Apple's parsers assert on some Qualcomm TLVs, e.g. NAS SysInfo 0x26) */
#define NSTRIP	32
static struct strip { int svc, msg; uint8_t ids[32]; int nids; } strips[NSTRIP];
static int nstrip;
static unsigned stripped;

/* drop the TLVs listed for (svc, msg) from TLV list p (n bytes) in place; returns the new length */
static uint32_t strip_tlvs(unsigned svc, unsigned msg, uint8_t *p, uint32_t n)
{
	const struct strip *st = NULL;
	for (int i = 0; i < nstrip && !st; i++)
		if (strips[i].svc == (int)svc && strips[i].msg == (int)msg)
			st = &strips[i];
	if (!st)
		return n;
	uint32_t o = 0;
	for (uint32_t k = 0; k + 3 <= n;) {
		uint32_t l = 3u + (uint32_t)(p[k + 1] | (p[k + 2] << 8));
		if (k + l > n)
			break;
		int drop = 0;
		for (int j = 0; j < st->nids; j++)
			drop |= st->ids[j] == p[k];
		if (drop)
			stripped++;
		else {
			memmove(p + o, p + k, l);
			o += l;
		}
		k += l;
	}
	return o;
}

static int parse_num(const char *s, int *v);
static int parse_strip(char *line, int ln)
{
	char w[4][256];
	struct strip t;
	memset(&t, 0, sizeof(t));
	if (sscanf(line, "%255s %255s %255s %255s", w[0], w[1], w[2], w[3]) != 4 || nstrip >= NSTRIP || parse_num(w[1], &t.svc) ||
	    parse_num(w[2], &t.msg))
		goto bad;
	for (char *m = w[3]; *m && t.nids < 32;) {
		unsigned x;
		if (sscanf(m, "%x", &x) != 1 || x > 255)
			goto bad;
		t.ids[t.nids++] = (uint8_t)x;
		while (*m && *m != ',')
			m++;
		if (*m == ',')
			m++;
	}
	strips[nstrip++] = t;
	return 0;
bad:
	printf("abp: %s line %d: bad 'strip' line (strip <svc> <msg> <tlv>[,<tlv>..])\n", ABP_POLICY, ln);
	return -1;
}

/*
 * Built-in defaults (P2). Exact (svc, msg) entries win over the (svc, POL_ANY) entry; a service with no entry is
 * forwarded when it is a standard one (< 0xe0) the modem offers, else err. Protect wland's data path and the
 * modem's state: no operating mode / reset / system selection / attach / registration / SIM power, session or PIN
 * changes go to the modem from CommCenter in this phase; data (WDS/WDA/DSD/QOS/DPM/DFS) stays with wland until P4.
 */
static const struct pol abp_builtin[] = {
	/* DMS: forwarded except state changes */
	{ 0x02, POL_ANY, ACT_FWD, 0, "DMS" },
	{ 0x02, 0x0000, ACT_OK, 0, "DMS reset: protected" },
	{ 0x02, 0x002e, ACT_OK, 0, "DMS set operating mode: protected (wland owns the mode)" },
	{ 0x02, 0x0027, ACT_ERR, POL_ABPERR, "DMS UIM set PIN protection: protected" },
	{ 0x02, 0x0029, ACT_ERR, POL_ABPERR, "DMS UIM unblock PIN: protected" },
	{ 0x02, 0x002a, ACT_ERR, POL_ABPERR, "DMS UIM change PIN: protected" },
	{ 0x02, 0x0032, ACT_ERR, POL_ABPERR, "DMS activate automatic: protected" },
	{ 0x02, 0x0033, ACT_ERR, POL_ABPERR, "DMS activate manual: protected" },
	{ 0x02, 0x0035, ACT_ERR, POL_ABPERR, "DMS set user lock state: protected" },
	{ 0x02, 0x0036, ACT_ERR, POL_ABPERR, "DMS set user lock code: protected" },
	{ 0x02, 0x0038, ACT_ERR, POL_ABPERR, "DMS write user data: protected" },
	{ 0x02, 0x003a, ACT_ERR, POL_ABPERR, "DMS restore factory defaults: protected" },
	{ 0x02, 0x0041, ACT_ERR, POL_ABPERR, "DMS UIM set CK protection: protected" },
	{ 0x02, 0x0042, ACT_ERR, POL_ABPERR, "DMS UIM unblock CK: protected" },
	{ 0x02, 0x0048, ACT_ERR, POL_ABPERR, "DMS set firmware preference: protected" },
	{ 0x02, 0x004a, ACT_ERR, POL_ABPERR, "DMS delete stored image: protected" },
	{ 0x02, 0x0050, ACT_ERR, POL_ABPERR, "DMS set boot image download mode: protected" },
	{ 0x02, 0x0052, ACT_ERR, POL_ABPERR, "DMS set service programming code: protected" },
	/* NAS: GETs and indication registrations forwarded; selection / attach / registration changes not */
	{ 0x03, POL_ANY, ACT_FWD, 0, "NAS" },
	{ 0x03, 0x0000, ACT_OK, 0, "NAS reset: protected" },
	{ 0x03, 0x0001, ACT_OK, 0, "NAS abort: protected" },
	{ 0x03, 0x0021, ACT_ERR, POL_ABPERR, "NAS network scan: protected (drops service while it runs)" },
	{ 0x03, 0x0022, ACT_OK, 0, "NAS initiate network register: protected" },
	{ 0x03, 0x0023, ACT_OK, 0, "NAS initiate attach: protected" },
	{ 0x03, 0x0027, ACT_ERR, POL_ABPERR, "NAS set preferred networks: protected" },
	{ 0x03, 0x0029, ACT_ERR, POL_ABPERR, "NAS set forbidden networks: protected" },
	{ 0x03, 0x002a, ACT_ERR, POL_ABPERR, "NAS set technology preference: protected" },
	{ 0x03, 0x002d, ACT_ERR, POL_ABPERR, "NAS set ACCOLC: protected" },
	{ 0x03, 0x0030, ACT_ERR, POL_ABPERR, "NAS set device config: protected" },
	{ 0x03, 0x0033, ACT_OK, 0, "NAS set system selection preference: protected (wland's nolte/csonly own it)" },
	{ 0x03, 0x0057, ACT_ERR, POL_ABPERR, "NAS update AKEY: protected" },
	/* UIM: reads, events, PIN verify forwarded; card power, session, writes and PIN changes not */
	{ 0x0b, POL_ANY, ACT_FWD, 0, "UIM" },
	{ 0x0b, 0x0000, ACT_OK, 0, "UIM reset: protected" },
	{ 0x0b, 0x0022, ACT_ERR, POL_ABPERR, "UIM write transparent: protected" },
	{ 0x0b, 0x0023, ACT_ERR, POL_ABPERR, "UIM write record: protected" },
	{ 0x0b, 0x0025, ACT_ERR, POL_ABPERR, "UIM set PIN protection (enable/disable): protected" },
	{ 0x0b, 0x0027, ACT_ERR, POL_ABPERR, "UIM unblock PIN: protected" },
	{ 0x0b, 0x0028, ACT_ERR, POL_ABPERR, "UIM change PIN: protected" },
	{ 0x0b, 0x0029, ACT_ERR, POL_ABPERR, "UIM depersonalization: protected" },
	{ 0x0b, 0x0030, ACT_OK, 0, "UIM power down: protected" },
	{ 0x0b, 0x0031, ACT_OK, 0, "UIM power up: protected" },
	{ 0x0b, 0x0033, ACT_OK, 0, "UIM change provisioning session: protected" },
	{ 0x0b, 0x0046, ACT_ERR, POL_ABPERR, "UIM switch slot: protected" },
	/* forwarded as they are */
	{ 0x05, POL_ANY, ACT_FWD, 0, "WMS" },
	{ 0x0a, POL_ANY, ACT_FWD, 0, "CAT" },
	{ 0x0c, POL_ANY, ACT_FWD, 0, "PBM" },
	/* data path: stays with wland until P4 */
	{ 0x01, POL_ANY, ACT_ERR, POL_ABPERR, "WDS: data stays with wland (P4)" },
	{ 0x04, POL_ANY, ACT_ERR, POL_ABPERR, "QOS: data stays with wland (P4)" },
	{ 0x1a, POL_ANY, ACT_ERR, POL_ABPERR, "WDA: data stays with wland (P4)" },
	{ 0x2a, POL_ANY, ACT_ERR, POL_ABPERR, "DSD: data stays with wland (P4)" },
	{ 0x2f, POL_ANY, ACT_ERR, POL_ABPERR, "DPM: data stays with wland (P4)" },
	{ 0x30, POL_ANY, ACT_ERR, POL_ABPERR, "DFS: data stays with wland (P4)" },
	/* other standard services kept away from the modem for now */
	{ 0x09, POL_ANY, ACT_ERR, POL_ABPERR, "VOICE: not in P2" },
	{ 0x06, POL_ANY, ACT_ERR, POL_ABPERR, "PDS: not in P2" },
	/* r3: locationd (through ABM's 'location' QMux) asks the baseband for a HardReset whenever QMI_PDS_RESET fails
	 * (P2 run1: "Reset requested ... AP-triggered: locationd Qmi Error Response: QMI_ERR_NOT_SUPPORTED,MSG,
	 * qmi::pds::QMI_PDS_RESET" every ~5 s): answered success here, never forwarded */
	{ 0x06, 0x0000, ACT_OK, 0, "PDS reset: answered here (an error makes locationd reset the baseband)" },
	{ 0x10, POL_ANY, ACT_ERR, POL_ABPERR, "LOC: not in P2" },
	{ 0x08, POL_ANY, ACT_ERR, POL_ABPERR, "AT: not in P2" },
	{ 0x22, POL_ANY, ACT_OK, 0, "COEX: answered success here (would change LTE/Wi-Fi/BT coexistence under wland's Wi-Fi)" },
	{ 0x24, POL_ANY, ACT_ERR, POL_ABPERR, "PDC: modem configuration (MBN) selection: protected" },
	{ 0x2b, POL_ANY, ACT_ERR, POL_ABPERR, "SSCTL: subsystem restart: protected" },
	{ 0x29, POL_ANY, ACT_ERR, POL_ABPERR, "RFRPE: RF settings: protected" },
	{ 0x11, POL_ANY, ACT_ERR, POL_ABPERR, "SAR: RF power: protected" },
};

#define NOVR	256
static struct pol ovr[NOVR];
static struct { const uint8_t *data; uint16_t dlen; const char *tmpl; } ovr_data[NOVR];	/* r3: resp:<hex> of ovr[i]; n12b: template */
static int novr;
static time_t ovr_mtime;
static off_t ovr_size;

static int parse_num(const char *s, int *v)
{
	char *e;
	long x = strtol(s, &e, 0);
	if (e == s || *e)
		return -1;
	*v = (int)x;
	return 0;
}

/* hex string ("0102ab", '.', '_' and ':' ignored) into pol_data; NULL on a bad string or a full buffer */
static const uint8_t *parse_hex(const char *h, uint16_t *len)
{
	uint8_t *start = pol_data + pol_dlen;
	uint32_t n = 0;
	int hi = -1;
	for (; *h; h++) {
		int v;
		if (*h == '.' || *h == '_' || *h == ':')
			continue;
		if (*h >= '0' && *h <= '9')
			v = *h - '0';
		else if (*h >= 'a' && *h <= 'f')
			v = *h - 'a' + 10;
		else if (*h >= 'A' && *h <= 'F')
			v = *h - 'A' + 10;
		else
			return NULL;
		if (hi < 0) {
			hi = v;
			continue;
		}
		if (pol_dlen + n >= sizeof(pol_data))
			return NULL;
		start[n++] = (uint8_t)(hi << 4 | v);
		hi = -1;
	}
	if (hi >= 0 || n > 4096)
		return NULL;
	pol_dlen += n;
	*len = (uint16_t)n;
	return start;
}

/* TLVs t|len16|value must add up exactly */
static int tlvs_ok(const uint8_t *p, uint32_t n)
{
	while (n) {
		if (n < 3)
			return 0;
		uint32_t l = (uint32_t)(p[1] | (p[2] << 8));
		if (l > n - 3)
			return 0;
		p += 3 + l;
		n -= 3 + l;
	}
	return 1;
}

/*
 * n12b (2026-11-12): templates in resp:<hex> and 'on .. ind <msg> <hex>' data, expanded when the message is sent.
 * After UniXNU/QcomBaseband (BSD-2-Clause, baseband/coachz_bb_data.h + coachz_baseband.cpp): an Apple modem answers the
 * BSP activation register (0xe2 0xb00c) with an indication carrying the SIM's ICCID and IMSI it read itself, and acks each
 * carrier-bundle segment (0xe2 0xa000) with the next segment number. Placeholders (inside the hex, no spaces):
 *   <id>{ICCID}   TLV <id> = 10 bytes, the ICCID in reading order (BCD, F-padded), as wland read it from the card
 *   <id>{IMSI}    TLV <id> = 8 bytes, the IMSI in reading order (BCD, F-padded)
 *                 (both include the TLV length; while not known the whole TLV is left out, and an indication with one is
 *                  deferred until the card has been read; it is sent again whenever the card's identity changes)
 *   {RQ<tlv>:<off>:<len>[+<n>]}   <len> bytes copied from the request's TLV <tlv> (hex) at byte <off>; with +<n> (len 1, 2
 *                 or 4) the little-endian value plus n. A request without that field gets QMI error 0x11 (missing arg) for a
 *                 resp line, and no indication for an 'on' line.
 * 'on' lines may also end in "if RQ<a>=RQ<b>" (two field references, compared as numbers), e.g. last segment:
 *   0xe2 0xa000 resp:400200{RQ01:2:2+1}
 *   on 0xe2 0xa000 ind 0xa001 010200{RQ01:0:1}0010010001 if RQ01:2:2+1=RQ01:4:2
 * ICCID/IMSI values are never logged (lengths only).
 */
#define SIM_ICCID_LEN	10
#define SIM_IMSI_LEN	8
static struct {
	uint8_t iccid[SIM_ICCID_LEN], imsi[SIM_IMSI_LEN];
	int have_iccid, have_imsi, refresh, tries;
	unsigned gen, reads, snoops;
	uint64_t next;
} simid;
static char pol_text[8192];	/* template texts of one policy load */
static uint32_t pol_tlen;


/* "RQ<tlv>:<off>:<len>[+<n>]" (s up to '}' or '=' or end); 0 ok */
static int parse_fref(const char *s, struct fref *f)
{
	unsigned t;
	int off, len, add = 0, k = 0;
	memset(f, 0, sizeof(*f));
	if ((s[0] != 'R' && s[0] != 'r') || (s[1] != 'Q' && s[1] != 'q'))
		return -1;
	if (sscanf(s + 2, "%x:%d:%d%n", &t, &off, &len, &k) != 3 || t > 255 || off < 0 || off > 4096 || len < 1 || len > 64)
		return -1;
	s += 2 + k;
	if (*s == '+') {
		if (sscanf(s + 1, "%d", &add) != 1 || (len != 1 && len != 2 && len != 4))
			return -1;
		f->hasadd = 1;
	}
	f->tlv = (int)t;
	f->off = off;
	f->len = len;
	f->add = add;
	return 0;
}

/* the value of TLV id in list p (n bytes): pointer and length, NULL if absent */
static const uint8_t *tlv_find(const uint8_t *p, uint32_t n, unsigned id, uint32_t *len)
{
	for (uint32_t k = 0; k + 3 <= n;) {
		uint32_t l = (uint32_t)(p[k + 1] | (p[k + 2] << 8));
		if (k + 3 + l > n)
			return NULL;
		if (p[k] == id) {
			*len = l;
			return p + k + 3;
		}
		k += 3 + l;
	}
	return NULL;
}

/* the field's bytes (with +n applied) into out; dummy: zeros; 0 ok, -1 the request does not have it */
static int fref_get(const struct fref *f, const uint8_t *rq, uint32_t rqn, int dummy, uint8_t *out)
{
	uint32_t l = 0;
	const uint8_t *v = dummy ? NULL : tlv_find(rq, rqn, (unsigned)f->tlv, &l);
	if (dummy) {
		memset(out, 0, (size_t)f->len);
	} else {
		if (!v || (uint32_t)f->off + (uint32_t)f->len > l)
			return -1;
		memcpy(out, v + f->off, (size_t)f->len);
	}
	if (f->hasadd) {
		uint32_t x = 0;
		for (int i = 0; i < f->len; i++)
			x |= (uint32_t)out[i] << (8 * i);
		x += (uint32_t)f->add;
		for (int i = 0; i < f->len; i++)
			out[i] = (uint8_t)(x >> (8 * i));
	}
	return 0;
}

static int fref_num(const struct fref *f, const uint8_t *rq, uint32_t rqn, uint32_t *x)
{
	uint8_t b[64];
	if (f->len > 4 || fref_get(f, rq, rqn, 0, b))
		return -1;
	*x = 0;
	for (int i = 0; i < f->len; i++)
		*x |= (uint32_t)b[i] << (8 * i);
	return 0;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/*
 * expand template h: the bytes (length) into out; -1 bad template / no room, -2 a request field is missing.
 * dummy: every placeholder as if known (load-time check). *missing: an ICCID/IMSI TLV was left out (not known yet).
 */
static int tmpl_expand(const char *h, const uint8_t *rq, uint32_t rqn, int dummy, uint8_t *out, uint32_t room, int *missing)
{
	uint32_t o = 0;
	int hi = -1;
	*missing = 0;
	for (; *h; h++) {
		if (*h == '.' || *h == '_' || *h == ':')
			continue;
		if (*h == '{') {
			const char *e = strchr(h, '}');
			if (!e || hi >= 0)
				return -1;
			size_t nl = (size_t)(e - h - 1);
			if ((nl == 5 && !strncmp(h + 1, "ICCID", 5)) || (nl == 4 && !strncmp(h + 1, "IMSI", 4))) {
				int ic = nl == 5;
				uint32_t l = ic ? SIM_ICCID_LEN : SIM_IMSI_LEN;
				if (dummy || (ic ? simid.have_iccid : simid.have_imsi)) {
					if (o + 2 + l > room)
						return -1;
					out[o++] = (uint8_t)l;
					out[o++] = 0;
					if (dummy)
						memset(out + o, 0xff, l);
					else
						memcpy(out + o, ic ? simid.iccid : simid.imsi, l);
					o += l;
				} else {
					if (!o)
						return -1;
					o--;		/* the TLV id: the whole TLV is left out */
					*missing = 1;
				}
			} else {
				struct fref f;
				if (parse_fref(h + 1, &f) || o + (uint32_t)f.len > room)
					return -1;
				if (fref_get(&f, rq, rqn, dummy, out + o))
					return -2;
				o += (uint32_t)f.len;
			}
			h = e;
			continue;
		}
		int v = hexval(*h);
		if (v < 0)
			return -1;
		if (hi < 0) {
			hi = v;
			continue;
		}
		if (o >= room)
			return -1;
		out[o++] = (uint8_t)(hi << 4 | v);
		hi = -1;
	}
	return hi >= 0 ? -1 : (int)o;
}

static int tmpl_sim(const char *h)
{
	return h && (strstr(h, "{ICCID}") || strstr(h, "{IMSI}"));
}

/* a template into pol_text, checked (dummy expansion must be whole TLVs); NULL if bad */
static const char *parse_tmpl(const char *h)
{
	uint8_t b[4096];
	int miss, n = tmpl_expand(h, NULL, 0, 1, b, sizeof(b), &miss);
	size_t l = strlen(h);
	if (n < 0 || !tlvs_ok(b, (uint32_t)n) || pol_tlen + l + 1 > sizeof(pol_text))
		return NULL;
	char *t = pol_text + pol_tlen;
	memcpy(t, h, l + 1);
	pol_tlen += (uint32_t)l + 1;
	return t;
}

/* "on svc msg ind <msg> <hex>" / "on svc msg replay <get> <ind> [map a=b,c=d]"; 0 ok */
static int parse_trigger(char *line, int ln)
{
	char w[7][256];
	struct trg t;
	memset(&t, 0, sizeof(t));
	t.cond = -1;
	/* P2 reg: optional trailing "if <tlv>" (cut off before the rest is parsed) */
	char *iff = strstr(line, " if ");
	if (iff) {
		unsigned c;
		char *eq = strchr(iff + 4, '=');
		while (iff[4] == ' ')
			iff++;
		if (eq) {		/* n12b: "if RQ..=RQ.." */
			if (parse_fref(iff + 4, &t.fa) || parse_fref(eq + 1, &t.fb) || t.fa.len > 4 || t.fb.len > 4)
				goto bad;
			t.ceq = 1;
		} else {
			if (sscanf(iff + 4, "%x", &c) != 1 || c > 255)
				goto bad;
			t.cond = (int)c;
		}
		*iff = 0;
	}
	int k = sscanf(line, "%255s %255s %255s %255s %255s %255s %255s", w[0], w[1], w[2], w[3], w[4], w[5], w[6]);
	int a, b;
	if (k < 5 || ntrg >= NTRG || parse_num(w[1], &t.svc) || t.svc < 1 || t.svc > 255 || parse_num(w[2], &t.msg) ||
	    parse_num(w[4], &a) || a < 0 || a > 0xffff)
		goto bad;
	t.m2 = (uint16_t)a;
	if (!strcmp(w[3], "ind")) {
		t.kind = TRG_IND;
		if (k >= 6 && strchr(w[5], '{')) {
			t.tmpl = parse_tmpl(w[5]);	/* n12b */
			if (!t.tmpl)
				goto bad;
		} else if (k >= 6) {
			t.data = parse_hex(w[5], &t.dlen);
			if (!t.data || !tlvs_ok(t.data, t.dlen))
				goto bad;
		}
	} else if (!strcmp(w[3], "replay")) {
		t.kind = TRG_REPLAY;
		if (k < 6 || parse_num(w[5], &b) || b < 0 || b > 0xffff)
			goto bad;
		t.m3 = (uint16_t)b;
		if (k >= 7 && strncmp(w[6], "map", 3))
			goto bad;
		if (k >= 7) {
			/* "map" then the pairs in the rest of the line */
			char *m = strstr(line, "map");
			m += 3;
			while (*m && t.nmap < TRG_MAXMAP) {
				unsigned x, y;
				while (*m == ' ' || *m == '\t' || *m == ',')
					m++;
				if (!*m || *m == '\n' || *m == '\r')
					break;
				if (sscanf(m, "%x=%x", &x, &y) != 2 || x > 255 || y > 255)
					goto bad;
				t.map[t.nmap][0] = (uint8_t)x;
				t.map[t.nmap][1] = (uint8_t)y;
				t.nmap++;
				while (*m && *m != ',' && *m != ' ' && *m != '\t')
					m++;
			}
		}
	} else {
		goto bad;
	}
	trg[ntrg++] = t;
	return 0;
bad:
	printf("abp: %s line %d: bad 'on' line (on <svc> <msg> ind <msg> [<hex>] | on <svc> <msg> replay <get> <ind> [map a=b,..]; optional ' if <tlv>' at the end)\n",
	       ABP_POLICY, ln);
	return -1;
}

static void policy_load(int force)
{
	struct stat sb;
	if (stat(ABP_POLICY, &sb)) {
		if (novr || ovr_mtime) {
			printf("[%6llu] abp: %s gone: built-in policy only\n", (unsigned long long)link_ms(), ABP_POLICY);
			novr = 0;
			ntrg = 0;
			nstrip = 0;
			pol_dlen = 0;
			ovr_mtime = 0;
			nseen = 0;	/* decisions are logged again */
		}
		return;
	}
	if (!force && sb.st_mtime == ovr_mtime && sb.st_size == ovr_size)
		return;
	ovr_mtime = sb.st_mtime;
	ovr_size = sb.st_size;
	FILE *f = fopen(ABP_POLICY, "r");
	if (!f)
		return;
	static char line[12288];
	int n = 0, bad = 0, ln = 0;
	ntrg = 0;
	nstrip = 0;
	pol_dlen = 0;
	pol_tlen = 0;
	while (fgets(line, sizeof(line), f) && n < NOVR) {
		char a[64], b[64], c[8400];
		ln++;
		char *h = strchr(line, '#');
		if (h)
			*h = 0;
		if (sscanf(line, "%63s", a) == 1 && !strcmp(a, "on")) {
			bad += parse_trigger(line, ln) != 0;
			continue;
		}
		if (sscanf(line, "%63s", a) == 1 && !strcmp(a, "strip")) {
			bad += parse_strip(line, ln) != 0;
			continue;
		}
		if (sscanf(line, "%63s %63s %8399s", a, b, c) != 3)
			continue;
		struct pol p = { 0, POL_ANY, ACT_ERR, POL_ABPERR, "conf" };
		const uint8_t *pdata = NULL;
		const char *ptmpl = NULL;
		uint16_t pdlen = 0;
		if (parse_num(a, &p.svc) || p.svc < 1 || p.svc > 255 || (strcmp(b, "*") && parse_num(b, &p.msg))) {
			printf("abp: %s line %d: bad svc/msg\n", ABP_POLICY, ln);
			bad++;
			continue;
		}
		if (!strcmp(c, "fwd"))
			p.act = ACT_FWD;
		else if (!strcmp(c, "ok"))
			p.act = ACT_OK;
		else if (!strcmp(c, "drop"))
			p.act = ACT_DROP;
		else if (!strncmp(c, "err:", 4) && !parse_num(c + 4, &p.err))
			p.act = ACT_ERR;
		else if (!strcmp(c, "err"))
			p.act = ACT_ERR;
		else if (!strncmp(c, "resp:", 5) && strchr(c + 5, '{') && (ptmpl = parse_tmpl(c + 5)) != NULL)
			p.act = ACT_RESP;	/* n12b: template */
		else if (!strncmp(c, "resp:", 5) && (pdata = parse_hex(c + 5, &pdlen)) != NULL && tlvs_ok(pdata, pdlen))
			p.act = ACT_RESP;
		else {
			printf("abp: %s line %d: bad action '%.40s' (fwd | ok | err:<code> | drop | resp:<hex TLVs>)\n", ABP_POLICY, ln, c);
			bad++;
			continue;
		}
		ovr_data[n].data = pdata;
		ovr_data[n].dlen = pdlen;
		ovr_data[n].tmpl = ptmpl;
		ovr[n++] = p;
	}
	fclose(f);
	novr = n;
	nseen = 0;
	printf("[%6llu] abp: policy %s: %d override line(s), %d trigger line(s), %d strip line(s)%s, decisions logged again\n", (unsigned long long)link_ms(),
	       ABP_POLICY, n, ntrg, nstrip, bad ? " (some lines refused)" : "");
}

static const uint8_t *dec_data;	/* ACT_RESP: the TLVs of the last decide() */
static uint16_t dec_dlen;
static const char *dec_tmpl;	/* n12b: ACT_RESP template */

/* the action for (svc, msg); *err = the error code, *why = the reason (for the log) */
static int decide(unsigned svc, unsigned msg, int *err, const char **why, char *whybuf, size_t wl)
{
	const struct pol *p = NULL;
	int a, from_conf = 0;
	*err = err_code;
	for (int i = 0; i < novr && !p; i++)
		if (ovr[i].svc == (int)svc && ovr[i].msg == (int)msg)
			p = &ovr[i];
	for (int i = 0; i < novr && !p; i++)
		if (ovr[i].svc == (int)svc && ovr[i].msg == POL_ANY)
			p = &ovr[i];
	from_conf = p != NULL;
	for (size_t i = 0; i < sizeof(abp_builtin) / sizeof(abp_builtin[0]) && !p; i++)
		if (abp_builtin[i].svc == (int)svc && abp_builtin[i].msg == (int)msg)
			p = &abp_builtin[i];
	for (size_t i = 0; i < sizeof(abp_builtin) / sizeof(abp_builtin[0]) && !p; i++)
		if (abp_builtin[i].svc == (int)svc && abp_builtin[i].msg == POL_ANY)
			p = &abp_builtin[i];
	dec_data = NULL;
	dec_dlen = 0;
	dec_tmpl = NULL;
	if (p) {
		a = p->act;
		if (from_conf) {
			dec_data = ovr_data[p - ovr].data;
			dec_dlen = ovr_data[p - ovr].dlen;
			dec_tmpl = ovr_data[p - ovr].tmpl;
		}
		if (p->err != POL_ABPERR)
			*err = p->err;
		snprintf(whybuf, wl, "%s%s", from_conf ? "conf " : "", p->why);
	} else if (svc >= 0xe0) {
		a = ACT_ERR;
		snprintf(whybuf, wl, "Apple-only service");
	} else {
		a = ACT_FWD;
		snprintf(whybuf, wl, "standard service");
	}
	if (a == ACT_FWD && svc >= 0xe0) {
		a = ACT_ERR;
		*err = err_code;
		snprintf(whybuf, wl, "Apple-only service: never forwarded");
	} else if (a == ACT_FWD && msg >= 0x5500) {
		a = ACT_ERR;
		*err = err_code;
		snprintf(whybuf, wl, "Apple message id: never forwarded");
	} else if (a == ACT_FWD && srv_find(svc) < 0) {
		a = ACT_ERR;
		*err = err_code;
		snprintf(whybuf, wl, "no modem server for svc 0x%02x", svc);
	}
	*why = whybuf;
	return a;
}

static double secs(void)
{
	return (double)(link_ms() - t0) / 1000.0;
}

static void hexline(char *out, size_t outlen, const uint8_t *p, uint32_t n)
{
	size_t o = 0;
	for (uint32_t i = 0; i < n && o + 4 < outlen; i++)
		o += (size_t)snprintf(out + o, outlen - o, "%02x%s", p[i], (i + 1) % 4 == 0 && i + 1 < n ? " " : "");
	out[o] = 0;
}

/* the TLVs of an SDU, "t[len]=hex ..." (values cut at 16 bytes) */
static void tlvs(char *out, size_t outlen, const uint8_t *p, uint32_t n)
{
	size_t o = 0;
	out[0] = 0;
	while (n >= 3 && o + 8 < outlen) {
		unsigned t = p[0], l = p[1] | (p[2] << 8);
		if (l > n - 3) {
			o += (size_t)snprintf(out + o, outlen - o, " BAD-TLV");
			break;
		}
		char h[64];
		hexline(h, sizeof(h), p + 3, l < 16 ? l : 16);
		o += (size_t)snprintf(out + o, outlen - o, " %02x[%u]=%s%s", t, l, h, l > 16 ? ".." : "");
		p += 3 + l;
		n -= 3 + l;
	}
}

/* first time (svc, msg) is seen from CommCenter: one line with the action taken */
static void note_seen(unsigned svc, unsigned msg, const char *act, const char *why)
{
	for (unsigned i = 0; i < nseen; i++)
		if (seen[i].svc == svc && seen[i].msg == msg)
			return;
	if (nseen < sizeof(seen) / sizeof(seen[0])) {
		seen[nseen].svc = (uint8_t)svc;
		seen[nseen].msg = (uint16_t)msg;
		nseen++;
	}
	printf("[%6llu] abp: NEW #%u t+%.1fs %s/%u svc 0x%02x %s msg 0x%04x%s%s -> %s (%s)\n", (unsigned long long)link_ms(), nseen,
	       secs(), pipe_name[cur_pipe], cur_sess, svc, svc_name(svc), msg, svc == 0 ? " " : "", svc == 0 ? ctl_name(msg) : "",
	       act, why);
}

/* log one frame (abplog=1 or mode 1); dir "CC->" (CommCenter wrote it) or "->CC" (our answer / the modem's) */
static void log_frame(unsigned pipe, const char *dir, const uint8_t *f, uint32_t n)
{
	if (!logframes)
		return;
	char h[3 * 96 + 8], t[600];
	char pn[24];
	snprintf(pn, sizeof(pn), "%s/%u", pipe < NPIPES ? pipe_name[pipe] : "?", cur_sess);
	hexline(h, sizeof(h), f, n < 80 ? n : 80);
	if (lf) {
		static char full[3 * 2048];
		hexline(full, sizeof(full), f, n < 1500 ? n : 1500);
		fprintf(lf, "%.3f %s %s len %u: %s\n", secs(), pn, dir, n, full);
		fflush(lf);
	}
	if (n >= 3 && f[0] == 0xa5 && f[1] == 0xef && f[2] == 0xbe) {
		printf("[%6llu] abp: %s %s len %u MAV_MUX header (A5 EF BE): %s\n", (unsigned long long)link_ms(), pn, dir, n, h);
		return;
	}
	if (n < 6 || f[0] != 0x01) {
		printf("[%6llu] abp: %s %s len %u NOT QMUX: %s\n", (unsigned long long)link_ms(), pn, dir, n, h);
		return;
	}
	unsigned qlen = f[1] | (f[2] << 8), flags = f[3], svc = f[4], cid = f[5];
	const uint8_t *s = f + 6;
	uint32_t sn = n - 6;
	if (svc == 0 && sn >= 6) {
		unsigned sf = s[0], tx = s[1], msg = s[2] | (s[3] << 8), ml = s[4] | (s[5] << 8);
		tlvs(t, sizeof(t), s + 6, sn - 6 < ml ? sn - 6 : ml);
		printf("[%6llu] abp: %s %s CTL %s(0x%02x) %s tx %u len %u qlen %u qflags 0x%02x:%s\n", (unsigned long long)link_ms(), pn,
		       dir, ctl_name(msg), msg, sf == 0 ? "req" : sf == 1 ? "resp" : sf == 2 ? "ind" : "?", tx, n, qlen, flags, t);
	} else if (sn >= 7) {
		unsigned sf = s[0], tx = s[1] | (s[2] << 8), msg = s[3] | (s[4] << 8), ml = s[5] | (s[6] << 8);
		tlvs(t, sizeof(t), s + 7, sn - 7 < ml ? sn - 7 : ml);
		printf("[%6llu] abp: %s %s %s(0x%02x) cid %u msg 0x%04x %s tx %u len %u qflags 0x%02x:%s\n", (unsigned long long)link_ms(),
		       pn, dir, svc_name(svc), svc, cid, msg, sf == 0 ? "req" : sf == 2 ? "resp" : sf == 4 ? "ind" : "?", tx, n, flags, t);
	} else {
		printf("[%6llu] abp: %s %s short QMUX len %u: %s\n", (unsigned long long)link_ms(), pn, dir, n, h);
	}
}

/* one frame to (pipe, sess) */
static int send_to(unsigned pipe, uint32_t sess, const uint8_t *f, uint32_t n)
{
	static uint8_t b[sizeof(struct abpb_hdr) + ABP_MAXFRAME];
	struct abpb_hdr h = { ABPB_MAGIC, (uint8_t)pipe, 0, sess, n };
	if (fd < 0 || n > ABP_MAXFRAME)
		return -1;
	memcpy(b, &h, sizeof(h));
	memcpy(b + sizeof(h), f, n);
	uint32_t ps = cur_sess;
	cur_sess = sess;
	log_frame(pipe, "->CC", f, n);
	cur_sess = ps;
	nframes_out++;
	if (write(fd, b, sizeof(h) + n) != (ssize_t)(sizeof(h) + n)) {
		static unsigned told;
		if (told++ < 20)
			printf("[%6llu] abp: write to %s (%s/%u, %u bytes) failed: %s\n", (unsigned long long)link_ms(), ABP_DEV,
			       pipe < NPIPES ? pipe_name[pipe] : "?", sess, n, strerror(errno));
		return -1;
	}
	return 0;
}

static int send_frame(unsigned pipe, const uint8_t *f, uint32_t n)
{
	return send_to(pipe, cur_sess, f, n);
}

/* append TLV t to buf at *o */
static void put_tlv(uint8_t *b, uint32_t *o, unsigned t, const void *v, unsigned l)
{
	b[(*o)++] = (uint8_t)t;
	b[(*o)++] = (uint8_t)l;
	b[(*o)++] = (uint8_t)(l >> 8);
	memcpy(b + *o, v, l);
	*o += l;
}

static void finish_qmux(uint8_t *b, uint32_t n, unsigned svc, unsigned cid)
{
	b[0] = 0x01;
	b[1] = (uint8_t)((n - 1) & 0xff);
	b[2] = (uint8_t)((n - 1) >> 8);
	b[3] = 0x80;			/* from the service */
	b[4] = (uint8_t)svc;
	b[5] = (uint8_t)cid;
}

/* ---- router clients ---- */

static void pend_drop_port(uint32_t port)
{
	for (int i = 0; i < NPEND; i++)
		if (pend[i].used && pend[i].port == port)
			pend[i].used = 0;	/* its answer, if it comes, counts as late */
}

/* really give the port back: DEL_CLIENT to the modem */
static void cl_destroy(struct cl *c, const char *why)
{
	if (!c->used)
		return;
	pend_drop_port(c->port);
	/* tell the modem the client is gone (Linux qrtr_port_remove: DEL_CLIENT from that port to the control port) */
	uint32_t pkt[5] = { QRTR_TYPE_DEL_CLIENT, QRTR_NODE_APPS, c->port, 0, 0 };
	qrtr_send(QRTR_TYPE_DEL_CLIENT, c->port, QRTR_NODE_BCAST, QRTR_PORT_CTRL, pkt, sizeof(pkt));
	printf("[%6llu] abp: %s(0x%02x) port %#x given back to the modem (DEL_CLIENT; %s)\n", (unsigned long long)link_ms(),
	       svc_name(c->svc), c->svc, c->port, why);
	memset(c, 0, sizeof(*c));
}

/* a CommCenter client ends: pooled (abpreuse=1) or destroyed */
static void cl_free(struct cl *c, const char *why)
{
	if (c->used != 1)
		return;
	if (logframes || !reuse)
		printf("[%6llu] abp: %s/%u %s(0x%02x) cid %u port %#x released (%s; %u fwd, %u resp, %u ind)%s\n", (unsigned long long)link_ms(),
		       pipe_name[c->pipe], c->sess, svc_name(c->svc), c->svc, c->cid, c->port, why, c->fwd, c->resp, c->ind,
		       reuse ? " -> idle pool" : "");
	if (!reuse) {
		cl_destroy(c, why);
		return;
	}
	pend_drop_port(c->port);
	c->used = 2;
	c->pipe = c->sess = 0;
	c->cid = 0;
}

static struct cl *cl_get(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid, int create)
{
	struct cl *fr = NULL, *idle = NULL, *anyidle = NULL;
	for (int i = 0; i < NCL; i++) {
		if (cl[i].used == 1 && cl[i].pipe == pipe && cl[i].sess == sess && cl[i].svc == svc && cl[i].cid == cid)
			return &cl[i];
		if (!cl[i].used && !fr)
			fr = &cl[i];
		if (cl[i].used == 2 && cl[i].svc == svc && !idle)
			idle = &cl[i];
		if (cl[i].used == 2 && !anyidle)
			anyidle = &cl[i];
	}
	if (!create)
		return NULL;
	if (idle && reuse) {		/* the pooled modem client of this service gets the new owner */
		idle->used = 1;
		idle->pipe = pipe;
		idle->sess = sess;
		idle->cid = (uint8_t)cid;
		idle->fwd = idle->resp = idle->ind = 0;
		if (logframes)
			printf("[%6llu] abp: %s/%u %s(0x%02x) cid %u -> pooled QRTR port %#x\n", (unsigned long long)link_ms(),
			       pipe_name[pipe], sess, svc_name(svc), svc, cid, idle->port);
		return idle;
	}
	if (!fr && anyidle) {
		cl_destroy(anyidle, "table full: oldest idle of another service");
		fr = anyidle;
	}
	if (!fr) {
		static unsigned told;
		if (told++ < 5)
			printf("[%6llu] abp: all %d router clients in use: request refused\n", (unsigned long long)link_ms(), NCL);
		return NULL;
	}
	/* ports rotate through ABP_PORT0 .. +ABP_NPORTS so a just-released port is not reused at once */
	uint32_t port = 0;
	for (unsigned k = 0; k < ABP_NPORTS && !port; k++) {
		uint32_t p = ABP_PORT0 + (next_port_gen++ % ABP_NPORTS);
		int busy = 0;
		for (int i = 0; i < NCL; i++)
			busy |= cl[i].used && cl[i].port == p;	/* idle ports stay ours too */
		if (!busy)
			port = p;
	}
	if (!port)
		return NULL;
	pend_drop_port(port);
	memset(fr, 0, sizeof(*fr));
	fr->used = 1;
	fr->pipe = pipe;
	fr->sess = sess;
	fr->svc = (uint8_t)svc;
	fr->cid = (uint8_t)cid;
	fr->port = port;
	int s = srv_find(svc);
	printf("[%6llu] abp: %s/%u %s(0x%02x) cid %u -> QRTR port %#x -> modem %u:%x\n", (unsigned long long)link_ms(), pipe_name[pipe],
	       sess, svc_name(svc), svc, cid, port, s >= 0 ? srv[s].node : 0, s >= 0 ? srv[s].port : 0);
	return fr;
}

/* only router clients (ports ABP_PORT0..) of that CommCenter session: wland's own QMI clients are never touched */
static void cl_free_sess(uint32_t pipe, uint32_t sess, const char *why)
{
	unsigned n = 0;
	for (int i = 0; i < NCL; i++)
		if (cl[i].used == 1 && cl[i].pipe == pipe && cl[i].sess == sess) {
			cl_free(&cl[i], why);
			n++;
		}
	if (n && !logframes && reuse)
		printf("[%6llu] abp: %s/%u: %u router client(s) to the idle pool (%s)\n", (unsigned long long)link_ms(),
		       pipe < NPIPES ? pipe_name[pipe] : "?", sess, n, why);
}

static void cl_free_all(const char *why)
{
	for (int i = 0; i < NCL; i++)
		cl_destroy(&cl[i], why);
}

/* ---- CTL, per session ---- */

/* the client-id table of (pipe, sess); a free or the oldest slot is taken for a new session */
static uint8_t (*sess_cids(uint32_t pipe, uint32_t sess))[32]
{
	unsigned best = 0;
	for (unsigned i = 0; i < NSESS; i++) {
		if (st[i].sess == sess && st[i].pipe == pipe)
			return st[i].cids;
		if (st[i].sess < st[best].sess)
			best = i;
	}
	if (st[best].sess)
		cl_free_sess(st[best].pipe, st[best].sess, "session slot reused");
	memset(&st[best], 0, sizeof(st[best]));
	st[best].pipe = pipe;
	st[best].sess = sess;
	return st[best].cids;
}

static void sess_end(uint32_t pipe, uint32_t sess)
{
	for (unsigned i = 0; i < NSESS; i++)
		if (st[i].sess == sess && st[i].pipe == pipe)
			memset(&st[i], 0, sizeof(st[i]));
	cl_free_sess(pipe, sess, "session closed");
}

static int cid_alloc(unsigned svc)
{
	uint8_t (*cids)[32] = sess_cids(cur_pipe, cur_sess);
	for (unsigned c = 1; c < 255; c++)
		if (!(cids[svc][c / 8] & (1u << (c % 8)))) {
			cids[svc][c / 8] |= (uint8_t)(1u << (c % 8));
			return (int)c;
		}
	return -1;
}

/* GetVersionInfo list: the modem's standard services on QRTR (router), else a fixed SM7250 list */
static uint32_t version_list(uint8_t *v, uint32_t max)
{
	static const uint8_t fixed[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x09, 0x0a, 0x0b, 0x0c, 0x1a, 0x22, 0x24, 0x2a, 0x2b, 0x30 };
	uint8_t list[128];
	unsigned n = 0;
	if (mode >= 2) {
		list[n++] = 0x00;
		for (unsigned s = 1; s < 0xe0 && n < sizeof(list); s++)
			if (srv_find(s) >= 0)
				list[n++] = (uint8_t)s;
	}
	if (n <= 1) {
		memcpy(list, fixed, sizeof(fixed));
		n = sizeof(fixed);
	}
	if (1 + n * 5 > max)
		n = (max - 1) / 5;
	v[0] = (uint8_t)n;
	for (unsigned i = 0; i < n; i++) {
		v[1 + i * 5] = list[i];
		v[2 + i * 5] = 1;	/* major 1 */
		v[3 + i * 5] = 0;
		v[4 + i * 5] = 0x40;	/* minor 64 */
		v[5 + i * 5] = 0;
	}
	return 1 + n * 5;
}

/* CTL request: answered here */
static void ctl_answer(unsigned pipe, const uint8_t *s, uint32_t sn)
{
	unsigned tx = s[1], msg = s[2] | (s[3] << 8), ml = s[4] | (s[5] << 8);
	const uint8_t *tl = s + 6;
	uint32_t tn = sn - 6 < ml ? sn - 6 : ml;
	uint8_t b[1024];
	uint32_t o = 12;		/* QMUX 6 + CTL SDU header 6 */
	uint16_t res[2] = { 0, 0 };	/* result success, error none */
	int req_svc = -1;
	note_seen(0, msg, "local", "CTL");
	for (uint32_t i = 0; i + 3 <= tn;) {	/* TLV 0x01 = service (GetClientId) or {svc, cid} (ReleaseClientId) */
		unsigned t = tl[i], l = tl[i + 1] | (tl[i + 2] << 8);
		if (t == 0x01 && l >= 1 && i + 3 + l <= tn)
			req_svc = tl[i + 3];
		if (t == 0x01 && msg == 0x23 && l >= 2 && i + 3 + l <= tn) {
			unsigned sv = tl[i + 3], c = tl[i + 4];
			sess_cids(cur_pipe, cur_sess)[sv][c / 8] &= (uint8_t) ~(1u << (c % 8));
			struct cl *k = cl_get(cur_pipe, cur_sess, sv, c, 0);
			if (k)
				cl_free(k, "ReleaseClientId");
		}
		i += 3 + l;
	}
	if (msg == 0x22) {		/* GetClientId */
		int c = -1, e = 0;
		if (req_svc > 0 && req_svc < 0xe0 && mode >= 2 && cidfail && srv_find((unsigned)req_svc) < 0)
			e = QMI_ERR_INVALID_SERVICE_TYPE;	/* abpcidfail=1: as a QMUX without that service would */
		else
			c = req_svc >= 0 ? cid_alloc((unsigned)req_svc) : -1;
		if (c < 0) {
			res[0] = 1;
			res[1] = (uint16_t)(e ? e : QMI_ERR_CLIENT_IDS_EXHAUSTED);
			put_tlv(b, &o, 0x02, res, 4);
			printf("[%6llu] abp: %s/%u GetClientId %s(0x%02x) refused (error 0x%x)\n", (unsigned long long)link_ms(),
			       pipe_name[pipe], cur_sess, svc_name((unsigned)req_svc), req_svc, res[1]);
		} else {
			put_tlv(b, &o, 0x02, res, 4);
			uint8_t v[2] = { (uint8_t)req_svc, (uint8_t)c };
			put_tlv(b, &o, 0x01, v, 2);
			if (logframes || mode < 2)
				printf("[%6llu] abp: %s/%u gave %s(0x%02x) client id %d\n", (unsigned long long)link_ms(), pipe_name[pipe],
				       cur_sess, svc_name((unsigned)req_svc), req_svc, c);
		}
	} else if (msg == 0x23) {	/* ReleaseClientId: echo {svc, cid} */
		put_tlv(b, &o, 0x02, res, 4);
		for (uint32_t i = 0; i + 3 <= tn;) {
			unsigned t = tl[i], l = tl[i + 1] | (tl[i + 2] << 8);
			if (t == 0x01 && l == 2 && i + 5 <= tn)
				put_tlv(b, &o, 0x01, tl + i + 3, 2);
			i += 3 + l;
		}
	} else if (msg == 0x21) {	/* GetVersionInfo */
		uint8_t v[1 + 128 * 5];
		uint32_t vl = version_list(v, sizeof(b) - 40 < sizeof(v) ? (uint32_t)(sizeof(b) - 40) : (uint32_t)sizeof(v));
		put_tlv(b, &o, 0x02, res, 4);
		put_tlv(b, &o, 0x01, v, vl);
	} else if (msg == 0x26) {	/* SetDataFormat: echo the link protocol TLV 0x10 if any */
		put_tlv(b, &o, 0x02, res, 4);
		for (uint32_t i = 0; i + 3 <= tn;) {
			unsigned t = tl[i], l = tl[i + 1] | (tl[i + 2] << 8);
			if (t == 0x10 && i + 3 + l <= tn && l <= 4)
				put_tlv(b, &o, 0x10, tl + i + 3, l);
			i += 3 + l;
		}
	} else if (msg == 0x27) {	/* Sync: a QMUX releases every client id of the stream */
		put_tlv(b, &o, 0x02, res, 4);
		for (unsigned i = 0; i < NSESS; i++)
			if (st[i].sess == cur_sess && st[i].pipe == cur_pipe)
				memset(st[i].cids, 0, sizeof(st[i].cids));
		cl_free_sess(cur_pipe, cur_sess, "CTL Sync");
	} else {			/* power save, instance id, anything else: success */
		put_tlv(b, &o, 0x02, res, 4);
	}
	b[6] = 0x01;			/* response */
	b[7] = (uint8_t)tx;
	b[8] = (uint8_t)msg;
	b[9] = (uint8_t)(msg >> 8);
	b[10] = (uint8_t)((o - 12) & 0xff);
	b[11] = (uint8_t)((o - 12) >> 8);
	finish_qmux(b, o, 0, 0);
	send_frame(pipe, b, o);
}

/* a request answered here: success without TLVs (ok), success with the policy's TLVs (resp) or a QMI failure (err) */
static void svc_local_tlv(unsigned pipe, unsigned svc, unsigned cid, const uint8_t *s, int fail, int code, const uint8_t *tl,
			  uint16_t tn)
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
	static uint8_t b[32 + 4096];
	uint32_t o = 13;		/* QMUX 6 + SDU header 7 */
	uint16_t res[2] = { (uint16_t)(fail ? 1 : 0), (uint16_t)(fail ? code : 0) };
	put_tlv(b, &o, 0x02, res, 4);
	if (!fail && tl && tn && o + tn <= sizeof(b)) {
		memcpy(b + o, tl, tn);
		o += tn;
	}
	b[6] = 0x02;			/* response */
	b[7] = s[1];			/* txid u16 */
	b[8] = s[2];
	b[9] = s[3];			/* msg id */
	b[10] = s[4];
	b[11] = (uint8_t)((o - 13) & 0xff);
	b[12] = (uint8_t)((o - 13) >> 8);
	finish_qmux(b, o, svc, cid);
	send_frame(pipe, b, o);
}

static void svc_local(unsigned pipe, unsigned svc, unsigned cid, const uint8_t *s, int fail, int code)
{
	svc_local_tlv(pipe, svc, cid, s, fail, code, NULL, 0);
}

/* an indication (svc, msg, TLVs) to one CommCenter client */
static void send_ind(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid, unsigned msg, const uint8_t *tl, uint32_t tn)
{
	static uint8_t b[16 + 65536];
	static uint16_t itx = 1;
	if (13 + tn > sizeof(b))
		return;
	b[6] = 0x04;			/* indication */
	b[7] = (uint8_t)itx;
	b[8] = (uint8_t)(itx >> 8);
	itx = (uint16_t)(itx + 1 ? itx + 1 : 1);
	b[9] = (uint8_t)msg;
	b[10] = (uint8_t)(msg >> 8);
	b[11] = (uint8_t)(tn & 0xff);
	b[12] = (uint8_t)(tn >> 8);
	if (tn)
		memcpy(b + 13, tl, tn);
	finish_qmux(b, 13 + tn, svc, cid);
	uint32_t pp = cur_pipe;
	send_to(pipe, sess, b, 13 + tn);
	cur_pipe = pp;
}

static void pend_add(struct pend *x)
{
	int slot = -1;
	uint64_t oldest = ~0ull;
	for (int i = 0; i < NPEND; i++) {
		if (!pend[i].used) {
			slot = i;
			break;
		}
		if (pend[i].t < oldest) {
			oldest = pend[i].t;
			slot = i;
		}
	}
	pend[slot] = *x;
	pend[slot].used = 1;
	pend[slot].t = link_ms();
}

static struct cl *cl_get(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid, int create);

/*
 * r3: the 'on' lines for (svc, msg) after a successful answer went to CommCenter client (pipe, sess, svc, cid):
 * synthesized indications and state replays (GET to the modem, answer re-wrapped as an indication).
 */
/* the first byte of TLV id in a TLV list; -1 if absent or empty */
static int tlv_byte(const uint8_t *p, uint32_t n, unsigned id)
{
	for (uint32_t k = 0; k + 3 <= n;) {
		unsigned l = (unsigned)(p[k + 1] | (p[k + 2] << 8));
		if (k + 3 + l > n)
			return -1;
		if (p[k] == id)
			return l ? p[k + 3] : -1;
		k += 3 + l;
	}
	return -1;
}

/*
 * n12b: the SIM identity for {ICCID}/{IMSI} templates, read from the card through the modem as an Apple baseband does
 * itself: CommCenter's own UIM Read Transparent answers (EF_ICCID 0x2fe2, EF_IMSI 0x6f07) are looked at on their way back,
 * and while an indication waits for them (or after a UIM status change indication) the router reads both files itself
 * (UIM 0x0020 from a CommCenter UIM client's port, read-only, every 5 s while needed). Indications that used them are
 * remembered per CommCenter client and sent again when the identity changes (another SIM). Values are never logged.
 */
#define NSUB	8
static struct sub {
	int used, pending;
	uint32_t pipe, sess;
	uint8_t svc, cid;
	uint16_t msg, m2;
	uint8_t rqn, rq[96];
	uint64_t t;
} subs[NSUB];

/* is client id cid of svc held by CommCenter session (pipe, sess)? (no session slot is taken) */
static int cid_live(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid)
{
	for (unsigned i = 0; i < NSESS; i++)
		if (st[i].sess && st[i].sess == sess && st[i].pipe == pipe)
			return (st[i].cids[svc & 0xff][(cid & 0xff) / 8] >> (cid % 8)) & 1;
	return 0;
}

static void sub_note(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid, unsigned msg, unsigned m2, const uint8_t *rq,
		     uint32_t rqn, int pending)
{
	int k = -1;
	for (int i = 0; i < NSUB; i++)
		if (subs[i].used && subs[i].pipe == pipe && subs[i].sess == sess && subs[i].svc == svc && subs[i].cid == cid &&
		    subs[i].msg == msg && subs[i].m2 == m2)
			k = i;
	for (int i = 0; i < NSUB && k < 0; i++)
		if (!subs[i].used || !cid_live(subs[i].pipe, subs[i].sess, subs[i].svc, subs[i].cid))
			k = i;
	if (k < 0) {
		k = 0;
		for (int i = 1; i < NSUB; i++)
			if (subs[i].t < subs[k].t)
				k = i;
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
	if (rq && s->rqn)
		memcpy(s->rq, rq, s->rqn);
	s->t = link_ms();
}

/* an 'on .. ind' line's indication (data or template) to one client; 0 sent, 1 deferred (SIM identity unknown), -1 not sent */
static int trg_ind(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid, unsigned msg, const struct trg *t, const uint8_t *rq,
		   uint32_t rqn, const char *why)
{
	if (!t->tmpl) {
		synth_inds++;
		printf("[%6llu] abp: on %s(0x%02x) 0x%04x: indication 0x%04x (%u TLV bytes) to %s/%u cid %u\n", (unsigned long long)link_ms(),
		       svc_name(svc), svc, msg, t->m2, t->dlen, pipe_name[pipe], sess, cid);
		send_ind(pipe, sess, svc, cid, t->m2, t->data, t->dlen);
		return 0;
	}
	static uint8_t b[4096];
	int miss = 0, n = tmpl_expand(t->tmpl, rq, rqn, 0, b, sizeof(b), &miss);
	if (n < 0 || !tlvs_ok(b, (uint32_t)n)) {
		printf("[%6llu] abp: on %s(0x%02x) 0x%04x: indication 0x%04x not sent (%s)\n", (unsigned long long)link_ms(), svc_name(svc), svc,
		       msg, t->m2, n == -2 ? "the request lacks a template field" : "template does not expand to whole TLVs");
		return -1;
	}
	if (tmpl_sim(t->tmpl))
		sub_note(pipe, sess, svc, cid, msg, t->m2, rq, rqn, miss);
	if (miss) {
		printf("[%6llu] abp: on %s(0x%02x) 0x%04x: indication 0x%04x to %s/%u cid %u deferred: SIM identity not read yet (ICCID %s, IMSI %s)\n",
		       (unsigned long long)link_ms(), svc_name(svc), svc, msg, t->m2, pipe_name[pipe], sess, cid, simid.have_iccid ? "known" : "unknown",
		       simid.have_imsi ? "known" : "unknown");
		return 1;
	}
	synth_inds++;
	printf("[%6llu] abp: on %s(0x%02x) 0x%04x: indication 0x%04x (%d TLV bytes, template%s%s) to %s/%u cid %u\n", (unsigned long long)link_ms(),
	       svc_name(svc), svc, msg, t->m2, n, tmpl_sim(t->tmpl) ? " with the SIM identity" : "", why, pipe_name[pipe], sess, cid);
	send_ind(pipe, sess, svc, cid, t->m2, b, (uint32_t)n);
	return 0;
}

/* the identity changed or became known: the remembered indications again */
static void simid_changed(void)
{
	for (int i = 0; i < NSUB; i++) {
		struct sub *s = &subs[i];
		if (!s->used)
			continue;
		if (!cid_live(s->pipe, s->sess, s->svc, s->cid)) {
			s->used = 0;
			continue;
		}
		for (int k = 0; k < ntrg; k++)
			if (trg[k].kind == TRG_IND && trg[k].svc == s->svc && trg[k].msg == s->msg && trg[k].m2 == s->m2 && trg[k].tmpl) {
				trg_ind(s->pipe, s->sess, s->svc, s->cid, s->msg, &trg[k], s->rq, s->rqn, ", SIM identity changed");
				break;
			}
	}
}

/* a successful UIM Read Transparent answer's TLVs (tl, tn) for EF file: the content (TLV 0x11 {u16 len, data}) */
static void simid_from_read(unsigned file, const uint8_t *tl, uint32_t tn, const char *src)
{
	uint32_t l = 0;
	const uint8_t *v = tlv_find(tl, tn, 0x11, &l);
	if (!v || l < 2 || (uint32_t)(v[0] | (v[1] << 8)) + 2 > l)
		return;
	uint32_t cl = (uint32_t)(v[0] | (v[1] << 8));
	const uint8_t *ef = v + 2;
	uint8_t val[SIM_ICCID_LEN];
	int ic;
	if (file == 0x2fe2) {
		/* EF_ICCID keeps each digit pair low nibble first: swapped into reading order (QcomBaseband cbd_iccid_from_ef) */
		if (cl != SIM_ICCID_LEN)
			return;
		for (uint32_t i = 0; i < SIM_ICCID_LEN; i++)
			val[i] = (uint8_t)(ef[i] << 4 | ef[i] >> 4);
		ic = 1;
	} else if (file == 0x6f07) {
		/* EF_IMSI: length byte, digit 1 in the high nibble of the first byte (above the parity nibble), then pairs low
		 * nibble first; repacked in reading order, F-padded (QcomBaseband cbd_imsi_from_ef) */
		uint8_t dg[16];
		uint32_t nd = 0;
		if (cl < 2 || !ef[0] || ef[0] > 8 || cl < 1u + ef[0])
			return;
		for (uint32_t i = 1; i <= ef[0]; i++) {
			uint8_t lo = ef[i] & 0xf, hi = ef[i] >> 4;
			if (i > 1) {
				if (lo > 9)
					break;
				dg[nd++] = lo;
			}
			if (hi > 9)
				break;
			dg[nd++] = hi;
		}
		if (nd < 6 || nd > 15)
			return;
		for (uint32_t i = 0; i < SIM_IMSI_LEN; i++) {
			uint8_t a = 2 * i < nd ? dg[2 * i] : 0xf, b = 2 * i + 1 < nd ? dg[2 * i + 1] : 0xf;
			val[i] = (uint8_t)(a << 4 | b);
		}
		ic = 0;
	} else {
		return;
	}
	uint8_t *have = ic ? simid.iccid : simid.imsi;
	int *known = ic ? &simid.have_iccid : &simid.have_imsi;
	uint32_t len = ic ? SIM_ICCID_LEN : SIM_IMSI_LEN;
	int changed = !*known || memcmp(have, val, len);
	if (changed) {
		printf("[%6llu] abp: SIM identity: %s %u bytes %s (from %s)\n", (unsigned long long)link_ms(), ic ? "ICCID" : "IMSI", len,
		       *known ? "CHANGED" : "read", src);
		memcpy(have, val, len);
		*known = 1;
		simid.gen++;
	}
	if (simid.have_iccid && simid.have_imsi) {
		simid.refresh = 0;
		simid.tries = 0;
	}
	if (changed)
		simid_changed();
}

/* the router's own read of EF file from CommCenter UIM client c's port */
static void simid_read(struct cl *c, int si, unsigned file)
{
	static const uint8_t iccid[20] = { 0x01, 2, 0, 0x06, 0x00, 0x02, 5, 0, 0xe2, 0x2f, 2, 0x00, 0x3f, 0x03, 4, 0, 0, 0, 0, 0 };
	static const uint8_t imsi[22] = { 0x01, 2, 0, 0x00, 0x00, 0x02, 7, 0, 0x07, 0x6f, 4, 0x00, 0x3f, 0xff, 0x7f, 0x03, 4, 0, 0, 0, 0, 0 };
	const uint8_t *tl = file == 0x2fe2 ? iccid : imsi;
	uint32_t tn = file == 0x2fe2 ? sizeof(iccid) : sizeof(imsi);
	uint8_t q[7 + 22];
	if (++replay_txn == 0xffff)
		replay_txn = 0xff00;
	q[0] = 0x00;
	q[1] = (uint8_t)replay_txn;
	q[2] = (uint8_t)(replay_txn >> 8);
	q[3] = 0x20;
	q[4] = 0x00;
	q[5] = (uint8_t)tn;
	q[6] = 0;
	memcpy(q + 7, tl, tn);
	if (qrtr_send(QRTR_TYPE_DATA, c->port, srv[si].node, srv[si].port, q, 7 + tn)) {
		cnt.sendfail++;
		return;
	}
	struct pend x;
	memset(&x, 0, sizeof(x));
	x.port = c->port;
	x.txn = replay_txn;
	x.msg = 0x20;
	x.svc = 0x0b;
	x.simread = 1;
	x.file = (uint16_t)file;
	pend_add(&x);
	simid.reads++;
}

static void simid_poll(uint64_t now)
{
	int need = simid.refresh;
	for (int i = 0; i < NSUB && !need; i++)
		if (subs[i].used && subs[i].pending && cid_live(subs[i].pipe, subs[i].sess, subs[i].svc, subs[i].cid))
			need = 1;
	if (!need || now < simid.next)
		return;
	simid.next = now + 5000;
	if (simid.refresh && ++simid.tries > 3) {
		simid.refresh = 0;
		simid.tries = 0;
		if (simid.have_iccid && simid.have_imsi)
			return;
	}
	int si = srv_find(0x0b), code;
	const char *why;
	char wb[128];
	if (si < 0 || decide(0x0b, 0x0020, &code, &why, wb, sizeof(wb)) != ACT_FWD)
		return;
	struct cl *c = NULL;
	for (int i = 0; i < NCL && !c; i++)
		if (cl[i].used == 1 && cl[i].svc == 0x0b)
			c = &cl[i];
	for (int i = 0; i < NCL && !c; i++)
		if (cl[i].used == 2 && cl[i].svc == 0x0b)
			c = &cl[i];
	if (!c)
		return;
	printf("[%6llu] abp: SIM identity: reading EF_ICCID and EF_IMSI (UIM 0x0020 from port %#x)\n", (unsigned long long)now, c->port);
	simid_read(c, si, 0x2fe2);
	simid_read(c, si, 0x6f07);
}

static void run_triggers(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid, unsigned msg, const uint8_t *rq, uint32_t rqn)
{
	for (int i = 0; i < ntrg; i++) {
		struct trg *t = &trg[i];
		if (t->svc != (int)svc || t->msg != (int)msg)
			continue;
		if (t->cond >= 0 && tlv_byte(rq, rqn, (unsigned)t->cond) <= 0)
			continue;	/* P2 reg: this request did not enable that indication */
		if (t->ceq) {		/* n12b: "if RQ..=RQ.." */
			uint32_t a, b;
			if (fref_num(&t->fa, rq, rqn, &a) || fref_num(&t->fb, rq, rqn, &b) || a != b)
				continue;
		}
		if (t->kind == TRG_IND) {
			trg_ind(pipe, sess, svc, cid, msg, t, rq, rqn, "");
			continue;
		}
		/* replay: only a GET the policy itself would forward (protected / Apple / data messages never go out this way) */
		int code;
		const char *why;
		char wb[128];
		if (decide(svc, t->m2, &code, &why, wb, sizeof(wb)) != ACT_FWD) {
			printf("[%6llu] abp: on %s(0x%02x) 0x%04x: replay GET 0x%04x refused (policy: %s)\n", (unsigned long long)link_ms(),
			       svc_name(svc), svc, msg, t->m2, why);
			continue;
		}
		int si = srv_find(svc);
		struct cl *c = cl_get(pipe, sess, svc, cid, 1);
		if (si < 0 || !c)
			continue;
		uint8_t q[7] = { 0x00, 0, 0, (uint8_t)t->m2, (uint8_t)(t->m2 >> 8), 0, 0 };
		struct pend x;
		memset(&x, 0, sizeof(x));
		if (++replay_txn == 0xffff)
			replay_txn = 0xff00;
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
		if (qrtr_send(QRTR_TYPE_DATA, c->port, srv[si].node, srv[si].port, q, sizeof(q))) {
			cnt.sendfail++;
			continue;
		}
		pend_add(&x);
		replays++;
		printf("[%6llu] abp: on %s(0x%02x) 0x%04x: replay GET 0x%04x -> indication 0x%04x for %s/%u cid %u (txn 0x%04x)\n",
		       (unsigned long long)link_ms(), svc_name(svc), svc, msg, t->m2, t->m3, pipe_name[pipe], sess, cid, replay_txn);
	}
}

/* a request for the modem: SDU unchanged over QRTR from this client's port */
static void svc_forward(unsigned pipe, unsigned svc, unsigned cid, const uint8_t *s, uint32_t sn)
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
	if (qrtr_send(QRTR_TYPE_DATA, c->port, srv[si].node, srv[si].port, s, len)) {
		cnt.sendfail++;
		svc_local(pipe, svc, cid, s, 1, 0x03 /* QMI_ERR_INTERNAL */);
		return;
	}
	c->fwd++;
	cnt.fwd++;
	struct pend x;
	memset(&x, 0, sizeof(x));
	x.port = c->port;
	x.txn = (uint16_t)(s[1] | (s[2] << 8));
	x.msg = (uint16_t)(s[3] | (s[4] << 8));
	x.svc = (uint8_t)svc;
	uint32_t tn = len - 7 < sizeof(x.rq) ? len - 7 : (uint32_t)sizeof(x.rq);
	x.rqn = (uint8_t)tn;
	memcpy(x.rq, s + 7, tn);
	pend_add(&x);
}

static void handle(unsigned pipe, uint32_t sess, unsigned type, const uint8_t *d, uint32_t n)
{
	if (pipe >= NPIPES)
		return;
	cur_pipe = pipe;
	cur_sess = sess;
	if (type != 0) {
		static const char *ev[] = { "data", "OPEN", "CLOSE", "START", "ABORT" };
		unsigned long long v = 0;
		if (n >= 8)
			memcpy(&v, d, 8);
		printf("[%6llu] abp: t+%.1fs %s/%u %s (arg 0x%llx)\n", (unsigned long long)link_ms(), secs(), pipe_name[pipe], sess,
		       type < 5 ? ev[type] : "?", v);
		if (lf && logframes) {
			fprintf(lf, "%.3f %s/%u event %s arg 0x%llx\n", secs(), pipe_name[pipe], sess, type < 5 ? ev[type] : "?", v);
			fflush(lf);
		}
		if (type == 2)
			sess_end(pipe, sess);
		return;
	}
	nframes_in++;
	log_frame(pipe, "CC->", d, n);
	if (n < 6 || d[0] != 0x01)
		return;			/* not plain QMUX: logged, not answered */
	unsigned svc = d[4], cid = d[5];
	const uint8_t *s = d + 6;
	uint32_t sn = n - 6;
	if (svc == 0) {
		if (sn >= 6 && s[0] == 0x00)
			ctl_answer(pipe, s, sn);
		return;
	}
	if (sn < 7 || s[0] != 0x00)
		return;			/* only requests are answered */
	unsigned msg = s[3] | (s[4] << 8);
	if (mode < 2) {			/* P1 observe */
		note_seen(svc, msg, "err", "abpbridge=1 observe");
	struct mst *mo = mst_get(svc, msg);
	if (mo) {
		mo->count++;
		mo->act = ACT_ERR;
	}
		cnt.local_err++;
		svc_local(pipe, svc, cid, s, 1, err_code);
		return;
	}
	int code;
	const char *why;
	char wb[128];
	int a = decide(svc, msg, &code, &why, wb, sizeof(wb));
	char an[24];
	if (a == ACT_ERR)
		snprintf(an, sizeof(an), "err:0x%x", code);
	else
		snprintf(an, sizeof(an), "%s", act_name[a]);
	note_seen(svc, msg, an, why);
	struct mst *ms = mst_get(svc, msg);
	if (ms) {
		ms->count++;
		ms->act = (uint8_t)a;
		mst_dirty = 1;
	}
	switch (a) {
	case ACT_FWD:
		svc_forward(pipe, svc, cid, s, sn);
		break;
	case ACT_OK:
		cnt.local_ok++;
		svc_local(pipe, svc, cid, s, 0, 0);
		run_triggers(pipe, cur_sess, svc, cid, msg, s + 7, sn - 7 < (uint32_t)(s[5] | (s[6] << 8)) ? sn - 7 : (uint32_t)(s[5] | (s[6] << 8)));
		break;
	case ACT_RESP:
		if (dec_tmpl) {		/* n12b: template, expanded with this request's TLVs */
			static uint8_t tb[4096];
			uint32_t rl = sn - 7 < (uint32_t)(s[5] | (s[6] << 8)) ? sn - 7 : (uint32_t)(s[5] | (s[6] << 8));
			int miss = 0, tn = tmpl_expand(dec_tmpl, s + 7, rl, 0, tb, sizeof(tb), &miss);
			if (tn < 0 || !tlvs_ok(tb, (uint32_t)tn)) {
				cnt.local_err++;
				svc_local(pipe, svc, cid, s, 1, 0x11 /* QMI_ERR_MISSING_ARG */);
				break;
			}
			cnt.local_ok++;
			svc_local_tlv(pipe, svc, cid, s, 0, 0, tb, (uint16_t)tn);
			run_triggers(pipe, cur_sess, svc, cid, msg, s + 7, rl);
			break;
		}
		cnt.local_ok++;
		svc_local_tlv(pipe, svc, cid, s, 0, 0, dec_data, dec_dlen);
		run_triggers(pipe, cur_sess, svc, cid, msg, s + 7, sn - 7 < (uint32_t)(s[5] | (s[6] << 8)) ? sn - 7 : (uint32_t)(s[5] | (s[6] << 8)));
		break;
	case ACT_ERR:
		cnt.local_err++;
		svc_local(pipe, svc, cid, s, 1, code);
		break;
	default:
		cnt.dropped++;
		break;
	}
}

/* ---- from wland's QRTR loop ---- */

void abpbridge_server(uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
	int fr = -1;
	for (int i = 0; i < NSRV; i++) {
		if (srv[i].up && srv[i].svc == service && srv[i].inst == instance && srv[i].node == node && srv[i].port == port)
			return;
		if (!srv[i].up && fr < 0)
			fr = i;
	}
	if (fr < 0)
		return;
	srv[fr].svc = service;
	srv[fr].inst = instance;
	srv[fr].node = node;
	srv[fr].port = port;
	srv[fr].up = 1;
}

void abpbridge_server_gone(uint32_t node, uint32_t port)
{
	for (int i = 0; i < NSRV; i++)
		if (srv[i].up && srv[i].node == node && (port == 0xffffffffu || srv[i].port == port)) {
			srv[i].up = 0;
			if (fd >= 0 && mode >= 2)
				printf("[%6llu] abp: modem server svc 0x%02x (%s) gone\n", (unsigned long long)link_ms(), srv[i].svc,
				       svc_name(srv[i].svc));
			/* the router clients of that service: their modem-side state went with it */
			for (int k = 0; k < NCL; k++)
				if (cl[k].used && cl[k].svc == srv[i].svc && srv_find(srv[i].svc) < 0) {
					pend_drop_port(cl[k].port);
					memset(&cl[k], 0, sizeof(cl[k]));	/* the modem side is gone already: no DEL_CLIENT */
				}
		}
}

/* QRTR DATA to one of our router ports: a response or an indication for a CommCenter client */
void abpbridge_qrtr(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	/* r3: the answer to a GET the router sent itself (replay) becomes an indication; it never reaches CommCenter as a response */
	if (n >= 7 && d[0] == 0x02) {
		uint16_t txn = (uint16_t)(d[1] | (d[2] << 8));
		for (int i = 0; i < NPEND; i++) {
			struct pend *x = &pend[i];
			if (!x->used || !(x->replay || x->simread) || x->port != h->dst_port || x->txn != txn)
				continue;
			x->used = 0;
			if (x->simread) {	/* n12b: the router's own EF read */
				uint32_t sl = (uint32_t)(d[5] | (d[6] << 8)), rl2 = 0;
				if (sl > n - 7 || !tlvs_ok(d + 7, sl))
					return;
				const uint8_t *r2 = tlv_find(d + 7, sl, 0x02, &rl2);
				if (!r2 || rl2 < 4 || r2[0] | r2[1]) {
					printf("[%6llu] abp: SIM identity: EF %04x read failed (error 0x%x)\n", (unsigned long long)link_ms(), x->file,
					       r2 && rl2 >= 4 ? (unsigned)(r2[2] | (r2[3] << 8)) : 0xffffu);
					return;
				}
				simid_from_read(x->file, d + 7, sl, "the router's read");
				return;
			}
			uint32_t ml = (uint32_t)(d[5] | (d[6] << 8));
			if (ml > n - 7 || !tlvs_ok(d + 7, ml)) {
				malformed++;
				printf("[%6llu] abp: MALFORMED replay answer %s(0x%02x) msg 0x%04x (%u bytes): dropped\n", (unsigned long long)link_ms(),
				       svc_name(x->svc), x->svc, x->msg, n);
				return;
			}
			struct qmi_msg qm;
			uint16_t rl = 0;
			const uint8_t *r = NULL;
			if (!qmi_parse(d, n, &qm))
				r = qmi_get(&qm, 0x02, &rl);
			unsigned res = r && rl >= 4 ? (unsigned)(r[0] | (r[1] << 8)) : 0xffff;
			unsigned er = r && rl >= 4 ? (unsigned)(r[2] | (r[3] << 8)) : 0xffff;
			if (res != 0) {
				printf("[%6llu] abp: replay %s(0x%02x) GET 0x%04x: modem error 0x%x, no indication\n", (unsigned long long)link_ms(),
				       svc_name(x->svc), x->svc, x->msg, er);
				return;
			}
			static uint8_t t[65536];
			uint32_t o = 0;
			const uint8_t *p = d + 7;
			for (uint32_t k = 0; k + 3 <= ml;) {
				unsigned id = p[k], l = (unsigned)(p[k + 1] | (p[k + 2] << 8));
				if (id != 0x02) {
					unsigned nid = id;
					for (int m = 0; m < x->nmap; m++)
						if (x->map[m][0] == id)
							nid = x->map[m][1];
					t[o++] = (uint8_t)nid;
					memcpy(t + o, p + k + 1, 2 + l);
					o += 2 + l;
				}
				k += 3 + l;
			}
			o = strip_tlvs(x->svc, x->ind_msg, t, o);
			synth_inds++;
			printf("[%6llu] abp: replay %s(0x%02x) GET 0x%04x -> indication 0x%04x (%u TLV bytes) to %s/%u cid %u\n",
			       (unsigned long long)link_ms(), svc_name(x->svc), x->svc, x->msg, x->ind_msg, o, pipe_name[x->rpipe], x->rsess, x->rcid);
			/* only while that CommCenter client still exists */
			struct cl *c = cl_get(x->rpipe, x->rsess, x->svc, x->rcid, 0);
			if (c)
				send_ind(x->rpipe, x->rsess, x->svc, x->rcid, x->ind_msg, t, o);
			return;
		}
	}
	struct cl *c = NULL;
	for (int i = 0; i < NCL && !c; i++)
		if (cl[i].used == 1 && cl[i].port == h->dst_port)
			c = &cl[i];
	if (!c || fd < 0 || n < 7) {
		late++;			/* a released / idle router port (or the bridge closed): dropped silently */
		if (n >= 7 && d[0] == 0x02) {
			uint16_t txn = (uint16_t)(d[1] | (d[2] << 8));
			for (int i = 0; i < NPEND; i++)
				if (pend[i].used && pend[i].port == h->dst_port && pend[i].txn == txn)
					pend[i].used = 0;
		}
		return;
	}
	/* r3: never hand CommCenter an SDU whose lengths do not add up (QMIServiceMsg.cpp:94 exception -> ABM ASSERT) */
	unsigned ml = d[5] | (d[6] << 8);
	if (ml > n - 7 || !tlvs_ok(d + 7, ml)) {
		malformed++;
		char hx[3 * 48 + 8];
		hexline(hx, sizeof(hx), d, n < 40 ? n : 40);
		printf("[%6llu] abp: MALFORMED %s from the modem for %s(0x%02x) cid %u (%u bytes, msg len %u): dropped: %s\n",
		       (unsigned long long)link_ms(), d[0] == 0x04 ? "indication" : d[0] == 0x02 ? "response" : "frame", svc_name(c->svc), c->svc,
		       c->cid, n, ml, hx);
		return;
	}
	unsigned res = 0xffff, rmsg = (unsigned)(d[3] | (d[4] << 8));
	uint8_t rq[96];
	uint32_t rqn = 0;
	if (d[0] == 0x02) {		/* a response: the pending request and its result */
		uint16_t txn = (uint16_t)(d[1] | (d[2] << 8)), msg = (uint16_t)rmsg;
		for (int i = 0; i < NPEND; i++)
			if (pend[i].used && pend[i].port == h->dst_port && pend[i].txn == txn) {
				pend[i].used = 0;
				rqn = pend[i].rqn;
				memcpy(rq, pend[i].rq, rqn);
				break;
			}
		struct qmi_msg qm;
		uint16_t rl = 0;
		const uint8_t *r = NULL;
		if (!qmi_parse(d, n, &qm))
			r = qmi_get(&qm, 0x02, &rl);
		res = r && rl >= 4 ? (unsigned)(r[0] | (r[1] << 8)) : 0xffff;
		unsigned err = r && rl >= 4 ? (unsigned)(r[2] | (r[3] << 8)) : 0xffff;
		struct mst *m = mst_get(c->svc, msg);
		if (m) {
			if (res == 0) {
				m->ok++;
			} else {
				m->err++;
				m->last_err = (uint16_t)err;
			}
			mst_dirty = 1;
			if (!m->answered_logged) {
				m->answered_logged = 1;
				printf("[%6llu] abp: ANSWER %s(0x%02x) msg 0x%04x: %s", (unsigned long long)link_ms(), svc_name(c->svc), c->svc, msg,
				       res == 0 ? "success" : res == 0xffff ? "no result TLV" : "FAILURE");
				if (res != 0 && res != 0xffff)
					printf(" error 0x%x", err);
				printf(" (%u bytes)\n", n);
			}
		}
	}
	/* n12b: CommCenter's own EF_ICCID / EF_IMSI reads give the SIM identity; a UIM status change asks for a fresh read */
	if (d[0] == 0x02 && res == 0 && c->svc == 0x0b && rmsg == 0x0020 && rqn) {
		uint32_t fl = 0;
		const uint8_t *fv = tlv_find(rq, rqn, 0x02, &fl);
		if (fv && fl >= 3)
			simid_from_read((unsigned)(fv[0] | (fv[1] << 8)), d + 7, ml, "CommCenter's read");
	}
	if (d[0] == 0x04 && c->svc == 0x0b && rmsg == 0x0032 && !simid.refresh) {
		simid.refresh = 1;
		simid.tries = 0;
		simid.next = link_ms() + 2000;
	}
	uint32_t len = 7 + ml;
	static uint8_t b[6 + ABP_MAXFRAME];
	if (6 + len > sizeof(b))
		return;
	memcpy(b + 6, d, len);
	if (nstrip) {
		uint32_t nl = strip_tlvs(c->svc, rmsg, b + 13, ml);
		if (nl != ml) {
			b[11] = (uint8_t)nl;
			b[12] = (uint8_t)(nl >> 8);
			len = 7 + nl;
		}
	}
	finish_qmux(b, 6 + len, c->svc, c->cid);
	if (d[0] == 0x04) {
		c->ind++;
		cnt.ind++;
	} else {
		c->resp++;
		cnt.resp++;
	}
	uint32_t pp = cur_pipe, cpipe = c->pipe, csess = c->sess;
	unsigned csvc = c->svc, ccid = c->cid;
	cur_pipe = c->pipe;
	send_to(c->pipe, c->sess, b, 6 + len);
	cur_pipe = pp;
	if (d[0] == 0x02 && res == 0)
		run_triggers(cpipe, csess, csvc, ccid, rmsg, rq, rqn);
}

/* the per-(svc, msg) table: svc msg action count ok err[last code] noanswer */
static void mst_table(const char *why)
{
	printf("[%6llu] abp: table (%s, %d rows, late %u): svc msg action count ok err[code] noans\n", (unsigned long long)link_ms(), why,
	       nmst, late);
	for (int i = 0; i < nmst; i++) {
		struct mst *m = &mst[i];
		char e[24] = "";
		if (m->err)
			snprintf(e, sizeof(e), "[0x%x]", m->last_err);
		printf("abp:  %02x %-12s %04x %-5s %u %u %u%s %u\n", m->svc, svc_name(m->svc), m->msg,
		       m->count || m->act ? act_name[m->act < 6 ? m->act : 4] : "-", m->count, m->ok, m->err, e, m->noans);
	}
	mst_dirty = 0;
}

/* ---- device, words, ready ---- */

static void set_ready(int r, const char *why)
{
	if (fd < 0 || r == ready)
		return;
	uint32_t v = (uint32_t)r;
	if (ioctl(fd, ABPB_IOC_READY, &v)) {
		if (!ioctl_bad++)
			printf("[%6llu] abp: READY ioctl: %s (kernel without 1063: the services are published at boot)\n",
			       (unsigned long long)link_ms(), strerror(errno));
	} else {
		printf("[%6llu] abp: told the kernel: %s (%s)\n", (unsigned long long)link_ms(), r ? "READY" : "not ready", why);
	}
	ready = r;
}

static void read_words(void)
{
	char b[1024] = "";
	FILE *f = fopen(CELL_ON, "r");
	mode = 0;
	logframes = 0;
	cidfail = 0;
	reuse = 1;
	dumpword = 0;
	err_code = QMI_ERR_NOT_SUPPORTED;
	if (f) {
		size_t k = fread(b, 1, sizeof(b) - 1, f);
		fclose(f);
		b[k] = 0;
		const char *w = strstr(b, "abpbridge=");
		mode = w && (w == b || w[-1] == ' ' || w[-1] == '\n' || w[-1] == '\t') ? atoi(w + 10) : 0;
		w = strstr(b, "abperr=");
		if (w)
			err_code = (int)strtol(w + 7, NULL, 0);
		w = strstr(b, "abplog=");
		logframes = w ? atoi(w + 7) : 0;
		w = strstr(b, "abpcidfail=");
		cidfail = w ? atoi(w + 11) : 0;
		w = strstr(b, "abpreuse=");
		reuse = w ? atoi(w + 9) : 1;
		w = strstr(b, "abpkick=");
		kickon = w ? atoi(w + 8) : 1;
		w = strstr(b, "abpdump=");
		dumpword = w ? atoi(w + 8) : 0;
	}
	if (mode == 1)
		logframes = 1;		/* P1 observe always logs every frame */
}

static void dev_open(uint64_t now)
{
	fd = open(ABP_DEV, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		static int told;
		if (!told++)
			printf("[%6llu] abp: abpbridge=%d but %s: %s (kernel 1061/1063 with sweetabp=1?)\n", (unsigned long long)now, mode,
			       ABP_DEV, strerror(errno));
		return;
	}
	t0 = now;
	memset(st, 0, sizeof(st));
	nseen = 0;
	ready = -1;
	ioctl_bad = 0;
	if (!lf && logframes)
		lf = fopen(ABP_LOG, "a");
	if (lf) {
		fprintf(lf, "---- open at wland %llu ms, mode %d, abperr 0x%x\n", (unsigned long long)now, mode, err_code);
		fflush(lf);
	}
	printf("[%6llu] abp: %s open, mode %d (%s), abperr 0x%x, abplog %d\n", (unsigned long long)now, ABP_DEV, mode,
	       mode >= 2 ? "router: CTL here, policy table, forwards to the modem over QRTR" :
	       "observe: CTL here, other services get the QMI error, nothing to the modem", err_code, logframes);
	if (mode >= 2)
		policy_load(1);
}

static void dev_close(uint64_t now, const char *why)
{
	cl_free_all(why);
	close(fd);
	fd = -1;
	ready = -1;
	printf("[%6llu] abp: %s closed (%s; %u frames in, %u out)\n", (unsigned long long)now, ABP_DEV, why, nframes_in, nframes_out);
}

static void update_ready(void)
{
	if (fd < 0)
		return;
	if (mode < 2) {
		set_ready(1, "observe mode");
		return;
	}
	int ok = srv_find(0x02) >= 0 && srv_find(0x03) >= 0 && srv_find(0x0b) >= 0;
	set_ready(ok, ok ? "modem DMS, NAS and UIM servers known" : "a modem DMS/NAS/UIM server missing");
}

/* early in wland's start: open the bridge before the modem is up (the kernel queues CommCenter's frames) */
void abpbridge_early(void)
{
	read_words();
	if (mode && fd < 0 && !access(ABP_DEV, F_OK))
		dev_open(link_ms());
	next_check = link_ms() + 10000;
	update_ready();
}

/*
 * P2 reg (2026-11-09): a forwarded request unanswered for 5 s -> an unsolicited RESUME_TX from that router port to the
 * modem's server (at most once per port per 5 s). After the link.c nesting bug the modem had stopped sending to the
 * pooled NAS ports (a lost RESUME_TX = the modem's flow control towards that port never re-opens); the pool kept
 * handing those dead ports to every new CommCenter client. Disable with abpkick=0.
 */
static unsigned kicks;
static uint64_t kick_t[ABP_NPORTS];
static void kick(uint32_t port, unsigned svc, uint64_t now)
{
	int si = srv_find(svc);
	if (!kickon || si < 0 || port < ABP_PORT0 || port >= ABP_PORT0 + ABP_NPORTS)
		return;
	if (kick_t[port - ABP_PORT0] && now - kick_t[port - ABP_PORT0] < 5000)
		return;
	kick_t[port - ABP_PORT0] = now;
	kicks++;
	printf("[%6llu] abp: kick: RESUME_TX port %#x -> %s server 0:%x (a request got no answer in 5 s)\n", (unsigned long long)now,
	       port, svc_name(svc), srv[si].port);
	qrtr_kick(port, srv[si].node, srv[si].port);
}

void abpbridge_poll(void)
{
	uint64_t now = link_ms();
	if (now >= next_check) {
		int old_mode = mode;
		next_check = now + 10000;
		read_words();
		if (fd >= 0 && mode != old_mode) {
			dev_close(now, mode ? "mode changed" : "abpbridge word gone");
		}
		if (mode && fd < 0)
			dev_open(now);
		if (fd >= 0 && mode >= 2)
			policy_load(0);
		if (fd >= 0 && logframes && !lf)
			lf = fopen(ABP_LOG, "a");
	}
	if (fd < 0)
		return;
	update_ready();
	if (mode >= 2) {
		for (int i = 0; i < NPEND; i++)
			if (pend[i].used && now - pend[i].t > 5000) {
				pend[i].used = 0;
				kick(pend[i].port, pend[i].svc, now);
				struct mst *m = mst_get(pend[i].svc, pend[i].msg);
				if (m) {
					m->noans++;
					mst_dirty = 1;
					if (!m->answered_logged) {
						m->answered_logged = 1;
						printf("[%6llu] abp: ANSWER %s(0x%02x) msg 0x%04x: NO ANSWER after 5 s\n", (unsigned long long)now,
						       svc_name(pend[i].svc), pend[i].svc, pend[i].msg);
					}
				}
			}
	}
	if (mode >= 2)
		simid_poll(now);	/* n12b */
	if (dumpword && dumpword != dumpword_old)
		mst_table("abpdump");
	dumpword_old = dumpword;
	if (mode >= 2 && now >= next_stats) {
		if (next_stats) {
			int ncl = 0, nidle = 0;
			unsigned qmx, qdr;
			link_queue_stats(&qmx, &qdr);
			for (int i = 0; i < NCL; i++) {
				ncl += cl[i].used == 1;
				nidle += cl[i].used == 2;
			}
			printf("[%6llu] abp: stats: %u frames in, %u out; %u fwd, %u modem resp, %u ind; local ok %u, err %u, drop %u; "
			       "%d router clients (%d idle); no server %u, send fail %u, late %u; malformed %u, replays %u, synth ind %u; "
			       "bad records %u, kicks %u, rxq max %u drop %u, stripped %u\n",
			       (unsigned long long)now, nframes_in, nframes_out, cnt.fwd, cnt.resp, cnt.ind, cnt.local_ok, cnt.local_err, cnt.dropped,
			       ncl, nidle, cnt.noserver, cnt.sendfail, late, malformed, replays, synth_inds, badrec, kicks, qmx, qdr, stripped);
			if (mst_dirty)
				mst_table("60 s");
		}
		next_stats = now + 60000;
	}
	for (int round = 0; round < 8; round++) {
		ssize_t r = read(fd, rbuf, sizeof(rbuf));
		if (r <= 0) {
			if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				printf("[%6llu] abp: read: %s\n", (unsigned long long)now, strerror(errno));
			return;
		}
		uint32_t off = 0;
		while (off + sizeof(struct abpb_hdr) <= (uint32_t)r) {
			struct abpb_hdr h;
			memcpy(&h, rbuf + off, sizeof(h));
			if (h.magic != ABPB_MAGIC || h.len > (uint32_t)r - off - sizeof(h)) {
				badrec++;
				printf("[%6llu] abp: bad record at %u (magic 0x%x len %u)\n", (unsigned long long)now, off, h.magic, h.len);
				return;
			}
			handle(h.pipe, h.sess, h.type, rbuf + off + sizeof(h), h.len);
			off += (uint32_t)sizeof(h) + h.len;
		}
	}
}
