/* Host test for sweetlocd's engine lock handling (loc.c unlock=mi / unlock=none, restore, crash repair; englock.c), with a
 * mock modem behind the stubbed QRTR link (test/run.sh builds and runs it). The mock answers GET_ENGINE_LOCK like redfin
 * (LOC rev 0x83: only TLV 0x12 lockClient), applies SET_ENGINE_LOCK (lockType, or lockClient when given), refuses START
 * with QMI error 0x2e + POSITION_REPORT status 7 (ENGINE_LOCKED) while the MI bit is set. Each scenario runs in its own
 * child process (fresh loc.c state); the modem's lock value is carried from a "crashed" child to the next one. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include "../link.h"
#include "qmi.h"
#include "../englock.h"
#include "../pdsemu.h"
#include "../loc.h"

static uint64_t t;
uint64_t link_ms(void) { return t; }
static uint64_t modem = 3;
static int set_fail, type_ignored, drop_set, nset, nget, nstart, start_ok, nlog, nfail;
static uint16_t log_id[1024];
static uint32_t last_set_type;
static int last_set_client;
static uint64_t last_set_client_v;
/* ws-gnss-f: real_client = refuse START by the client type REG_EVENTS declared (TLV 0x11; absent = NFW), as the modem does */
static int real_client, nopmode, opmode_before_start, start_has12, start_has11, start_has15, start_has18, first_set_client = -1;
static uint32_t reg_ctype, reg_has_strid, opmode_v;
static uint64_t reg_mask;
static uint8_t outq[32][128];
static uint32_t outn[32];
static int nout;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); nfail++; } } while (0)

static uint32_t tlv(uint8_t *o, uint8_t ty, const void *v, uint16_t l)
{
	o[0] = ty; o[1] = (uint8_t)l; o[2] = (uint8_t)(l >> 8);
	memcpy(o + 3, v, l);
	return 3u + l;
}

static void emit(uint8_t type, uint16_t txn, uint16_t id, const uint8_t *tl, uint32_t tn)
{
	uint8_t *b = outq[nout];
	b[0] = type; b[1] = (uint8_t)txn; b[2] = (uint8_t)(txn >> 8); b[3] = (uint8_t)id; b[4] = (uint8_t)(id >> 8);
	b[5] = (uint8_t)tn; b[6] = (uint8_t)(tn >> 8);
	memcpy(b + 7, tl, tn);
	outn[nout++] = 7 + tn;
}

static void resp(uint16_t txn, uint16_t id, int ok)
{
	uint8_t r[8], v[4] = { ok ? 0 : 1, 0, ok ? 0 : 0x2e, 0 };
	emit(QMI_RESPONSE, txn, id, r, tlv(r, 0x02, v, 4));
}

int qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *p, uint32_t len)
{
	const uint8_t *b = p;
	struct qmi_msg m;
	uint8_t r[64];
	uint32_t n = 0, st = 0;
	uint16_t l;
	const uint8_t *v;
	if (dst_port == PDSR_KPORT)
		return 0;
	CHECK(src_port == LOC_PORT && dst_node == 0 && dst_port == 0x57 && type == QRTR_TYPE_DATA);
	if (qmi_parse(b, len, &m))
		return 0;
	log_id[nlog++ % 1024] = m.id;
	switch (m.id) {
	case LOC_GET_ENGINE_LOCK:
		nget++;
		resp(m.txn, m.id, 1);
		n = tlv(r, 0x01, &st, 4);
		n += tlv(r + n, 0x12, &modem, 8);
		emit(QMI_INDICATION, 1, m.id, r, n);
		break;
	case LOC_SET_ENGINE_LOCK: {
		nset++;
		uint32_t ty = 0;
		if ((v = qmi_get(&m, 0x01, &l)) && l == 4)
			memcpy(&ty, v, 4);
		CHECK((v = qmi_get(&m, 0x10, &l)) && l == 4 && v[0] == LK_SUB_ALL);
		last_set_type = ty;
		last_set_client = (v = qmi_get(&m, 0x11, &l)) != NULL && l == 8;
		if (last_set_client)
			memcpy(&last_set_client_v, v, 8);
		if (first_set_client < 0)
			first_set_client = last_set_client;
		if (set_fail) {
			resp(m.txn, m.id, 0);
			break;
		}
		if (last_set_client)
			modem = last_set_client_v;
		else if (!type_ignored)
			modem = lk_mask_of_type(ty);
		if (drop_set)
			break;		/* applied, but the answer is lost */
		resp(m.txn, m.id, 1);
		n = tlv(r, 0x01, &st, 4);
		emit(QMI_INDICATION, 1, m.id, r, n);
		break;
	}
	case LOC_REG_EVENTS:
		reg_ctype = 0;
		reg_has_strid = 0;
		if ((v = qmi_get(&m, 0x01, &l)) && l == 8)
			memcpy(&reg_mask, v, 8);
		if ((v = qmi_get(&m, 0x11, &l)) && l == 4)
			memcpy(&reg_ctype, v, 4);
		if ((v = qmi_get(&m, 0x10, &l)) && l == 3 && !memcmp(v, "HAL", 3))
			reg_has_strid = 1;
		resp(m.txn, m.id, 1);
		break;
	case LOC_SET_OPERATION_MODE:
		nopmode++;
		if ((v = qmi_get(&m, 0x01, &l)) && l == 4)
			memcpy(&opmode_v, v, 4);
		resp(m.txn, m.id, 1);
		n = tlv(r, 0x01, &st, 4);
		emit(QMI_INDICATION, 1, m.id, r, n);
		break;
	case LOC_START:
		nstart++;
		opmode_before_start = nopmode > 0;
		start_has11 = qmi_get(&m, 0x11, &l) != NULL;
		start_has12 = qmi_get(&m, 0x12, &l) != NULL;
		start_has15 = qmi_get(&m, 0x15, &l) != NULL;
		start_has18 = qmi_get(&m, 0x18, &l) != NULL;
		if (real_client ? (modem & (reg_ctype == 1 ? LK_AFW : reg_ctype == 3 ? LK_PRIV : LK_NFW)) != 0 : (modem & LK_AFW) != 0) {
			resp(m.txn, m.id, 0);
			st = 7;
			n = tlv(r, 0x01, &st, 4);
			emit(QMI_INDICATION, 1, LOC_IND_POSITION, r, n);
		} else {
			start_ok++;
			resp(m.txn, m.id, 1);
		}
		break;
	default:
		resp(m.txn, m.id, 1);
		break;
	}
	return 0;
}

static void pump(int ms)
{
	for (int i = 0; i < ms / 10; i++) {
		t += 10;
		loc_poll();
		int k = nout;
		nout = 0;
		for (int j = 0; j < k; j++) {
			struct qrtr_hdr h = { 0 };
			loc_handle(&h, outq[j], outn[j]);
		}
	}
}

static void wordfile(const char *s)
{
	if (s) { FILE *f = fopen(LOC_ON, "w"); fputs(s, f); fclose(f); } else unlink(LOC_ON);
	t += 10001;
	pump(10);
}

static int file_mask(uint64_t *m) { return lk_load(LOCK_ORIG, m, NULL); }
static int idx_last(uint16_t id) { for (int i = nlog - 1; i >= 0; i--) if (log_id[i] == id) return i; return -1; }
static int idx_first(uint16_t id) { for (int i = 0; i < nlog; i++) if (log_id[i] == id) return i; return -1; }
static void up(void) { loc_server(0x10, 2, 0, 0x57); }

/* unlock=mi, session runs, then SIGTERM restores */
static void s_ok(void)
{
	uint64_t fm = 0;
	up();
	wordfile("session=30 unlock=mi\n");
	pump(300);
	CHECK(nlog >= 4 && log_id[0] == LOC_GET_ENGINE_LOCK && log_id[1] == LOC_GET_ENGINE_LOCK && log_id[2] == LOC_SET_ENGINE_LOCK &&
	      log_id[3] == LOC_GET_ENGINE_LOCK);
	CHECK(last_set_type == LK_TYPE_MT && !last_set_client);	/* HAL-identical: lockType MT + subType ALL_SUB */
	CHECK(modem == LK_NFW);					/* NI stays locked */
	CHECK(file_mask(&fm) == 0 && fm == 3);
	CHECK(start_ok == 1 && idx_first(LOC_START) > idx_first(LOC_SET_ENGINE_LOCK));
	loc_shutdown("SIGTERM");
	CHECK(!loc_quiet());
	pump(300);
	/* ws-gnss-f: the restore uses the original's TLV kind: the mock's GET reports lockClient, so lockType ALL + lockClient 3 */
	CHECK(modem == 3 && nset == 2 && last_set_type == LK_TYPE_ALL && last_set_client && last_set_client_v == 3);
	CHECK(idx_last(LOC_SET_ENGINE_LOCK) > idx_last(LOC_STOP));	/* restore after the session's STOP */
	CHECK(access(LOCK_ORIG, F_OK) != 0 && loc_quiet());
}

