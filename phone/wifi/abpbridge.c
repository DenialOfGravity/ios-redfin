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
 * Never forwarded whatever the file says: Apple-only services (svc >= 0xe0) and Apple message ids (>= 0x5500).
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
	int used;
	uint32_t pipe, sess, port;
	uint8_t svc, cid;
	unsigned fwd, resp, ind;
} cl[NCL];
static uint32_t next_port_gen;

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

enum { ACT_FWD, ACT_OK, ACT_ERR, ACT_DROP, ACT_LOCAL };
static const char *act_name[] = { "fwd", "ok", "err", "drop", "local" };
#define POL_ANY		(-1)
#define POL_ABPERR	(-1)	/* err code: abperr (default 0x5e) */

struct pol {
	int svc, msg, act, err;
	const char *why;
};

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
	{ 0x10, POL_ANY, ACT_ERR, POL_ABPERR, "LOC: not in P2" },
	{ 0x08, POL_ANY, ACT_ERR, POL_ABPERR, "AT: not in P2" },
	{ 0x22, POL_ANY, ACT_ERR, POL_ABPERR, "COEX: would change LTE/Wi-Fi/BT coexistence under wland's Wi-Fi" },
	{ 0x24, POL_ANY, ACT_ERR, POL_ABPERR, "PDC: modem configuration (MBN) selection: protected" },
	{ 0x2b, POL_ANY, ACT_ERR, POL_ABPERR, "SSCTL: subsystem restart: protected" },
	{ 0x29, POL_ANY, ACT_ERR, POL_ABPERR, "RFRPE: RF settings: protected" },
	{ 0x11, POL_ANY, ACT_ERR, POL_ABPERR, "SAR: RF power: protected" },
};

#define NOVR	256
static struct pol ovr[NOVR];
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

