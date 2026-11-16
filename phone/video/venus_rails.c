/*
 * venus_rails (redfin video plan P0c, logs/research/video-plan-2026-11-16.md): rails and TBU votes for Venus.
 *
 * Which rails. The plan took MMCX + MX from upstream Linux SM8250 ("scale MMCX power domain on SM8250"). redfin is
 * SM7250 (lito) and its live DT (recon/live.dtb) has no mmcx.lvl at all: qcom,videocc@ab00000 (lito-videocc) has
 * one supply, vdd_cx-supply = pm8150_s1_level = RPMh "cx.lvl"; the ARC resources in the DT are cx/mx/ebi/gfx/lcx/lmx/
 * mss.lvl. So on lito the video clocks and GDSCs sit on CX, and MX must stay >= CX (Qualcomm rule; 0921 notes). The
 * code still looks mmcx.lvl up in cmd-db and votes it too if a SoC has it. qcom,vidc0 allowed-clock-rates: 240, 338,
 * 365 MHz; power on runs iris at 240 (or 192) MHz. Corner: VENUSCTL_RAILS=1 -> NOM (256), VENUSCTL_RAILS=N -> corner
 * N (e.g. 384 TURBO). The vote is an ARC level index (hlvl) from cmd-db's aux table, as wland's cx/mss votes.
 *
 * Raise only, and put back what was there. RPMh keeps ONE vote per DRV per resource (apps DRV: wland's cx top level,
 * venus, adsp, ... all share it: last write wins), so a vote can lower someone else's. Before voting:
 *   prev = the apps DRV's last cx/mx value: kern.sweet_rpmh_votes (xnu 1131: every vote XNU sent since boot), else
 *          the RSC's TCS command registers (what the last sender, e.g. ABL, left in them), else unknown.
 *   prev known and >= target: no vote, nothing to undo.
 *   prev known and < target: vote target; power off votes prev back (never 0: a 0 is left raised).
 *   prev unknown: vote the TOP level (cannot be lower than anything), left in place at power off (unknown = never
 *          lowered). VENUSCTL_RAILS_UNKNOWN=1: vote the target instead (the one place this may lower a vote).
 *          "Removing our vote" is not possible: the apps DRV has one vote per resource shared by every XNU client (no
 *          per-client drop; xnu 1131 is bookkeeping only), and the only way to withdraw it is an ARC 0, never sent.
 * p0b: TOP = the highest index with a nonzero corner. cmd-db's aux tables have 16 entries and lito's undefined tail is
 * corner 0 (mx.lvl and mmcx.lvl: [7..15] = 0, top = 6 = corner 416 / 384); r.n - 1 = 15 would have voted "corner 0"
 * and left it at power off. No index with corner 0 is ever a target or voted; a current/previous level with corner 0
 * is taken as unknown (raise) or not put back (restore).
 * At power off a rail goes back only if kern.sweet_rpmh_votes still shows our value (nobody voted since).
 * r17a: an mx.lvl vote back to 0 took the phone down: no ARC vote of 0 is ever sent from here.
 *
 * TBU: mnoc_sf_0_tbu (SIDs 0x1000-0x13ff) is powered only by someone else's vote. VENUSCTL_TBUVOTE=1 clears
 * SW_COLLAPSE in our hlos1 vote register (GCC 0x17d054), waits for PWR_ON (100 ms), and sets it again at power off
 * only if it was set before.
 * The state of both lives in RAILS_STATE with the boot ID, so `power on` and a later `power off` (another process,
 * or venusd) share it. venusctl rails: everything above, read only, plus what VENUSCTL_RAILS would do.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "venus_rails.h"

#define RSC_PA		0x18220000ull	/* apps RSC (bsd/dev/sweet_rpmh.c RH_RSC_PA), DRV 2 */
#define TCS_PA		0x18220d00ull
#define TCS_STRIDE	0x2a0u
#define CMD_STRIDE	0x14u
#define TCS_ENABLE	0x1cu
#define TCS_ADDR	0x34u
#define TCS_DATA	0x38u
#define TCS_N		8u		/* lito DT order: ACTIVE 0-1, SLEEP 2-4, WAKE 5-7 */
#define CMDDB_LITO	0x80860000ull
#define TBU_VOTE	0x0017d054ull	/* GCC hlos1_vote_mmnoc_mmu_tbu_sf0_gdsc */
#define TBU_COLLAPSE	(1u << 0)
#define TBU_PWR_ON	(1u << 31)
#define MAXLVL		32

