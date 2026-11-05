/*
 * cell: read-only QMI client for the modem's UIM (0x0b), NAS (0x03) and DMS (0x02) services, on wland's
 * QRTR link (see cell.h). One request in flight, a round every 30 s while /var/local/cell.on exists:
 *   DMS  Get Model 0x22, Get Revision 0x23, Get IDs 0x25, Get Hardware Revision 0x2c (once; IMEI only counted),
 *        Get Operating Mode 0x2d
 *   UIM  Get Slots Status 0x47 (physical tray / eUICC; ICCID only counted), Get Card Status 0x2f (logical)
 *   NAS  Get Serving System 0x24, Get System Info 0x4d, Get Signal Info 0x4f
 * Message ids and TLV layouts: libqmi data/qmi-service-{uim,nas,dms}.json (as HakiOS vayu_cell_qmi.h).
 * Nothing here writes to the modem: no Set Operating Mode, no Network Register, no PIN, no WMS/WDS/VOICE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cell.h"
#include "link.h"
#include "qmi.h"

enum { SVC_DMS, SVC_NAS, SVC_UIM, NSVC };
static const uint32_t svc_id[NSVC] = { 0x02, 0x03, 0x0b };
static const char *svc_name[NSVC] = { "dms", "nas", "uim" };
static struct { uint32_t node, port; int up; } srv[NSVC];

static const struct { int svc; uint16_t id; int once; } steps[] = {
	{ SVC_DMS, 0x22, 1 }, { SVC_DMS, 0x23, 1 }, { SVC_DMS, 0x25, 1 }, { SVC_DMS, 0x2c, 1 }, { SVC_DMS, 0x2d, 0 },
	{ SVC_UIM, 0x47, 0 }, { SVC_UIM, 0x2f, 0 },
	{ SVC_NAS, 0x24, 0 }, { SVC_NAS, 0x4d, 0 }, { SVC_NAS, 0x4f, 0 },
};
#define NSTEPS (int)(sizeof(steps) / sizeof(steps[0]))

static int enabled, step = -1, done_once[NSTEPS];
static uint16_t txn = 1, want_txn;
static uint64_t sent_ms, next_round, next_check, last_log;
static char model[48] = "?", hwrev[48] = "?", rev[128] = "?", imei[24] = "?";
static char f_mode[32], f_sim[256], f_slots[192], f_reg[160], f_svc[96], f_sig[96];
static char last_line[1280];
static int bars;

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
		else
			snprintf(f_mode, sizeof(f_mode), "mode=%s(%u)", NM(opmode, v), v);
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
static void on_uim(const struct qmi_msg *m, unsigned e)
{
	uint16_t l;
	const uint8_t *p = qmi_get(m, 0x10, &l);
	if (e || !p || l < 9) {
		snprintf(f_sim, sizeof(f_sim), "sim=err%#x", e);
		return;
	}
	uint32_t o = 8, ncards = p[o++];
	int n = snprintf(f_sim, sizeof(f_sim), "sim=");
	if (!ncards)
		n += snprintf(f_sim + n, sizeof(f_sim) - n, "no-cards");
	for (uint32_t c = 0; c < ncards && n < (int)sizeof(f_sim) - 1; c++) {
		if (o + 6 > l) {
			n += snprintf(f_sim + n, sizeof(f_sim) - n, "%sshort", c ? ";" : "");
			break;
		}
		uint8_t cs = p[o], err = p[o + 4], napps = p[o + 5];
		o += 6;
		n += snprintf(f_sim + n, sizeof(f_sim) - n, "%sL%u:%s", c ? ";" : "", c + 1, NM(card_state, cs));
		if (cs == 2)
			n += snprintf(f_sim + n, sizeof(f_sim) - n, "(%s,%u)", NM(card_err, err), err);
		if (cs == 1 && !napps)
			n += snprintf(f_sim + n, sizeof(f_sim) - n, " apps=0");
		for (uint8_t a = 0; a < napps; a++) {
			if (o + 7 > l) {
				n += snprintf(f_sim + n, sizeof(f_sim) - n, " short");
				o = l;
				break;
			}
			uint8_t type = p[o], st = p[o + 1], aidl = p[o + 6];
			uint32_t q = o + 7 + aidl;	/* upin_replaces_pin1, pin1_state, pin1_retries, puk1_retries, ... */
			n += snprintf(f_sim + n, sizeof(f_sim) - n, " %s:%s", NM(app_type, type), NM(app_state, st));
			if (q + 4 <= l)
				n += snprintf(f_sim + n, sizeof(f_sim) - n, "/pin1=%s(%u)", NM(pin_state, p[q + 1]), p[q + 2]);
			o = q + 7;
		}
	}
	const uint8_t *hs = qmi_get(m, 0x11, &l);	/* "Hot Swap": count, then a flag per card */
	if (hs && l >= 2 && n < (int)sizeof(f_sim) - 1)
		snprintf(f_sim + n, sizeof(f_sim) - n, " hotswap=%u", hs[1]);
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
		n += snprintf(f_slots + n, sizeof(f_slots) - n, "%sP%u:%s%s,%s,L%u", i ? "|" : "", i + 1,
			      euicc == 1 ? "euicc," : "", NM(cardst, card), act == 1 ? "active" : "inactive", logical);
		if (apps >= 0)
			n += snprintf(f_slots + n, sizeof(f_slots) - n, ",%s,apps%d", NM(proto, pr), apps);
		if (il)
			n += snprintf(f_slots + n, sizeof(f_slots) - n, ",iccid(%u)", il);
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
			n += snprintf(f_reg + n, sizeof(f_reg) - n, "-");
		for (uint8_t i = 0; i < nr && 5u + i < l; i++)
			n += snprintf(f_reg + n, sizeof(f_reg) - n, "%s%s", i ? "+" : "", radio_name((int8_t)p[5 + i]));
		const uint8_t *r = qmi_get(m, 0x10, &l);
		if (r && l)
			n += snprintf(f_reg + n, sizeof(f_reg) - n, " roam=%s", r[0] == 0 ? "on" : r[0] == 1 ? "off" : "?");
		const uint8_t *pl = qmi_get(m, 0x12, &l);	/* network (not subscriber) identity: MCC/MNC + name */
		if (pl && l >= 5) {
			uint16_t mcc, mnc;
			char name[40];
			memcpy(&mcc, pl, 2);
			memcpy(&mnc, pl + 2, 2);
			uint16_t dl = pl[4] < l - 5 ? pl[4] : (uint16_t)(l - 5);
			clean(name, sizeof(name), pl + 5, dl);
			snprintf(f_reg + n, sizeof(f_reg) - n, " plmn=%03u-%02u '%s'", mcc, mnc, name);
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
				n += snprintf(f_svc + n, sizeof(f_svc) - n, "%s%s:%s", any++ ? "," : "", ts[i].n, NM(srv_status, p[0]));
		}
		if (!any)
			snprintf(f_svc + n, sizeof(f_svc) - n, "-");
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
			n += snprintf(f_sig + n, sizeof(f_sig) - n, "lte rssi=%d rsrq=%d rsrp=%d snr=%d.%d", (int8_t)p[0], (int8_t)p[1], rsrp,
				      snr / 10, abs(snr % 10));
			bars = rsrp >= -95 ? 4 : rsrp >= -105 ? 3 : rsrp >= -115 ? 2 : rsrp >= -125 ? 1 : 0;
			any = 1;
		}
		if ((p = qmi_get(m, 0x17, &l)) && l >= 4) {	/* 5G: rsrp i16, snr i16 */
			int16_t rsrp, snr;
			memcpy(&rsrp, p, 2);
			memcpy(&snr, p + 2, 2);
			n += snprintf(f_sig + n, sizeof(f_sig) - n, "%snr5g rsrp=%d snr=%d", any ? " " : "", rsrp, snr);
			any = 1;
		}
		if ((p = qmi_get(m, 0x13, &l)) && l >= 1) {
			n += snprintf(f_sig + n, sizeof(f_sig) - n, "%swcdma rssi=%d", any ? " " : "", (int8_t)p[0]);
			any = 1;
		}
		if ((p = qmi_get(m, 0x12, &l)) && l >= 1) {
			n += snprintf(f_sig + n, sizeof(f_sig) - n, "%sgsm rssi=%d", any ? " " : "", (int8_t)p[0]);
			any = 1;
		}
		if (!any)
			snprintf(f_sig + n, sizeof(f_sig) - n, "none");
	}
}