static void policy_load(int force)
{
	struct stat sb;
	if (stat(ABP_POLICY, &sb)) {
		if (novr || ovr_mtime) {
			printf("[%6llu] abp: %s gone: built-in policy only\n", (unsigned long long)link_ms(), ABP_POLICY);
			novr = 0;
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
	char line[256];
	int n = 0, bad = 0, ln = 0;
	while (fgets(line, sizeof(line), f) && n < NOVR) {
		char a[64], b[64], c[64];
		ln++;
		char *h = strchr(line, '#');
		if (h)
			*h = 0;
		if (sscanf(line, "%63s %63s %63s", a, b, c) != 3)
			continue;
		struct pol p = { 0, POL_ANY, ACT_ERR, POL_ABPERR, "conf" };
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
		else {
			printf("abp: %s line %d: bad action '%s' (fwd | ok | err:<code> | drop)\n", ABP_POLICY, ln, c);
			bad++;
			continue;
		}
		ovr[n++] = p;
	}
	fclose(f);
	novr = n;
	nseen = 0;
	printf("[%6llu] abp: policy %s: %d override line(s)%s, decisions logged again\n", (unsigned long long)link_ms(), ABP_POLICY,
	       n, bad ? " (some lines refused)" : "");
}

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
	if (p) {
		a = p->act;
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

static void cl_free(struct cl *c, const char *why)
{
	if (!c->used)
		return;
	/* tell the modem the client is gone (Linux qrtr_port_remove: DEL_CLIENT from that port to the control port) */
	uint32_t pkt[5] = { QRTR_TYPE_DEL_CLIENT, QRTR_NODE_APPS, c->port, 0, 0 };
	qrtr_send(QRTR_TYPE_DEL_CLIENT, c->port, QRTR_NODE_BCAST, QRTR_PORT_CTRL, pkt, sizeof(pkt));
	printf("[%6llu] abp: %s/%u %s(0x%02x) cid %u port %#x released (%s; %u fwd, %u resp, %u ind)\n", (unsigned long long)link_ms(),
	       pipe_name[c->pipe], c->sess, svc_name(c->svc), c->svc, c->cid, c->port, why, c->fwd, c->resp, c->ind);
	memset(c, 0, sizeof(*c));
}

static struct cl *cl_get(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid, int create)
{
	struct cl *fr = NULL;
	for (int i = 0; i < NCL; i++) {
		if (cl[i].used && cl[i].pipe == pipe && cl[i].sess == sess && cl[i].svc == svc && cl[i].cid == cid)
			return &cl[i];
		if (!cl[i].used && !fr)
			fr = &cl[i];
	}
	if (!create)
		return NULL;
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
			busy |= cl[i].used && cl[i].port == p;
		if (!busy)
			port = p;
	}
	if (!port)
		return NULL;
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

static void cl_free_sess(uint32_t pipe, uint32_t sess, const char *why)
{
	for (int i = 0; i < NCL; i++)
		if (cl[i].used && cl[i].pipe == pipe && cl[i].sess == sess)
			cl_free(&cl[i], why);
}

static void cl_free_all(const char *why)
{
	for (int i = 0; i < NCL; i++)
		cl_free(&cl[i], why);
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

/* a request answered here: success without TLVs (ok) or a QMI failure (err) */
static void svc_local(unsigned pipe, unsigned svc, unsigned cid, const uint8_t *s, int fail, int code)
{
	uint8_t b[32];
	uint32_t o = 13;		/* QMUX 6 + SDU header 7 */
	uint16_t res[2] = { (uint16_t)(fail ? 1 : 0), (uint16_t)(fail ? code : 0) };
	put_tlv(b, &o, 0x02, res, 4);
	b[6] = 0x02;			/* response */
	b[7] = s[1];			/* txid u16 */
	b[8] = s[2];
	b[9] = s[3];			/* msg id */
	b[10] = s[4];
	b[11] = (uint8_t)(o - 13);
	b[12] = 0;
	finish_qmux(b, o, svc, cid);
	send_frame(pipe, b, o);
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
	switch (a) {
	case ACT_FWD:
		svc_forward(pipe, svc, cid, s, sn);
		break;
	case ACT_OK:
		cnt.local_ok++;
		svc_local(pipe, svc, cid, s, 0, 0);
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
				if (cl[k].used && cl[k].svc == srv[i].svc && srv_find(srv[i].svc) < 0)
					cl_free(&cl[k], "modem server gone");
		}
}

/* QRTR DATA to one of our router ports: a response or an indication for a CommCenter client */
void abpbridge_qrtr(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	struct cl *c = NULL;
	for (int i = 0; i < NCL && !c; i++)
		if (cl[i].used && cl[i].port == h->dst_port)
			c = &cl[i];
	if (!c || fd < 0 || n < 7) {
		if (cnt.noport++ < 20)
			printf("[%6llu] abp: QRTR %u bytes from %u:%x to port %#x: no router client (dropped)\n", (unsigned long long)link_ms(),
			       n, h->src_node, h->src_port, h->dst_port);
		return;
	}
	unsigned ml = d[5] | (d[6] << 8);
	uint32_t len = 7 + (ml < n - 7 ? ml : n - 7);
	static uint8_t b[6 + ABP_MAXFRAME];
	if (6 + len > sizeof(b))
		return;
	memcpy(b + 6, d, len);
	finish_qmux(b, 6 + len, c->svc, c->cid);
	if (d[0] == 0x04) {
		c->ind++;
		cnt.ind++;
	} else {
		c->resp++;
		cnt.resp++;
	}
	cur_pipe = c->pipe;
	send_to(c->pipe, c->sess, b, 6 + len);
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
	if (mode >= 2 && now >= next_stats) {
		if (next_stats) {
			int ncl = 0;
			for (int i = 0; i < NCL; i++)
				ncl += cl[i].used;
			printf("[%6llu] abp: stats: %u frames in, %u out; %u fwd, %u modem resp, %u ind; local ok %u, err %u, drop %u; "
			       "%d router clients; no server %u, send fail %u, no port %u\n", (unsigned long long)now, nframes_in, nframes_out,
			       cnt.fwd, cnt.resp, cnt.ind, cnt.local_ok, cnt.local_err, cnt.dropped, ncl, cnt.noserver, cnt.sendfail,
			       cnt.noport);
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
				printf("[%6llu] abp: bad record at %u (magic 0x%x len %u)\n", (unsigned long long)now, off, h.magic, h.len);
				return;
			}
			handle(h.pipe, h.sess, h.type, rbuf + off + sizeof(h), h.len);
			off += (uint32_t)sizeof(h) + h.len;
		}
	}
}
