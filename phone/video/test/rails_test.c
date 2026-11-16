/*
 * Host test for venus_rails.c (redfin video P0c): cc -DRAILS_HOST_TEST -I.. rails_test.c ../venus_rails.c && ./a.out
 * A mock phone: a fake cmd-db with cx.lvl/mx.lvl (no mmcx.lvl, as lito), apps RSC TCS registers, the TBU vote
 * register, and an RPMh that records votes and keeps kern.sweet_rpmh_votes like xnu 1131.
 * Run by phone/video/test/run.sh.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../venus_rails.h"

#define CMDDB	0x80860000ull
#define MEMSZ	0x2000
static uint8_t db[MEMSZ];
static uint32_t rsc_cfg = (8u << 27);	/* 8 cmds per TCS */
static uint32_t tcs_addr[8][16], tcs_data[8][16];
static uint32_t tbu = 0x80000000u;	/* PWR_ON, not collapsed by us */
static int tbu_follow = 1;		/* PWR_ON follows our vote */

struct vote { char res[16]; uint32_t addr, val; };
static struct vote sent[64];
static int nsent;
static char votes_str[1024];
static int have_votes = 1;
static uint32_t addr_of(const char *res) { return !strcmp(res, "cx.lvl") ? 0x30010 : !strcmp(res, "mx.lvl") ? 0x30020 : 0x50000; }

static void note(uint32_t addr, uint32_t val)	/* xnu 1131's bookkeeping, as text */
{
	char out[1024] = "", *p = votes_str;
	int found = 0;
	while (*p) {
		char *e;
		unsigned long a = strtoul(p, &e, 16), v = strtoul(e + 1, &e, 16);
		if (a != addr)
			snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s%lx=%lx", out[0] ? " " : "", a, v);
		else
			found = 1;
		while (*e == ' ')
			e++;
		p = e;
	}
	(void)found;
	snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s%x=%x", out[0] ? " " : "", addr, val);
	strcpy(votes_str, out);
}

static uint32_t m_rd32(uint64_t pa)
{
	if (pa >= CMDDB && pa + 4 <= CMDDB + MEMSZ) {
		uint32_t v;
		memcpy(&v, db + (pa - CMDDB), 4);
		return v;
	}
	if (pa == 0x18220000ull)
		return 0x02000000;
	if (pa == 0x1822000cull)
		return rsc_cfg;
	if (pa >= 0x18220d00ull && pa < 0x18220d00ull + 8 * 0x2a0) {
		uint32_t o = (uint32_t)(pa - 0x18220d00ull), t = o / 0x2a0, r = o % 0x2a0;
		if (r >= 0x34 && (r - 0x34) % 0x14 == 0)
			return tcs_addr[t][(r - 0x34) / 0x14];
		if (r >= 0x38 && (r - 0x38) % 0x14 == 0)
			return tcs_data[t][(r - 0x38) / 0x14];
		return 0;
	}
	if (pa == 0x17d054ull)
		return tbu;
	return 0xdeadbeef;
}
static void m_wr32(uint64_t pa, uint32_t v)
{
	assert(pa == 0x17d054ull);	/* the only register rails may write */
	tbu = v;
	if (tbu_follow)
		tbu = (v & 1) ? (v & ~0x80000000u) : (v | 0x80000000u);
}
static int m_rpmh(const char *res, unsigned n, const uint32_t *off, const uint32_t *val)
{
	assert(n == 1 && off[0] == 0);
	assert(val[0] != 0);	/* r17a: never an ARC 0 */
	snprintf(sent[nsent].res, sizeof(sent[0].res), "%s", res);
	sent[nsent].addr = addr_of(res), sent[nsent].val = val[0];
	nsent++;
	note(addr_of(res), val[0]);
	return 0;
}
static int m_votes(char *b, size_t len)
{
	if (!have_votes)
		return -1;
	snprintf(b, len, "%s", votes_str);
	return 0;
}
static double m_up(void) { return 123.4; }
static void m_sleep(unsigned ms) { (void)ms; }
static const char *m_boot(void) { return "BOOT-A"; }
static char state_path[64];
static struct rails_io mock = { m_rd32, m_wr32, m_rpmh, m_votes, m_up, m_sleep, state_path, m_boot };
const struct rails_io *rails_io = &mock;

