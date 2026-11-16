#ifndef SSCTL_H
#define SSCTL_H
/* ssctl.c: SSCTL "adsp after-powerup" to the modem (word ssctladsp=1 in /var/local/cell.on; see ssctl.c) */
#include <stdint.h>
#include "link.h"

#define SSCTL_PORT	0x32		/* our client port */

void ssctl_server(uint32_t service, uint32_t instance, uint32_t node, uint32_t port);	/* NEW_SERVER */
void ssctl_server_gone(uint32_t node, uint32_t port);					/* DEL_SERVER / BYE */
void ssctl_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n);		/* dst SSCTL_PORT */
void ssctl_poll(void);									/* idle loop */
#endif
