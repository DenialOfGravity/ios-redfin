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
/*
 * redfin (0967): one-byte write through this EE's APID channel, ONLY with boot-arg
 * sweetchg=1 and ONLY to the charger registers on the allow-list (sweet_spmi.c).
 * Thread context. 0 = written and read back equal, -1 refused/failed, -2 read back differs.
 */
int sweet_spmi_write(unsigned sid, unsigned addr, uint8_t val);
int sweet_spmi_write_enabled(void);
#ifdef __cplusplus
}
#endif
#endif
