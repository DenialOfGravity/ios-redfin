/*
 * sweet: Apple's UAT (GPU unified address translator) header is not in the
 * open source drop. No UAT on Qualcomm hardware; bootstrap is a no-op.
 */
#ifndef _ARM64_PPL_UAT_H_
#define _ARM64_PPL_UAT_H_

static inline void
uat_bootstrap(void)
{
}

#endif /* _ARM64_PPL_UAT_H_ */
