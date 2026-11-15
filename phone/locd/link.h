#ifndef LOCD_LINK_H
#define LOCD_LINK_H
/* sweetlocd's QRTR link: /dev/sweet-mqrtr-aux (xnu 1124), one QRTR packet (struct qrtr_hdr + payload) per read/write,
 * as wland's /dev/sweet-mqrtr. Implemented in sweetlocd.c (stubbed by the host tests). */
#include <stdint.h>

#define QRTR_NODE_APPS		1
#define QRTR_NODE_MODEM		0
#define QRTR_NODE_BCAST		0xffffffffu
#define QRTR_PORT_CTRL		0xfffffffeu

enum {
	QRTR_TYPE_DATA = 1, QRTR_TYPE_HELLO, QRTR_TYPE_BYE, QRTR_TYPE_NEW_SERVER,
	QRTR_TYPE_DEL_SERVER, QRTR_TYPE_DEL_CLIENT, QRTR_TYPE_RESUME_TX, QRTR_TYPE_EXIT,
	QRTR_TYPE_PING, QRTR_TYPE_NEW_LOOKUP, QRTR_TYPE_DEL_LOOKUP,
};

struct qrtr_hdr {
	uint32_t version, type, src_node, src_port, confirm_rx, size, dst_node, dst_port;
};

uint64_t link_ms(void);		/* since start */
int qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *payload, uint32_t len);
#endif
