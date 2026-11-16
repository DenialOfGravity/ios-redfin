/*
 * pdsemu: QMI PDS emulation for locationd's Eureka GNSS back end, translated to the modem's QMI LOC (Claude 2026-11-15).
 * Switch: word pds=1 in /var/local/loc.on (loc.c). Without it nothing here runs and abpbridge's PDS policy answers as before.
 *
 * What locationd (D22 16.7, research/cc/locd/locationd) does, from the binary (project folder
 * logs/research/gnss-plan-2026-11-15.md, section "PDS session and layouts (ws-gnss-c)"):
 *  - Session start, CLEurekaController::startInternal lambda @0x1011660b4: SET_DEFAULT_TRACKING_SESSION 0x002a
 *    (TLV 0x01 {u8 mode 0, u8 timeout 255, u32 interval 1, u32 accuracy 400}, ctor @0x1012a2e78), then
 *    SET_AUTO_TRACKING_STATE 0x0031 (TLV 0x01 u8 1, ctor @0x1012a2eec), then Apple 0x5558 (policy answers it).
 *    Stop (stopInternal lambda @0x101166330): SET_AUTO_TRACKING_STATE 0. lock/unlockGps: SET_GPS_SERVICE_STATE 0x0021.
 *    No START_TRACKING_SESSION exists; locationd does send SET_EVENT_REPORT 0x0001 once at init.
 *  - Fixes: EVENT_REPORT_IND 0x0001 (ind handler registered @0x101151b74), CLEurekaController::handlePdsEventReport
 *    @0x101159b54 reads TLV 0x12 session status (default 9 = absent) and TLV 0x13 parsedPositionData;
 *    handlePositionData @0x10115db80 takes the fix only when status == 0 (cbnz w21) and lat/lon were valid.
 *    The fix's operating mode byte (TLV 0x13 offset 0x68) must be 0, 1 or 2 ("InvalidFixMode" for 3/0xff).
 *  - TLV 0x13 is exactly 105 bytes (legacy unpackTlvToFixData @0x1004f09ec: cmp len, #0x69), packed little endian:
 *      0x00 u32 valid mask   0x04 u16 year  0x06 u8 month  0x07 u8 day of week  0x08 u8 day  0x09 u8 hour
 *      0x0a u8 minute  0x0b u8 second  0x0c u16 ms  0x0e u8 leap seconds  0x0f u64 UTC ms since 1970 (x 0.001 minus
 *      kCFAbsoluteTimeIntervalSince1970 @0x10115b134)  0x17 u32 time uncertainty  0x1b f64 latitude  0x23 f64 longitude
 *      0x2b f32 alt ellipsoid  0x2f f32 alt MSL  0x33 f32 h speed  0x37 f32 v speed  0x3b f32 heading
 *      0x3f f32 h unc circular  0x43 semi-major  0x47 semi-minor  0x4b azimuth  0x4f f32 v unc  0x53 f32 h vel unc
 *      0x57 f32 v vel unc  0x5b u8 h confidence  0x5c f32 PDOP  0x60 HDOP  0x64 VDOP  0x68 u8 operating mode
 *    Valid mask bits as used by handleParsedPositionData @0x10115af88: 4|5 lat/lon, 6 alt ell, 7 alt MSL (undulation),
 *    8 h speed (+ h vel unc), 10 heading, 11-15 h unc circ/semi-major/semi-minor/azimuth/v unc, 18 h confidence;
 *    all of 0x4f8ff + time unc <= 249 updates locationd's leap seconds.
 *  - TLV 0x17 satellite info is only logged by handleSatelliteInfo @0x10115b6a4: not sent.
 * libqmi's public PDS schema has 0x12 (u8 QmiPdsPositionSessionStatus) but no 0x13; numbering of the status values
 * 0-7 is the same in PDS and LOC (0 success, 1 in progress, 2 failure, 3 timeout, 4 user end, 5 bad param,
 * 6 offline, 7 engine locked; PDS 8 = E911).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "pdsemu.h"

static void put(uint8_t *b, uint32_t *o, const void *v, unsigned l)
{
	memcpy(b + *o, v, l);
	*o += l;
}

static void tlv(uint8_t *b, uint32_t *o, uint8_t t, const void *v, uint16_t l)
{
	b[(*o)++] = t;
	b[(*o)++] = (uint8_t)l;
	b[(*o)++] = (uint8_t)(l >> 8);
	put(b, o, v, l);
}

static const uint8_t *find(const uint8_t *p, uint32_t n, unsigned id, uint16_t *len)
{
	for (uint32_t i = 0; i + 3 <= n;) {
		uint16_t l = (uint16_t)(p[i + 1] | (p[i + 2] << 8));
		if (i + 3 + l > n)
			return NULL;
		if (p[i] == id) {
			*len = l;
			return p + i + 3;
		}
		i += 3 + l;
	}
	return NULL;
}

void pdsemu_init(struct pds_state *p)
{
	memset(p, 0, sizeof(*p));
	p->svc_on = 1;
	p->def_interval = 1;
}

static void ok(struct pds_answer *a, const char *what)
{
	memset(a, 0, sizeof(*a));
	a->what = what;
}

int pdsemu_request(struct pds_state *p, const struct pds_client *c, const uint8_t *s, uint32_t sn, struct pds_answer *a)
{
	if (sn < 7 || s[0] != 0)
		return 0;
	unsigned msg = s[3] | (s[4] << 8), ml = s[5] | (s[6] << 8);
	const uint8_t *tl = s + 7;
	uint32_t tn = sn - 7 < ml ? sn - 7 : ml;
	uint16_t l = 0;
	const uint8_t *v;
	switch (msg) {
	case PDS_EVENT_REPORT:
		ok(a, "SET_EVENT_REPORT: noted");
		for (unsigned t = 0x10; t < 0x30; t++)
			if ((v = find(tl, tn, t, &l)) && l >= 1 && v[0])
				p->evmask |= 1u << (t - 0x10);
		break;
	case PDS_GET_SVC_STATE:
		ok(a, "GET_GPS_SERVICE_STATE: from the bridge's state");
		a->tl[0] = 0x01;
		a->tl[1] = 2;
		a->tl[2] = 0;
		a->tl[3] = (uint8_t)(p->svc_on != 0);
		a->tl[4] = p->auto_on && p->svc_on ? 2 : 1;	/* tracking session: 1 inactive, 2 active */
		a->tn = 5;
		break;
	case PDS_SET_SVC_STATE:
		ok(a, "SET_GPS_SERVICE_STATE: noted");
		if ((v = find(tl, tn, 0x01, &l)) && l >= 1)
			p->svc_on = v[0] != 0;
		break;
	case PDS_GET_DEF_TRACKING: {
		ok(a, "GET_DEFAULT_TRACKING_SESSION: last set values");
		uint32_t o = 0;
		uint8_t b[10];
		b[0] = p->def_mode;
		b[1] = p->have_def ? p->def_timeout : 255;
		memcpy(b + 2, &p->def_interval, 4);
		memcpy(b + 6, &p->def_accuracy, 4);
		tlv(a->tl, &o, 0x01, b, 10);
		a->tn = (uint16_t)o;
		break;
	}
	case PDS_SET_DEF_TRACKING:
		ok(a, "SET_DEFAULT_TRACKING_SESSION: noted");
		if ((v = find(tl, tn, 0x01, &l)) && l >= 10) {
			p->def_mode = v[0];
			p->def_timeout = v[1];
			memcpy(&p->def_interval, v + 2, 4);
			memcpy(&p->def_accuracy, v + 6, 4);
			p->have_def = 1;
		}
		break;
	case PDS_GET_AUTO_TRACKING:
		ok(a, "GET_AUTO_TRACKING_STATE: from the bridge's state");
		a->tl[0] = 0x01;
		a->tl[1] = 1;
		a->tl[2] = 0;
		a->tl[3] = (uint8_t)(p->auto_on != 0);
		a->tn = 4;
		break;
	case PDS_SET_AUTO_TRACKING:
		ok(a, "SET_AUTO_TRACKING_STATE: starts/stops the LOC session");
		if ((v = find(tl, tn, 0x01, &l)) && l >= 1) {
			p->auto_on = v[0] != 0;
			p->cancelled = 0;
		}
		break;
	case PDS_CANCEL_SESSION:
		ok(a, "CANCEL_ONGOING_SESSION: stops the LOC session");
		p->cancelled = 1;
		break;
	default:
		return 0;
	}
	if (c && c->valid)
		p->client = *c;
	p->nreq++;
	return 1;
}

