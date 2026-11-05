/*
 * cell: read-only QMI client for the modem's UIM (0x0b), NAS (0x03), DMS (0x02) and (wds=1) WDS (0x01) services,
 * on wland's QRTR link (see cell.h). One request in flight, a round every 30 s while /var/local/cell.on exists:
 *   DMS  Get Model 0x22, Get Revision 0x23, Get IDs 0x25, Get Hardware Revision 0x2c (once; IMEI only counted),
 *        Get Operating Mode 0x2d
 *   UIM  Get Slots Status 0x47 (physical tray / eUICC; ICCID only counted), Get Card Status 0x2f (logical)
 *   NAS  Get Serving System 0x24, Get System Info 0x4d, Get Signal Info 0x4f
 *   WDS  (only with wds=1; data design step X0, read-only) Get Packet Service Status 0x22, Get Current Data Bearer
 *        Technology 0x44, Get Profile List 0x2a (3GPP), Get Default Profile Number 0x49 (3GPP, embedded) every round;
 *        Get Profile Settings 0x2b once per listed profile (again when cell.on, the list or the WDS server changes).
 *        Appends "| wds=... bearer=... profiles=N default=K apn='...' pdp=..." to the status line and logs each
 *        profile once ("cell: wds profile N ..."). Username/password TLVs (0x1b/0x1c) are never read.
 * cell.on words: slots=1 hw=1 wds=1 (reads), slotswitch=1 online=1/0 pin=1 (writes; see maybe_write and the PIN
 * section). Message ids and TLV layouts: libqmi data/qmi-service-{uim,nas,dms,wds}.json (as HakiOS vayu_cell_qmi.h).
 * No Network Register, no SMS/data/voice, no WDS Start Network or any other WDS write, never a PUK.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include "cell.h"
#include "link.h"
#include "qmi.h"

enum { SVC_DMS, SVC_NAS, SVC_UIM, SVC_WDS, NSVC };
static const uint32_t svc_id[NSVC] = { 0x02, 0x03, 0x0b, 0x01 };
static const char *svc_name[NSVC] = { "dms", "nas", "uim", "wds" };
static struct { uint32_t node, port; int up; } srv[NSVC];

/* opt: 0 = always (the fe5bee5 set), else only with that word in /var/local/cell.on (OPT_*) */
enum { OPT_SLOTS = 1, OPT_HW = 2, OPT_SWITCH = 4, OPT_ONLINE = 8, OPT_LPM = 16, OPT_PIN = 32, OPT_WDS = 64 };
static int p1_known, p1_present, p1_active, cur_mode = -1;
static const struct { int svc; uint16_t id; int once, opt; } steps[] = {
	{ SVC_DMS, 0x22, 1, 0 }, { SVC_DMS, 0x23, 1, 0 }, { SVC_DMS, 0x25, 1, 0 }, { SVC_DMS, 0x2c, 1, OPT_HW },
	{ SVC_DMS, 0x2d, 0, 0 },
	{ SVC_UIM, 0x47, 0, OPT_SLOTS }, { SVC_UIM, 0x2f, 0, 0 },
	{ SVC_NAS, 0x24, 0, 0 }, { SVC_NAS, 0x4d, 0, 0 }, { SVC_NAS, 0x4f, 0, 0 },
	/* appended, so the indexes above (done_once[3] = DMS 0x2c) stay */
	{ SVC_WDS, 0x22, 0, OPT_WDS }, { SVC_WDS, 0x44, 0, OPT_WDS }, { SVC_WDS, 0x2a, 0, OPT_WDS },
	{ SVC_WDS, 0x49, 0, OPT_WDS }, { SVC_WDS, 0x2b, 0, OPT_WDS },	/* 0x2b: one request per profile, see wds_more */
};
#define NSTEPS (int)(sizeof(steps) / sizeof(steps[0]))

static int enabled, opts, allowed, step = -1, done_once[NSTEPS], fails[NSTEPS], timeouts;
static uint64_t allow_ms, first_poll, next_send;
static uint16_t txn = 1, want_txn;
static uint64_t sent_ms, next_round, next_check, last_log;
static char model[48] = "?", hwrev[48] = "?", rev[128] = "?", imei[24] = "?";
static char f_mode[32], f_sim[256], f_slots[192], f_reg[160], f_svc[96], f_sig[96];
static char last_line[1536];
static int bars;


/*
 * Append to a fixed field, never past its end. (cfc3df9 used `n += snprintf(buf + n, sizeof(buf) - n, ...)`
 * inside the per-application loop with no check: once n passed the field's size the size argument wrapped
 * to ~2^64 and the next write ran over the globals after it -- the suspect for the 2026-11-05 wedge.)
 */