/* cmd-db: header magic at +4, 8 slave headers at +8 (16 bytes), data at +8+128+8 */
static void db_add(int idx, const char *name, uint32_t addr, const uint16_t *vl, unsigned n)
{
	uint32_t magic = 0x0c0330db;
	memcpy(db + 4, &magic, 4);
	uint32_t data = 4 + 4 + 8 * 16 + 4 + 4;
	/* slave 0: entries at hoff 0, aux at doff 0x400; cnt = idx + 1 */
	uint32_t w0 = 0 << 16, w1 = 0x400 | ((uint32_t)(idx + 1) << 16);
	memcpy(db + 8, &w0, 4);
	memcpy(db + 12, &w1, 4);
	uint8_t *ent = db + data + 24 * idx;
	memset(ent, 0, 24);
	memcpy(ent, name, strlen(name) < 8 ? strlen(name) : 8);
	memcpy(ent + 16, &addr, 4);
	uint32_t auxoff = 0x40 * idx, w5 = (2 * n) | (auxoff << 16);
	memcpy(ent + 20, &w5, 4);
	memcpy(db + data + 0x400 + auxoff, vl, 2 * n);
}

static void reset(void)
{
	nsent = 0;
	votes_str[0] = 0;
	have_votes = 1;
	memset(tcs_addr, 0, sizeof(tcs_addr));
	memset(tcs_data, 0, sizeof(tcs_data));
	tbu = 0x80000000u;
	tbu_follow = 1;
	unlink(state_path);
	unsetenv("VENUSCTL_RAILS");
	unsetenv("VENUSCTL_RAILS_UNKNOWN");
	unsetenv("VENUSCTL_TBUVOTE");
}

static int count_for(const char *res) { int c = 0; for (int i = 0; i < nsent; i++) c += !strcmp(sent[i].res, res); return c; }
static uint32_t last_for(const char *res) { uint32_t v = 0; for (int i = 0; i < nsent; i++) if (!strcmp(sent[i].res, res)) v = sent[i].val; return v; }

