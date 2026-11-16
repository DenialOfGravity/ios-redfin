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
	int has_tsrc;			/* ws-gnss-k: TLV 0x29 timeSrc (qmiLocTimeSourceEnumT_v02) */
	uint32_t time_src;
	int has_svused;			/* ws-gnss-k: TLV 0x2c gnssSvUsedList (u8 count + u16 ids): the count only */
	unsigned sv_used;
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

/* ws-gnss-g: assistance (location_service_v02.h, hardware/qcom/gps loc_api_v02):
 *   INJECT_UTC_TIME 0x0038 req 0x01 timeUtc u64 (ms since 1970-01-01 UTC), 0x02 timeUnc u32 (ms), both mandatory;
 *   its indication (same id) carries 0x01 status u32 (0 = success).
 *   Event mask bits: INJECT_TIME_REQ 0x10 (ind 0x0028: opt 0x10 time server list = delayThreshold u32 + up to 3 URLs),
 *   INJECT_PREDICTED_ORBITS_REQ 0x20 (ind 0x0029: 0x01 allowedSizes = maxFileSize u32 + maxPartSize u32, opt 0x10
 *   server list up to 3 URLs), INJECT_POSITION_REQ 0x40 (ind 0x002A). */
#define LOC_INJECT_UTC_TIME	0x0038
#define LOC_IND_INJECT_TIME_REQ	0x0028
#define LOC_IND_XTRA_REQ	0x0029
#define LOC_IND_INJECT_POS_REQ	0x002a
#define LOC_EVMASK_ASSIST	0x70ull
uint32_t loc_tlv_inject_utc(uint8_t *out, uint64_t utc_ms, uint32_t unc_ms);
/* ws-gnss-k: HAL-like init and XTRA (CLO LA.UM.9.12 location_service_v02.h/.c, LocApiV02.cpp; all default off):
 *   REGISTER_MASTER_CLIENT 0x00BA: 0x01 key u32 0xBAABCDEF (LocApiV02::registerMasterClient); ind 0x01 status u32
 *     (0 success, 1 invalid key, 2 already have a master, 3 internal). The master's REG_EVENTS clientStrId is "MHAL".
 *   SET_GNSS_CONSTELL_REPORT_CONFIG 0x0088: 0x10 measReportConfig u64, 0x11 svPolyReportConfig u64 (eQMI_SYSTEM_* mask:
 *     GPS 0x1 GLO 0x2 BDS 0x4 GAL 0x8 QZSS 0x10 NavIC 0x20); ind 0x01 status. Without it the modem sends no
 *     EVENT_GNSS_MEASUREMENT_REPORT (0x0086) even when the event bit is registered ("Reports are generated only for the
 *     GNSS satellite constellations that are enabled using QMI_LOC_SET_GNSS_CONSTELL_REPORT_CONFIG").
 *   INJECT_XTRA_DATA 0x00A7 (what the stock xtra-daemon's IzatApi uses) and INJECT_PREDICTED_ORBITS_DATA 0x0035
 *     (deprecated, same layout): 0x01 totalSize u32, 0x02 totalParts u16, 0x03 partNum u16 (from 1), 0x04 partData
 *     (u16 length + at most 1024 bytes), 0x10 formatType u32 0 (XTRA); ind 0x01 status u32, 0x10 partNum u16,
 *     0x11 ConstellationMask u64 (0x00A7 only).
 *   GET_PREDICTED_ORBITS_DATA_VALIDITY 0x0037 (read-only): ind 0x01 status, 0x10 {startTimeInUTC u64, durationHours u16}. */
#define LOC_REG_MASTER		0x00ba
#define LOC_MASTER_KEY		0xBAABCDEFu
#define LOC_SET_CONSTELL_REPORT	0x0088
#define LOC_SYSTEMS_ALL		0x3full	/* eQMI_SYSTEM_ GPS|GLO|BDS|GAL|QZSS|NAVIC, as setSvMeasurementConstellation */
#define LOC_INJECT_PRED_ORBITS	0x0035
#define LOC_INJECT_XTRA_DATA	0x00a7
#define LOC_GET_XTRA_VALIDITY	0x0037
#define LOC_XTRA_PART		1024u	/* QMI_LOC_MAX_XTRA_PART_LEN_V02 = QMI_LOC_MAX_PREDICTED_ORBITS_PART_LEN_V02 */
#define LOC_XTRA_GATE		0xffffu	/* loc.c queue marker: XTRA parts go out here, never sent itself */
uint32_t loc_tlv_master(uint8_t *out);
uint32_t loc_tlv_constell_report(uint8_t *out, uint64_t meas, uint64_t poly);
/* one XTRA part (part from 1); out needs 3*4 + 7 + 2 + len + 7 bytes; returns bytes */
uint32_t loc_tlv_xtra_part(uint8_t *out, uint32_t total, uint16_t parts, uint16_t part, const uint8_t *d, uint16_t len);

/* GNSS_SV_INFO (0x0025): TLV 0x10 svList (u8 count + count x 28-byte qmiLocSvInfoStructT_v02: validMask u32, system u32,
 * gnssSvId u16, health u8, svStatus u32 (1 idle, 2 search, 3 track), svInfoMask u8, elevation f32, azimuth f32, snr f32
 * dB-Hz; snr valid with validMask 0x80) and TLV 0x11 expandedSvList (u8 count + count x 29 bytes: the same struct +
 * gloFrequency u8; max 176). ws-gnss-k: the expanded list is used when present (as LocApiV02::reportSv does: "If the
 * service reports expandedSvList, svList is not reported"), else svList. Summary only: counts, CN0, constellations;
 * never elevation/azimuth. System enum (qmiLocSvSystemEnumT_v02): 1 GPS, 2 GAL, 3 SBAS, 4 COMPASS (deprecated), 5 GLO,
 * 6 BDS, 7 QZSS, 8 NavIC; ws-gnss-j and older named 4 "BDS", 6 "QZSS", 7 "NavIC" and put 8 under "other". */
#define LOC_SV_NSYS	9
struct loc_sv_sum {
	unsigned listed, tracked, searching, with_snr, ge30, ge20, entry_size;
	float best_snr, sum_top4;	/* sum_top4: the 4 best CN0 values added up (avg = sum_top4 / min(4, with_snr)) */
	unsigned sys[LOC_SV_NSYS];	/* SVs with CN0 > 0, by system enum (above); 0 other / system not valid */
	unsigned lsys[LOC_SV_NSYS];	/* ws-gnss-k: every listed SV by system, CN0 or not (searched ones included) */
	int expanded;			/* ws-gnss-k: 1 = parsed from TLV 0x11 expandedSvList */
};
/* 0 ok, -1 no/short TLV 0x10 and 0x11, -2 unexpected entry size (entry_size set) */
int loc_sv_summary(const uint8_t *tl, uint32_t tn, struct loc_sv_sum *s);
/* "N listed, T tracked, S with CN0 (>=30: a, >=20: b), best X dB-Hz, top4 avg Y; GPS g GLO r ...; listed GPS g GLO r ...
 *  (searching s, expanded list)" */
int loc_sv_summary_str(const struct loc_sv_sum *s, char *b, size_t n);
#endif