#include <stdarg.h>
static int ap_(char *buf, size_t size, int n, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int ap_(char *buf, size_t size, int n, const char *fmt, ...)
{
	if (n < 0 || (size_t)n >= size - 1)
		return n < 0 ? 0 : (int)(size - 1);
	va_list va;
	va_start(va, fmt);
	int k = vsnprintf(buf + n, size - (size_t)n, fmt, va);
	va_end(va);
	if (k < 0)
		return n;
	return (size_t)n + (size_t)k >= size ? (int)(size - 1) : n + k;
}
#define AP(buf, n, ...) ((n) = ap_((buf), sizeof(buf), (n), __VA_ARGS__))

#define ROUND_MS	30000
#define REPLY_MS	5000

static void clean(char *dst, size_t dsz, const uint8_t *s, uint16_t n)
{
	size_t k = 0;
	for (uint16_t i = 0; i < n && k + 1 < dsz; i++)
		dst[k++] = (s[i] > 32 && s[i] < 127) ? (char)s[i] : '_';
	dst[k] = 0;
}

/* TLV 2: 0 = success, else the QMI error code (or 0xffff if missing) */
static unsigned qerr(const struct qmi_msg *m)
{
	uint16_t l;
	const uint8_t *p = qmi_get(m, 2, &l);
	if (!p || l < 4)
		return 0xffff;
	uint16_t r, e;
	memcpy(&r, p, 2);
	memcpy(&e, p + 2, 2);
	return r ? (e ? e : 0xfffe) : 0;
}

static const char *nm(const char *const *t, unsigned n, unsigned v)
{
	return v < n ? t[v] : "?";
}
#define NM(t, v) nm(t, sizeof(t) / sizeof(t[0]), v)

static const char *const opmode[] = { "online", "low-power", "factory-test", "offline", "resetting", "shutting-down",
				      "persistent-low-power", "mode-only-low-power", "network-test" };
static const char *const card_state[] = { "absent", "present", "error" };
static const char *const card_err[] = { "unknown", "power-down", "poll-error", "no-atr", "volt-mismatch", "parity",
					"possibly-removed", "technical", "null-bytes", "sap-connected", "cmd-timeout" };
static const char *const app_type[] = { "unknown", "sim", "usim", "ruim", "csim", "isim" };
static const char *const app_state[] = { "unknown", "detected", "pin1-required", "puk1-required", "perso-check",
					 "pin1-blocked", "illegal", "ready" };
static const char *const pin_state[] = { "unknown", "enabled-unverified", "enabled-verified", "disabled", "blocked",
					 "perm-blocked" };
static const char *const reg_state[] = { "not-registered", "registered", "searching", "denied", "unknown" };
static const char *const srv_status[] = { "none", "limited", "available", "limited-regional", "power-save" };

static const char *radio_name(int8_t r)
{
	switch ((uint8_t)r) {
	case 0: return "none";
	case 1: return "cdma1x";
	case 2: return "cdma-hrpd";
	case 3: return "amps";
	case 4: return "gsm";
	case 5: return "umts";
	case 8: return "lte";
	case 9: return "td-scdma";
	case 0x0c: return "nr5g";
	default: return "?";
	}
}

static void on_dms(uint16_t id, const struct qmi_msg *m, unsigned e)
{
	uint16_t l;
	const uint8_t *p = qmi_get(m, 1, &l);
	if (id == 0x22) {
		/* redfin's MPSS answers "0" here (seen 2026-11-05): the hardware revision (0x2c) names the chip */
		if (e)
			snprintf(model, sizeof(model), "err%#x", e);
		else if (p)
			clean(model, sizeof(model), p, l);
	} else if (id == 0x2c) {
		if (e)
			snprintf(hwrev, sizeof(hwrev), "err%#x", e);
		else if (p)
			clean(hwrev, sizeof(hwrev), p, l);
	} else if (id == 0x23) {
		if (!e && p)
			clean(rev, sizeof(rev), p, l);
	} else if (id == 0x25) {
		const uint8_t *q = qmi_get(m, 0x11, &l);	/* IMEI: counted, never printed or stored */
		if (e)
			snprintf(imei, sizeof(imei), "err%#x", e);
		else if (q && l)
			snprintf(imei, sizeof(imei), "set(%u)", l);
		else
			snprintf(imei, sizeof(imei), "none");
	} else if (id == 0x2d) {
		uint8_t v;
		if (e || qmi_get_u8(m, 1, &v))
			snprintf(f_mode, sizeof(f_mode), "mode=err%#x", e);
		else {
			snprintf(f_mode, sizeof(f_mode), "mode=%s(%u)", NM(opmode, v), v);
			cur_mode = v;
		}
	}
}

/*
 * UIM Get Card Status (0x2f), TLV 0x10 (libqmi "Card Status"): 4 u16 session indexes, then cards[u8]:
 *   card_state, upin_state, upin_retries, upuk_retries, error_code, applications[u8]:
 *     type, state, perso_state, perso_feature, perso_retries, perso_unblock_retries, aid[u8],
 *     upin_replaces_pin1, pin1_state, pin1_retries, puk1_retries, pin2_state, pin2_retries, puk2_retries
 * These are LOGICAL slots: on redfin one, mapped to either the physical tray or the eUICC (Get Slots Status
 * says which). A present card with no applications is an eUICC with no enabled profile (or a card still
 * being read). Every card and every application is printed now (the first version stopped at card 1).
 */
/* the application whose PIN1 the SIM PIN UI unlocks (filled from Get Card Status; see the PIN section) */
static struct {
	int known;			/* 1 = this round's card status had a usable application */
	uint8_t slot;			/* logical card 1.. */
	uint8_t state, pin1, retries, puk_retries;
	uint8_t aid_len, aid[32];
	int prov;			/* it is the primary GW provisioning application (session type 0 works) */
} papp;

static void on_uim(const struct qmi_msg *m, unsigned e)
{
	uint16_t l;
	const uint8_t *p = qmi_get(m, 0x10, &l);
	papp.known = 0;
	if (e || !p || l < 9) {
		snprintf(f_sim, sizeof(f_sim), "sim=err%#x", e);
		return;
	}
	uint16_t gw_primary;		/* Index GW Primary: slot << 8 | application, 0xffff = none */
	memcpy(&gw_primary, p, 2);
	uint32_t o = 8, ncards = p[o++];
	int n = snprintf(f_sim, sizeof(f_sim), "sim=");
	if (!ncards)
		AP(f_sim, n, "no-cards");
	for (uint32_t c = 0; c < ncards && n < (int)sizeof(f_sim) - 1; c++) {
		if (o + 6 > l) {
			AP(f_sim, n, "%sshort", c ? ";" : "");
			break;
		}
		uint8_t cs = p[o], err = p[o + 4], napps = p[o + 5];
		o += 6;
		AP(f_sim, n, "%sL%u:%s", c ? ";" : "", c + 1, NM(card_state, cs));
		if (cs == 2)
			AP(f_sim, n, "(%s,%u)", NM(card_err, err), err);
		if (cs == 1 && !napps)
			AP(f_sim, n, " apps=0");
		for (uint8_t a = 0; a < napps; a++) {
			if (o + 7 > l) {
				AP(f_sim, n, " short");
				o = l;
				break;
			}
			uint8_t type = p[o], st = p[o + 1], aidl = p[o + 6];
			uint32_t q = o + 7 + aidl;	/* upin_replaces_pin1, pin1_state, pin1_retries, puk1_retries, ... */
			AP(f_sim, n, " %s:%s", NM(app_type, type), NM(app_state, st));
			if (q + 4 <= l) {
				AP(f_sim, n, "/pin1=%s(%u)", NM(pin_state, p[q + 1]), p[q + 2]);
				int prov = gw_primary != 0xffff && (gw_primary >> 8) == c && (gw_primary & 0xff) == a;
				/* sim/usim only; the provisioning application wins over an earlier one */
				if ((type == 1 || type == 2) && (!papp.known || (prov && !papp.prov)) && aidl <= sizeof(papp.aid)) {
					papp.known = 1;
					papp.slot = (uint8_t)(c + 1);
					papp.state = st;
					papp.pin1 = p[q + 1];
					papp.retries = p[q + 2];
					papp.puk_retries = p[q + 3];
					papp.aid_len = aidl;
					memcpy(papp.aid, p + o + 7, aidl);
					papp.prov = prov;
				}
			}
			o = q + 7;
		}
	}
	const uint8_t *hs = qmi_get(m, 0x11, &l);	/* "Hot Swap": count, then a flag per card */
	if (hs && l >= 2 && n < (int)sizeof(f_sim) - 1)
		AP(f_sim, n, " hotswap=%u", hs[1]);
}

/*
 * UIM Get Slots Status (0x47): PHYSICAL slots. TLV 0x10 "Physical Slot Status" [u8]: card u32 (0 unknown,
 * 1 absent, 2 present), slot u32 (0 inactive, 1 active), logical slot u8, iccid[u8] (only counted);
 * TLV 0x11 "Physical Slot Information" [u8]: protocol u32 (0 unknown, 1 icc, 2 uicc), valid apps u8,
 * atr[u8], is_euicc u8. Shown as "slots=P1:present,active,L1,uicc,apps1|P2:euicc,present,inactive,...".
 */
static void on_slots(const struct qmi_msg *m, unsigned e)
{
	uint16_t l, l2 = 0;
	const uint8_t *p = qmi_get(m, 0x10, &l), *x = qmi_get(m, 0x11, &l2);
	if (e || !p || l < 1) {
		snprintf(f_slots, sizeof(f_slots), "slots=err%#x", e);
		return;
	}
	static const char *const cardst[] = { "unknown", "absent", "present" };
	static const char *const proto[] = { "?", "icc", "uicc" };
	int n = snprintf(f_slots, sizeof(f_slots), "slots=");
	p1_known = 0;
	uint32_t o = 1, xo = 1, nx = x && l2 ? x[0] : 0;
	for (uint32_t i = 0; i < p[0] && o + 10 <= l && n < (int)sizeof(f_slots) - 1; i++) {
		uint32_t card, act;
		memcpy(&card, p + o, 4);
		memcpy(&act, p + o + 4, 4);
		uint8_t logical = p[o + 8], il = p[o + 9];
		o += 10 + il;
		int euicc = -1, apps = -1;
		uint32_t pr = 0;
		if (i < nx && xo + 6 <= l2) {
			memcpy(&pr, x + xo, 4);
			apps = x[xo + 4];
			uint8_t atrl = x[xo + 5];
			if (xo + 6 + atrl + 1 <= l2)
				euicc = x[xo + 6 + atrl];
			xo += 6 + atrl + 1;
		}
		if (i == 0) {
			p1_known = 1;
			p1_present = card == 2 && euicc != 1;
			p1_active = act == 1;
		}
		AP(f_slots, n, "%sP%u:%s%s,%s,L%u", i ? "|" : "", i + 1,
			      euicc == 1 ? "euicc," : "", NM(cardst, card), act == 1 ? "active" : "inactive", logical);
		if (apps >= 0)
			AP(f_slots, n, ",%s,apps%d", NM(proto, pr), apps);
		if (il)
			AP(f_slots, n, ",iccid(%u)", il);
	}
}

static void on_nas(uint16_t id, const struct qmi_msg *m, unsigned e)
{
	uint16_t l;
	if (id == 0x24) {
		const uint8_t *p = qmi_get(m, 1, &l);
		if (e || !p || l < 5) {
			snprintf(f_reg, sizeof(f_reg), "reg=err%#x", e);
			return;
		}
		int n = snprintf(f_reg, sizeof(f_reg), "reg=%s cs=%u ps=%u radio=", NM(reg_state, p[0]), p[1], p[2]);
		uint8_t nr = p[4];
		if (!nr)
			AP(f_reg, n, "-");
		for (uint8_t i = 0; i < nr && 5u + i < l; i++)
			AP(f_reg, n, "%s%s", i ? "+" : "", radio_name((int8_t)p[5 + i]));
		const uint8_t *r = qmi_get(m, 0x10, &l);
		if (r && l)
			AP(f_reg, n, " roam=%s", r[0] == 0 ? "on" : r[0] == 1 ? "off" : "?");
		const uint8_t *pl = qmi_get(m, 0x12, &l);	/* network (not subscriber) identity: MCC/MNC + name */
		if (pl && l >= 5) {
			uint16_t mcc, mnc;
			char name[40];
			memcpy(&mcc, pl, 2);
			memcpy(&mnc, pl + 2, 2);
			uint16_t dl = pl[4] < l - 5 ? pl[4] : (uint16_t)(l - 5);
			clean(name, sizeof(name), pl + 5, dl);
			AP(f_reg, n, " plmn=%03u-%02u '%s'", mcc, mnc, name);
		}
	} else if (id == 0x4d) {
		static const struct { uint8_t t; const char *n; } ts[] = { { 0x12, "gsm" }, { 0x13, "wcdma" }, { 0x14, "lte" }, { 0x4a, "nr5g" } };
		int n = snprintf(f_svc, sizeof(f_svc), "svc=");
		if (e) {
			snprintf(f_svc, sizeof(f_svc), "svc=err%#x", e);
			return;
		}
		int any = 0;
		for (unsigned i = 0; i < sizeof(ts) / sizeof(ts[0]); i++) {
			const uint8_t *p = qmi_get(m, ts[i].t, &l);
			if (p && l)
				AP(f_svc, n, "%s%s:%s", any++ ? "," : "", ts[i].n, NM(srv_status, p[0]));
		}
		if (!any)
			AP(f_svc, n, "-");
	} else if (id == 0x4f) {
		bars = 0;
		if (e) {
			snprintf(f_sig, sizeof(f_sig), "sig=none(err%#x)", e);
			return;
		}
		int n = snprintf(f_sig, sizeof(f_sig), "sig=");
		int any = 0;
		const uint8_t *p;
		if ((p = qmi_get(m, 0x14, &l)) && l >= 6) {	/* LTE: rssi i8, rsrq i8, rsrp i16, snr i16 (0.1 dB) */
			int16_t rsrp, snr;
			memcpy(&rsrp, p + 2, 2);
			memcpy(&snr, p + 4, 2);
			AP(f_sig, n, "lte rssi=%d rsrq=%d rsrp=%d snr=%d.%d", (int8_t)p[0], (int8_t)p[1], rsrp,
				      snr / 10, abs(snr % 10));
			bars = rsrp >= -95 ? 4 : rsrp >= -105 ? 3 : rsrp >= -115 ? 2 : rsrp >= -125 ? 1 : 0;
			any = 1;
		}
		if ((p = qmi_get(m, 0x17, &l)) && l >= 4) {	/* 5G: rsrp i16, snr i16 */
			int16_t rsrp, snr;
			memcpy(&rsrp, p, 2);
			memcpy(&snr, p + 2, 2);
			AP(f_sig, n, "%snr5g rsrp=%d snr=%d", any ? " " : "", rsrp, snr);
			any = 1;
		}
		if ((p = qmi_get(m, 0x13, &l)) && l >= 1) {
			AP(f_sig, n, "%swcdma rssi=%d", any ? " " : "", (int8_t)p[0]);
			any = 1;
		}
		if ((p = qmi_get(m, 0x12, &l)) && l >= 1) {
			AP(f_sig, n, "%sgsm rssi=%d", any ? " " : "", (int8_t)p[0]);
			any = 1;
		}
		if (!any)
			AP(f_sig, n, "none");
	}
}

/*
 * WDS (wds=1 only; data design X0): read-only. Layouts from libqmi data/qmi-service-wds.json:
 *   0x22 Get Packet Service Status: out 0x01 u8 (0 unknown, 1 disconnected, 2 connected, 3 suspended, 4 authenticating)
 *   0x44 Get Current Data Bearer Technology: out 0x01 Current / 0x10 Last {network type u8 (1 3gpp2, 2 3gpp),
 *        rat mask u32, so mask u32}; out of a call the modem answers an error (expected) and may still give Last
 *   0x2a Get Profile List: in 0x10 profile type u8 (0 = 3GPP); out 0x01 [u8] {type u8, index u8, name[u8]}
 *   0x49 Get Default Profile Number: in 0x01 {type u8 0 = 3GPP, family u8 0 = embedded}; out 0x01 index u8
 *   0x2b Get Profile Settings: in 0x01 {type u8, index u8}; out 0x10 name, 0x11 PDP type u8, 0x14 APN (strings
 *        fill their TLV), 0x1d auth u8 (bit 0 PAP, bit 1 CHAP), 0x3e roaming disallowed u8. Never read: 0x1b
 *        username, 0x1c password.
 */
#define WDS_MAXPROF	16
static struct {
	uint8_t index, have;		/* have: 1 = settings read without an error */
	uint8_t pdp, auth, roam_dis;	/* 0xff = TLV absent */
	char apn[64], name[40];
} wprof[WDS_MAXPROF];
static int wds_nprof = -1, wds_next, wds_default = -1;
static char w_ps[40] = "?", w_bearer[64] = "?", w_list[24] = "?", w_def[24] = "?";

static const char *const wds_conn[] = { "unknown", "disconnected", "connected", "suspended", "authenticating" };
static const char *const wds_pdp[] = { "ipv4", "ppp", "ipv6", "ipv4v6", "non-ip" };

/* forget the profile table (cell.on changed, or the WDS server came back): settings are read again */
static void wds_reset(void)
{
	memset(wprof, 0, sizeof(wprof));
	wds_nprof = -1;
	wds_next = 0;
	wds_default = -1;
	snprintf(w_ps, sizeof(w_ps), "?");
	snprintf(w_bearer, sizeof(w_bearer), "?");
	snprintf(w_list, sizeof(w_list), "?");
	snprintf(w_def, sizeof(w_def), "?");
}

/* one bearer TLV {network type, rat mask, so mask} as a short name */
static void wds_bearer_name(char *dst, size_t dsz, const uint8_t *p, uint16_t l)
{
	if (!p || l < 9) {
		snprintf(dst, dsz, "?");
		return;
	}
	uint32_t rat;
	memcpy(&rat, p + 1, 4);
	if (p[0] == 1) {
		snprintf(dst, dsz, "3gpp2:%#x", rat);
		return;
	}
	if (p[0] != 2) {
		snprintf(dst, dsz, "unknown");
		return;
	}
	static const struct { uint32_t bit; const char *n; } r[] = {
		{ 1u << 10, "nr5g" }, { 1u << 5, "lte" }, { 1u << 7, "dc-hsdpa+" }, { 1u << 6, "hsdpa+" }, { 1u << 3, "hsupa" },
		{ 1u << 2, "hsdpa" }, { 1u << 0, "wcdma" }, { 1u << 9, "td-scdma" }, { 1u << 4, "edge" }, { 1u << 1, "gprs" },
		{ 1u << 15, "null-bearer" },
	};
	for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); i++)
		if (rat & r[i].bit) {
			snprintf(dst, dsz, "%s", r[i].n);
			return;
		}
	if (rat)
		snprintf(dst, dsz, "3gpp:%#x", rat);
	else
		snprintf(dst, dsz, "none");
}