int pdsemu_want_session(const struct pds_state *p, uint32_t *interval_ms)
{
	uint32_t i = p->def_interval;
	if (i == 0)
		i = 1000;
	else if (i < 1000)
		i *= 1000;	/* locationd sends 1: seconds */
	if (i > 60000)
		i = 60000;
	if (interval_ms)
		*interval_ms = i;
	return p->auto_on && p->svc_on && !p->cancelled && p->client.valid;
}

void pdsemu_client_gone(struct pds_state *p, uint32_t pipe, uint32_t sess)
{
	if (p->client.valid && p->client.pipe == pipe && p->client.sess == sess) {
		p->client.valid = 0;
		p->auto_on = 0;
	}
}

int loc_parse_position(const uint8_t *tl, uint32_t tn, struct loc_fix *f)
{
	uint16_t l;
	const uint8_t *v;
	memset(f, 0, sizeof(*f));
	if (!(v = find(tl, tn, 0x01, &l)) || l != 4)
		return -1;
	memcpy(&f->status, v, 4);
	if ((v = find(tl, tn, 0x02, &l)) && l == 1)
		f->session = v[0];
#define F(id, has, field, sz) if ((v = find(tl, tn, id, &l)) && l == (sz)) { memcpy(&f->field, v, sz); f->has = 1; }
	F(0x10, has_lat, lat, 8)
	F(0x11, has_lon, lon, 8)
	F(0x12, has_unc, unc, 4)
	F(0x13, has_semimin, semimin, 4)
	F(0x14, has_semimaj, semimaj, 4)
	F(0x15, has_azim, azim, 4)
	F(0x16, has_hconf, hconf, 1)
	F(0x18, has_speed, speed, 4)
	F(0x19, has_speedunc, speedunc, 4)
	F(0x1a, has_alt_ell, alt_ell, 4)
	F(0x1b, has_alt_msl, alt_msl, 4)
	F(0x1c, has_vunc, vunc, 4)
	F(0x1f, has_vspeed, vspeed, 4)
	F(0x20, has_heading, heading, 4)
	F(0x25, has_utc, utc_ms, 8)
	F(0x26, has_leap, leap, 1)
	F(0x28, has_tunc, tunc, 4)
#undef F
	if ((v = find(tl, tn, 0x23, &l)) && l == 4)
		memcpy(&f->tech, v, 4);
	if ((v = find(tl, tn, 0x24, &l)) && l == 12) {
		memcpy(&f->pdop, v, 4);
		memcpy(&f->hdop, v + 4, 4);
		memcpy(&f->vdop, v + 8, 4);
		f->has_dop = 1;
	}
	if ((v = find(tl, tn, 0x27, &l)) && l == 6) {
		memcpy(&f->gps_week, v, 2);
		memcpy(&f->gps_tow_ms, v + 2, 4);
		f->has_gpstime = 1;
	}
	return 0;
}

