#ifndef PDSEMU_H
#define PDSEMU_H
/*
 * pdsemu: QMI PDS (service 0x06) emulation for Apple locationd's "Eureka" GNSS back end, fed by the modem's QMI LOC
 * (service 0x10). Pure translation and session state, no I/O: abpbridge.c hands it locationd's PDS requests, loc.c
 * hands it LOC indications and asks it whether a LOC session is wanted. Built into wland and into the host test
 * (test/pdsemu_test.c). Layouts and evidence: pdsemu.c header.
 */
#include <stdint.h>
#include <stddef.h>

#define PDS_SVC			0x06
#define PDS_RESET		0x0000
#define PDS_EVENT_REPORT	0x0001	/* request SET_EVENT_REPORT and indication EVENT_REPORT_IND */
#define PDS_GET_SVC_STATE	0x0020
#define PDS_SET_SVC_STATE	0x0021
#define PDS_GET_DEF_TRACKING	0x0029
#define PDS_SET_DEF_TRACKING	0x002a
#define PDS_GET_AUTO_TRACKING	0x0030
#define PDS_SET_AUTO_TRACKING	0x0031
#define PDS_CANCEL_SESSION	0x0084

#define LOC_SVC			0x10
#define LOC_INFORM_CLIENT_REV	0x0020
#define LOC_REG_EVENTS		0x0021
#define LOC_START		0x0022
#define LOC_STOP		0x0023
#define LOC_IND_POSITION	0x0024
#define LOC_IND_SV_INFO		0x0025
#define LOC_IND_ENGINE_STATE	0x002b
#define LOC_IND_FIX_SESSION	0x002c
#define LOC_GET_SERVICE_REV	0x0032
#define LOC_GET_FIX_CRITERIA	0x0033
#define LOC_GET_XTRA_SOURCE	0x0036
#define LOC_GET_ENGINE_LOCK	0x003b
#define LOC_SET_OPERATION_MODE	0x004a		/* ws-gnss-f, opmode=standalone only */
#define LOC_OPMODE_STANDALONE	4
#define LOC_IND_ENGINE_LOCK_STATE 0x00bb	/* ws-gnss-f lockev=1 (event mask bit 37) */
#define LOC_EVMASK_ENGINE_LOCK_STATE 0x2000000000ull
#define LOC_CLIENT_AFW		1		/* REG_EVENTS TLV 0x11 qmiLocClientTypeEnumT_v02 */
#define LOC_CLIENT_NFW		2

#define LOC_EVMASK_BRIDGE	0x183ull	/* POSITION_REPORT 0x1 | GNSS_SV_INFO 0x2 | ENGINE_STATE 0x80 | FIX_SESSION_STATE 0x100 */
#define LOC_SESSION_ID		1
#define PDS_FIX_TLV_LEN		105		/* locationd: "PDS fix data Tlv length incorrect" unless exactly 0x69 */

struct pds_client {			/* the CommCenter/ABM client that sent PDS requests (indications go there) */
	uint32_t pipe, sess;
	unsigned cid;
	int valid;
};

struct pds_answer {			/* a local answer to one PDS request */
	int fail, code;
	uint8_t tl[64];			/* response TLVs after the result TLV */
	uint16_t tn;
	const char *what;		/* for the log */
};

/* one LOC POSITION_REPORT_IND, parsed (has_* = TLV present) */
struct loc_fix {
	uint32_t status;		/* TLV 0x01 sessionStatus (0 success, 1 in progress, 2.. failures) */
	uint8_t session;
	int has_lat, has_lon, has_unc, has_semimin, has_semimaj, has_azim, has_hconf, has_speed, has_speedunc;
	int has_alt_ell, has_alt_msl, has_vunc, has_vspeed, has_heading, has_dop, has_utc, has_leap, has_gpstime, has_tunc;
	double lat, lon;
	float unc, semimin, semimaj, azim, speed, speedunc, alt_ell, alt_msl, vunc, vspeed, heading, pdop, hdop, vdop, tunc;
	uint8_t hconf, leap;
	uint64_t utc_ms;
	uint16_t gps_week;
	uint32_t gps_tow_ms;
	uint32_t tech;
};

struct pds_state {
	int svc_on;			/* SET_GPS_SERVICE_STATE (default 1: what the old policy answer said) */
	int auto_on;			/* SET_AUTO_TRACKING_STATE */
	int cancelled;
	uint8_t def_mode, def_timeout;	/* SET_DEFAULT_TRACKING_SESSION */
	uint32_t def_interval, def_accuracy;
	int have_def;
	uint32_t evmask;		/* SET_EVENT_REPORT TLVs seen (bit = TLV id - 0x10) */
	struct pds_client client;
	unsigned nreq, nfix, nind;
};

void pdsemu_init(struct pds_state *p);
/* locationd's PDS request (QMI SDU: u8 type | u16 txn | u16 msg | u16 len | TLVs). 1 = answered here (a filled in),
 * 0 = not one of ours: the normal policy answers it (as without pds=1). */
int pdsemu_request(struct pds_state *p, const struct pds_client *c, const uint8_t *sdu, uint32_t sn, struct pds_answer *a);
/* 1 = a LOC session should run now; *interval_ms = the fix interval */
int pdsemu_want_session(const struct pds_state *p, uint32_t *interval_ms);
void pdsemu_client_gone(struct pds_state *p, uint32_t pipe, uint32_t sess);

/* LOC POSITION_REPORT_IND TLV area -> loc_fix; 0 ok */
int loc_parse_position(const uint8_t *tl, uint32_t tn, struct loc_fix *f);
/* loc_fix -> EVENT_REPORT_IND TLV area (0x12 session status, and 0x13 parsed position when the fix is a final one
 * with latitude and longitude); returns the TLV bytes written, 0 if nothing to send */
uint32_t pdsemu_event_report(const struct loc_fix *f, uint8_t *out, uint32_t room);
/* the 105-byte TLV 0x13 value alone (returns its valid mask); exposed for the test */
uint32_t pdsemu_parsed_position(const struct loc_fix *f, uint8_t v[PDS_FIX_TLV_LEN]);

/* LOC request TLV areas (after the 7-byte QMI header); return bytes */
uint32_t loc_tlv_reg_events(uint8_t *out, uint64_t mask);
uint32_t loc_tlv_start(uint8_t *out, uint32_t interval_ms);
uint32_t loc_tlv_stop(uint8_t *out);
uint32_t loc_tlv_client_rev(uint8_t *out, uint32_t rev);
uint32_t loc_tlv_reg_events_ex(uint8_t *out, uint64_t mask, uint32_t ctype, const char *strid);
uint32_t loc_tlv_start_android(uint8_t *out, uint32_t interval_ms);
uint32_t loc_tlv_opmode_standalone(uint8_t *out);
#endif
