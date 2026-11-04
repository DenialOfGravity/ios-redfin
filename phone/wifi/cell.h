#ifndef CELL_H
#define CELL_H
/*
 * Read-only cellular status (track T step 1): QMI UIM/NAS/DMS on the modem, over wland's own QRTR link to it
 * (wland boots MPSS and is the only QRTR endpoint on the apps side for the modem). Off unless /var/local/cell.on
 * exists (checked every 10 s, so it can be switched on and off without restarting wland). Never registers,
 * never changes the operating mode, no SMS, no data. Results: one line in wland.log ("cell: ...") and in
 * /var/local/cell.status (`wland cell` prints it).
 */
#include <stdint.h>
#include "link.h"

#define CELL_PORT	0x30		/* our client port */
#define CELL_ON		"/var/local/cell.on"
#define CELL_STATUS	"/var/local/cell.status"

void cell_server(uint32_t service, uint32_t instance, uint32_t node, uint32_t port);	/* NEW_SERVER */
void cell_server_gone(uint32_t node, uint32_t port);					/* DEL_SERVER / BYE */
void cell_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n);
void cell_poll(void);			/* from wland's idle loop */
int cell_print(void);			/* `wland cell` */
#endif
