/* rfm: ws-gnss-j rfmetrics=1 decoders (see rfm.h). Read-only: these only format what the modem reports. */
#include <stdio.h>
#include <string.h>
#include "rfm.h"

#define SVM_ENTRY	86u	/* packed qmiLocSVMeasurementStructT_v02 on the wire */

static const uint8_t *tfind(const uint8_t *tl, uint32_t tn, uint8_t ty, uint16_t *len)
{
	for (uint32_t o = 0; o + 3 <= tn;) {
		uint16_t l = (uint16_t)(tl[o + 1] | tl[o + 2] << 8);
		if (o + 3u + l > tn)
			return NULL;
		if (tl[o] == ty) {
			*len = l;
			return tl + o + 3;
		}
		o += 3u + l;
	}
	return NULL;
}

static int due(uint8_t *have, uint64_t *last, uint64_t now)
{
	if (*have && now - *last < RFM_PERIOD_MS)
		return 0;
	*have = 1;
	*last = now;
	return 1;
}

void rfm_reset(struct rfm *r) { memset(r, 0, sizeof(*r)); }

static const char *const band_nm[10] = { "GPS L1CA", "GLO G1", "BDS B1", "GAL E1", "GPS L2C", "GLO G2", "BDS B2", NULL,
					 "GPS L5", "GAL E5A" };

int rfm_band(struct rfm *r, const uint8_t *tl, uint32_t tn, uint64_t now, char (*lines)[RFM_LINE], int max)
{
	uint16_t l;
	const uint8_t *v;
	int32_t pga1 = 0, pga5 = 0;
	int has1 = 0, has5 = 0, k = 0;
	r->n_band++;
	if ((v = tfind(tl, tn, 0x02, &l)) && l >= 4) { memcpy(&pga1, v, 4); has1 = 1; }
	if ((v = tfind(tl, tn, 0x17, &l)) && l >= 4) { memcpy(&pga5, v, 4); has5 = 1; }
	int any = 0;
	for (int i = 0; i < 10; i++) {
		if (!band_nm[i] || !(v = tfind(tl, tn, (uint8_t)(0x10 + i), &l)) || l < 8)
			continue;
		any = 1;
		if (k >= max || !due(&r->have_band[i], &r->last_band[i], now))
			continue;
		uint16_t bi, bq;
		int32_t jp;
		memcpy(&bi, v, 2); memcpy(&bq, v + 2, 2); memcpy(&jp, v + 4, 4);
		int b5 = i == 4 || i == 5 || i == 6 || i == 8 || i == 9;	/* L2/L5-side bands use band5 PGA */
		int hp = b5 ? has5 : has1;
		snprintf(lines[k++], RFM_LINE, "rf: band %s: jammerPwr %d dB, bpAmp I %u Q %u, PGA%s %s%d dB (#%u)", band_nm[i],
			 (int)jp, bi, bq, b5 ? "5" : "1", hp ? "" : "n/a ", hp ? (int)(b5 ? pga5 : pga1) : 0, r->n_band);
	}
	return any || has1 ? k : -1;
}

static const char *sig_nm(int b)
{
	static const char *const nm[20] = { "GPS L1CA", "GPS L1C", "GPS L2C", "GPS L5", "GLO G1", "GLO G2", "GAL E1", "GAL E5A",
					    "GAL E5B", "BDS B1I", "BDS B1C", "BDS B2I", "BDS B2AI", "QZSS L1CA", "QZSS L1S",
					    "QZSS L2C", "QZSS L5", "SBAS L1", "NavIC L5", "BDS B2AQ" };
	return b >= 0 && b < 20 ? nm[b] : "sig?";
}