int main(void)
{
	snprintf(state_path, sizeof(state_path), "/tmp/rails-test-%d.state", (int)getpid());
	/* lito-like tables: index -> corner */
	static const uint16_t cx[] = { 0, 16, 48, 64, 128, 192, 256, 320, 384, 416 };
	static const uint16_t mx[] = { 0, 16, 48, 64, 128, 192, 256, 384, 416 };
	db_add(0, "cx.lvl", 0x30010, cx, 10);
	db_add(1, "mx.lvl", 0x30020, mx, 9);
	int pass = 0;

	/* helpers */
	assert(rails_hlvl_for(cx, 10, 256) == 6);
	assert(rails_hlvl_for(cx, 10, 200) == 6);
	assert(rails_hlvl_for(cx, 10, 9999) == 9);
	assert(rails_hlvl_for(cx, 0, 256) == -1);
	uint32_t v;
	assert(rails_vote_lookup("30010=3 30020=5 30010=7", 0x30010, &v) && v == 7);
	assert(!rails_vote_lookup("30010=3", 0x30020, &v));
	assert(!rails_vote_lookup("", 0x30020, &v));
	assert(rails_target_corner(NULL) == 0 && rails_target_corner("0") == 0 && rails_target_corner("1") == 256);
	assert(rails_target_corner("384") == 384 && rails_target_corner("-3") == 0);
	pass++;

	/* 1. off by default: nothing voted, nothing written */
	reset();
	assert(rails_raise() == 0 && tbu_vote_on() == 0 && nsent == 0 && access(state_path, F_OK) != 0);
	assert(rails_restore() == 0 && tbu_vote_off() == 0 && nsent == 0);
	pass++;

	/* 2. known lower votes (xnu 1131): raise to NOM, restore to exactly what was there, mx before cx, cx back first */
	reset();
	setenv("VENUSCTL_RAILS", "1", 1);
	strcpy(votes_str, "30010=3 30020=4");
	assert(rails_raise() == 0);
	assert(nsent == 2 && !strcmp(sent[0].res, "mx.lvl") && sent[0].val == 6 && !strcmp(sent[1].res, "cx.lvl") &&
	       sent[1].val == 6);
	assert(rails_restore() == 0);
	assert(nsent == 4 && !strcmp(sent[2].res, "cx.lvl") && sent[2].val == 3 && !strcmp(sent[3].res, "mx.lvl") &&
	       sent[3].val == 4);
	assert(access(state_path, F_OK) != 0);
	pass++;

	/* 3. already higher (wland's cx at the top): no cx vote, nothing to undo for cx */
	reset();
	setenv("VENUSCTL_RAILS", "1", 1);
	strcpy(votes_str, "30010=9 30020=2");
	assert(rails_raise() == 0 && nsent == 1 && !strcmp(sent[0].res, "mx.lvl"));
	assert(rails_restore() == 0 && nsent == 2 && last_for("mx.lvl") == 2 && count_for("cx.lvl") == 0);
	pass++;

	/* 4. unknown before (no xnu 1131, empty TCS): the top level, left in place at power off */
	reset();
	setenv("VENUSCTL_RAILS", "1", 1);
	have_votes = 0;
	assert(rails_raise() == 0 && last_for("mx.lvl") == 8 && last_for("cx.lvl") == 9);
	int before = nsent;
	assert(rails_restore() == 0 && nsent == before);
	pass++;

	/* 5. unknown + VENUSCTL_RAILS_UNKNOWN=1: the target, still never lowered afterwards */
	reset();
	setenv("VENUSCTL_RAILS", "1", 1);
	setenv("VENUSCTL_RAILS_UNKNOWN", "1", 1);
	have_votes = 0;
	assert(rails_raise() == 0 && last_for("cx.lvl") == 6);
	before = nsent;
	assert(rails_restore() == 0 && nsent == before);
	pass++;

	/* 6. TCS evidence (ABL left cx=4 in an active TCS, mx=0 in wake): cx goes back to 4, mx (0 before) is left */
	reset();
	setenv("VENUSCTL_RAILS", "1", 1);
	have_votes = 0;
	tcs_addr[1][2] = 0x30010, tcs_data[1][2] = 4;
	tcs_addr[6][0] = 0x30020, tcs_data[6][0] = 0;
	assert(rails_raise() == 0 && last_for("cx.lvl") == 6 && last_for("mx.lvl") == 6);
	before = nsent;
	assert(rails_restore() == 0 && nsent == before + 1 && last_for("cx.lvl") == 4);
	pass++;

	/* 7. someone voted after us: left alone */
	reset();
	setenv("VENUSCTL_RAILS", "1", 1);
	strcpy(votes_str, "30010=3 30020=4");
	assert(rails_raise() == 0);
	note(0x30010, 9);	/* wland boots the modem meanwhile */
	before = nsent;
	assert(rails_restore() == 0 && nsent == before + 1 && last_for("mx.lvl") == 4 && last_for("cx.lvl") == 6);
	pass++;

	/* 8. state from another boot is ignored */
	reset();
	FILE *f = fopen(state_path, "w");
	fprintf(f, "boot BOOT-OLD\nrail cx.lvl 0x30010 prev 0x3 voted 0x6 known 1\ntbu 0x1\n");
	fclose(f);
	assert(rails_restore() == 0 && tbu_vote_off() == 0 && nsent == 0 && tbu == 0x80000000u);
	pass++;

	/* 9. TBU: collapsed before -> cleared, PWR_ON waited, set again at power off; already on -> untouched */
	reset();
	setenv("VENUSCTL_TBUVOTE", "1", 1);
	tbu = 0x00000001u;
	assert(tbu_vote_on() == 0 && tbu == 0x80000000u);
	assert(tbu_vote_off() == 0 && (tbu & 1));
	reset();
	setenv("VENUSCTL_TBUVOTE", "1", 1);
	assert(tbu_vote_on() == 0 && tbu == 0x80000000u && access(state_path, F_OK) != 0);
	assert(tbu_vote_off() == 0 && tbu == 0x80000000u);
	/* no PWR_ON -> failure, still undone at power off */
	reset();
	setenv("VENUSCTL_TBUVOTE", "1", 1);
	tbu = 1, tbu_follow = 0;
	assert(tbu_vote_on() == 1);
	assert(tbu_vote_off() == 0 && (tbu & 1));
	pass++;

	/* 10. second power on does not re-record (would lose the original prev) */
	reset();
	setenv("VENUSCTL_RAILS", "1", 1);
	strcpy(votes_str, "30010=3 30020=4");
	assert(rails_raise() == 0 && rails_raise() == 0 && nsent == 2);
	assert(rails_restore() == 0 && last_for("cx.lvl") == 3 && last_for("mx.lvl") == 4);
	pass++;

	/* 11. read-only commands send nothing */
	reset();
	strcpy(votes_str, "30010=3");
	char line[256];
	assert(rails_report() == 0);
	rails_line(line, sizeof(line));
	printf("line: %s\n", line);
	assert(strstr(line, "up 123.4 s") && strstr(line, "cx.lvl 3/64") && strstr(line, "mx.lvl ?") && strstr(line, "tbu on"));
	assert(nsent == 0 && tbu == 0x80000000u);
	pass++;

	unlink(state_path);
	printf("rails_test: %d groups passed\n", pass);
	return 0;
}