static void publish(void)
{
	char line[1280];
	if (!strstr(f_reg, "reg=registered"))
		bars = 0;
	snprintf(line, sizeof(line), "%s %s %s | %s %s | %s bars=%d | model=%s hw=%s rev=%s imei=%s", f_sim, f_slots, f_mode, f_reg, f_svc, f_sig,
		 bars, model, hwrev, rev, imei);
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

static void send_step(void)
{
	for (; step < NSTEPS; step++) {
		if (steps[step].once && done_once[step])
			continue;
		int s = steps[step].svc;
		if (!srv[s].up)
			continue;
		struct qmi_buf *q = malloc(sizeof(*q));
		if (!q)
			return;
		want_txn = txn++;
		if (!txn)
			txn = 1;
		qmi_start(q, QMI_REQUEST, want_txn, steps[step].id);
		size_t len = qmi_finish(q);
		qrtr_send(QRTR_TYPE_DATA, CELL_PORT, srv[s].node, srv[s].port, q->b, (uint32_t)len);
		free(q);
		sent_ms = link_ms();
		return;
	}
	step = -1;
	publish();
	next_round = link_ms() + ROUND_MS;
}

void cell_server(uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
	for (int i = 0; i < NSVC; i++)
		if (svc_id[i] == service && !srv[i].up) {
			srv[i].node = node;
			srv[i].port = port;
			srv[i].up = 1;
			if (!access(CELL_ON, F_OK))
				printf("cell: %s server (instance %#x) at %u:%x\n", svc_name[i], instance, node, port);
		}
}

void cell_server_gone(uint32_t node, uint32_t port)
{
	for (int i = 0; i < NSVC; i++)
		if (srv[i].up && srv[i].node == node && (port == 0xffffffffu || srv[i].port == port)) {
			srv[i].up = 0;
			if (enabled)
				printf("cell: %s server gone\n", svc_name[i]);
		}
}

void cell_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	struct qmi_msg m;
	if (qmi_parse(d, n, &m) || m.type != QMI_RESPONSE)
		return;			/* indications: none registered for */
	if (step < 0 || m.txn != want_txn || m.id != steps[step].id)
		return;			/* late answer to a timed-out request */
	unsigned e = qerr(&m);
	switch (steps[step].svc) {
	case SVC_DMS: on_dms(m.id, &m, e); break;
	case SVC_UIM: if (m.id == 0x47) on_slots(&m, e); else on_uim(&m, e); break;
	case SVC_NAS: on_nas(m.id, &m, e); break;
	}
	if (!e)
		done_once[step] = 1;
	step++;
	send_step();
}