static void s_word_gone(void)
{
	up();
	wordfile("session=30 unlock=mi\n");
	pump(300);
	CHECK(modem == 2 && start_ok == 1);
	wordfile("session=30\n");
	pump(300);
	CHECK(modem == 3 && nset == 2 && access(LOCK_ORIG, F_OK) != 0);
	CHECK(nstart == 2 && start_ok == 1);	/* the new session (no word) is refused as before */
	CHECK(idx_last(LOC_START) > idx_last(LOC_SET_ENGINE_LOCK));
}

static void s_locon_gone(void)
{
	up();
	wordfile("pds=0 session=30 unlock=mi\n");
	pump(300);
	CHECK(modem == 2);
	wordfile(NULL);
	pump(300);
	CHECK(modem == 3 && nset == 2 && access(LOCK_ORIG, F_OK) != 0 && loc_quiet());
}

static void s_set_fail(void)
{
	set_fail = 1;
	up();
	wordfile("session=30 unlock=mi\n");
	pump(500);
	CHECK(modem == 3 && access(LOCK_ORIG, F_OK) != 0);
	CHECK(nset == 2);			/* try 1 + try 2; modem unchanged, so no restore SET */
	CHECK(nstart == 1 && start_ok == 0);	/* sessions go on as before, refused ENGINE_LOCKED */
	int k = nset;
	pump(30000);
	CHECK(nset == k);			/* no retry loop within the same loc.on */
}

static void s_type_ignored(void)
{
	type_ignored = 1;
	up();
	wordfile("session=30 unlock=mi\n");
	pump(300);
	CHECK(modem == 2 && nset == 2 && last_set_client && last_set_client_v == 2 && start_ok == 1);
	loc_shutdown("SIGTERM");
	pump(300);
	CHECK(modem == 3 && nset == 3 && last_set_client && last_set_client_v == 3 && access(LOCK_ORIG, F_OK) != 0);
}

static void s_none(void)
{
	up();
	wordfile("session=30 unlock=none\n");
	pump(300);
	CHECK(modem == 0 && last_set_type == LK_TYPE_NONE && start_ok == 1);
	loc_shutdown("SIGTERM");
	pump(300);
	CHECK(modem == 3 && access(LOCK_ORIG, F_OK) != 0);
}

static void s_already(void)
{
	modem = 2;
	up();
	wordfile("session=30 unlock=mi\n");
	pump(300);
	CHECK(nset == 0 && access(LOCK_ORIG, F_OK) != 0 && start_ok == 1);
	loc_shutdown("SIGTERM");
	pump(300);
	CHECK(nset == 0 && modem == 2);
}

static void s_unknown(void)
{
	up();
	wordfile("session=30 unlock=all\n");
	pump(300);
	wordfile("probe=1 pds=1 session=5 unlock=\n");
	pump(10000);
	loc_shutdown("SIGTERM");
	pump(300);
	CHECK(nset == 0 && modem == 3 && access(LOCK_ORIG, F_OK) != 0);
}

/* unlocked, then killed: no restore. The modem value goes to the next scenario. */
static void s_crash(void)
{
	up();
	wordfile("session=30 unlock=mi\n");
	pump(300);
	CHECK(modem == 2 && access(LOCK_ORIG, F_OK) == 0);
}

static void s_repair(void)
{
	up();
	wordfile("session=30\n");
	pump(300);
	CHECK(modem == 3 && nset == 1 && last_set_type == LK_TYPE_ALL && access(LOCK_ORIG, F_OK) != 0);
	CHECK(nstart == 1 && idx_first(LOC_START) > idx_last(LOC_SET_ENGINE_LOCK));	/* session only after the repair */
}

static void s_repair_offline(void)
{
	wordfile(NULL);
	up();
	pump(300);
	CHECK(modem == 3 && nset == 1 && access(LOCK_ORIG, F_OK) != 0 && nstart == 0 && loc_quiet());
}

static void s_crash_rewant(void)
{
	uint64_t fm = 0;
	up();
	wordfile("session=30 unlock=mi\n");
	pump(300);
	CHECK(nset == 0 && modem == 2 && start_ok == 1 && file_mask(&fm) == 0 && fm == 3);
	loc_shutdown("SIGTERM");
	pump(300);
	CHECK(modem == 3 && nset == 1 && access(LOCK_ORIG, F_OK) != 0);
}

