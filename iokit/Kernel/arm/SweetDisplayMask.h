/*
 * The display's corner mask and touch clipping: Apple's continuous corner
 * (UIBezierPath's rounded rect since iOS 7, CALayer cornerCurve continuous:
 * per corner three cubics and a short line, reaching 1.52866483 r along each
 * edge), in integer math. The corners belong to the presented iOS image (the
 * GPU's fit, or the whole panel for the CPU copy), so the outline runs from
 * the panel's top edge into the image's side edge without a step.
 *
 * Radius in panel pixels. The default is the iPhone X's display corner, 39 pt
 * = 117 px, scaled to the fit (x 2280/2436): the corners iOS draws its UI
 * for. Against Pixel 4's glass outline (Android's config_mainDisplayShape for
 * flame) that curve lies inside the visible area below row 3.
 */
#ifndef SWEET_DISPLAY_MASK_H
#define SWEET_DISPLAY_MASK_H

#include <stdint.h>

#define SWEET_DISPLAY_CORNER_DEFAULT 110u
#define SWEET_DISPLAY_CORNER_MAX     160u       /* extent 245: fits the GPU's 256-px tiles */
#define SWEET_CORNER_STEPS           32u        /* lines per cubic */
#define SWEET_CORNER_SUB             16u        /* coverage sub-rows per pixel row */

/*
 * Top-left corner, x from the side edge and y from the top edge, in units of
 * r (16.16). From the top edge down: cubic, line (as a cubic), cubic, cubic.
 */
static const int32_t sweet_corner_k[13][2] = {
	{ 100183, 0 }, { 71335, 0 }, { 56912, 0 }, { 43905, 4292 },
	{ 43905, 4292 }, { 41386, 4909 }, { 41386, 4909 },
	{ 24433, 11079 }, { 11079, 24433 }, { 4909, 41386 },
	{ 0, 56912 }, { 0, 71335 }, { 0, 100183 },
};

/* Pixels the corner covers along each edge. */
static inline uint32_t
sweet_display_corner_extent(uint32_t r)
{
	return (uint32_t)(((uint64_t)sweet_corner_k[0][0] * r + 0xffff) >> 16);
}

/* Point i (0 .. 4 * STEPS) of the flattened corner, 1/65536 px; y grows with i. */
static inline void
sweet_display_corner_point(uint32_t r, uint32_t i, int64_t *x, int64_t *y)
{
	if (i == 0) {
		*x = (int64_t)sweet_corner_k[0][0] * r;
		*y = (int64_t)sweet_corner_k[0][1] * r;
		return;
	}
	const int32_t (*p)[2] = &sweet_corner_k[3 * ((i - 1) / SWEET_CORNER_STEPS)];
	int64_t k = (i - 1) % SWEET_CORNER_STEPS + 1, u = SWEET_CORNER_STEPS - k;
	int64_t w0 = u * u * u, w1 = 3 * u * u * k, w2 = 3 * u * k * k, w3 = k * k * k;
	int64_t n3 = (int64_t)SWEET_CORNER_STEPS * SWEET_CORNER_STEPS * SWEET_CORNER_STEPS;
	*x = (w0 * p[0][0] + w1 * p[1][0] + w2 * p[2][0] + w3 * p[3][0]) * r / n3;
	*y = (w0 * p[0][1] + w1 * p[1][1] + w2 * p[2][1] + w3 * p[3][1]) * r / n3;
}

/*
 * x where the visible area starts at height y (both 1/65536 px). *seg is a
 * cursor: start at 0, then ask for growing y.
 */
static inline int64_t
sweet_display_corner_edge(uint32_t r, int64_t y, uint32_t *seg)
{
	const uint32_t last = 4 * SWEET_CORNER_STEPS;
	int64_t x0, y0, x1, y1;

	for (;;) {
		sweet_display_corner_point(r, *seg, &x0, &y0);
		if (*seg >= last) {
			return y >= y0 ? 0 : x0;
		}
		sweet_display_corner_point(r, *seg + 1, &x1, &y1);
		if (y <= y1) {
			break;
		}
		(*seg)++;
	}
	if (y <= y0 || y1 == y0) {
		return x0;
	}
	return x0 + (x1 - x0) * (y - y0) / (y1 - y0);
}

/*
 * Coverage of the visible image (0..255) for row py of the top-left corner,
 * ext = sweet_display_corner_extent(r) bytes: exact horizontally, SUB sub-rows
 * vertically. *seg as above (rows in increasing order).
 */
static inline void
sweet_display_corner_row(uint32_t r, uint32_t ext, uint32_t py, uint32_t *seg, uint8_t *out)
{
	const int64_t one = 65536;
	int64_t edge[SWEET_CORNER_SUB];

	for (uint32_t s = 0; s < SWEET_CORNER_SUB; s++) {
		int64_t y = (int64_t)py * one + (2 * s + 1) * one / (2 * SWEET_CORNER_SUB);
		edge[s] = sweet_display_corner_edge(r, y, seg);
	}
	for (uint32_t px = 0; px < ext; px++) {
		int64_t sum = 0;
		for (uint32_t s = 0; s < SWEET_CORNER_SUB; s++) {
			int64_t c = (px + 1) * one - edge[s];
			sum += c <= 0 ? 0 : (c >= one ? one : c);
		}
		out[px] = (uint8_t)((sum * 255 + SWEET_CORNER_SUB * one / 2) / (SWEET_CORNER_SUB * one));
	}
}

/* Whether pixel (x, y) of the top-left corner (from its edges) is visible, at its centre. */
static inline bool
sweet_display_corner_point_visible(uint32_t x, uint32_t y, uint32_t r)
{
	uint32_t seg = 0;
	return ((int64_t)x << 16) + 0x8000 >= sweet_display_corner_edge(r, ((int64_t)y << 16) + 0x8000, &seg);
}

/* BGRA pixel times coverage c / 255 (alpha kept). */
static inline uint32_t
sweet_display_dim(uint32_t p, uint32_t c)
{
	uint32_t b = ((p & 0xff) * c + 127) / 255;
	uint32_t g = (((p >> 8) & 0xff) * c + 127) / 255;
	uint32_t r = (((p >> 16) & 0xff) * c + 127) / 255;
	return (p & 0xff000000) | (r << 16) | (g << 8) | b;
}

#endif /* SWEET_DISPLAY_MASK_H */
