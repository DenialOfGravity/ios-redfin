/* Shared C/C++ interface. Init once, on the SweetBattery start thread. */
#ifndef SWEET_BATTERY_SPMI_H
#define SWEET_BATTERY_SPMI_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int sweet_spmi_init(void);
/* Thread context only; 0 success, -1 failure; *out unchanged on failure. */
int sweet_spmi_read(unsigned sid, unsigned addr, uint8_t *out);
#ifdef __cplusplus
}
#endif
#endif
