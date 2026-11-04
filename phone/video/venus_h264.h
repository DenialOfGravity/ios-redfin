/*
 * venus_h264: the H.264 Annex B helpers of venusctl decode (stage 3), shared by venusctl/venusd (venus_core.c) and
 * the venusd test client venusc, which has no /dev/pmem: access unit splitter, SPS size/crop, zlib CRC32.
 * Moved out of venusctl.c unchanged (video 4a); the prints are venusctl's.
 */
#ifndef VENUS_H264_H
#define VENUS_H264_H
#include <stddef.h>
#include <stdint.h>

struct h264_sps { int w, h, cx, cy, cw, ch; };	/* coded size; visible crop */

uint32_t crc32_buf(const uint8_t *p, size_t n);	/* zlib crc32: what tools/sunfish-venus-yuvcmp.py prints */
int sps_parse(const uint8_t *nal, size_t len, struct h264_sps *sps);	/* NAL from its header byte; 0 = sane size */
/* up to nmax access units of s: offsets and lengths; parses the first SPS into *sps. nau if an SPS parsed, else
 * -nau - 1 */
int split_aus(const uint8_t *s, size_t len, uint32_t *off, uint32_t *alen, int nmax, struct h264_sps *sps);

#endif