uint32_t pdsemu_parsed_position(const struct loc_fix *f, uint8_t v[PDS_FIX_TLV_LEN])
{
	uint32_t m = 0;
	memset(v, 0, PDS_FIX_TLV_LEN);
	if (f->has_utc) {
		time_t t = (time_t)(f->utc_ms / 1000);
		struct tm tm;
		uint16_t yr, ms = (uint16_t)(f->utc_ms % 1000);
		gmtime_r(&t, &tm);
		yr = (uint16_t)(tm.tm_year + 1900);
		memcpy(v + 0x04, &yr, 2);
		v[0x06] = (uint8_t)(tm.tm_mon + 1);
		v[0x07] = (uint8_t)tm.tm_wday;
		v[0x08] = (uint8_t)tm.tm_mday;
		v[0x09] = (uint8_t)tm.tm_hour;
		v[0x0a] = (uint8_t)tm.tm_min;
		v[0x0b] = (uint8_t)tm.tm_sec;
		memcpy(v + 0x0c, &ms, 2);
		memcpy(v + 0x0f, &f->utc_ms, 8);
		m |= 1u << 0 | 1u << 1;
	}
	if (f->has_leap) {
		v[0x0e] = f->leap;
		m |= 1u << 2;
	}
	if (f->has_tunc) {
		uint32_t u = f->tunc < 0 ? 0 : f->tunc > 4e9f ? 0xffffffffu : (uint32_t)(f->tunc + 0.5f);
		memcpy(v + 0x17, &u, 4);
		m |= 1u << 3;
	}
	if (f->has_lat && f->has_lon) {
		memcpy(v + 0x1b, &f->lat, 8);
		memcpy(v + 0x23, &f->lon, 8);
		m |= 1u << 4 | 1u << 5;
	}
#define P(bit, has, field, off) if (f->has) { memcpy(v + (off), &f->field, 4); m |= 1u << (bit); }
	P(6, has_alt_ell, alt_ell, 0x2b)
	P(7, has_alt_msl, alt_msl, 0x2f)
	P(8, has_speed, speed, 0x33)
	P(9, has_vspeed, vspeed, 0x37)
	P(10, has_heading, heading, 0x3b)
	P(11, has_unc, unc, 0x3f)
	P(12, has_semimaj, semimaj, 0x43)
	P(13, has_semimin, semimin, 0x47)
	P(14, has_azim, azim, 0x4b)
	P(15, has_vunc, vunc, 0x4f)
	P(16, has_speedunc, speedunc, 0x53)
#undef P
	if (f->has_hconf) {
		v[0x5b] = f->hconf;
		m |= 1u << 18;
	}
	if (f->has_dop) {
		memcpy(v + 0x5c, &f->pdop, 4);
		memcpy(v + 0x60, &f->hdop, 4);
		memcpy(v + 0x64, &f->vdop, 4);
		m |= 1u << 19 | 1u << 20 | 1u << 21;
	}
	v[0x68] = 0;			/* operating mode: standalone (the modem's own fix, no SUPL) */
	m |= 1u << 22;
	memcpy(v, &m, 4);
	return m;
}

