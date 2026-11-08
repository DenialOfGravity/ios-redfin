#ifndef IPAPROBE_H
#define IPAPROBE_H
/*
 * ipaprobe.c (Claude 2026-11-09): diagnostics for the cellular data path, independent of iOS.
 *
 * p2pkt=N in cell.on: decode and log the IP packets that cross the IPA (N=1: RX from the modem, N=2: RX and TX),
 *   one line each (proto, addresses, ports, TTL, TCP flags, ICMP type/code, DNS id/rcode), the first 300 only.
 * p2probe=N: after the call is up, wland itself sends test packets from the call's address every 20 s (N=1: 6 rounds,
 *   N=2: for ever): ICMP echo to the gateway and 8.8.8.8, DNS A queries to the operator's DNS 1 and to 8.8.8.8/1.1.1.1,
 *   TCP SYN to 1.1.1.1:80, 1.1.1.1:443 and 8.8.8.8:53.  Replies are recognised and logged as "probe REPLY".
 *   Per round the modem's GSI pointers (EE1 ch0 ep5 DL, ch2 ep8 DL, ch3 pipe 10, ch7 ep14 UL) are logged before and
 *   after, so DL that reaches the IPA but not us (route table 5 -> pipe 10) is visible as well.
 * Nothing here touches hardware except through the send callback (ipagsi's TX ring) and the read-only pointer reads.
 */
#include <stddef.h>
#include <stdint.h>
#include "ipagsi.h"

void ipaprobe_pkt(const char *dir, uint8_t mux, const uint8_t *ip, size_t n);	/* rx_to_utun / tx_send */
void ipaprobe_poll(const struct ipagsi_call *c, int (*send)(const uint8_t *ip, size_t len), unsigned tx_free,
		   uint32_t (*rd32)(uint64_t pa));
#endif
