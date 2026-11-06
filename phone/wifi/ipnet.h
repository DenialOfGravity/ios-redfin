#ifndef IPNET_H
#define IPNET_H
#include <stddef.h>
#include <stdint.h>
int ipnet_init(void);	/* a utun for the WLAN's IPv4; hooks into the radio (link up -> DHCP) */
void ipnet_poll(void);	/* from the link's idle loop */
/* shared utun helpers (also the cellular data path, ipagsi.c) */
int ipnet_utun_open(char *name, size_t n);	/* a new utunN, non-blocking; fd or -1 */
int ipnet_utun_set(const char *name, uint32_t ip, uint32_t peer, unsigned mtu);	/* network order; 0 or -1 */
#endif