static const char *const rail_names[] = { "mx.lvl", "mmcx.lvl", "cx.lvl" };	/* raise order; restore runs backwards */
#define NRAIL (sizeof(rail_names) / sizeof(rail_names[0]))

/* ---- pure helpers ---- */

/* p0b: cmd-db's aux tables have 16 entries; the undefined tail is corner 0 (lito mx.lvl/mmcx.lvl: [7..15] = 0).
 * Such an index is not a level: it is never a target and never voted. */
int rails_top_level(const uint16_t *vl, unsigned n)
{
	for (unsigned i = n; i-- > 1;)
		if (vl[i])
			return (int)i;
	return -1;
}

int rails_level_ok(const uint16_t *vl, unsigned n, uint32_t hlvl)
{
	return hlvl > 0 && hlvl < n && vl[hlvl] != 0;
}

int rails_hlvl_for(const uint16_t *vl, unsigned n, unsigned corner)
{
	for (unsigned i = 1; i < n; i++)
		if (vl[i] && vl[i] >= corner)
			return (int)i;
	return rails_top_level(vl, n);
}

int rails_vote_lookup(const char *votes, uint32_t addr, uint32_t *val)
{
	int found = 0;
	const char *p = votes;
	while (p && *p) {
		char *e;
		unsigned long a = strtoul(p, &e, 16);
		if (e == p || *e != '=')
			break;
		unsigned long v = strtoul(e + 1, &e, 16);
		if (a == addr) {	/* oldest first: the last match is the newest */
			*val = (uint32_t)v;
			found = 1;
		}
		while (*e == ' ')
			e++;
		p = e;
	}
	return found;
}

unsigned rails_target_corner(const char *env)
{
	if (!env || !*env)
		return 0;
	long v = strtol(env, NULL, 0);
	if (v <= 0)
		return 0;
	if (v == 1)
		return CORNER_NOM;
	return v > 0xffff ? 0xffff : (unsigned)v;
}

/* ---- cmd-db (as venus_core.c cmddb_aux, through rails_io) ---- */

struct rail {
	const char *name;
	uint32_t addr;
	unsigned n;
	uint16_t vl[MAXLVL];
};

static int cmddb_find(const char *id, struct rail *r)
{
	uint32_t (*rd)(uint64_t) = rails_io->rd32;
	uint64_t db = CMDDB_LITO;
	r->name = id;
	r->n = 0;
	if (rd(db + 4) != 0x0c0330db)
		return -1;
	uint64_t data = db + 4 + 4 + 8 * 16 + 4 + 4;
	for (uint32_t s = 0; s < 8; s++) {
		uint64_t rh = db + 8 + 16 * s;
		uint32_t w0 = rd(rh), w1 = rd(rh + 4);
		uint32_t hoff = w0 >> 16, doff = w1 & 0xffff, cnt = w1 >> 16;
		for (uint32_t e = 0; e < cnt && e < 512; e++) {
			uint64_t ent = data + hoff + 24 * e;
			char name[9];
			for (uint32_t i = 0; i < 8; i++)
				name[i] = (char)(rd(ent + (i & ~3u)) >> (8 * (i & 3)));
			name[8] = 0;
			if (strncmp(name, id, 8))
				continue;
			uint32_t w5 = rd(ent + 20);
			uint32_t len = w5 & 0xffff;
			uint64_t aux = data + doff + (w5 >> 16);
			r->addr = rd(ent + 16);
			for (uint32_t i = 0; i < len / 2 && i < MAXLVL; i++)
				r->vl[r->n++] = (uint16_t)((rd(aux + 2 * (i & ~1u)) >> (16 * (i & 1))) & 0xffff);
			return 0;
		}
	}
	return -1;
}

static unsigned corner_of(const struct rail *r, uint32_t hlvl)
{
	return hlvl < r->n ? r->vl[hlvl] : 0;
}