void cell_poll(void)
{
	uint64_t now = link_ms();
	if (now >= next_check) {
		next_check = now + 10000;
		int on = !access(CELL_ON, F_OK);
		if (on != enabled) {
			enabled = on;
			printf("[%6llu] cell: %s (%s); servers dms %s nas %s uim %s\n", (unsigned long long)now, on ? "on" : "off", CELL_ON,
			       srv[SVC_DMS].up ? "up" : "-", srv[SVC_NAS].up ? "up" : "-", srv[SVC_UIM].up ? "up" : "-");
			step = -1;
			next_round = now + 1000;
			if (!on)
				unlink(CELL_STATUS);
		}
	}
	if (!enabled)
		return;
	if (step >= 0) {
		if (now - sent_ms > REPLY_MS) {
			const char *what = svc_name[steps[step].svc];
			printf("[%6llu] cell: no answer from %s to %#x in 5 s\n", (unsigned long long)now, what, steps[step].id);
			step++;
			send_step();
		}
		return;
	}
	if (now < next_round || !(srv[SVC_DMS].up || srv[SVC_NAS].up || srv[SVC_UIM].up))
		return;
	snprintf(f_mode, sizeof(f_mode), "mode=?");
	snprintf(f_sim, sizeof(f_sim), "sim=?");
	snprintf(f_slots, sizeof(f_slots), "slots=?");
	snprintf(f_reg, sizeof(f_reg), "reg=?");
	snprintf(f_svc, sizeof(f_svc), "svc=?");
	snprintf(f_sig, sizeof(f_sig), "sig=?");
	step = 0;
	send_step();
}

int cell_print(void)
{
	char line[1400];
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