static void wds_log_profile(int i, unsigned e)
{
	if (e) {
		printf("[%6llu] cell: wds profile %u err%#x\n", (unsigned long long)link_ms(), wprof[i].index, e);
		return;
	}
	static const char *const auth[] = { "none", "pap", "chap", "pap+chap" };
	printf("[%6llu] cell: wds profile %u apn='%s' pdp=%s auth=%s name='%s' roaming=%s%s\n", (unsigned long long)link_ms(),
	       wprof[i].index, wprof[i].apn, wprof[i].pdp == 0xff ? "?" : NM(wds_pdp, wprof[i].pdp),
	       wprof[i].auth == 0xff ? "?" : NM(auth, wprof[i].auth & 3), wprof[i].name,
	       wprof[i].roam_dis == 0xff ? "?" : wprof[i].roam_dis ? "disallowed" : "allowed",
	       (int)wprof[i].index == wds_default ? " (default)" : "");
}

/* 1 = this round's 0x2b step still has a profile to read */
static int wds_more(void)
{
	return wds_nprof > 0 && wds_next < wds_nprof;
}

/* the request TLVs of a WDS step (none for 0x22/0x44) */
static void wds_tlvs(struct qmi_buf *q, uint16_t id)
{
	if (id == 0x2a)
		qmi_tlv_u8(q, 0x10, 0);			/* 3GPP */
	else if (id == 0x49) {
		uint8_t t[2] = { 0, 0 };		/* 3GPP, embedded */
		qmi_tlv(q, 1, t, 2);
	} else if (id == 0x2b) {
		uint8_t t[2] = { 0, wds_more() ? wprof[wds_next].index : 0 };
		qmi_tlv(q, 1, t, 2);
	}
}

