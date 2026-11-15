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
void sweet_spmi_write_runtime(int on);       /* 1123: kern.sweet_chg */
/*
 * 1062: one lock for every SPMI transaction of every sweet driver (keys, torch, camera GPIOs, haptic,
 * CS35L41 GPIOs, battery/charger/thermal/RTC here): command, status and data of one transfer can no
 * longer interleave with another driver's. sweet_spmi_bus_lock() returns whether it took the lock
 * (boot-arg sweetspmilock=0 or sysctl kern.sweet_spmi_lock=0: no lock, the old behaviour); pass that
 * to sweet_spmi_bus_unlock(). Thread context only (a mutex; the transfers spin up to 1 ms).
 */
int sweet_spmi_bus_lock(void);
void sweet_spmi_bus_unlock(int held);
#ifdef __cplusplus
}
#endif
#endif
