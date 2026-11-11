/*
 * venus_h264: venusctl's H.264 Annex B helpers (venus_h264.h). Moved out of venusctl.c for venusd/venusc (video 4a):
 * the SPS now fills a struct h264_sps instead of venusctl's decoder state, nothing else changed.
 */
#include <stdio.h>
#include <string.h>
#include "venus_h264.h"

static uint32_t crc_tab[256];

uint32_t crc32_buf(const uint8_t *p, size_t n)	/* zlib crc32: what tools/sunfish-venus-yuvcmp.py prints */
{
	if (!crc_tab[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
			crc_tab[i] = c;
		}
	uint32_t c = 0xffffffffu;
	while (n--)
		c = crc_tab[(c ^ *p++) & 0xff] ^ (c >> 8);
	return c ^ 0xffffffffu;
}

/* RBSP bit reader for the SPS (H.264 7.3.2.1.1) */
struct bits { const uint8_t *p; size_t n, i; };

static uint32_t gb(struct bits *b, int k)
{
	uint32_t v = 0;
	while (k-- > 0) {
		v <<= 1;
		if (b->i < 8 * b->n)
			v |= (b->p[b->i >> 3] >> (7 - (b->i & 7))) & 1;
		b->i++;
	}
	return v;
}

static uint32_t ue(struct bits *b)
{
	int z = 0;
	while (!gb(b, 1) && z < 32)
		z++;
	return z >= 32 ? 0 : ((1u << z) - 1) + gb(b, z);
}

static int32_t se(struct bits *b)
{
	uint32_t k = ue(b);
	return k & 1 ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

/* coded size and crop from the first SPS: the FRAME_SIZE the client knows before streaming (msm_vdec_s_fmt) */
int sps_parse(const uint8_t *nal, size_t len, struct h264_sps *sp)
{
	uint8_t r[256];
	size_t n = 0;
	for (size_t i = 1; i < len && n < sizeof(r); i++) {	/* emulation prevention 00 00 03 */
		if (i >= 3 && nal[i] == 3 && nal[i - 1] == 0 && nal[i - 2] == 0)
			continue;
		r[n++] = nal[i];
	}
	struct bits b = { r, n, 0 };
	uint32_t profile = gb(&b, 8), cfi = 1;
	gb(&b, 16);	/* constraint flags, level */
	ue(&b);		/* sps id */
	if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
	    profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
	    profile == 135) {
		cfi = ue(&b);
		if (cfi == 3)
			gb(&b, 1);
		ue(&b);
		ue(&b);
		gb(&b, 1);
		if (gb(&b, 1))	/* seq_scaling_matrix_present: skip the lists */
			for (int i = 0; i < (cfi == 3 ? 12 : 8); i++)
				if (gb(&b, 1))
					for (int j = 0, last = 8, next = 8; j < (i < 6 ? 16 : 64); j++) {
						if (next)
							next = (last + se(&b) + 256) % 256;
						last = next ? next : last;
					}
	}
	ue(&b);		/* log2_max_frame_num */
	uint32_t poc = ue(&b);
	if (poc == 0) {
		ue(&b);
	} else if (poc == 1) {
		gb(&b, 1);
		se(&b);
		se(&b);
		for (uint32_t i = 0, k = ue(&b); i < k && i < 256; i++)
			se(&b);
	}
	ue(&b);		/* max_num_ref_frames */
	gb(&b, 1);
	uint32_t wmb = ue(&b) + 1, hmu = ue(&b) + 1, fmo = gb(&b, 1);
	if (!fmo)
		gb(&b, 1);
	gb(&b, 1);
	sp->w = (int)wmb * 16;
	sp->h = (int)((2 - fmo) * hmu * 16);
	sp->cx = sp->cy = 0;
	sp->cw = sp->w;
	sp->ch = sp->h;
	if (gb(&b, 1)) {	/* frame_cropping: units 2 x 2(2 - frame_mbs_only) for 4:2:0 */
		uint32_t ux = cfi ? 2 : 1, uy = (cfi ? 2 : 1) * (2 - fmo);
		uint32_t l = ue(&b), rr = ue(&b), t = ue(&b), bt = ue(&b);
		sp->cx = (int)(l * ux);
		sp->cy = (int)(t * uy);
		sp->cw = sp->w - (int)((l + rr) * ux);
		sp->ch = sp->h - (int)((t + bt) * uy);
	}
	printf("venus: SPS profile %u: coded %dx%d, visible %dx%d at %d,%d\n", profile, sp->w, sp->h, sp->cw, sp->ch, sp->cx,
	       sp->cy);
	return sp->w > 0 && sp->h > 0 && sp->w <= 4096 && sp->h <= 2304 && sp->cw > 0 && sp->ch > 0 ? 0 : -1;
}

/* Annex B access units: a new AU starts at an AUD/SEI/SPS/PPS or at a slice with first_mb_in_slice 0, once the
 * current one has a slice (H.264 7.4.1.2.3). One AU per ETB with ENDOFFRAME, as the v4l2 client's frame parser. */
int split_aus(const uint8_t *s, size_t len, uint32_t *off, uint32_t *alen, int nmax, struct h264_sps *sp)
{
	int nau = 0, vcl = 0, sps = 0;
	size_t i = 0;
	while (i + 3 <= len) {
		if (!(s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1)) {
			i++;
			continue;
		}
		size_t sc = i > 0 && s[i - 1] == 0 ? i - 1 : i, h = i + 3;
		if (h >= len)
			break;
		unsigned t = s[h] & 0x1f;
		int first = (t == 1 || t == 5) && h + 1 < len && (s[h + 1] & 0x80);
		if (nau == 0 || (vcl && (first || t == 6 || t == 7 || t == 8 || t == 9 || (t >= 14 && t <= 18)))) {
			if (nau >= nmax)
				break;
			if (nau)
				alen[nau - 1] = (uint32_t)(sc - off[nau - 1]);
			off[nau] = nau ? (uint32_t)sc : 0;
			nau++;
			vcl = 0;
		}
		if (t == 1 || t == 5)
			vcl = 1;
		if (t == 7 && !sps) {
			size_t e = h + 1;
			while (e + 3 <= len && !(s[e] == 0 && s[e + 1] == 0 && (s[e + 2] == 1 || s[e + 2] == 0)))
				e++;
			sps = !sps_parse(s + h, (e + 3 <= len ? e : len) - h, sp);
		}
		i = h;
	}
	if (nau)
		alen[nau - 1] = (uint32_t)(len - off[nau - 1]);
	return sps ? nau : -nau - 1;
}