/* an answer to a WDS step; returns 1 if the same step is to be sent again (more profiles) */
static int on_wds(uint16_t id, const struct qmi_msg *m, unsigned e)
{
	uint16_t l;
	const uint8_t *p;
	if (id == 0x22) {
		uint8_t v;
		if (e || qmi_get_u8(m, 1, &v))
			snprintf(w_ps, sizeof(w_ps), "err%#x", e);
		else
			snprintf(w_ps, sizeof(w_ps), "%s", NM(wds_conn, v));
	} else if (id == 0x44) {
		char cur[24], last[24];
		p = qmi_get(m, 0x10, &l);
		wds_bearer_name(last, sizeof(last), p, p ? l : 0);
		if (!e) {
			p = qmi_get(m, 1, &l);
			wds_bearer_name(cur, sizeof(cur), p, p ? l : 0);
			snprintf(w_bearer, sizeof(w_bearer), "%s", cur);
		} else if (strcmp(last, "?"))
			snprintf(w_bearer, sizeof(w_bearer), "none(err%#x,last=%s)", e, last);
		else
			snprintf(w_bearer, sizeof(w_bearer), "none(err%#x)", e);
	} else if (id == 0x2a) {
		p = qmi_get(m, 1, &l);
		if (e || !p || l < 1) {
			snprintf(w_list, sizeof(w_list), "err%#x", e);
			return 0;		/* the table read before (if any) is kept */
		}
		uint8_t idx[WDS_MAXPROF];
		int n = 0;
		for (uint32_t o = 1, i = 0; i < p[0] && o + 3 <= l && n < WDS_MAXPROF; i++) {
			idx[n++] = p[o + 1];	/* type (o), index (o + 1), name length (o + 2), name */
			o += 3u + p[o + 2];
		}
		int same = n == wds_nprof;
		for (int i = 0; same && i < n; i++)
			same = wprof[i].index == idx[i];
		if (!same) {			/* first list, or the modem's list changed: read every profile again */
			memset(wprof, 0, sizeof(wprof));
			for (int i = 0; i < n; i++)
				wprof[i].index = idx[i];
			wds_nprof = n;
			wds_next = 0;
			printf("[%6llu] cell: wds %d 3gpp profile%s listed%s\n", (unsigned long long)link_ms(), n, n == 1 ? "" : "s",
			       p[0] > n ? " (more not kept)" : "");
		}
		snprintf(w_list, sizeof(w_list), "%d", n);
	} else if (id == 0x49) {
		uint8_t v;
		if (e || qmi_get_u8(m, 1, &v)) {
			snprintf(w_def, sizeof(w_def), "err%#x", e);
			wds_default = -1;
		} else {
			snprintf(w_def, sizeof(w_def), "%u", v);
			wds_default = v;
		}
	} else if (id == 0x2b) {
		if (!wds_more())
			return 0;
		int i = wds_next++;
		wprof[i].have = !e;
		wprof[i].pdp = wprof[i].auth = wprof[i].roam_dis = 0xff;
		if (!e) {
			if ((p = qmi_get(m, 0x14, &l)))
				clean(wprof[i].apn, sizeof(wprof[i].apn), p, l);	/* clamped to 63 chars */
			if ((p = qmi_get(m, 0x10, &l)))
				clean(wprof[i].name, sizeof(wprof[i].name), p, l);
			if ((p = qmi_get(m, 0x11, &l)) && l >= 1)
				wprof[i].pdp = p[0];
			if ((p = qmi_get(m, 0x1d, &l)) && l >= 1)
				wprof[i].auth = p[0];
			if ((p = qmi_get(m, 0x3e, &l)) && l >= 1)
				wprof[i].roam_dis = p[0];
		}
		wds_log_profile(i, e);
		return wds_more();
	}
	return 0;
}

