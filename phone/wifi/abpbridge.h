#ifndef ABPBRIDGE_H
#define ABPBRIDGE_H
/* abpbridge: wland's end of kernel 1061/1063's /dev/abpbridge (Apple CommCenter's QMUX pipes qmux1/qmux2). Off unless
 * abpbridge=N is in /var/local/cell.on: 1 = P1 observe (CTL here, everything else a QMI error), 2 = P2 router (CTL here,
 * policy table, standard services forwarded to the modem over QRTR from ports ABP_PORT0..). abpbridge.c header. */
#include <stdint.h>
#include "link.h"

#define ABP_PORT0	0x4000		/* router client ports: one per (pipe, session, service, client id) */
#define ABP_NPORTS	0x400

void abpbridge_early(void);		/* from wland's net() start: open the bridge early (if the word is set) */
void abpbridge_poll(void);		/* from wland's idle loop */
void abpbridge_server(uint32_t service, uint32_t instance, uint32_t node, uint32_t port);	/* NEW_SERVER */
void abpbridge_server_gone(uint32_t node, uint32_t port);				/* DEL_SERVER / BYE */
void abpbridge_qrtr(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n);		/* DATA to ABP_PORT0.. */
#endif
