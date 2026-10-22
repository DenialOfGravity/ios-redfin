/*
 * Small shared helpers for the display's continuous-corner mask and touch
 * clipping. A fourth-power superellipse gives the corners a soft, squircle
 * profile similar to Core Animation's continuous corner curve.
 */
#ifndef SWEET_DISPLAY_MASK_H
#define SWEET_DISPLAY_MASK_H

#include <stdint.h>

#define SWEET_DISPLAY_CORNER_DEFAULT 64u
#define SWEET_DISPLAY_MASK_SAMPLES   4u

static inline uint64_t
sweet_display_fourth(uint32_t v)
{
	uint64_t v2 = (uint64_t)v * v;
	return v2 * v2;
}

/* dx/dy and radius use the same fixed-point scale. */
static inline bool
sweet_display_corner_inside(uint32_t dx, uint32_t dy, uint32_t radius)
{
	return sweet_display_fourth(dx) + sweet_display_fourth(dy) <= sweet_display_fourth(radius);
}

/* Pixel coverage for a top-corner pixel; return 0..16 for 4x4 sampling. */
static inline uint32_t
sweet_display_corner_coverage(uint32_t x, uint32_t y, uint32_t radius)
{
	if (radius == 0 || x >= radius || y >= radius) {
		return 16;
	}
	if (sweet_display_corner_inside(radius - x, radius - y, radius)) {
		return 16;
	}
	if (!sweet_display_corner_inside(radius - x - 1, radius - y - 1, radius)) {
		return 0;
	}

	uint32_t hits = 0;
	uint32_t sample_scale = SWEET_DISPLAY_MASK_SAMPLES * 2;
	uint32_t scaled_radius = radius * sample_scale;
	for (uint32_t sy = 0; sy < SWEET_DISPLAY_MASK_SAMPLES; sy++) {
		uint32_t py = y * sample_scale + sy * 2 + 1;
		uint32_t dy = scaled_radius - py;
		for (uint32_t sx = 0; sx < SWEET_DISPLAY_MASK_SAMPLES; sx++) {
			uint32_t px = x * sample_scale + sx * 2 + 1;
			uint32_t dx = scaled_radius - px;
			if (sweet_display_fourth(dx) + sweet_display_fourth(dy) <=
			    sweet_display_fourth(scaled_radius)) {
				hits++;
			}
		}
	}
	return hits;
}

/* Touch coordinates are pixel indices; test their pixel centers. */
static inline bool
sweet_display_corner_point_visible(uint32_t x, uint32_t y, uint32_t radius)
{
	if (radius == 0 || x >= radius || y >= radius) {
		return true;
	}
	uint32_t scaled_radius = radius * 2;
	uint32_t px = x * 2 + 1;
	uint32_t py = y * 2 + 1;
	return sweet_display_corner_inside(scaled_radius - px, scaled_radius - py, scaled_radius);
}

#endif /* SWEET_DISPLAY_MASK_H */