/* the TCS registers' last value for addr: active TCS first, then wake (sleep votes are not the running level) */
static int tcs_find(uint32_t addr, uint32_t *val, unsigned *where)
{
	uint32_t ncpt = (rails_io->rd32(RSC_PA + 0x0c) >> 27) & 0x1f;
	if (ncpt < 1 || ncpt > 16)
		return 0;
	static const unsigned order[] = { 0, 1, 5, 6, 7 };
	for (unsigned k = 0; k < sizeof(order) / sizeof(order[0]); k++) {
		unsigned t = order[k];
		for (uint32_t c = 0; c < ncpt; c++) {
			uint64_t b = TCS_PA + t * TCS_STRIDE + c * CMD_STRIDE;
			if (rails_io->rd32(b + TCS_ADDR) == addr) {
				*val = rails_io->rd32(b + TCS_DATA);
				*where = t;
				return 1;
			}
		}
	}
	return 0;
}

/* ---- state ---- */

struct rstate {
	int nr;
	struct { char name[16]; uint32_t addr, prev, voted; int known; } r[NRAIL];
	int tbu;		/* 1: we cleared SW_COLLAPSE */
	uint32_t tbu_prev;
};

static void st_load(struct rstate *s)
{
	memset(s, 0, sizeof(*s));
	FILE *f = fopen(rails_io->state, "r");
	if (!f)
		return;
	char line[256], id[128] = "";
	int ok = 0;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "boot %127s", id) == 1) {
			ok = !strcmp(id, rails_io->bootid());
			continue;
		}
		if (!ok)
			continue;
		if (s->nr < (int)NRAIL && sscanf(line, "rail %15s %x prev %x voted %x known %d", s->r[s->nr].name,
						 &s->r[s->nr].addr, &s->r[s->nr].prev, &s->r[s->nr].voted,
						 &s->r[s->nr].known) == 5)
			s->nr++;
		else if (sscanf(line, "tbu %x", &s->tbu_prev) == 1)
			s->tbu = 1;
	}
	fclose(f);
	if (!ok && (s->nr || s->tbu || id[0]))
		memset(s, 0, sizeof(*s));
	if (!ok && id[0])
		printf("venus: rails: %s is from another boot (%s): ignored\n", rails_io->state, id);
}

static void st_save(const struct rstate *s)
{
	if (!s->nr && !s->tbu) {
		unlink(rails_io->state);
		return;
	}
	char tmp[300];
	snprintf(tmp, sizeof(tmp), "%s.new", rails_io->state);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		printf("venus: rails: can't write %s: power off won't know what to put back\n", rails_io->state);
		return;
	}
	fprintf(f, "boot %s\n", rails_io->bootid());
	for (int i = 0; i < s->nr; i++)
		fprintf(f, "rail %s %#x prev %#x voted %#x known %d\n", s->r[i].name, s->r[i].addr, s->r[i].prev,
			s->r[i].voted, s->r[i].known);
	if (s->tbu)
		fprintf(f, "tbu %#x\n", s->tbu_prev);
	fclose(f);
	rename(tmp, rails_io->state);
}

static int arc_vote(const char *res, uint32_t hlvl)
{
	if (!hlvl) {	/* r17a */
		printf("venus: rails: refusing an ARC vote of 0 for %s\n", res);
		return -1;
	}
	uint32_t off[1] = { 0 }, val[1] = { hlvl };
	int rc = rails_io->rpmh(res, 1, off, val);
	for (int i = 1, w = 5; rc == 16 /* EBUSY */ && i < 6; i++, w *= 2) {
		rails_io->sleep_ms((unsigned)w);
		rc = rails_io->rpmh(res, 1, off, val);
	}
	printf("venus: rails: vote %s level %u -> %d\n", res, hlvl, rc);
	return rc;
}

/* prev of a rail: 2 = kern.sweet_rpmh_votes, 1 = TCS registers, 0 = unknown */
static int rail_prev(const struct rail *r, const char *votes, uint32_t *prev, unsigned *tcs)
{
	if (votes && rails_vote_lookup(votes, r->addr, prev))
		return 2;
	if (tcs_find(r->addr, prev, tcs))
		return 1;
	return 0;
}

static const char *src_name(int src)
{
	return src == 2 ? "xnu" : src == 1 ? "tcs" : "unknown";
}

/* p0b: is hlvl a defined level of this rail in cmd-db (nonzero corner)? */
static int rail_level_valid(const char *name, uint32_t hlvl)
{
	struct rail r;
	return !cmddb_find(name, &r) && rails_level_ok(r.vl, r.n, hlvl);
}

/* ---- commands ---- */

