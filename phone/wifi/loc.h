#ifndef LOC_H
#define LOC_H
/* loc: wland's QMI LOC client (modem GNSS, service 0x10) on its own QRTR port, and the PDS bridge for locationd
 * (pdsemu.c). Off unless /var/local/loc.on exists (re-read every 10 s). loc.c header. */
#include <stdint.h>
#include "link.h"

#define LOC_PORT	0x33		/* our client port (0x32 is ssctl.c's) */
#ifndef LOC_ON			/* the host test (test/run.sh) points these elsewhere */
#define LOC_ON		"/var/local/loc.on"
#define LOC_STATUS	"/var/local/loc.status"
#endif

void loc_poll(void);
void loc_server(uint32_t service, uint32_t instance, uint32_t node, uint32_t port);
void loc_server_gone(uint32_t node, uint32_t port);
void loc_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n);
int loc_print(void);

/* abpbridge.c: 1 when pds=1 is set (PDS requests then go to loc_pds_request first) */
int loc_pds_on(void);
/* abpbridge.c: a PDS request from CommCenter client (pipe, sess, cid); 1 = answered (fail, code, tl, tn filled) */
int loc_pds_request(uint32_t pipe, uint32_t sess, unsigned cid, const uint8_t *sdu, uint32_t sn, int *fail, int *code,
		    const uint8_t **tl, uint16_t *tn, const char **what);
void loc_pds_session_gone(uint32_t pipe, uint32_t sess);
#endif
