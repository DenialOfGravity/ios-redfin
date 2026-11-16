/* Host test for pdsemu.c (cc -I.. -o /tmp/pdsemu_test test/pdsemu_test.c pdsemu.c && /tmp/pdsemu_test).
 * Feeds locationd-style PDS requests (the sequence seen on the phone + the startInternal/stopInternal ones from the
 * binary) and checks the answers and the LOC session wish; feeds constructed LOC POSITION_REPORT TLVs and checks the
 * PDS EVENT_REPORT bytes against the layout locationd parses (pdsemu.c header). Coordinates here are synthetic. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../pdsemu.h"

static int nfail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); nfail++; } } while (0)

static uint32_t req(uint8_t *b, uint16_t msg, const uint8_t *tl, uint16_t tn)
{
	b[0] = 0; b[1] = 7; b[2] = 0; b[3] = (uint8_t)msg; b[4] = (uint8_t)(msg >> 8);
	b[5] = (uint8_t)tn; b[6] = (uint8_t)(tn >> 8);
	memcpy(b + 7, tl, tn);
	return 7u + tn;
}

static void put(uint8_t *b, uint32_t *o, uint8_t t, const void *v, uint16_t l)
{
	b[(*o)++] = t; b[(*o)++] = (uint8_t)l; b[(*o)++] = (uint8_t)(l >> 8);
	memcpy(b + *o, v, l); *o += l;
}

int main(void)
{
	struct pds_state p;
	struct pds_answer a;
	struct pds_client c = { 1, 7, 3, 1 };
	uint8_t b[256];
	uint32_t n, ivl;
	pdsemu_init(&p);

	/* not ours: version, reset, Apple 0x5558, SET_FORCE_RECEIVER_OFF -> the normal policy */
	CHECK(!pdsemu_request(&p, &c, b, req(b, 0x0036, NULL, 0), &a));
	CHECK(!pdsemu_request(&p, &c, b, req(b, 0x0000, NULL, 0), &a));
	CHECK(!pdsemu_request(&p, &c, b, req(b, 0x5558, (const uint8_t *)"\x01\x01\x00\x01", 4), &a));
	CHECK(!pdsemu_request(&p, &c, b, req(b, 0x0049, (const uint8_t *)"\x01\x01\x00\x00", 4), &a));
	CHECK(!pdsemu_want_session(&p, NULL));

	/* GET_GPS_SERVICE_STATE before any session = the old policy answer 01 0200 01 01 */
	CHECK(pdsemu_request(&p, &c, b, req(b, 0x0020, NULL, 0), &a));
	CHECK(!a.fail && a.tn == 5 && !memcmp(a.tl, "\x01\x02\x00\x01\x01", 5));

	/* SET_EVENT_REPORT 0x12/0x13 on */
	n = 0; put(b + 100, &n, 0x12, "\x01", 1); put(b + 100, &n, 0x16, "\x01", 1);
	CHECK(pdsemu_request(&p, &c, b, req(b, 0x0001, b + 100, (uint16_t)n), &a) && !a.fail && a.tn == 0);
	CHECK(p.evmask == ((1u << 2) | (1u << 6)));

	/* startInternal: SET_DEFAULT_TRACKING_SESSION {0, 255, 1, 400} then SET_AUTO_TRACKING_STATE 1 */
	uint8_t def[10] = { 0, 255, 1, 0, 0, 0, 0x90, 1, 0, 0 };
	n = 0; put(b + 100, &n, 0x01, def, 10);
	CHECK(pdsemu_request(&p, &c, b, req(b, 0x002a, b + 100, (uint16_t)n), &a) && !a.fail);
	CHECK(p.def_interval == 1 && p.def_accuracy == 400 && p.def_timeout == 255);
	n = 0; put(b + 100, &n, 0x01, "\x01", 1);
	CHECK(pdsemu_request(&p, &c, b, req(b, 0x0031, b + 100, (uint16_t)n), &a) && !a.fail);
	CHECK(pdsemu_want_session(&p, &ivl) && ivl == 1000);
	CHECK(p.client.pipe == 1 && p.client.sess == 7 && p.client.cid == 3);
	CHECK(pdsemu_request(&p, &c, b, req(b, 0x0020, NULL, 0), &a) && a.tl[4] == 2);	/* tracking active */
	CHECK(pdsemu_request(&p, &c, b, req(b, 0x0029, NULL, 0), &a) && a.tn == 13 && a.tl[3] == 0 && a.tl[4] == 255);

	/* LOC requests the bridge sends */
	n = loc_tlv_reg_events(b, LOC_EVMASK_BRIDGE);
	CHECK(n == 11 && !memcmp(b, "\x01\x08\x00\x83\x01\x00\x00\x00\x00\x00\x00", 11));
	n = loc_tlv_start(b, ivl);
	CHECK(n == 4 + 7 + 7 + 7);
	CHECK(!memcmp(b, "\x01\x01\x00\x01" "\x10\x04\x00\x01\x00\x00\x00" "\x12\x04\x00\x02\x00\x00\x00" "\x13\x04\x00\xe8\x03\x00\x00", 25));
	n = loc_tlv_stop(b);
	CHECK(n == 4 && !memcmp(b, "\x01\x01\x00\x01", 4));

	/* a LOC POSITION_REPORT (synthetic) -> EVENT_REPORT 0x12 + 0x13 */
	uint8_t tl[256];
	uint32_t o = 0, st = 0, tech = 1;
	double lat = 12.5, lon = -45.25;
	float unc = 8.5f, smin = 6.0f, smaj = 10.0f, az = 30.0f, spd = 1.5f, sunc = 0.5f, ae = 120.0f, am = 75.0f, vu = 12.0f,
	      hd = 270.0f, tu = 20.0f, dop[3] = { 1.5f, 0.9f, 1.2f };
	uint64_t utc = 1763236800123ull;	/* 2025-11-15 20:00:00.123 UTC, a Saturday */
	uint8_t sid = 1, conf = 68, leap = 18;
	put(tl, &o, 0x01, &st, 4); put(tl, &o, 0x02, &sid, 1); put(tl, &o, 0x10, &lat, 8); put(tl, &o, 0x11, &lon, 8);
	put(tl, &o, 0x12, &unc, 4); put(tl, &o, 0x13, &smin, 4); put(tl, &o, 0x14, &smaj, 4); put(tl, &o, 0x15, &az, 4);
	put(tl, &o, 0x16, &conf, 1); put(tl, &o, 0x18, &spd, 4); put(tl, &o, 0x19, &sunc, 4); put(tl, &o, 0x1a, &ae, 4);
	put(tl, &o, 0x1b, &am, 4); put(tl, &o, 0x1c, &vu, 4); put(tl, &o, 0x20, &hd, 4); put(tl, &o, 0x23, &tech, 4);
	put(tl, &o, 0x24, dop, 12); put(tl, &o, 0x25, &utc, 8); put(tl, &o, 0x26, &leap, 1); put(tl, &o, 0x28, &tu, 4);
	struct loc_fix f;
	CHECK(!loc_parse_position(tl, o, &f));
	CHECK(f.status == 0 && f.has_lat && f.lat == 12.5 && f.has_lon && f.lon == -45.25 && f.has_unc && f.has_dop && f.has_utc);
	uint8_t ev[256];
	n = pdsemu_event_report(&f, ev, sizeof(ev));
	CHECK(n == 4 + 3 + PDS_FIX_TLV_LEN);
	CHECK(ev[0] == 0x12 && ev[1] == 1 && ev[2] == 0 && ev[3] == 0);
	CHECK(ev[4] == 0x13 && ev[5] == PDS_FIX_TLV_LEN && ev[6] == 0);
	const uint8_t *v = ev + 7;
	uint32_t mask; uint16_t yr, ms; uint64_t u; double d; float fl; uint32_t tun;
	memcpy(&mask, v, 4);
	CHECK((mask & 0x4f8ffu) == 0x4f8ffu);	/* all of locationd's leap-second set + fix bits */
	CHECK(mask == 0x7dfdff);		/* everything but v speed (9) and v vel unc (17) */
	memcpy(&yr, v + 4, 2); memcpy(&ms, v + 0x0c, 2);
	CHECK(yr == 2025 && v[6] == 11 && v[7] == 6 && v[8] == 15 && v[9] == 20 && v[10] == 0 && v[11] == 0 && ms == 123);
	CHECK(v[0x0e] == 18);
	memcpy(&u, v + 0x0f, 8); CHECK(u == utc);
	memcpy(&tun, v + 0x17, 4); CHECK(tun == 20);
	memcpy(&d, v + 0x1b, 8); CHECK(d == 12.5);
	memcpy(&d, v + 0x23, 8); CHECK(d == -45.25);
	memcpy(&fl, v + 0x2b, 4); CHECK(fl == 120.0f);
	memcpy(&fl, v + 0x2f, 4); CHECK(fl == 75.0f);
	memcpy(&fl, v + 0x33, 4); CHECK(fl == 1.5f);
	memcpy(&fl, v + 0x3b, 4); CHECK(fl == 270.0f);
	memcpy(&fl, v + 0x3f, 4); CHECK(fl == 8.5f);
	memcpy(&fl, v + 0x43, 4); CHECK(fl == 10.0f);	/* semi-major */
	memcpy(&fl, v + 0x47, 4); CHECK(fl == 6.0f);	/* semi-minor */
	memcpy(&fl, v + 0x4b, 4); CHECK(fl == 30.0f);
	memcpy(&fl, v + 0x4f, 4); CHECK(fl == 12.0f);
	memcpy(&fl, v + 0x53, 4); CHECK(fl == 0.5f);
	CHECK(v[0x5b] == 68);
	memcpy(&fl, v + 0x5c, 4); CHECK(fl == 1.5f);
	memcpy(&fl, v + 0x64, 4); CHECK(fl == 1.2f);
	CHECK(v[0x68] == 0);	/* operating mode standalone: locationd accepts 0..2 */

	/* in progress: nothing; failure: 0x12 only */
	f.status = 1; CHECK(pdsemu_event_report(&f, ev, sizeof(ev)) == 0);
	f.status = 3; n = pdsemu_event_report(&f, ev, sizeof(ev)); CHECK(n == 4 && ev[3] == 3);
	/* a fix without lat/lon: status only */
	o = 0; st = 0; put(tl, &o, 0x01, &st, 4);
	CHECK(!loc_parse_position(tl, o, &f)); n = pdsemu_event_report(&f, ev, sizeof(ev)); CHECK(n == 4);
	/* malformed: no status TLV */
	CHECK(loc_parse_position(tl + 7, 0, &f) == -1);

	/* stopInternal: SET_AUTO_TRACKING_STATE 0 -> no session; cancel and session close also stop */
	n = 0; put(b + 100, &n, 0x01, "\x00", 1);
	CHECK(pdsemu_request(&p, &c, b, req(b, 0x0031, b + 100, (uint16_t)n), &a) && !pdsemu_want_session(&p, NULL));
	n = 0; put(b + 100, &n, 0x01, "\x01", 1);
	pdsemu_request(&p, &c, b, req(b, 0x0031, b + 100, (uint16_t)n), &a);
	CHECK(pdsemu_want_session(&p, NULL));
	CHECK(pdsemu_request(&p, &c, b, req(b, 0x0084, NULL, 0), &a) && !pdsemu_want_session(&p, NULL));
	pdsemu_request(&p, &c, b, req(b, 0x0031, b + 100, (uint16_t)n), &a);
	CHECK(pdsemu_want_session(&p, NULL));
	pdsemu_client_gone(&p, 1, 7);
	CHECK(!pdsemu_want_session(&p, NULL));
	/* lockGps: SET_GPS_SERVICE_STATE 0 blocks a session */
	pdsemu_request(&p, &c, b, req(b, 0x0031, b + 100, (uint16_t)n), &a);
	n = 0; put(b + 100, &n, 0x01, "\x00", 1);
	pdsemu_request(&p, &c, b, req(b, 0x0021, b + 100, (uint16_t)n), &a);
	CHECK(!pdsemu_want_session(&p, NULL));

	printf("pdsemu_test: %s (%d failed)\n", nfail ? "FAILED" : "all ok", nfail);
	return nfail != 0;
}