/* the status line's WDS part: "wds=... bearer=... profiles=N default=K apn='...' pdp=..." */
static void wds_field(char *dst, size_t dsz)
{
	const char *apn = "?", *pdp = "?";
	for (int i = 0; i < wds_nprof && i < WDS_MAXPROF; i++)
		if ((int)wprof[i].index == wds_default && wprof[i].have) {
			apn = wprof[i].apn;
			pdp = wprof[i].pdp == 0xff ? "?" : NM(wds_pdp, wprof[i].pdp);
		}
	snprintf(dst, dsz, "wds=%s bearer=%s profiles=%s default=%s apn='%s' pdp=%s", srv[SVC_WDS].up ? w_ps : "no-server",
		 w_bearer, w_list, w_def, apn, pdp);
}

static void publish(void)
{
	char line[1536], wds[256] = "";
	if (!strstr(f_reg, "reg=registered"))
		bars = 0;
	if (opts & OPT_WDS) {
		wds[0] = ' ', wds[1] = '|', wds[2] = ' ';
		wds_field(wds + 3, sizeof(wds) - 3);
	}
	snprintf(line, sizeof(line), "%s %s %s | %s %s | %s bars=%d | model=%s hw=%s rev=%s imei=%s%s", f_sim, f_slots, f_mode, f_reg, f_svc, f_sig,
		 bars, model, hwrev, rev, imei, wds);
	uint64_t now = link_ms();
	if (strcmp(line, last_line) || now - last_log > 600000) {
		printf("[%6llu] cell: %s\n", (unsigned long long)now, line);
		last_log = now;
		snprintf(last_line, sizeof(last_line), "%s", line);
	}
	FILE *f = fopen(CELL_STATUS ".tmp", "w");
	if (f) {
		fprintf(f, "%s t=%llus\n", line, (unsigned long long)now / 1000);
		fclose(f);
		rename(CELL_STATUS ".tmp", CELL_STATUS);
	}
}

#define MAX_FAILS	3		/* a step left unanswered 3 times running is dropped until cell.on changes */
#define MAX_TIMEOUTS	2		/* 2 unanswered requests in one round: stop the round, pause 5 min */
#define SEND_GAP_MS	200		/* never two requests closer than this; never sent from the receive path */
#define ALLOW_AFTER_MS	15000		/* first round 15 s after Wi-Fi's WLFW bring-up ended (cell_allow) */
#define ALLOW_LATEST_MS	180000		/* or 3 min after wland's link came up, if WLFW never shows */

static void end_round(uint64_t pause)
{
	step = -1;
	publish();
	next_round = link_ms() + pause;
}

/* the next request of the round, or the end of it; called only from cell_poll */
static void send_step(void)
{
	for (; step >= 0 && step < NSTEPS; step++) {
		if (steps[step].once && done_once[step])
			continue;
		if (steps[step].opt && !(opts & steps[step].opt))
			continue;
		if (fails[step] >= MAX_FAILS)
			continue;
		int s = steps[step].svc;
		if (!srv[s].up)
			continue;
		if (s == SVC_WDS && steps[step].id == 0x2b && !wds_more())
			continue;		/* every listed profile read already (or no list yet) */
		struct qmi_buf *q = malloc(sizeof(*q));
		if (!q) {
			end_round(ROUND_MS);
			return;
		}
		want_txn = txn++;
		if (!txn)
			txn = 1;
		qmi_start(q, QMI_REQUEST, want_txn, steps[step].id);
		if (s == SVC_WDS)
			wds_tlvs(q, steps[step].id);
		size_t len = qmi_finish(q);
		qrtr_send(QRTR_TYPE_DATA, CELL_PORT, srv[s].node, srv[s].port, q->b, (uint32_t)len);
		free(q);
		sent_ms = link_ms();
		return;
	}
	end_round(ROUND_MS);
}

/* wland: Wi-Fi's WLFW bring-up is over (ok or not): cell may start 15 s later */
void cell_allow(void)
{
	if (!allowed) {
		allowed = 1;
		allow_ms = link_ms() + ALLOW_AFTER_MS;
	}
}

static int read_opts(void)
{
	char b[128] = "";
	FILE *f = fopen(CELL_ON, "r");
	if (!f)
		return -1;
	size_t k = fread(b, 1, sizeof(b) - 1, f);
	fclose(f);
	b[k] = 0;
	int o = 0;
	if (strstr(b, "slots=1"))
		o |= OPT_SLOTS;
	if (strstr(b, "hw=1"))
		o |= OPT_HW;
	if (strstr(b, "slotswitch=1"))
		o |= OPT_SWITCH | OPT_SLOTS;	/* the switch needs the slot status */
	if (strstr(b, "pin=1"))
		o |= OPT_PIN;
	if (strstr(b, "wds=1"))
		o |= OPT_WDS;
	if (strstr(b, "online=1"))
		o |= OPT_ONLINE;
	else if (strstr(b, "online=0"))
		o |= OPT_LPM;
	return o;
}

/*
 * The two writes (track T step 2), each only with its own word in cell.on, never by themselves:
 *   slotswitch=1  UIM Switch Slot 0x46 (TLV 1 logical slot u8 = 1, TLV 2 physical slot u32 = 1): once per cell.on
 *                 change, only when Get Slots Status said P1 holds a card and is inactive. The modem keeps the
 *                 mapping in its EFS (served by wland's rmtfs), so the rmtfs images are copied to *.pre-slot first
 *                 and nothing is sent if that copy fails.
 *   online=1      DMS Set Operating Mode 0x2e (TLV 1 u8: 0 online); online=0 sends 1 (low power). At most 2 tries
 *                 per cell.on change, only when Get Operating Mode reads something else; *.pre-online copies first.
 *                 An offline (3) modem refuses both (QMI: offline only goes to reset); no reset is ever sent.
 * One write at a time, in place of a round's first request, then the round 5 s later re-reads everything.
 */
static int wr_txn, wr_svc, sw_tries, mode_tries;
static uint16_t wr_id;
static uint64_t wr_sent;

