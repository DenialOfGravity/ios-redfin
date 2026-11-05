#ifndef IPAQMI_H
#define IPAQMI_H
/*
 * ipaqmi: the AP side of the modem <-> AP IPA QMI handshake (data design D2/D3). wland hosts QMI service 0x31
 * instance 1 (the "IPA host" service) and is a client of the modem's 0x31 instance 2. Protocol only: no IPA
 * register, no SMP2P, no SMEM. Off unless the word ipaqmi=1 (or ipaqmi=2) is in /var/local/cell.on (re-read every
 * 10 s): with it off no server is announced at all. See ipaqmi.c for the messages.
 */
#include <stdint.h>
#include <stddef.h>
#include "link.h"

#define IPAQMI_SERVICE		0x31
#define IPAQMI_HOST_INSTANCE	1		/* ours (AP) */
#define IPAQMI_MODEM_INSTANCE	2		/* the modem's */
#define IPAQMI_PORT		0x13		/* our server port */
#define IPAQMI_CLIENT_PORT	0x31		/* our client port (INIT_DRIVER to the modem's server) */
#define IPAQMI_SOCK		"/var/run/wland-ipa.sock"	/* `wland ipa init-driver` <-> the running wland (root only) */

void ipaqmi_init(void);			/* after pdmapper_init: reads cell.on once (announces if enabled) */
void ipaqmi_allow(void);		/* next to cell_allow: ipaqmi=2's one send waits for this + 15 s */
void ipaqmi_server(uint32_t service, uint32_t verinst, uint32_t node, uint32_t port);	/* NEW_SERVER (raw word 2) */
void ipaqmi_server_gone(uint32_t node, uint32_t port);					/* DEL_SERVER / BYE (port ~0) */
void ipaqmi_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n);		/* dst IPAQMI_PORT / _CLIENT_PORT */
void ipaqmi_poll(void);			/* from wland's idle loop */
int ipaqmi_cmd(int argc, char **argv);	/* `wland ipa ...` (argv[1] == "ipa") */

/* INIT_DRIVER (0x21) request, header included, into b (>= 7 + 162 bytes). smem = SHARED_MEM_BADDR bytes (0 by
 * default), ssr = include TLV 0x18 is_ssr_bootup = 1 (never on a cold boot). Returns the size, 0 if cap is short. */
size_t ipaqmi_encode_init_driver(uint8_t *b, size_t cap, uint16_t txn, uint32_t smem, int ssr);

#endif