static void s_cap(void)
{
	lk_save(LOCK_ORIG, 3, 5);
	modem = 2;
	up();
	wordfile("session=30\n");
	pump(300);
	CHECK(nset == 0 && access(LOCK_ORIG, F_OK) == 0);
	unlink(LOCK_ORIG);
}

static void s_lost_answers(void)
{
	drop_set = 1;
	up();
	wordfile("session=30 unlock=mi\n");
	pump(7000);
	CHECK(modem == 2 && nset == 1 && start_ok == 1);
	loc_shutdown("SIGTERM");
	pump(7000);
	CHECK(modem == 3 && nset == 2 && access(LOCK_ORIG, F_OK) != 0 && loc_quiet());
}

static void s_none_ever(void)	/* no unlock word, no file: SET_ENGINE_LOCK never goes out */
{
	up();
	wordfile("probe=1 session=10\n");
	pump(20000);
	wordfile("pds=1\n");
	pump(1000);
	loc_shutdown("SIGTERM");
	pump(300);
	CHECK(nset == 0 && modem == 3);
}

/* ---- ws-gnss-f ---- */
/* the phone's result: lock 0x2 (AFW open) and still ENGINE_LOCKED, because a client without clientType is NFW */
static void s_nfw_default(void)
{
	real_client = 1;
	up();
	wordfile("session=30 unlock=mi\n");
	pump(300);
	CHECK(modem == 2 && reg_ctype == 0 && nstart == 1 && start_ok == 0);
	loc_shutdown("SIGTERM");
	pump(300);
	CHECK(modem == 3);
}

static void s_afw(void)
{
	real_client = 1;
	up();
	wordfile("session=30 unlock=mi client=afw\n");
	pump(300);
	CHECK(modem == 2 && reg_ctype == 1 && !reg_has_strid && start_ok == 1);
	CHECK(!first_set_client);			/* without lockclient=1 the first SET is lockType only */
	CHECK(!start_has11 && start_has12 && !nopmode);	/* our START, no SET_OPERATION_MODE */
	loc_shutdown("SIGTERM");
	pump(300);
	CHECK(modem == 3 && reg_ctype == 1 && access(LOCK_ORIG, F_OK) != 0);	/* REG_EVENTS 0 keeps the client type */
}

/* client=afw alone with the lock at 0x3: still refused (AFW locked), nothing written */
static void s_afw_locked(void)
{
	real_client = 1;
	up();
	wordfile("session=30 client=afw clientid=1 lockev=1\n");
	pump(300);
	CHECK(nset == 0 && reg_ctype == 1 && reg_has_strid && (reg_mask & LOC_EVMASK_ENGINE_LOCK_STATE) && start_ok == 0);
}

static void s_android_start(void)
{
	real_client = 1;
	modem = 2;
	up();
	wordfile("session=30 client=afw opmode=standalone start=android\n");
	pump(300);
	CHECK(nset == 0 && nopmode == 1 && opmode_v == LOC_OPMODE_STANDALONE && opmode_before_start && start_ok == 1);
	CHECK(start_has11 && !start_has12 && start_has15 && start_has18);
	loc_shutdown("SIGTERM");
	pump(300);
	CHECK(nset == 0 && modem == 2);
}

static void s_lockclient(void)
{
	up();
	wordfile("session=30 unlock=mi lockclient=1 opmode=msb client=priv\n");	/* unknown values ignored */
	pump(300);
	CHECK(modem == 2 && nset == 1 && first_set_client && last_set_client_v == 2 && nopmode == 0 && reg_ctype == 0);
	loc_shutdown("SIGTERM");
	pump(300);
	CHECK(modem == 3);
}

static void efs_touch(const char *f, int sec)
{
	char p[512];
	snprintf(p, sizeof(p), "%s/%s", EFS_DIR, f);
	FILE *o = fopen(p, "w");
	fputs("x", o);
	fclose(o);
	struct timespec ts[2] = { { 1000000 + sec, 0 }, { 1000000 + sec, 0 } };
	utimensat(AT_FDCWD, p, ts, 0);
}

static void efs_make(void)
{
	mkdir(EFS_DIR, 0755);
	efs_touch("modemst1.img", 1);
	efs_touch("modemst2.img", 2);
}