static int backup_rmtfs(const char *suffix)
{
	static const char *const f[] = { "modemst1.img", "modemst2.img", "fsg.img", "fsc.img" };
	static char buf[65536];
	int copied = 0;
	for (unsigned i = 0; i < 4; i++) {
		char src[128], dst[160];
		snprintf(src, sizeof(src), "/var/local/fw/rmtfs/%s", f[i]);
		snprintf(dst, sizeof(dst), "%s.%s", src, suffix);
		if (!access(dst, F_OK))
			continue;			/* the first backup is the one kept */
		int a = open(src, O_RDONLY), b = a < 0 ? -1 : open(dst, O_WRONLY | O_CREAT | O_EXCL, 0600);
		ssize_t k = 0;
		while (b >= 0 && (k = read(a, buf, sizeof(buf))) > 0)
			if (write(b, buf, (size_t)k) != k) {
				k = -1;
				break;
			}
		int bad = a < 0 || b < 0 || k < 0 || fsync(b);
		if (a >= 0)
			close(a);
		if (b >= 0)
			close(b);
		copied++;
		if (bad) {
			printf("[%6llu] cell: backup %s failed (%s): write not sent\n", (unsigned long long)link_ms(), dst, strerror(errno));
			unlink(dst);
			return -1;
		}
	}
	if (copied)
		printf("[%6llu] cell: rmtfs images backed up as *.%s\n", (unsigned long long)link_ms(), suffix);
	return 0;
}

static int send_write(int svc, uint16_t id, const void *t1, uint16_t l1, const void *t2, uint16_t l2)
{
	struct qmi_buf *q = malloc(sizeof(*q));
	if (!q)
		return -1;
	wr_txn = txn++;
	if (!txn)
		txn = 1;
	qmi_start(q, QMI_REQUEST, (uint16_t)wr_txn, id);
	qmi_tlv(q, 1, t1, l1);
	if (t2)
		qmi_tlv(q, 2, t2, l2);
	size_t len = qmi_finish(q);
	qrtr_send(QRTR_TYPE_DATA, CELL_PORT, srv[svc].node, srv[svc].port, q->b, (uint32_t)len);
	free(q);
	wr_id = id;
	wr_svc = svc;
	wr_sent = link_ms();
	return 0;
}

/* at the start of a round: one write if one is asked for and due; 1 = sent (the round waits) */
static int maybe_write(void)
{
	uint64_t now = link_ms();
	if ((opts & OPT_SWITCH) && !sw_tries && p1_known && p1_present && !p1_active && srv[SVC_UIM].up) {
		sw_tries = 1;
		if (backup_rmtfs("pre-slot"))
			return 0;
		uint8_t logical = 1;
		uint32_t physical = 1;
		printf("[%6llu] cell: WRITE uim switch slot: logical 1 -> physical 1\n", (unsigned long long)now);
		return !send_write(SVC_UIM, 0x46, &logical, 1, &physical, 4);
	}
	int want = opts & OPT_ONLINE ? 0 : opts & OPT_LPM ? 1 : -1;
	if (want >= 0 && cur_mode >= 0 && cur_mode != want && mode_tries < 2 && srv[SVC_DMS].up) {
		mode_tries++;
		if (backup_rmtfs("pre-online"))
			return 0;
		uint8_t m = (uint8_t)want;
		printf("[%6llu] cell: WRITE dms set operating mode %s (now %d, try %d/2)\n", (unsigned long long)now,
		       want ? "low-power" : "online", cur_mode, mode_tries);
		return !send_write(SVC_DMS, 0x2e, &m, 1, NULL, 0);
	}
	return 0;
}

/*
 * SIM PIN (ws-n-pin; only with "pin=1" in cell.on): SpringBoard's SIM PIN UI (SweetPIN in SweetFPS.dylib) talks to
 * wland over a unix socket, CELL_SOCK (root:mobile 0660; peers other than uid 0 and 501 are dropped). One client at
 * a time, one request per connection, all non-blocking from cell_poll:
 *   "S\n"            -> "pin app=<state> pin1=<state> retries=N puk=N slot=N on=0|1\n" (or "pin app=none ...")
 *   'V' len digits   -> after UIM Verify PIN 0x26 answers: "ok retries=N\n" / "err=0xE retries=N\n",
 *                       or at once "refused <why> retries=N\n"
 * The PIN: 4-8 ASCII digits, held only in pin_buf until the request is built (never logged, never in a file),
 * then pin_buf and the QMI buffer are zeroed. One Verify per user entry, never repeated (a timeout is reported, not
 * retried); refused when the application is not waiting for PIN1, when PIN1 has 1 try left or fewer (a wrong PIN
 * would block the card), or when the retry count is unknown. No PUK path at all. Session: primary GW provisioning
 * (type 0, no AID) when the card status names this application as the provisioning one, else non-provisioning
 * slot N (4/5) with its AID. After any answer the next round (2 s) re-reads the card status.
 */
#define PIN_REPLY_MS	8000
#define PIN_REQ_MS	2000
static int pin_lfd = -1, pin_cfd = -1, pin_txn, pin_queued;
static uint64_t pin_cdead, pin_sent;
static uint8_t pin_req[16], pin_buf[9];
static unsigned pin_reqn, pin_len;

static void pin_zero(void *p, size_t n)
{
	volatile uint8_t *v = p;
	while (n--)
		*v++ = 0;
}