uint32_t pdsemu_event_report(const struct loc_fix *f, uint8_t *out, uint32_t room)
{
	uint32_t o = 0;
	if (room < 3 + 1 + 3 + PDS_FIX_TLV_LEN)
		return 0;
	uint8_t st = f->status <= 7 ? (uint8_t)f->status : 2;
	tlv(out, &o, 0x12, &st, 1);
	if (f->status == 0 && f->has_lat && f->has_lon) {
		uint8_t v[PDS_FIX_TLV_LEN];
		pdsemu_parsed_position(f, v);
		tlv(out, &o, 0x13, v, PDS_FIX_TLV_LEN);
	} else if (f->status == 1) {
		return 0;		/* in progress, no final fix: locationd only takes status 0; nothing to say */
	}
	return o;
}

uint32_t loc_tlv_reg_events(uint8_t *out, uint64_t mask)
{
	uint32_t o = 0;
	tlv(out, &o, 0x01, &mask, 8);
	return o;
}

uint32_t loc_tlv_start(uint8_t *out, uint32_t interval_ms)
{
	uint32_t o = 0, rec = 1 /* periodic */, inter = 2 /* intermediate reports off */;
	uint8_t id = LOC_SESSION_ID;
	tlv(out, &o, 0x01, &id, 1);
	tlv(out, &o, 0x10, &rec, 4);
	tlv(out, &o, 0x12, &inter, 4);
	tlv(out, &o, 0x13, &interval_ms, 4);
	return o;
}

uint32_t loc_tlv_stop(uint8_t *out)
{
	uint32_t o = 0;
	uint8_t id = LOC_SESSION_ID;
	tlv(out, &o, 0x01, &id, 1);
	return o;
}

/* ws-gnss-f: REG_EVENTS as the QTI HAL's locClientRegisterEventMask (loc_api_v02_client.cpp, LA.UM.9.12) sends it:
 * 0x10 clientStrId (QMI string at the top level: the bytes alone, no length prefix; "HAL"), 0x11 clientType
 * (qmiLocClientTypeEnumT_v02: 1 AFW, 2 NFW, 3 privileged; absent = NFW per location_service_v02.h), 0x12
 * enablePosRequestNotification u8 (the HAL sends 0). ctype 0 = no 0x11/0x12; strid NULL = no 0x10. */
