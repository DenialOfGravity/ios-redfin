#ifndef LOC_H
#define LOC_H
/* loc: sweetlocd's QMI LOC client (modem GNSS, service 0x10) on its own QRTR port of /dev/sweet-mqrtr-aux (xnu 1124), and
 * the PDS bridge for locationd (pdsemu.c) through the kernel router's relay (xnu 1122/1124). Off unless
 * /var/local/loc.on exists (re-read every 10 s). loc.c header. */
#include <stdint.h>
#include "link.h"

#define LOC_PORT	0x4700		/* our LOC client port: the kernel's aux ports are 0x4700-0x470f (SBB_AUX_PORT0) */
#define PDSR_WPORT	0x4701		/* our end of the kernel router's PDS relay (kern.sweet_qmux_pds) */
#define PDSR_KPORT	0x43fd		/* the kernel's end (node 1, SBB_PDSR_PORT) */
#ifndef LOC_ON			/* the host test (test/run.sh) points these elsewhere */
#define LOC_ON		"/var/local/loc.on"
#define LOC_STATUS	"/var/local/loc.status"
#endif

void loc_poll(void);
void loc_server(uint32_t service, uint32_t instance, uint32_t node, uint32_t port);
void loc_server_gone(uint32_t node, uint32_t port);
void loc_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n);
int loc_print(void);

/* 1 when pds=1 is set (and not dropped) */
int loc_pds_on(void);
/* a PDS request from CommCenter client (pipe, sess, cid) as the relay hands it; 1 = answered (fail, code, tl, tn filled) */
int loc_pds_request(uint32_t pipe, uint32_t sess, unsigned cid, const uint8_t *sdu, uint32_t sn, int *fail, int *code,
		    const uint8_t **tl, uint16_t *tn, const char **what);
void loc_pds_session_gone(uint32_t pipe, uint32_t sess);
/* sweetlocd.c: a DATA packet to PDSR_WPORT (the kernel router's relay records) */
void loc_pdsr_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n);
/* 1: the kernel is CommCenter's QMUX router (kern.sweet_bb & 2); the host test sets loc_fake_kernel_router */
int loc_kernel_router(void);
extern int loc_fake_kernel_router;

/* sweetlocd.c: loc.on present (as last read); shutdown (STOP + REG_EVENTS 0, relay BYE); nothing in flight/queued */
int loc_on_present(void);
void loc_shutdown(const char *why);
int loc_quiet(void);
/* 1 while an engine lock change/restore/repair (unlock= word, /var/local/locd.lock-orig) is running or due */
int loc_lock_busy(void);
/* ws-gnss-g: 1 while only the EFS write-back wait (after a confirmed restore) is left: sweetlocd stays up for it, up to
 * loc.c's EFS_WAIT_MS (900 s), instead of giving up after 60 s */
int loc_efs_waiting(void);
#endif