int rails_raise(void)
{
	unsigned corner = rails_target_corner(getenv("VENUSCTL_RAILS"));
	if (!corner)
		return 0;
	char votes[1024];
	int have = rails_io->votes(votes, sizeof(votes)) == 0;
	if (!have)
		printf("venus: rails: no kern.sweet_rpmh_votes (kernel before xnu 1131): only the TCS registers tell the"
		       " current votes\n");
	struct rstate s;
	st_load(&s);
	for (unsigned i = 0; i < NRAIL; i++) {
		struct rail r;
		int dup = 0;
		for (int k = 0; k < s.nr; k++)
			dup |= !strcmp(s.r[k].name, rail_names[i]);
		if (dup) {
			printf("venus: rails: %s already raised by an earlier power on (state %s)\n", rail_names[i],
			       rails_io->state);
			continue;
		}
		if (cmddb_find(rail_names[i], &r) || !r.n) {
			if (!strcmp(rail_names[i], "mmcx.lvl")) {
				printf("venus: rails: mmcx.lvl not in cmd-db (lito: video is on cx.lvl)\n");
				continue;
			}
			printf("venus: rails: STOP: %s not in cmd-db\n", rail_names[i]);
			return 1;
		}
		int target = rails_hlvl_for(r.vl, r.n, corner);
		if (target < 0) {
			printf("venus: rails: STOP: %s has no level with a nonzero corner\n", r.name);
			return 1;
		}
		uint32_t prev = 0;
		unsigned tcs = 0;
		int src = rail_prev(&r, have ? votes : NULL, &prev, &tcs);
		if (src && prev && !rails_level_ok(r.vl, r.n, prev)) {
			printf("venus: rails: %s: current level %u (%s) has corner 0 (not a level): taken as unknown\n", r.name,
			       prev, src_name(src));
			src = 0;
		}
		uint32_t v = (uint32_t)target;
		if (src && prev >= (uint32_t)target) {
			printf("venus: rails: %s %#x at level %u (corner %u, %s) >= level %u (corner %u): no vote\n", r.name,
			       r.addr, prev, corner_of(&r, prev), src_name(src), target, corner_of(&r, (uint32_t)target));
			continue;
		}
		if (!src && !getenv("VENUSCTL_RAILS_UNKNOWN"))
			v = (uint32_t)rails_top_level(r.vl, r.n);	/* the top defined level: lowers nobody */
		if (!rails_level_ok(r.vl, r.n, v)) {
			printf("venus: rails: STOP: %s level %u has corner 0: not voted\n", r.name, v);
			return 1;
		}
		printf("venus: rails: %s %#x: current %s%u (%s) -> level %u (corner %u; wanted corner %u)\n", r.name,
		       r.addr, src ? "" : "? ", prev, src_name(src), v, corner_of(&r, v), corner);
		/* recorded before the vote: a vote that may have gone through is still undone */
		snprintf(s.r[s.nr].name, sizeof(s.r[s.nr].name), "%s", r.name);
		s.r[s.nr].addr = r.addr, s.r[s.nr].prev = prev, s.r[s.nr].voted = v, s.r[s.nr].known = src != 0;
		s.nr++;
		st_save(&s);
		if (arc_vote(r.name, v)) {
			printf("venus: rails: STOP: %s vote failed\n", r.name);
			return 1;
		}
	}
	return 0;
}

int rails_restore(void)
{
	struct rstate s;
	st_load(&s);
	if (!s.nr)
		return 0;
	char votes[1024];
	int have = rails_io->votes(votes, sizeof(votes)) == 0;
	int rc = 0;
	for (int i = s.nr; i-- > 0;) {
		uint32_t now = 0;
		if (have && rails_vote_lookup(votes, s.r[i].addr, &now) && now != s.r[i].voted)
			printf("venus: rails: %s now %#x, not our %#x (someone voted since): left\n", s.r[i].name, now,
			       s.r[i].voted);
		else if (!s.r[i].known)
			printf("venus: rails: %s: the level before ours was unknown: left at %u\n", s.r[i].name,
			       s.r[i].voted);
		else if (!s.r[i].prev)
			printf("venus: rails: %s was 0 before: not voting 0 (r17a), left at %u\n", s.r[i].name,
			       s.r[i].voted);
		else if (!rail_level_valid(s.r[i].name, s.r[i].prev))
			printf("venus: rails: %s: level %u before has corner 0 (not a level): not voted, left at %u\n",
			       s.r[i].name, s.r[i].prev, s.r[i].voted);
		else if (arc_vote(s.r[i].name, s.r[i].prev)) {
			printf("venus: rails: %s: putting back %u failed (left at %u)\n", s.r[i].name, s.r[i].prev,
			       s.r[i].voted);
			rc = 1;
		} else {
			printf("venus: rails: %s back at level %u\n", s.r[i].name, s.r[i].prev);
		}
		s.nr--;
		st_save(&s);
	}
	return rc;
}