uint32_t loc_tlv_reg_events_ex(uint8_t *out, uint64_t mask, uint32_t ctype, const char *strid)
{
	uint32_t o = 0;
	uint8_t notif = 0;
	tlv(out, &o, 0x01, &mask, 8);
	if (strid)
		tlv(out, &o, 0x10, strid, (uint16_t)strlen(strid));
	if (ctype) {
		tlv(out, &o, 0x11, &ctype, 4);
		tlv(out, &o, 0x12, &notif, 1);
	}
	return o;
}

/* ws-gnss-f: START as LocApiV02::startFix fills it for a periodic GNSS request with preferred accuracy <= 100 m:
 * 0x01 sessionId 1, 0x10 fixRecurrence 1 (periodic), 0x11 horizontalAccuracyLevel 3 (HIGH), 0x13 minInterval,
 * 0x15 configAltitudeAssumed 2 (disabled), 0x18 sharePosition 1 (LocPosMode default); no 0x12 intermediate reports. */
uint32_t loc_tlv_start_android(uint8_t *out, uint32_t interval_ms)
{
	uint32_t o = 0, rec = 1, acc = 3, alt = 2;
	uint8_t id = LOC_SESSION_ID, share = 1;
	tlv(out, &o, 0x01, &id, 1);
	tlv(out, &o, 0x10, &rec, 4);
	tlv(out, &o, 0x11, &acc, 4);
	tlv(out, &o, 0x13, &interval_ms, 4);
	tlv(out, &o, 0x15, &alt, 4);
	tlv(out, &o, 0x18, &share, 1);
	return o;
}

/* ws-gnss-f: SET_OPERATION_MODE 0x01 operationMode (4 = STANDALONE; the only mode we ever send) */
uint32_t loc_tlv_opmode_standalone(uint8_t *out)
{
	uint32_t o = 0, mode = LOC_OPMODE_STANDALONE;
	tlv(out, &o, 0x01, &mode, 4);
	return o;
}

uint32_t loc_tlv_client_rev(uint8_t *out, uint32_t rev)
{
	uint32_t o = 0;
	tlv(out, &o, 0x01, &rev, 4);
	return o;
}

/* ws-gnss-g: INJECT_UTC_TIME (0x0038): 0x01 timeUtc u64 ms since 1970, 0x02 timeUnc u32 ms */
uint32_t loc_tlv_inject_utc(uint8_t *out, uint64_t utc_ms, uint32_t unc_ms)
{
	uint32_t o = 0;
	tlv(out, &o, 0x01, &utc_ms, 8);
	tlv(out, &o, 0x02, &unc_ms, 4);
	return o;
}

/* ws-gnss-k (pdsemu.h): REGISTER_MASTER_CLIENT 0x01 key */
uint32_t loc_tlv_master(uint8_t *out)
{
	uint32_t o = 0, key = LOC_MASTER_KEY;
	tlv(out, &o, 0x01, &key, 4);
	return o;
}

/* ws-gnss-k: SET_GNSS_CONSTELL_REPORT_CONFIG 0x10 measReportConfig, 0x11 svPolyReportConfig */
uint32_t loc_tlv_constell_report(uint8_t *out, uint64_t meas, uint64_t poly)
{
	uint32_t o = 0;
	tlv(out, &o, 0x10, &meas, 8);
	tlv(out, &o, 0x11, &poly, 8);
	return o;
}

/* ws-gnss-k: INJECT_XTRA_DATA / INJECT_PREDICTED_ORBITS_DATA part */
uint32_t loc_tlv_xtra_part(uint8_t *out, uint32_t total, uint16_t parts, uint16_t part, const uint8_t *d, uint16_t len)
{
	uint32_t o = 0, fmt = 0;
	if (len > LOC_XTRA_PART)
		return 0;
	tlv(out, &o, 0x01, &total, 4);
	tlv(out, &o, 0x02, &parts, 2);
	tlv(out, &o, 0x03, &part, 2);
	out[o] = 0x04;
	out[o + 1] = (uint8_t)((len + 2) & 0xff);
	out[o + 2] = (uint8_t)((len + 2) >> 8);
	out[o + 3] = (uint8_t)(len & 0xff);
	out[o + 4] = (uint8_t)(len >> 8);
	memcpy(out + o + 5, d, len);
	o += 5u + len;
	tlv(out, &o, 0x10, &fmt, 4);
	return o;
}

