/*
 * venus_hevc: the HEVC (H.265) Annex B helpers of video 4e, next to venus_h264's: access unit splitter, SPS size,
 * crop, profile, chroma format and bit depth, stream detection, IRAP NAL types. Used by venus_core (venusctl decode,
 * venusd), venusc and vtvenus (only hevc_sps_parse, which prints nothing). Venus here: 8-bit 4:2:0 Main only.
 */
#ifndef VENUS_HEVC_H
#define VENUS_HEVC_H
#include <stddef.h>
#include <stdint.h>
#include "venus_h264.h"

#define VD_HFI_CODEC_HEVC	0x2000u	/* HFI_VIDEO_CODEC_HEVC (vidc_hfi_helper.h:83); H.264 is 2 (:79) */

struct hevc_sps {
	int w, h, cx, cy, cw, ch;	/* pic_width/height_in_luma_samples; visible (conformance window) */
	int profile, compat;		/* general_profile_idc; general_profile_compatibility_flag bits (bit 31 - j = flag j) */
	int chroma, depth_y, depth_c;	/* chroma_format_idc; bit depths */
};

/* the NAL (from its 2-byte header) as an SPS; 0 = parsed to the bit depths with a sane size. Prints nothing. */
int hevc_sps_parse(const uint8_t *nal, size_t len, struct hevc_sps *sps);
/* 1: what Venus decodes here: Main (1) or Main Still Picture (3) or Main-compatible, 4:2:0, 8-bit luma and chroma */
int hevc_main8(const struct hevc_sps *sps);
static inline int hevc_irap(unsigned type)	/* BLA 16-18, IDR 19-20, CRA 21, reserved IRAP 22-23 */
{
	return type >= 16 && type <= 23;
}
/* 1: the first NAL of s looks like HEVC (VPS/SPS/PPS/AUD/prefix SEI with a valid 2-byte header), not H.264 */
int hevc_detect(const uint8_t *s, size_t len);
/* split_aus for HEVC (H.265 7.4.2.4.4): up to nmax access units; the first SPS gives *sps (size, crop) and, if hs,
 * *hs. nau if an SPS parsed, else -nau - 1. Prints the SPS line as venus_h264's sps_parse. */
int split_aus_hevc(const uint8_t *s, size_t len, uint32_t *off, uint32_t *alen, int nmax, struct h264_sps *sps,
		   struct hevc_sps *hs);

#endif
