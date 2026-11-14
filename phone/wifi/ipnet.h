#ifndef IPNET_H
#define IPNET_H
#include <stddef.h>
#include <stdint.h>

int ipnet_init(void);	/* a utun for the WLAN's IPv4; hooks into the radio (link up -> DHCP) */
void ipnet_poll(void);	/* from the link's idle loop */

/* a second, independent utun for the cellular path (ipagsi.c): its own fd and name, nothing of
 * ipnet's own tun/tun_name/DHCP state is touched.  ipnet_utun_open() gives back the fd or -1 and
 * writes the interface name ("utunN"); ipnet_utun_set() gives it the address and MTU (all four
 * addresses in network order, mask 0 if the point-to-point default should be used). */
int ipnet_utun_open(char *name, size_t n);
int ipnet_utun_set(const char *name, uint32_t ip, uint32_t gw, uint32_t mask, unsigned mtu);
#endif