int tbu_vote_on(void)
{
	const char *e = getenv("VENUSCTL_TBUVOTE");
	if (!e || !atoi(e))
		return 0;
	struct rstate s;
	st_load(&s);
	uint32_t v = rails_io->rd32(TBU_VOTE);
	if (!(v & TBU_COLLAPSE)) {
		printf("venus: tbu: hlos1 vote %#llx = %#010x already on (sw_collapse 0)%s\n", TBU_VOTE, v,
		       s.tbu ? " (ours, earlier power on)" : ": nothing to undo");
	} else {
		s.tbu = 1, s.tbu_prev = v;
		st_save(&s);
		rails_io->wr32(TBU_VOTE, v & ~TBU_COLLAPSE);
		printf("venus: tbu: hlos1 vote %#llx %#010x -> %#010x\n", TBU_VOTE, v, v & ~TBU_COLLAPSE);
	}
	for (unsigned ms = 0; ms <= 100; ms++) {
		v = rails_io->rd32(TBU_VOTE);
		if (v & TBU_PWR_ON) {
			printf("venus: tbu: PWR_ON after %u ms (%#010x)\n", ms, v);
			return 0;
		}
		rails_io->sleep_ms(1);
	}
	printf("venus: tbu: STOP: no PWR_ON in 100 ms (%#010x)\n", v);
	return 1;
}

int tbu_vote_off(void)
{
	struct rstate s;
	st_load(&s);
	if (!s.tbu)
		return 0;
	uint32_t v = rails_io->rd32(TBU_VOTE);
	rails_io->wr32(TBU_VOTE, v | TBU_COLLAPSE);
	printf("venus: tbu: hlos1 vote %#llx %#010x -> %#010x (as before power on)\n", TBU_VOTE, v, v | TBU_COLLAPSE);
	s.tbu = 0;
	st_save(&s);
	return 0;
}

int rails_report(void)
{
	char votes[1024];
	int have = rails_io->votes(votes, sizeof(votes)) == 0;
	uint32_t id = rails_io->rd32(RSC_PA), cfg = rails_io->rd32(RSC_PA + 0x0c);
	printf("rails: apps RSC %#llx id %#x config %#x (cmds per TCS %u); read only, nothing is voted\n", RSC_PA, id,
	       cfg, (cfg >> 27) & 0x1f);
	printf("rails: kern.sweet_rpmh_votes: %s\n", have ? (votes[0] ? votes : "(empty: no vote since boot)")
							  : "not in this kernel (xnu 1131)");
	unsigned corner = rails_target_corner(getenv("VENUSCTL_RAILS"));
	for (unsigned i = 0; i < NRAIL; i++) {
		struct rail r;
		if (cmddb_find(rail_names[i], &r)) {
			printf("rails: %s: not in cmd-db\n", rail_names[i]);
			continue;
		}
		printf("rails: %s addr %#x, %u levels:", r.name, r.addr, r.n);
		for (unsigned k = 0; k < r.n; k++)
			printf(" [%u]=%u", k, r.vl[k]);
		printf("\n");
		uint32_t ncpt = (cfg >> 27) & 0x1f;
		for (unsigned t = 0; t < TCS_N && ncpt <= 16; t++)
			for (uint32_t c = 0; c < ncpt; c++) {
				uint64_t b = TCS_PA + t * TCS_STRIDE + c * CMD_STRIDE;
				if (rails_io->rd32(b + TCS_ADDR) != r.addr)
					continue;
				uint32_t d = rails_io->rd32(b + TCS_DATA), en = rails_io->rd32(TCS_PA + t * TCS_STRIDE + TCS_ENABLE);
				printf("rails:   TCS%u (%s) cmd %u: level %u (corner %u)%s\n", t,
				       t < 2 ? "active" : t < 5 ? "sleep" : "wake", c, d, corner_of(&r, d),
				       (en >> c) & 1 ? " enabled" : "");
			}
		uint32_t prev = 0;
		unsigned tcs = 0;
		int src = rail_prev(&r, have ? votes : NULL, &prev, &tcs);
		printf("rails:   apps vote now: %s", src ? "" : "unknown");
		if (src)
			printf("level %u (corner %u) from %s%s", prev, corner_of(&r, prev), src_name(src), src == 1 ?
			       (tcs < 2 ? " active" : " wake") : "");
		int tg = rails_hlvl_for(r.vl, r.n, corner ? corner : CORNER_NOM);
		if (tg < 0) {
			printf(" | no level with a nonzero corner: VENUSCTL_RAILS would stop\n");
			continue;
		}
		if (src && prev && !rails_level_ok(r.vl, r.n, prev)) {
			printf(" (corner 0: not a level, taken as unknown)");
			src = 0;
		}
		uint32_t v = !src && !getenv("VENUSCTL_RAILS_UNKNOWN") ? (uint32_t)rails_top_level(r.vl, r.n) : (uint32_t)tg;
		printf(" | VENUSCTL_RAILS=%u would %s\n", corner ? corner : 1,
		       src && prev >= (uint32_t)tg ? "not vote (already there)" : "vote");
		if (!(src && prev >= (uint32_t)tg))
			printf("rails:   ... level %u (corner %u), at power off %s\n", v, corner_of(&r, v),
			       !src ? "left at this level (unknown before; the apps DRV has one vote per resource, no per-client "
				      "drop: the only removal is an ARC 0, never sent)" :
			       !prev ? "left (never 0)" : "back to the level before");
	}
	uint32_t tv = rails_io->rd32(TBU_VOTE);
	printf("rails: tbu mnoc_sf_0 hlos1 vote %#llx = %#010x (sw_collapse %u, PWR_ON %u)\n", TBU_VOTE, tv,
	       tv & TBU_COLLAPSE, tv >> 31);
	struct rstate s;
	st_load(&s);
	for (int i = 0; i < s.nr; i++)
		printf("rails: state: %s raised to %u by venusctl (before: %s%u)\n", s.r[i].name, s.r[i].voted,
		       s.r[i].known ? "" : "unknown ", s.r[i].prev);
	if (s.tbu)
		printf("rails: state: tbu vote ours (before %#x)\n", s.tbu_prev);
	return 0;
}