static void pin_reply(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void pin_reply(const char *fmt, ...)
{
	char b[160];
	va_list va;
	va_start(va, fmt);
	int k = vsnprintf(b, sizeof(b), fmt, va);
	va_end(va);
	if (pin_cfd >= 0) {
		if (k > 0 && write(pin_cfd, b, (size_t)k < sizeof(b) ? (size_t)k : sizeof(b) - 1) < 0)
			(void)0;
		close(pin_cfd);
		pin_cfd = -1;
	}
	pin_zero(pin_req, sizeof(pin_req));
	pin_reqn = 0;
}

static void pin_listen(int on)
{
	if (!on) {
		if (pin_lfd >= 0) {
			close(pin_lfd);
			pin_lfd = -1;
			unlink(CELL_SOCK);
		}
		if (pin_cfd >= 0)
			pin_reply("refused off\n");
		pin_zero(pin_buf, sizeof(pin_buf));
		pin_queued = 0;
		return;
	}
	if (pin_lfd >= 0)
		return;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", CELL_SOCK);
	unlink(CELL_SOCK);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	mode_t um = umask(0177);
	int bad = fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 2);
	umask(um);
	if (!bad && (chown(CELL_SOCK, 0, 501) || chmod(CELL_SOCK, 0660)))
		bad = 1;
	if (bad) {
		printf("[%6llu] cell: pin socket %s: %s\n", (unsigned long long)link_ms(), CELL_SOCK, strerror(errno));
		if (fd >= 0)
			close(fd);
		unlink(CELL_SOCK);
		return;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	pin_lfd = fd;
	printf("[%6llu] cell: pin socket %s ready\n", (unsigned long long)link_ms(), CELL_SOCK);
}

static void pin_send(void)
{
	struct qmi_buf *q = malloc(sizeof(*q));
	if (!q) {
		pin_zero(pin_buf, sizeof(pin_buf));
		pin_reply("refused nomem retries=%u\n", papp.retries);
		return;
	}
	uint8_t sess[2 + sizeof(papp.aid)], info[2 + sizeof(pin_buf)];
	uint16_t sl = 2;
	if (papp.prov || papp.slot < 1 || papp.slot > 2) {
		sess[0] = 0;		/* primary GW provisioning, no AID */
		sess[1] = 0;
	} else {
		sess[0] = (uint8_t)(3 + papp.slot);	/* non-provisioning slot 1/2 */
		sess[1] = papp.aid_len;
		memcpy(sess + 2, papp.aid, papp.aid_len);
		sl = (uint16_t)(2 + papp.aid_len);
	}
	info[0] = 1;			/* PIN1 */
	info[1] = (uint8_t)pin_len;
	memcpy(info + 2, pin_buf, pin_len);
	pin_txn = txn++;
	if (!txn)
		txn = 1;
	qmi_start(q, QMI_REQUEST, (uint16_t)pin_txn, 0x26);
	qmi_tlv(q, 1, sess, sl);
	qmi_tlv(q, 2, info, (uint16_t)(2 + pin_len));
	size_t len = qmi_finish(q);
	qrtr_send(QRTR_TYPE_DATA, CELL_PORT, srv[SVC_UIM].node, srv[SVC_UIM].port, q->b, (uint32_t)len);
	pin_zero(q, sizeof(*q));
	free(q);
	pin_zero(info, sizeof(info));
	pin_zero(pin_buf, sizeof(pin_buf));
	pin_len = 0;
	pin_queued = 0;
	pin_sent = link_ms();
	printf("[%6llu] cell: verify pin sent (slot %u, session %u, retries %u)\n", (unsigned long long)pin_sent, papp.slot,
	       sess[0], papp.retries);
}

/* a whole request is in pin_req: answer a status query, or check and queue a verify */
static void pin_request(void)
{
	const char *const *as = app_state, *const *ps = pin_state;
	if (pin_req[0] == 'S') {
		if (!papp.known)
			pin_reply("pin app=none on=%d\n", !!(opts & OPT_PIN));
		else
			pin_reply("pin app=%s pin1=%s retries=%u puk=%u slot=%u on=%d\n", nm(as, 8, papp.state), nm(ps, 6, papp.pin1),
				  papp.retries, papp.puk_retries, papp.slot, !!(opts & OPT_PIN));
		return;
	}
	unsigned n = pin_req[1];
	int digits = n >= 4 && n <= 8;
	for (unsigned i = 0; digits && i < n; i++)
		digits = pin_req[2 + i] >= '0' && pin_req[2 + i] <= '9';
	const char *why = NULL;
	if (!digits)
		why = "format";		/* 4-8 digits */
	else if (!(opts & OPT_PIN))
		why = "off";
	else if (!srv[SVC_UIM].up)
		why = "uim-down";
	else if (!papp.known)
		why = "unknown";
	else if (papp.pin1 != 1 && papp.state != 2)
		why = "not-required";	/* only enabled-unverified / pin1-required; never PUK */
	else if (papp.retries <= 1)
		why = "last-try";	/* a wrong PIN would block the card: the user decides elsewhere */
	else if (pin_queued || pin_txn)
		why = "busy";
	if (why) {
		printf("[%6llu] cell: verify pin refused (%s), retries left %u\n", (unsigned long long)link_ms(), why,
		       papp.known ? papp.retries : 0);
		pin_reply("refused %s retries=%u\n", why, papp.known ? papp.retries : 0);
		return;
	}
	memcpy(pin_buf, pin_req + 2, n);
	pin_len = n;
	pin_zero(pin_req, sizeof(pin_req));
	pin_reqn = 0;
	pin_queued = 1;			/* sent by pin_poll between rounds; the client stays connected for the answer */
}

static void pin_poll(void)
{
	uint64_t now = link_ms();
	if (pin_lfd < 0)
		return;
	if (pin_txn && now - pin_sent > PIN_REPLY_MS) {
		printf("[%6llu] cell: verify pin -> no answer in %d s (not repeated)\n", (unsigned long long)now, PIN_REPLY_MS / 1000);
		pin_txn = 0;
		pin_reply("err=timeout retries=%u\n", papp.retries);
		next_round = now + 2000;
	}
	if (pin_queued && step < 0 && !wr_id && !pin_txn) {
		if (srv[SVC_UIM].up)
			pin_send();
		else {
			pin_zero(pin_buf, sizeof(pin_buf));
			pin_queued = 0;
			pin_reply("refused uim-down retries=%u\n", papp.retries);
		}
	}
	if (pin_cfd < 0) {
		int fd = accept(pin_lfd, NULL, NULL);
		if (fd < 0)
			return;
		uid_t uid = (uid_t)-1;
		gid_t gid = (gid_t)-1;
		if (getpeereid(fd, &uid, &gid) || (uid != 0 && uid != 501)) {
			close(fd);
			return;
		}
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		pin_cfd = fd;
		pin_cdead = now + PIN_REQ_MS;
		pin_reqn = 0;
		return;
	}
	if (pin_queued || pin_txn)
		return;			/* waiting for the modem's answer */
	ssize_t k = read(pin_cfd, pin_req + pin_reqn, sizeof(pin_req) - pin_reqn);
	if (k > 0)
		pin_reqn += (unsigned)k;
	int whole = pin_reqn >= 1 && (pin_req[0] == 'S' || (pin_req[0] == 'V' && pin_reqn >= 2 && pin_reqn >= 2u + pin_req[1]));
	if (whole && pin_req[0] == 'V' && pin_req[1] > 8)
		whole = 0, k = 0;	/* too long: dropped below */
	if (whole) {
		pin_request();
		return;
	}
	if (k == 0 || (k < 0 && errno != EAGAIN) || now > pin_cdead || pin_reqn >= sizeof(pin_req)) {
		pin_reply("refused request\n");
	}
}

/* UIM 0x26's answer */
static void pin_answer(const struct qmi_msg *m)
{
	unsigned e = qerr(m);
	uint16_t l;
	const uint8_t *r = qmi_get(m, 0x10, &l);	/* Retries Remaining (on failure): verify, unblock */
	int left = r && l >= 1 ? r[0] : e ? -1 : (int)papp.retries;
	pin_txn = 0;
	if (left >= 0)
		papp.retries = (uint8_t)left;
	if (!e) {
		printf("[%6llu] cell: verify pin -> ok, retries left %d\n", (unsigned long long)link_ms(), left);
		pin_reply("ok retries=%d\n", left);
	} else {
		printf("[%6llu] cell: verify pin -> err %#x, retries left %d\n", (unsigned long long)link_ms(), e, left);
		pin_reply("err=%#x retries=%d\n", e, left);
	}
	papp.known = 0;			/* only a fresh card status may allow another try */
	next_round = link_ms() + 2000;	/* re-read: expect usim:ready */
}

void cell_server(uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
	for (int i = 0; i < NSVC; i++)
		if (svc_id[i] == service && !srv[i].up) {
			srv[i].node = node;
			srv[i].port = port;
			srv[i].up = 1;
			if (i == SVC_WDS)
				wds_reset();	/* a new modem session: its profiles are read again */
			if (!access(CELL_ON, F_OK) && (i != SVC_WDS || (opts & OPT_WDS)))
				printf("cell: %s server (instance %#x) at %u:%x\n", svc_name[i], instance, node, port);
		}
}

void cell_server_gone(uint32_t node, uint32_t port)
{
	for (int i = 0; i < NSVC; i++)
		if (srv[i].up && srv[i].node == node && (port == 0xffffffffu || srv[i].port == port)) {
			srv[i].up = 0;
			if (enabled && (i != SVC_WDS || (opts & OPT_WDS)))
				printf("cell: %s server gone\n", svc_name[i]);
		}
}

void cell_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	struct qmi_msg m;
	if (qmi_parse(d, n, &m) || m.type != QMI_RESPONSE)
		return;			/* indications: none registered for */
	if (pin_txn && m.txn == pin_txn && m.id == 0x26) {
		pin_answer(&m);
		return;
	}
	if (wr_id && m.txn == wr_txn && m.id == wr_id) {
		unsigned we = qerr(&m);
		printf("[%6llu] cell: WRITE %s %#x -> %s (err %#x)%s\n", (unsigned long long)link_ms(), svc_name[wr_svc], wr_id,
		       we ? "refused" : "ok", we, we == 0x35 ? " invalid transition: an offline modem only takes a reset" : "");
		wr_id = 0;
		cur_mode = -1;
		next_round = link_ms() + 5000;	/* re-read slots, card, mode, registration */
		return;
	}
	if (!enabled || step < 0 || step >= NSTEPS || m.txn != want_txn || m.id != steps[step].id)
		return;			/* off, or a late answer to a timed-out request */
	unsigned e = qerr(&m);
	int again = 0;
	switch (steps[step].svc) {
	case SVC_DMS: on_dms(m.id, &m, e); break;
	case SVC_UIM: if (m.id == 0x47) on_slots(&m, e); else on_uim(&m, e); break;
	case SVC_NAS: on_nas(m.id, &m, e); break;
	case SVC_WDS: again = on_wds(m.id, &m, e); break;
	}
	fails[step] = 0;	/* answered (an error answer, e.g. no signal without service, is normal) */
	if (!e)
		done_once[step] = 1;
	if (!again)		/* WDS 0x2b: the next profile goes out as the same step, after the same gap */
		step++;
	next_send = link_ms() + SEND_GAP_MS;	/* cell_poll sends the next one */
}

