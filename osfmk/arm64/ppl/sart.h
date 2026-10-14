/*
 * sweet: Apple's SART (IOMMU for NVMe/ANS) header is not in the open
 * source drop. There is no SART on Qualcomm hardware; bootstrap is a no-op.
 */
#ifndef _ARM64_PPL_SART_H_
#define _ARM64_PPL_SART_H_

static inline void
sart_bootstrap(void)
{
}

#endif /* _ARM64_PPL_SART_H_ */
