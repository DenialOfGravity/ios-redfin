#ifndef RFM_H
#define RFM_H
/* rfm: ws-gnss-j rfmetrics=1 read-only GNSS RF front-end metrics (QMI LOC indications, registration only, no SET).
 * Layouts from CLO location_service_v02.h/.c (LA.UM.9.12):
 *   0x00CE GET_BAND_MEASUREMENT_METRICS_IND (reg mask 0x40000000000): 0x01 systemTick u64, 0x02 band1PgaGainDb i32,
 *     0x10 gpsL1ca 0x11 gloG1 0x12 bdsB1 0x13 galE1 0x14 gpsL2c 0x15 gloG2 0x16 bdsB2 0x18 gpsL5 0x19 galE5a:
 *     qmiLocMePerGnssRfStructType {bpAmpI u16, bpAmpQ u16, jammerPwrDb i32 (dB)}; 0x17 band5PgaGainDb i32.
 *   0x0086 EVENT_GNSS_MEASUREMENT_REPORT_IND (reg mask 0x01000000): 0x01 seqNum u8, 0x02 maxMessageNum u8, 0x03 system u32,
 *     0x1B svMeasurement[] (u8 count, 86-byte entries: gnssSvId u16 @0, CNo u16 0.1 dB-Hz @27, gloRfLoss u16 0.1 dB @29;
 *     azimuth/elevation at the end are never read), 0x1E gnssSignalType u64 (one signal per message),
 *     0x1F jammerIndicator {agcMetricDb i32, bpMetricDb i32} in 0.01 dB (per signal: shared by every SV of the message).
 *   0x0025 GNSS_SV_INFO (already registered): 0x13 rfLoss[] float (u8 count; aligned with expandedSvList).
 * None of these carry a position. Each decoder is rate-limited: one line per band / signal / rfLoss per RFM_PERIOD_MS. */
#include <stddef.h>
#include <stdint.h>

#define LOC_IND_GNSS_MEAS	0x0086
#define LOC_IND_BAND_METRICS	0x00ce
#define LOC_EVMASK_GNSS_MEAS	0x01000000ull
#define LOC_EVMASK_BAND_METRICS	0x40000000000ull
#define LOC_EVMASK_RFMETRICS	(LOC_EVMASK_GNSS_MEAS | LOC_EVMASK_BAND_METRICS)
#define RFM_PERIOD_MS		10000u
#define RFM_LINE		400

struct rfm {
	uint64_t last_band[16];		/* by TLV id - 0x10 */
	uint64_t last_sig[64];		/* by gnssSignalType bit */
	uint64_t last_loss;
	uint8_t have_band[16], have_sig[64], have_loss;
	unsigned n_band, n_meas, n_loss;	/* indications seen */
};

void rfm_reset(struct rfm *r);
/* 0x00CE: fills up to max lines (one per band due now); returns the count, -1 if not parsable */
int rfm_band(struct rfm *r, const uint8_t *tl, uint32_t tn, uint64_t now, char (*lines)[RFM_LINE], int max);
/* 0x0086: 1 = line filled (this signal's window was due), 0 = rate-limited, -1 not parsable */
int rfm_meas(struct rfm *r, const uint8_t *tl, uint32_t tn, uint64_t now, char *line, size_t n);
/* 0x0025 rfLoss[]: 1 = line filled, 0 = rate-limited or no TLV 0x13 */
int rfm_rfloss(struct rfm *r, const uint8_t *tl, uint32_t tn, uint64_t now, char *line, size_t n);
#endif
