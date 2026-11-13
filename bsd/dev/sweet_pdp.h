/* redfin 1075: pdp_ip cellular data interfaces (sweet_pdp.c) and the API for the future in-kernel IPA driver. */
#ifndef _SWEET_PDP_H_
#define _SWEET_PDP_H_

#include <sys/kpi_mbuf.h>

#define SWEET_PDP_SBB_BIT       0x10u   /* boot-arg sweetbb bit (RO kern.sweet_bb) that turns all of 1075 on */
#define SWEET_PDP_NUNITS        8u
#define SWEET_PDP_MUX_FLAG      0x100u
#define SWEET_PDP_BY_MUX(mux)   (SWEET_PDP_MUX_FLAG | ((mux) & 0xffu))

__BEGIN_DECLS
/* fn owns m on every path (frees it or hands it on); returns 0 when the packet was taken for sending */
typedef int (*sweet_pdp_output_fn)(uint32_t unit, uint32_t mux, mbuf_t m);

bool sweet_pdp_enabled(void);
void sweet_pdp_publish(void);                           /* SweetABP publish: creates pdp_ip0..7 once */
int  sweet_pdp_input(uint32_t unit_or_mux, mbuf_t m);   /* always consumes m */
void sweet_pdp_set_output(sweet_pdp_output_fn fn);      /* NULL = drop (counted) */
int  sweet_pdp_link(uint32_t unit, int up);
void sweet_pdp_block(uint32_t index, int on);           /* AppleIPAppender blockPDPTraffic */
void sweet_pdp_unblock_all(void);
__END_DECLS

#endif
