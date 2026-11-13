/*
 * redfin 1076 (P4 M3a): the kernel IPA v4.7 / GSI data path (port of wland ipa.c meminit, ipagsi.c p2early/p2late,
 * ipaqmi.c, cell.c DPM Open Port). Boot-arg sweetbb & 0x10 (SBB_DATA); see sweet_ipa.c.
 */
#ifndef _SWEET_IPA_H_
#define _SWEET_IPA_H_
#include <stdint.h>
#include <stdbool.h>
#include <sys/cdefs.h>

#define SWEET_IPA_SRV_PORT      0x4600u /* our IPA QMI service 0x31/1 on the kernel's modem QRTR */
#define SWEET_IPA_CLI_PORT      0x4601u /* our client port: INIT_DRIVER / FILTER_INSTALLED_NOTIF to 0x31/2, DPM Open Port */

__BEGIN_DECLS
void sweet_ipa_init(void);              /* sweet_bb_init, only with sweetbb & 0x10 */
static inline bool
sweet_ipa_port(uint32_t port)
{
	return port == SWEET_IPA_SRV_PORT || port == SWEET_IPA_CLI_PORT;
}
/* a QRTR packet to one of our ports (sweet_bb.c qrtr_in, ml_lock held: it only queues a copy) */
void sweet_ipa_qrtr_in(uint32_t type, uint32_t src_node, uint32_t src_port, uint32_t dst_port, uint32_t confirm,
    const uint8_t *d, uint32_t n);
/* 1 when the data plane is ready (kern.sweet_ipa_state 3: INIT_COMPLETE sent + DPM port open); the QMI router may hold
 * WDS StartNetworkInterface until then. Lock-free, never blocks. */
int sweet_ipa_ready(void);
/* optional, for the router / PDP manager: a call came up (or went) on mux; gw = the call's IPv4 gateway (network order,
 * 0 = unknown) for the keep-alive. Arms the RX join like the first packet does; up 0 forgets the mux's gateway (1080).
 * Called by the QMI router (sweet_qmux.c, 1080) when CommCenter's calls come and go. */
void sweet_ipa_call(uint32_t mux, int up, uint32_t gw);
__END_DECLS
#endif
