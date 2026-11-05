#ifndef CELL_H
#define CELL_H
/*
 * Read-only cellular status (track T step 1): QMI UIM/NAS/DMS on the modem, over wland's own QRTR link to it
 * (wland boots MPSS and is the only QRTR endpoint on the apps side for the modem). Off unless /var/local/cell.on
 * exists (checked every 10 s, so it can be switched on and off without restarting wland); its content may hold
 * "slots=1" (UIM Get Slots Status) and "hw=1" (DMS hardware revision), both off otherwise. "pin=1" opens CELL_SOCK for
 * the SIM PIN UI (SweetPIN): one UIM Verify PIN per user entry (cell.c, PIN section). "wds=1" adds read-only WDS
 * queries (packet service status, bearer, 3GPP profiles; cell.c header). Starts only 15 s after
 * Wi-Fi's WLFW bring-up ended (cell_allow), or 3 min after the link came up if WLFW never shows. Never registers,
 * never changes the operating mode, no SMS, no data. Results: one line in wland.log ("cell: ...") and in
 * /var/local/cell.status (`wland cell` prints it).
 */
#include <stdint.h>
#include "link.h"

#define CELL_PORT	0x30		/* our client port */
#define CELL_ON		"/var/local/cell.on"
#define CELL_STATUS	"/var/local/cell.status"
#define CELL_SOCK	"/var/run/wland-cell.sock"	/* SIM PIN UI <-> wland (pin=1 in cell.on; cell.c) */

void cell_server(uint32_t service, uint32_t instance, uint32_t node, uint32_t port);	/* NEW_SERVER */
void cell_server_gone(uint32_t node, uint32_t port);					/* DEL_SERVER / BYE */
void cell_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n);
void cell_poll(void);
void cell_allow(void);		/* Wi-Fi WLFW bring-up over: cell may start 15 s later */			/* from wland's idle loop */
int cell_print(void);			/* `wland cell` */
#endif
