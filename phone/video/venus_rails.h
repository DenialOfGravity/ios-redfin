/*
 * venus_rails (redfin video P0c): the Venus power rails and the mnoc_sf_0 TBU vote, raise-only, with a way back.
 * See venus_rails.c. Default off: without VENUSCTL_RAILS / VENUSCTL_TBUVOTE nothing here writes anything.
 */
#ifndef VENUS_RAILS_H
#define VENUS_RAILS_H
#include <stddef.h>
#include <stdint.h>

/* RPMh ARC corners (Linux include/dt-bindings/power/qcom-rpmpd.h, RPMH_REGULATOR_LEVEL_*) */
#define CORNER_RET	16
#define CORNER_LOW_SVS	64
#define CORNER_SVS	128
#define CORNER_SVS_L1	192
#define CORNER_NOM	256
#define CORNER_TURBO	384

/* everything that touches the phone goes through this (host tests swap in a mock) */
struct rails_io {
	uint32_t (*rd32)(uint64_t pa);
	void (*wr32)(uint64_t pa, uint32_t v);
	int (*rpmh)(const char *res, unsigned n, const uint32_t *off, const uint32_t *val);	/* errno or 0 */
	int (*votes)(char *buf, size_t len);	/* kern.sweet_rpmh_votes (xnu 1131); -1: not in this kernel */
	double (*uptime)(void);			/* seconds since boot */
	void (*sleep_ms)(unsigned ms);
	const char *state;			/* what to undo at power off (survives between venusctl runs) */
	const char *(*bootid)(void);		/* the state file's "boot" line; another boot's file is ignored */
};
extern const struct rails_io *rails_io;

/* pure helpers (tested on the host) */
int rails_hlvl_for(const uint16_t *vl, unsigned n, unsigned corner);	/* smallest level >= corner; top if none; -1 none */
int rails_top_level(const uint16_t *vl, unsigned n);	/* highest index with a nonzero corner; -1 if none */
int rails_level_ok(const uint16_t *vl, unsigned n, uint32_t hlvl);	/* 1: 0 < hlvl < n and corner != 0 */
int rails_vote_lookup(const char *votes, uint32_t addr, uint32_t *val);	/* 1: found in "ADDR=VAL ..." (hex) */
unsigned rails_target_corner(const char *env);	/* VENUSCTL_RAILS: NULL/"0" -> 0 (off), "1" -> NOM, N -> N */

int rails_dt_allowed(const char *res);	/* p0c2: 1 for the video rails of redfin's DT (cx.lvl only) */
int rails_rpmh_halted(void);		/* p0c2: 1 after an RPMh vote timed out or got EIO in this process */
void rails_rpmh_note(const char *res, int rc);	/* p0c2: every RPMh vote result goes through here */
const char *rails_rpmh_halt_why(void);
int rails_rpmh_soft(void);		/* rpmh-tcs: VENUS_RPMH_SOFT=0|1, else /var/local/venus-rpmh-soft present */
void rails_rpmh_rearm(void);		/* rpmh-tcs: soft mode: a new power on clears the halt of an earlier attempt */

int rails_report(void);		/* venusctl rails: read only */
int rails_raise(void);		/* power on, first step: VENUSCTL_RAILS; 0 ok or off */
int rails_restore(void);	/* power off, last step */
int tbu_vote_on(void);		/* VENUSCTL_TBUVOTE=1: hlos1 vote of mnoc_sf_0_tbu, wait PWR_ON */
int tbu_vote_off(void);		/* only if tbu_vote_on changed it */
void rails_line(char *buf, size_t len);	/* watch: "up S | cx hlvl/corner (src) mx ... | tbu ..." */
#endif