int rfm_meas(struct rfm *r, const uint8_t *tl, uint32_t tn, uint64_t now, char *line, size_t n)
{
	uint16_t l;
	const uint8_t *v;
	uint64_t sig = 0;
	uint32_t sys = 0;
	uint8_t seq = 0, maxm = 0;
	r->n_meas++;
	if ((v = tfind(tl, tn, 0x1e, &l)) && l >= 8)
		memcpy(&sig, v, 8);
	if ((v = tfind(tl, tn, 0x03, &l)) && l >= 4)
		memcpy(&sys, v, 4);
	if ((v = tfind(tl, tn, 0x01, &l)) && l >= 1)
		seq = v[0];
	if ((v = tfind(tl, tn, 0x02, &l)) && l >= 1)
		maxm = v[0];
	int b = sig ? __builtin_ctzll(sig) : 63;	/* no signal type TLV: one shared slot */
	const uint8_t *sv = tfind(tl, tn, 0x1b, &l);
	uint16_t svl = l;
	if (sv && (svl < 1 || (uint32_t)(svl - 1) != (uint32_t)sv[0] * SVM_ENTRY))
		return -1;
	if (!due(&r->have_sig[b], &r->last_sig[b], now))
		return 0;
	int o = snprintf(line, n, "rf: meas %s (sys %u", sig ? sig_nm(b) : "no-signal-type", sys);
	if (o > 0 && (size_t)o < n && maxm)
		o += snprintf(line + o, n - (size_t)o, ", msg %u/%u", seq, maxm);
	if (o > 0 && (size_t)o < n) {
		if ((v = tfind(tl, tn, 0x1f, &l)) && l >= 8) {
			int32_t agc, bp;
			memcpy(&agc, v, 4); memcpy(&bp, v + 4, 4);
			o += snprintf(line + o, n - (size_t)o, "): AGC %.2f dB, jammer(BP) %.2f dB", agc / 100.0, bp / 100.0);
		} else {
			o += snprintf(line + o, n - (size_t)o, "): AGC/jammer n/a");
		}
	}
	unsigned cnt = sv ? sv[0] : 0;
	if (o > 0 && (size_t)o < n)
		o += snprintf(line + o, n - (size_t)o, "; %u SVs (agc/jammer apply to each) CN0 sv:dBHz", cnt);
	for (unsigned i = 0; i < cnt && o > 0 && (size_t)o < n; i++) {
		const uint8_t *e = sv + 1 + SVM_ENTRY * i;
		uint16_t id, cn, rl;
		memcpy(&id, e, 2); memcpy(&cn, e + 27, 2); memcpy(&rl, e + 29, 2);
		o += snprintf(line + o, n - (size_t)o, " %u:%.1f", id, cn / 10.0);
		if (rl && o > 0 && (size_t)o < n)
			o += snprintf(line + o, n - (size_t)o, "(rfl %.1f)", rl / 10.0);
	}
	if (!cnt && o > 0 && (size_t)o < n)
		snprintf(line + o, n - (size_t)o, " none");
	return 1;
}

int rfm_rfloss(struct rfm *r, const uint8_t *tl, uint32_t tn, uint64_t now, char *line, size_t n)
{
	uint16_t l;
	const uint8_t *v = tfind(tl, tn, 0x13, &l);
	if (!v || l < 1 || (uint32_t)(l - 1) != (uint32_t)v[0] * 4u)
		return 0;
	r->n_loss++;
	if (!due(&r->have_loss, &r->last_loss, now))
		return 0;
	unsigned cnt = v[0], used = 0;
	float mn = 0, mx = 0, sum = 0;
	for (unsigned i = 0; i < cnt; i++) {
		float f;
		memcpy(&f, v + 1 + 4 * i, 4);
		if (!(f == f))
			continue;
		if (!used || f < mn) mn = f;
		if (!used || f > mx) mx = f;
		sum += f;
		used++;
	}
	int o = snprintf(line, n, "rf: rfLoss %u SVs", cnt);
	if (used && o > 0 && (size_t)o < n)
		o += snprintf(line + o, n - (size_t)o, ": min %.1f avg %.1f max %.1f dB;", (double)mn, (double)(sum / (float)used),
			      (double)mx);
	for (unsigned i = 0; i < cnt && i < 24 && o > 0 && (size_t)o < n; i++) {
		float f;
		memcpy(&f, v + 1 + 4 * i, 4);
		o += snprintf(line + o, n - (size_t)o, " %.1f", (double)f);
	}
	if (cnt > 24 && o > 0 && (size_t)o < n)
		snprintf(line + o, n - (size_t)o, " ...");
	return 1;
}