void rails_line(char *buf, size_t len)
{
	static struct rail r[NRAIL];
	static int looked;
	if (!looked) {
		looked = 1;
		for (unsigned i = 0; i < NRAIL; i++)
			if (cmddb_find(rail_names[i], &r[i]))
				r[i].n = 0;
	}
	char votes[1024];
	int have = rails_io->votes(votes, sizeof(votes)) == 0;
	size_t o = (size_t)snprintf(buf, len, "up %.1f s |", rails_io->uptime());
	for (unsigned i = 0; i < NRAIL && o < len; i++) {
		if (!r[i].n)
			continue;
		uint32_t v = 0;
		if (have && rails_vote_lookup(votes, r[i].addr, &v))
			o += (size_t)snprintf(buf + o, len - o, " %s %u/%u", r[i].name, v, corner_of(&r[i], v));
		else
			o += (size_t)snprintf(buf + o, len - o, " %s ?", r[i].name);
	}
	uint32_t tv = rails_io->rd32(TBU_VOTE);
	if (o < len)
		snprintf(buf + o, len - o, " | tbu %s%s", tv & TBU_PWR_ON ? "on" : "OFF", tv & TBU_COLLAPSE ? "" : " (hlos1)");
}

/* ---- the phone's io ---- */
#ifndef RAILS_HOST_TEST
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include "pmem.h"
#include "venus_core.h"

static int io_votes(char *buf, size_t len)
{
	size_t n = len - 1;
	if (sysctlbyname("kern.sweet_rpmh_votes", buf, &n, NULL, 0))
		return -1;
	buf[n < len ? n : len - 1] = 0;
	return 0;
}

static double io_uptime(void)
{
	struct timeval bt = { 0, 0 }, now;
	size_t n = sizeof(bt);
	sysctlbyname("kern.boottime", &bt, &n, NULL, 0);
	gettimeofday(&now, NULL);
	return (double)(now.tv_sec - bt.tv_sec) + (now.tv_usec - bt.tv_usec) / 1e6;
}

static struct rails_io phone_io = {
	rd32, wr32, pm_rpmh, io_votes, io_uptime, delay_ms, "/var/tmp/venusctl.rails", boot_id,
};
const struct rails_io *rails_io = &phone_io;
#endif