int loc_sv_summary(const uint8_t *tl, uint32_t tn, struct loc_sv_sum *s)
{
	uint16_t l = 0, lx = 0;
	const uint8_t *v = find(tl, tn, 0x10, &l), *vx = find(tl, tn, 0x11, &lx);
	float top[4] = { 0, 0, 0, 0 };
	unsigned esz = 28;
	memset(s, 0, sizeof(*s));
	if (vx && lx >= 1) {		/* ws-gnss-k: the expanded list (the HAL's choice) when the modem sends it */
		v = vx;
		l = lx;
		esz = 29;
		s->expanded = 1;
	}
	if (!v || l < 1)
		return -1;
	s->listed = v[0];
	if (!s->listed)
		return 0;
	if ((uint32_t)(l - 1) % s->listed) {
		s->entry_size = 0;
		return -2;
	}
	s->entry_size = (uint32_t)(l - 1) / s->listed;
	if (s->entry_size != esz)
		return -2;
	for (unsigned i = 0; i < s->listed; i++) {
		const uint8_t *e = v + 1 + esz * i;
		uint32_t vm, sys, st;
		float snr;
		memcpy(&vm, e, 4);
		memcpy(&sys, e + 4, 4);
		memcpy(&st, e + 11, 4);
		memcpy(&snr, e + 24, 4);
		unsigned si = (vm & 0x01) && sys < LOC_SV_NSYS ? sys : 0;
		s->lsys[si]++;
		if ((vm & 0x08) && st == 3)
			s->tracked++;
		if ((vm & 0x08) && st == 2)
			s->searching++;
		if (!(vm & 0x80) || !(snr > 0.0f) || snr > 99.0f)
			continue;
		s->with_snr++;
		s->sys[si]++;
		if (snr >= 30.0f)
			s->ge30++;
		if (snr >= 20.0f)
			s->ge20++;
		if (snr > s->best_snr)
			s->best_snr = snr;
		for (int k = 0; k < 4; k++)	/* keep the 4 best */
			if (snr > top[k]) {
				for (int j = 3; j > k; j--)
					top[j] = top[j - 1];
				top[k] = snr;
				break;
			}
	}
	for (int k = 0; k < 4; k++)
		s->sum_top4 += top[k];
	return 0;
}

int loc_sv_summary_str(const struct loc_sv_sum *s, char *b, size_t n)
{
	static const char *const nm[LOC_SV_NSYS] = { "other", "GPS", "GAL", "SBAS", "COMPASS", "GLO", "BDS", "QZSS", "NavIC" };
	unsigned k = s->with_snr < 4 ? s->with_snr : 4;
	int o = snprintf(b, n, "%u listed, %u tracked, %u with CN0 (>=30: %u, >=20: %u), best %.1f dB-Hz, top%u avg %.1f;",
			 s->listed, s->tracked, s->with_snr, s->ge30, s->ge20, (double)s->best_snr, k,
			 k ? (double)(s->sum_top4 / (float)k) : 0.0);
	for (int i = 1; i <= LOC_SV_NSYS && o > 0 && (size_t)o < n; i++) {
		int j = i % LOC_SV_NSYS;	/* 1..8, then 0 (other) last */
		if (s->sys[j])
			o += snprintf(b + o, n - (size_t)o, " %s %u", nm[j], s->sys[j]);
	}
	if (!s->with_snr && o > 0 && (size_t)o < n)
		o += snprintf(b + o, n - (size_t)o, " none");
	if (s->listed && o > 0 && (size_t)o < n) {	/* ws-gnss-k: every listed SV by system (searched ones too) */
		o += snprintf(b + o, n - (size_t)o, "; listed");
		for (int i = 1; i <= LOC_SV_NSYS && o > 0 && (size_t)o < n; i++) {
			int j = i % LOC_SV_NSYS;
			if (s->lsys[j])
				o += snprintf(b + o, n - (size_t)o, " %s %u", nm[j], s->lsys[j]);
		}
		if (o > 0 && (size_t)o < n)
			o += snprintf(b + o, n - (size_t)o, " (searching %u, %s list)", s->searching, s->expanded ? "expanded" : "legacy");
	}
	return o;
}
