/*
 * redfin 1067/1068 (P4 phase 1): the kernel side of the modem (MPSS) link and the QMI router.
 * sweet_bb.c: boot-arg sweetbb, the GLINK/QRTR link to the modem, /dev/sweet-mqrtr, the "sweetbb" thread.
 * sweet_qmux.c: the QMUX <-> QRTR router behind the AppleBasebandPCI stand-in's qmux1/qmux2 (port of wland abpbridge.c).
 */
#ifndef _SWEET_BB_H_
#define _SWEET_BB_H_
#include <stdint.h>
#include <stdbool.h>

#define SBB_LINK                0x01u   /* kernel GLINK/QRTR to the modem + /dev/sweet-mqrtr (wland's link backend) */
#define SBB_DATA                0x10u   /* 1075 pdp_ip0..7 + 1076 kernel IPA data path (sweet_ipa.c; implies SBB_LINK) */
#define SBB_QMUX                0x02u   /* kernel QMI router behind qmux1/qmux2 (needs SBB_LINK and sweetabp & 1) */
#define SBB_REALBOOT            0x40u   /* 1071 (P4 phase 2b): ABM's real baseband boot against an emulated Mav17 BHI/Sahara
					 * target (pipes pbl/sahara, images discarded); the shouldBoot=0 page patch is off */
#define SBB_BITS                0x53u   /* bits this kernel implements (others are later phases: accepted, ignored) */

#define SBB_NODE_MODEM          0u
#define SBB_NODE_APPS           1u
#define SBB_NODE_BCAST          0xffffffffu
#define SBB_PORT_CTRL           0xfffffffeu

#define SBB_QMUX_PORT0          0x4000u /* router client ports (as wland's ABP_PORT0): one per (pipe, session, svc, cid) */
#define SBB_QMUX_NPORTS         0x400u

enum {
	SBB_QRTR_DATA = 1, SBB_QRTR_HELLO, SBB_QRTR_BYE, SBB_QRTR_NEW_SERVER, SBB_QRTR_DEL_SERVER, SBB_QRTR_DEL_CLIENT,
	SBB_QRTR_RESUME_TX, SBB_QRTR_EXIT, SBB_QRTR_PING, SBB_QRTR_NEW_LOOKUP, SBB_QRTR_DEL_LOOKUP,
};

struct sbb_qrtr_hdr {
	uint32_t version, type, src_node, src_port, confirm_rx, size, dst_node, dst_port;
};

extern uint32_t sweet_bb;               /* boot-arg sweetbb (RO kern.sweet_bb) */

/* sweet_bb.c */
uint64_t sbb_ms(void);
void sbb_wake(void);
bool sbb_link_open(void);
int sbb_link_state(void);               /* 1071: kern.sweet_mdm_up (0 down, 1 version done, 2 IPCRTR open) */
bool sbb_server(uint32_t svc, uint32_t inst, uint32_t *node, uint32_t *port, uint32_t *version);
bool sbb_server_svc(uint32_t svc, uint32_t *node, uint32_t *port, uint32_t *verinst);       /* 1076 */
int sbb_qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *data, uint32_t len);

/* sweet_qmux.c (all on the sweetbb thread, except sweet_qmux_c2w and the sysctls) */
bool sweet_qmux_on(void);
void sweet_qmux_start(void);
void sweet_qmux_c2w(unsigned pipe, uint32_t sess, unsigned type, const void *data, uint32_t len);
void sweet_qmux_poll(void);
void sweet_qmux_server(uint32_t service, uint32_t instance, uint32_t node, uint32_t port);
void sweet_qmux_server_gone(uint32_t node, uint32_t port);
void sweet_qmux_qrtr(const struct sbb_qrtr_hdr *h, const uint8_t *d, uint32_t n);
int sweet_qmux_stats(char *buf, size_t len);

/* sweet_abp.c */
void sweet_abp_set_ready(bool ready, const char *why);
#endif