/*
 * From wland's idle loop (every link_poll round). Bounded work per call: at most one file check (every 10 s)
 * and one request sent. Nothing before cell_allow() + 15 s (Wi-Fi bring-up and rmtfs come first), nothing
 * at all without /var/local/cell.on.
 */
void cell_poll(void)
{
	uint64_t now = link_ms();
	if (!first_poll)
		first_poll = now ? now : 1;
	if (!allowed && now - first_poll > ALLOW_LATEST_MS) {
		allowed = 1;
		allow_ms = now;
	}
	if (now >= next_check) {
		next_check = now + 10000;
		int o = read_opts(), on = o >= 0;
		if (on != enabled || (on && o != opts)) {
			enabled = on;
			opts = on ? o : 0;
			memset(fails, 0, sizeof(fails));
			sw_tries = mode_tries = 0;
			timeouts = 0;
			wds_reset();
			printf("[%6llu] cell: %s (%s%s%s%s%s%s%s)%s; servers dms %s nas %s uim %s%s%s\n", (unsigned long long)now, on ? "on" : "off",
			       CELL_ON, opts & OPT_SLOTS ? " slots=1" : "", opts & OPT_HW ? " hw=1" : "",
			       opts & OPT_SWITCH ? " slotswitch=1" : "", opts & OPT_PIN ? " pin=1" : "", opts & OPT_ONLINE ? " online=1" : opts & OPT_LPM ? " online=0" : "",
			       opts & OPT_WDS ? " wds=1" : "", allowed ? "" : ", waiting for the Wi-Fi bring-up", srv[SVC_DMS].up ? "up" : "-",
			       srv[SVC_NAS].up ? "up" : "-", srv[SVC_UIM].up ? "up" : "-", opts & OPT_WDS ? " wds " : "",
			       opts & OPT_WDS ? (srv[SVC_WDS].up ? "up" : "-") : "");
			step = -1;
			next_round = now + 1000;
			if (!on)
				unlink(CELL_STATUS);
			pin_listen(on && (opts & OPT_PIN));
		}
	}
	pin_poll();
	if (!enabled || !allowed || now < allow_ms)
		return;
	if (wr_id) {
		if (now - wr_sent > REPLY_MS) {
			printf("[%6llu] cell: WRITE %#x unanswered in 5 s (not repeated)\n", (unsigned long long)now, wr_id);
			wr_id = 0;
			next_round = now + 5000;
		}
		return;
	}
	if (step >= 0) {
		if (step >= NSTEPS) {
			end_round(ROUND_MS);
			return;
		}
		if (next_send) {			/* the last one was answered: the next after the gap */
			if (now < next_send)
				return;
			next_send = 0;
			send_step();
			return;
		}
		if (now - sent_ms > REPLY_MS) {
			printf("[%6llu] cell: no answer from %s to %#x in 5 s\n", (unsigned long long)now, svc_name[steps[step].svc],
			       steps[step].id);
			if (++fails[step] == MAX_FAILS)
				printf("[%6llu] cell: %#x dropped until cell.on changes\n", (unsigned long long)now, steps[step].id);
			if (++timeouts >= MAX_TIMEOUTS) {
				printf("[%6llu] cell: %d requests unanswered: pausing 5 min\n", (unsigned long long)now, timeouts);
				timeouts = 0;
				end_round(300000);
				return;
			}
			step++;
			next_send = now + SEND_GAP_MS;
		}
		return;
	}
	if (now < next_round || !(srv[SVC_DMS].up || srv[SVC_NAS].up || srv[SVC_UIM].up))
		return;
	if (maybe_write())
		return;
	snprintf(f_mode, sizeof(f_mode), "mode=?");
	snprintf(f_sim, sizeof(f_sim), "sim=?");
	snprintf(f_slots, sizeof(f_slots), "%s", opts & OPT_SLOTS ? "slots=?" : "slots=off");
	snprintf(f_reg, sizeof(f_reg), "reg=?");
	snprintf(f_svc, sizeof(f_svc), "svc=?");
	snprintf(f_sig, sizeof(f_sig), "sig=?");
	if (!(opts & OPT_HW) && !done_once[3])
		snprintf(hwrev, sizeof(hwrev), "off");
	timeouts = 0;
	step = 0;
	next_send = 0;
	send_step();
}

int cell_print(void)
{
	char line[1700];
	FILE *f = fopen(CELL_STATUS, "r");
	if (!f) {
		printf("cell: no %s (%s %s; wland running?)\n", CELL_STATUS, CELL_ON, access(CELL_ON, F_OK) ? "absent" : "present");
		return 1;
	}
	while (fgets(line, sizeof(line), f))
		fputs(line, stdout);
	fclose(f);
	return 0;
}
