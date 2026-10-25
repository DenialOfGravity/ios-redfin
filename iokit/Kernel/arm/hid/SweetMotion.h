/*
 * SWEET (redfin 0971/0972): the C interface between /dev/redwood-motion + `poke m` (bsd/dev/sweet_motion.c)
 * and the sensor HID services in SweetHID.cpp. ABI of the device: docs/redfin/qol-plan.md §3.3 and
 * phone/sensors/motion.h.
 */
#ifndef _SWEET_MOTION_H
#define _SWEET_MOTION_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What sensd writes: values are IEEE754 single-precision bit patterns (the kernel does no FP math). */
struct sweet_motion_sample {
	uint32_t type;		/* 13 accel m/s², 20 gyro rad/s, 12 als lux [1] CCT, 14 prox [0] near [1] raw,
				 * 21 compass µT, 31 pressure kPa, 10 orientation [0] UIDeviceOrientation */
	uint32_t flags;		/* bits 0-1 accuracy, bit 31 synthetic */
	uint64_t ts_mach;	/* SEE timestamp (19.2 MHz QTimer = mach_absolute_time); 0 = now */
	uint32_t v[4];
};
#define SWEET_MOTION_SYNTHETIC	0x80000000u

enum { SWEET_SENSOR_ACCEL, SWEET_SENSOR_GYRO, SWEET_SENSOR_ALS, SWEET_SENSOR_PROX, SWEET_SENSOR_COMPASS,
	SWEET_SENSOR_PRESSURE, SWEET_SENSOR_ORIENT, SWEET_SENSOR_COUNT };

struct sweet_motion_want {
	uint32_t version;
	uint32_t present;
	uint32_t clients[8];
	uint32_t interval_us[8];
	uint32_t interval_set[8];
};

/* Boot-arg sweetmotion: bit i enables sensor service i (default 0x77 = all but prox, which crash-looped
 * backboardd on 2026-10-26; 0x7f all); 0 = no services, no device; 0x100 = Transport "SPU";
 * 0x200 = every service answers copyEvent with a neutral sample until a real one exists. */
uint32_t sweet_motion_mask(void);
/* Post one sample from its service; 0, or ENODEV (no such service / disabled) / EINVAL (unknown type). */
int sweet_hid_sensor(const struct sweet_motion_sample *s);
/* Status text (one line per service); returns its length. */
int sweet_hid_sensor_status(char *buf, int len);
void sweet_hid_sensor_want(struct sweet_motion_want *w);
/* Runtime knobs (`poke m knob NAME VALUE`); 0, or -1 for an unknown name. */
int sweet_hid_sensor_knob(const char *name, int64_t value);
void sweet_hid_sensor_knobs_print(void);
void sweet_hid_sensor_relog(void);	/* forget which setProperties keys were logged */
/* Service index of a motion type, or -1. */
int sweet_motion_index(uint32_t type);

#ifdef __cplusplus
}
#endif
#endif /* _SWEET_MOTION_H */
