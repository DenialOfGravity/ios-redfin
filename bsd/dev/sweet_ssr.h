/*
 * redfin 1070 (P4 phase 2a): the modem's crash reason and the subsystem up/down notices (SSCTL) between the modem and
 * the audio DSP, as Linux qcom_q6v5 / qcom_sysmon do them. See sweet_ssr.c.
 */
#ifndef _SWEET_SSR_H_
#define _SWEET_SSR_H_
#include <stdint.h>
#include <stdbool.h>

#define SWEET_SSR_MODEM         0u
#define SWEET_SSR_ADSP          1u
#define SWEET_SSR_MDM_PORT      0x4500u /* our SSCTL client port on the kernel's modem QRTR (wland may not write from it) */
#define SWEET_SSR_ADSP_PORT     0x4002u /* our SSCTL client port on the kernel's DSP QRTR (0x4001 is pd-mapper) */

void sweet_ssr_init(void);
/* a DATA packet to one of our ports (called with the link's own locks held: it only queues) */
void sweet_ssr_qrtr_in(unsigned from, uint32_t type, uint32_t node, uint32_t port, uint32_t confirm, const uint8_t *d,
    uint32_t n);

/* sweet_bb.c */
int sbb_smem_copy(unsigned item, void *buf, uint32_t max, uint32_t *size);
bool sbb_server(uint32_t svc, uint32_t inst, uint32_t *node, uint32_t *port, uint32_t *version);
/* sweet_adsp.c */
int redwood_qrtr_send(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length);
bool redwood_qrtr_server(uint32_t service, uint32_t instance, uint32_t *node, uint32_t *port, uint32_t *version);
/* sweet_ufs.c */
extern int sweet_ufs_recover;
#endif