/* restore confirmed by GET, LOCK_ORIG kept until modemst2.img changes */
static void s_efs(void)
{
	struct lk_file lf;
	efs_make();
	up();
	wordfile("session=30 unlock=mi\n");
	pump(300);
	CHECK(modem == 2);
	wordfile("session=30\n");
	pump(300);
	CHECK(modem == 3 && access(LOCK_ORIG, F_OK) == 0 && lk_load_ex(LOCK_ORIG, &lf) == 0 && lf.efs == 1 &&
	      lf.kind == LK_KIND_CLIENT && lf.mask == 3);
	CHECK(loc_lock_busy());		/* the daemon stays up for it */
	pump(10000);
	CHECK(access(LOCK_ORIG, F_OK) == 0);
	efs_touch("modemst2.img", 50);
	pump(3000);
	CHECK(access(LOCK_ORIG, F_OK) != 0 && !loc_lock_busy());
}

/* restored but SIGTERM before the EFS write: file kept (efs=1); carried to s_efs_repair */
static void s_efs_term(void)
{
	efs_make();
	up();
	wordfile("session=30 unlock=mi\n");
	pump(300);
	loc_shutdown("SIGTERM");
	pump(300);
	CHECK(modem == 3 && access(LOCK_ORIG, F_OK) == 0 && loc_quiet());
}

/* next start, no reboot (modem still 3): no SET, waits for the EFS write, then removes the file */
static void s_efs_repair(void)
{
	up();
	wordfile(NULL);
	pump(3000);
	CHECK(nset == 0 && access(LOCK_ORIG, F_OK) == 0 && loc_lock_busy());
	efs_touch("modemst1.img", 60);
	pump(3000);
	CHECK(nset == 0 && access(LOCK_ORIG, F_OK) != 0);
}

/* next start after a hard reboot that lost the restore (modem back at 2): repair SETs 3 again with lockClient */
static void s_efs_lost(void)
{
	up();
	wordfile(NULL);
	pump(300);
	CHECK(modem == 3 && nset == 1 && last_set_client && last_set_client_v == 3 && access(LOCK_ORIG, F_OK) == 0);
	efs_touch("modemst1.img", 70);
	pump(3000);
	CHECK(access(LOCK_ORIG, F_OK) != 0);
}

static int run(const char *name, void (*f)(void), uint64_t *m)
{
	fflush(stdout);
	pid_t p = fork();
	if (p == 0) {
		modem = *m;
		f();
		printf("lock_test: %-16s %s (modem lock now 0x%llx, %d SET, %d GET)\n", name, nfail ? "FAILED" : "ok",
		       (unsigned long long)modem, nset, nget);
		fflush(stdout);
		_exit((nfail ? 100 : 0) + (int)(modem & 7));
	}
	int st = 0;
	waitpid(p, &st, 0);
	int c = WIFEXITED(st) ? WEXITSTATUS(st) : 200;
	*m = (uint64_t)(c % 100);
	return c >= 100;
}

int main(void)
{
	loc_fake_kernel_router = 1;
	unlink(LOCK_ORIG);
	int bad = 0;
	uint64_t m;
#define RUN(f) do { m = 3; bad += run(#f, f, &m); } while (0)
	RUN(s_ok);
	RUN(s_word_gone);
	RUN(s_locon_gone);
	RUN(s_set_fail);
	RUN(s_type_ignored);
	RUN(s_none);
	RUN(s_already);
	RUN(s_unknown);
	RUN(s_lost_answers);
	RUN(s_none_ever);
	m = 3; bad += run("s_crash", s_crash, &m);
	bad += run("s_repair", s_repair, &m);		/* m = 2 from the crash */
	CHECK(m == 3);
	m = 3; bad += run("s_crash", s_crash, &m);
	bad += run("s_repair_offline", s_repair_offline, &m);
	CHECK(m == 3);
	m = 3; bad += run("s_crash", s_crash, &m);
	bad += run("s_crash_rewant", s_crash_rewant, &m);
	CHECK(m == 3);
	RUN(s_cap);
	RUN(s_nfw_default);
	RUN(s_afw);
	RUN(s_afw_locked);
	RUN(s_android_start);
	RUN(s_lockclient);
	RUN(s_efs);
	m = 3; bad += run("s_efs_term", s_efs_term, &m);
	bad += run("s_efs_repair", s_efs_repair, &m);
	CHECK(m == 3);
	m = 3; bad += run("s_efs_term", s_efs_term, &m);
	m = 2;		/* the hard reboot lost the restore */
	bad += run("s_efs_lost", s_efs_lost, &m);
	CHECK(m == 3);
	unlink(LOC_ON); unlink(LOC_STATUS); unlink(LOCK_ORIG);
	printf("lock_test: %s (%d scenarios failed, %d checks failed)\n", bad || nfail ? "FAILED" : "all ok", bad, nfail);
	return bad || nfail;
}
