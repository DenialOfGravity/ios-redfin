/*
 * venus_hevc: video 4e's HEVC Annex B helpers (venus_hevc.h), the H.265 twins of venus_h264.c's splitter and SPS
 * parser. Syntax from ITU-T H.265: NAL header 7.3.1.2, SPS 7.3.2.2, profile_tier_level 7.3.3, access units 7.4.2.4.4.
 */
#include <stdio.h>
#include <string.h>
#include "venus_hevc.h"

/* RBSP bit reader (as venus_h264.c's) */
struct hbits { const uint8_t *p; size_t n, i; };

static uint32_t hgb(struct hbits *b, int k)
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

static uint32_t hue(struct hbits *b)
{
	int z = 0;
	while (!hgb(b, 1) && z < 32)
		z++;
	return z >= 32 ? 0 : ((1u << z) - 1) + hgb(b, z);
}

int hevc_sps_parse(const uint8_t *nal, size_t len, struct hevc_sps *sp)
{
	uint8_t r[256];
	size_t n = 0;
	memset(sp, 0, sizeof *sp);
	if (len < 3 || ((nal[0] >> 1) & 0x3f) != 33)
		return -1;
	for (size_t i = 2; i < len && n < sizeof(r); i++) {	/* emulation prevention 00 00 03 */
		if (i >= 4 && nal[i] == 3 && nal[i - 1] == 0 && nal[i - 2] == 0)
			continue;
		r[n++] = nal[i];
	}
	struct hbits b = { r, n, 0 };
	hgb(&b, 4);			/* sps_video_parameter_set_id */
	unsigned subl = hgb(&b, 3);	/* sps_max_sub_layers_minus1 */
	hgb(&b, 1);			/* temporal_id_nesting */
	/* profile_tier_level(1, subl) */
	hgb(&b, 3);			/* general_profile_space, general_tier_flag */
	sp->profile = (int)hgb(&b, 5);
	sp->compat = (int)hgb(&b, 32);
	b.i += 48;			/* progressive .. frame_only (4), 43 constraint bits, 1 */
	hgb(&b, 8);			/* general_level_idc */
	unsigned pp[8], lp[8];
	for (unsigned i = 0; i < subl; i++)
		pp[i] = hgb(&b, 1), lp[i] = hgb(&b, 1);
	if (subl)
		b.i += 2 * (8 - subl);	/* reserved_zero_2bits */
	for (unsigned i = 0; i < subl; i++)
		b.i += (pp[i] ? 88 : 0) + (lp[i] ? 8 : 0);
	hue(&b);			/* sps_seq_parameter_set_id */
	sp->chroma = (int)hue(&b);
	if (sp->chroma == 3)
		hgb(&b, 1);		/* separate_colour_plane_flag */
	sp->w = (int)hue(&b);
	sp->h = (int)hue(&b);
	sp->cx = sp->cy = 0;
	sp->cw = sp->w;
	sp->ch = sp->h;
	if (hgb(&b, 1)) {		/* conformance_window: units SubWidthC x SubHeightC (table 6-1) */
		int ux = sp->chroma == 1 || sp->chroma == 2 ? 2 : 1, uy = sp->chroma == 1 ? 2 : 1;
		uint32_t l = hue(&b), rr = hue(&b), t = hue(&b), bt = hue(&b);
		sp->cx = (int)l * ux;
		sp->cy = (int)t * uy;
		sp->cw = sp->w - (int)(l + rr) * ux;
		sp->ch = sp->h - (int)(t + bt) * uy;
	}
	sp->depth_y = (int)hue(&b) + 8;
	sp->depth_c = (int)hue(&b) + 8;
	if (b.i > 8 * b.n)
		return -1;
	return sp->w > 0 && sp->h > 0 && sp->w <= 4096 && sp->h <= 2304 && sp->cw > 0 && sp->ch > 0 && sp->cx >= 0 &&
	       sp->cy >= 0 ? 0 : -1;
}

int hevc_main8(const struct hevc_sps *sp)
{
	/* compatibility flag j is bit 31 - j of the 32 read: Main = 1, Main Still Picture = 3 */
	int main = sp->profile == 1 || sp->profile == 3 || (sp->compat & (1 << 30)) || (sp->compat & (1 << 28));
	return main && sp->chroma == 1 && sp->depth_y == 8 && sp->depth_c == 8;
}

/* H.264's NAL header byte has its low bit set for every slice/SPS/PPS/AUD/IDR type; HEVC's first byte is
 * forbidden_zero | type << 1 | layer bit 5 (0), the second's low 3 bits are nuh_temporal_id_plus1 (never 0) */
int hevc_detect(const uint8_t *s, size_t len)
{
	for (size_t i = 0; i + 4 < len && i < 4096; i++) {
		if (!(s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1))
			continue;
		uint8_t h0 = s[i + 3], h1 = s[i + 4];
		unsigned t = (h0 >> 1) & 0x3f;
		return !(h0 & 0x81) && (h1 & 7) && (t == 32 || t == 33 || t == 34 || t == 35 || t == 39 || hevc_irap(t));
	}
	return 0;
}

/* Annex B access units: a new AU starts at an AUD/VPS/SPS/PPS/prefix SEI (35, 32-34, 39, 41-44, 48-55) or at a VCL
 * NAL (0-31) with first_slice_segment_in_pic_flag 1, once the current one has a VCL NAL. One AU per ETB. */
int split_aus_hevc(const uint8_t *s, size_t len, uint32_t *off, uint32_t *alen, int nmax, struct h264_sps *sp,
		   struct hevc_sps *hs)
{
	int nau = 0, vcl = 0, sps = 0;
	size_t i = 0;
	while (i + 3 <= len) {
		if (!(s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1)) {
			i++;
			continue;
		}
		size_t sc = i > 0 && s[i - 1] == 0 ? i - 1 : i, h = i + 3;
		if (h + 1 >= len)
			break;
		unsigned t = (s[h] >> 1) & 0x3f;
		int isvcl = t < 32, first = isvcl && h + 2 < len && (s[h + 2] & 0x80);
		int starts = first || (t >= 32 && t <= 35) || t == 39 || (t >= 41 && t <= 44) || (t >= 48 && t <= 55);
		if (nau == 0 || (vcl && starts)) {
			if (nau >= nmax)
				break;
			if (nau)
				alen[nau - 1] = (uint32_t)(sc - off[nau - 1]);
			off[nau] = nau ? (uint32_t)sc : 0;
			nau++;
			vcl = 0;
		}
		if (isvcl)
			vcl = 1;
		if (t == 33 && !sps) {
			size_t e = h + 2;
			while (e + 3 <= len && !(s[e] == 0 && s[e + 1] == 0 && (s[e + 2] == 1 || s[e + 2] == 0)))
				e++;
			struct hevc_sps x;
			if (!hevc_sps_parse(s + h, (e + 3 <= len ? e : len) - h, &x)) {
				sps = 1;
				*sp = (struct h264_sps){ .w = x.w, .h = x.h, .cx = x.cx, .cy = x.cy, .cw = x.cw, .ch = x.ch };
				if (hs)
					*hs = x;
				printf("venus: HEVC SPS profile %d, chroma format %d, %d/%d bit: coded %dx%d, visible %dx%d at %d,%d\n",
				       x.profile, x.chroma, x.depth_y, x.depth_c, x.w, x.h, x.cw, x.ch, x.cx, x.cy);
			}
		}
		i = h;
	}
	if (nau)
		alen[nau - 1] = (uint32_t)(len - off[nau - 1]);
	return sps ? nau : -nau - 1;
}
