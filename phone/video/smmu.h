#ifndef SMMU_H
#define SMMU_H
#include <stdint.h>

/* stream SID/MASK through a translation-off bank (or one already set up like that); NAME for the log. 0 = ok. */
int smmu_identity(const char *name, uint32_t sid, uint32_t mask);
/* camera d: the registers the last smmu_identity changed (slot -1: it wrote nothing; cb/cbp set if it found one) */
struct smmu_saved {
	int slot, cb;
	uint64_t cbp, gr1;
	uint32_t smr, s2cr, cbar, cba2r, sctlr;
};
extern struct smmu_saved smmu_last;
int smmu_release(const char *name);	/* put them back (SMR first); 0 = ok or nothing to do, 1 = SMR invalid but
					 * other registers differ (video v2c), -1 = SMR still valid */

/* SIDS through a new translating bank: [IOVA, IOVA+LEN) -> PA (32-bit IOVA, within one 2 MB block). 0 = ok. */
int smmu_translate(const char *name, const uint16_t *sids, unsigned n, uint64_t iova, uint64_t pa, uint64_t len);
/* redfin track V: SIDS through one new translating bank mapping only [BASE, BASE+LEN) IOVA = PA (2 MB blocks), tables
 * at TBL (2 pages below 4 GB); everything else faults in that bank. saved[k] for smmu_release. Bank or -1. */
int smmu_window(const char *name, const uint16_t *sids, unsigned n, uint64_t base, uint64_t len, uint64_t tbl,
		struct smmu_saved *saved);
void smmu_faults(const char *name, uint32_t sid);	/* FSR/FAR/FSYNR0 of the bank SID goes through */
void smmu_status(const char *name, uint32_t sid);	/* sGFSR/sGFSYNR + smmu_faults, read-only */
void smmu_dump(const char *name, uint32_t sid, uint64_t iova);	/* bank registers + table walk, read-only */
extern int smmu_dry;	/* 1: only report what would be written */
extern uint64_t smmu_alias;	/* smmu_translate also maps IOVA | smmu_alias (default 3<<32) */

#endif
