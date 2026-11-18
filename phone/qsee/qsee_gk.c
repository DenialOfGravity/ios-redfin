/*
 * qsee_gk.c (ws-fpcd-a): qsee_lsnr.inc (qseeprobe ws-qsee-b e1bf971: listeners, keymaster64, gatekeeper) built as a
 * library part, plus the FPC hw-auth chain and the identify auth commands. API: qsee_gk.h.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../video/pmem.h"
#include "qsee_wl.h"
#include "qsee_lib.h"
#include "qsee_gk.h"

#define QSEE_LSNR_LIB 1
#include "qsee_lsnr.inc"

static int lib_log_open(void)
{
	if (!lsnr_log) {
		mkdir(TZ_DIR, 0700);
		mkdir(TZ_DIR "/log", 0700);
		lsnr_log = fopen(TZ_DIR "/log/lsnr.log", "a");
	}
	return 0;
}

int qsee_gk_verify(uint32_t km_appid, uint64_t challenge, const char *pw, uint8_t hat[QSEE_HAT_LEN], uint32_t *hat_len)
{
	lib_log_open();
	gk_writes_allowed = 1;	/* the kernel relays them only at kern.sweet_rpmb=2 (set by the caller) */
	int r = gk_verify_real(km_appid, challenge, pw, hat, hat_len);
	gk_writes_allowed = 0;
	return r;
}

int qsee_fpc_authorize_enrol(uint32_t fpc_appid, qsee_hat_source_fn src, void *ctx, int32_t *ta_status)
{
	uint32_t km;
	int rc = 1;
	uint8_t rsp[0x3c0] = { 0 }, d[0x400] = { 0 }, hat[QSEE_HAT_LEN] = { 0 }, req[0x40] = { 0 };
	*ta_status = 0;
	lib_log_open();
	write_attempts = 0;
	if (check_appid(fpc_appid) || km_appid(&km))
		return 1;
	rpmb_info_load();
	if (lsnr_all_up()) {
		lsnr_all_down();
		return 1;
	}
	rc = 2;
	put_u32(req, 0x205), put_u32(req + 4, 2);
	int r = km_send(km, req, 0x40, 0x1000, 0x40, 0x3c0, rsp, sizeof(rsp), 0);
	int32_t kst = (int32_t)g32(rsp);
	uint32_t koff = g32(rsp + 4), klen = g32(rsp + 8);
	lg("hw-auth: KM GET_AUTH_TOKEN_KEY rc %d status %d key blob length %u (not shown)", r, kst, klen);
	if (r || kst || !klen || klen > sizeof(d) - 0x10 || koff + klen > sizeof(rsp)) {
		*ta_status = r ? -1 : kst;
		goto out;
	}
	rc = 3;
	put_u32(d, 3), put_u32(d + 4, 5), put_u32(d + 0xc, klen);
	memcpy(d + 0x10, rsp + koff, klen);
	memset(rsp, 0, sizeof(rsp));
	r = fpc_send64(fpc_appid, d, 0x10 + klen, "fpc init_hw_auth (3,5)");
	*ta_status = (int32_t)g32(d + 8);
	lg("hw-auth: init_hw_auth (3,5): send rc %d TA status %d", r, *ta_status);
	memset(d, 0, sizeof(d));
	if (r || *ta_status)
		goto out;
	rc = 4;
	put_u32(d, 3), put_u32(d + 4, 2);
	r = fpc_send64(fpc_appid, d, FPC_SENSOR_LEN, "fpc get_enrol_challenge (3,2)");
	uint64_t ch = (uint64_t)g32(d + 0x10) | (uint64_t)g32(d + 0x14) << 32;
	*ta_status = (int32_t)g32(d + 8);
	lg("hw-auth: get_enrol_challenge (3,2): send rc %d TA status %d challenge %#llx", r, *ta_status,
	   (unsigned long long)ch);
	if (r || *ta_status || !ch)
		goto out;
	rc = 5;
	if (!src || src(ctx, km, ch, hat)) {
		lg("hw-auth: the credential source gave no HAT for challenge %#llx", (unsigned long long)ch);
		goto out;
	}
	rc = 6;
	memset(d, 0, sizeof(d));
	put_u32(d, 3), put_u32(d + 4, 3), put_u32(d + 0xc, QSEE_HAT_LEN);
	memcpy(d + 0x10, hat, QSEE_HAT_LEN);
	r = fpc_send64(fpc_appid, d, 0x10 + QSEE_HAT_LEN, "fpc authorize_enrol (3,3)");
	*ta_status = (int32_t)g32(d + 8);
	lg("hw-auth: authorize_enrol (3,3) with the HAT: send rc %d TA status %d", r, *ta_status);
	if (!r && !*ta_status)
		rc = 0;
out:
	memset(hat, 0, sizeof(hat)), memset(d, 0, sizeof(d)), memset(rsp, 0, sizeof(rsp));
	gk_writes_allowed = 0;
	lsnr_all_down();
	if (write_attempts) {
		lg("hw-auth: %u write attempt(s) refused: HALTED", write_attempts);
		rc = 7;
	}
	lg("hw-auth: %s (step %d)", rc ? "FAILED" : "authorised", rc);
	return rc;
}

int qsee_fpc_set_auth_challenge(uint32_t fpc_appid, uint64_t challenge, int32_t *ta_status)
{
	uint8_t d[FPC_SENSOR_LEN] = { 0 };
	put_u32(d, 3), put_u32(d + 4, 1), put_u64(d + 0x10, challenge);	/* fpc_tee_set_auth_challenge @0xf774 */
	int r = fpc_send64(fpc_appid, d, FPC_SENSOR_LEN, "fpc set_auth_challenge (3,1)");
	*ta_status = (int32_t)g32(d + 8);
	return r;
}

int qsee_fpc_get_auth_result(uint32_t fpc_appid, uint8_t hat[QSEE_HAT_LEN], int32_t *ta_status)
{
	uint8_t d[0x10 + QSEE_HAT_LEN] = { 0 };
	put_u32(d, 3), put_u32(d + 4, 4), put_u32(d + 0xc, QSEE_HAT_LEN);	/* fpc_tee_get_auth_result @0xf8e0 */
	int r = fpc_send64(fpc_appid, d, sizeof(d), "fpc get_auth_result (3,4)");
	*ta_status = (int32_t)g32(d + 8);
	if (!r && !*ta_status)
		memcpy(hat, d + 0x10, QSEE_HAT_LEN);
	memset(d, 0, sizeof(d));
	return r;
}

void qsee_hat_parse(const uint8_t t[QSEE_HAT_LEN], struct qsee_hat_info *i)
{
	memset(i, 0, sizeof(*i));
	i->version = t[0];
	for (int k = 8; k >= 1; k--)
		i->challenge = i->challenge << 8 | t[k];
	for (int k = 16; k >= 9; k--)
		i->user_id = i->user_id << 8 | t[k];
	for (int k = 24; k >= 17; k--)
		i->authenticator_id = i->authenticator_id << 8 | t[k];
	i->authenticator_type = (uint32_t)t[25] << 24 | t[26] << 16 | t[27] << 8 | t[28];
	for (int k = 29; k <= 36; k++)
		i->timestamp = i->timestamp << 8 | t[k];
}

/* ---- ws-fpcd-b: in-memory template db directory + FPC db_command ---- */
void qsee_vfs_init(struct qsee_vfs *v, const char *prefix, uint32_t max_size)
{
	memset(v, 0, sizeof(*v));
	snprintf(v->prefix, sizeof(v->prefix), "%s", prefix);
	v->max_size = max_size;
}

void qsee_vfs_clear(struct qsee_vfs *v)
{
	for (int i = 0; i < QSEE_VFS_FILES; i++) {
		struct qsee_vfile *f = &v->f[i];
		if (f->data)
			memset(f->data, 0, f->cap), free(f->data);
		memset(f, 0, sizeof(*f));
	}
}

struct qsee_vfile *qsee_vfs_get(struct qsee_vfs *v, const char *name)
{
	for (int i = 0; i < QSEE_VFS_FILES; i++)
		if (v->f[i].name[0] && !strcmp(v->f[i].name, name))
			return &v->f[i];
	return NULL;
}

int qsee_vfs_put(struct qsee_vfs *v, const char *name, const uint8_t *d, uint32_t len)
{
	if (!name[0] || strlen(name) >= QSEE_VFS_NAME || strchr(name, '/') || len > v->max_size)
		return -1;
	struct qsee_vfile *f = qsee_vfs_get(v, name);
	for (int i = 0; !f && i < QSEE_VFS_FILES; i++)
		if (!v->f[i].name[0])
			f = &v->f[i];
	if (!f)
		return -1;
	uint32_t cap = len ? len : 1;
	uint8_t *nd = calloc(1, cap);
	if (!nd)
		return -1;
	if (len)
		memcpy(nd, d, len);
	if (f->data)
		memset(f->data, 0, f->cap), free(f->data);
	snprintf(f->name, sizeof(f->name), "%s", name);
	f->data = nd, f->cap = cap, f->len = len;
	return 0;
}

int qsee_vfs_rename(struct qsee_vfs *v, const char *from, const char *to)
{
	struct qsee_vfile *a = qsee_vfs_get(v, from), *b = qsee_vfs_get(v, to);
	if (!a || strlen(to) >= QSEE_VFS_NAME || strchr(to, '/'))
		return -1;
	if (b && b != a) {
		if (b->data)
			memset(b->data, 0, b->cap), free(b->data);
		memset(b, 0, sizeof(*b));
	}
	snprintf(a->name, sizeof(a->name), "%s", to);
	return 0;
}

/* fpc_send64 hook: serve listener requests (file system from vfs_active) until TZ gives a final result */
static int db_lsnr_serve(uint64_t x[8], uint32_t appid, const char *what)
{
	for (int round = 0; round < 4000; round++) {
		if ((int64_t)x[0])
			return 0;
		uint32_t result = (uint32_t)x[1], type = (uint32_t)x[2], data = (uint32_t)x[3];
		if (result == QSEOS_RESULT_INCOMPLETE) {
			serve_one(x, data);
			continue;
		}
		if (result == QSEOS_RESULT_BLOCKED) {
			lg("%s: BLOCKED_ON_LISTENER %#x session %#x: continue-blocked", what, data, type);
			uint64_t y[8] = { QSEE_SMC_CONT_BLOCKED_SI, QSEE_ARGS_CONT_BLOCKED, type };
			if (qsmc(y)) {
				uint64_t w[8] = { QSEE_SMC_CONT_BLOCKED, QSEE_ARGS_CONT_BLOCKED, appid };
				qsmc(w);
				memcpy(x, w, 4 * sizeof(uint64_t));
			} else {
				memcpy(x, y, 4 * sizeof(uint64_t));
			}
			continue;
		}
		return 0;
	}
	lg("%s: listener loop gave up after 4000 rounds", what);
	return 1;
}

int qsee_fpc_db_cmd(uint32_t fpc_appid, uint32_t id, const char *path, struct qsee_vfs *v, int32_t *ta_status)
{
	uint8_t d[0x200] = { 0 };
	size_t pl = strlen(path) + 1;
	*ta_status = 0;
	lib_log_open();
	if ((id != QSEE_FPC_DB_LOAD && id != QSEE_FPC_DB_STORE) || pl > sizeof(d) - 0x10 || check_appid(fpc_appid))
		return 1;
	write_attempts = 0;
	rpmb_info_load();
	if (lsnr_all_up()) {
		lsnr_all_down();
		return 1;
	}
	vfs_active = v;
	memset(vfd, 0, sizeof(vfd));
	v->opens = v->reads = v->writes = v->unlinks = v->refused = 0;
	v->bytes_read = v->bytes_written = 0;
	put_u32(d, 2), put_u32(d + 4, id), put_u32(d + 0xc, (uint32_t)pl);
	memcpy(d + 0x10, path, pl);
	fpc_lsnr_serve = db_lsnr_serve;
	int r = fpc_send64(fpc_appid, d, 0x10 + pl, id == QSEE_FPC_DB_STORE ? "fpc store_template_db (2,12)" :
			   "fpc load_template_db (2,11)");
	fpc_lsnr_serve = NULL;
	*ta_status = (int32_t)g32(d + 8);
	vfs_active = NULL;
	memset(vfd, 0, sizeof(vfd));
	lsnr_all_down();
	lg("db %s \"%s\": send rc %d TA status %d | vfs opens %u reads %u (%u B) writes %u (%u B) unlinks %u",
	   id == QSEE_FPC_DB_STORE ? "store" : "load", path, r, *ta_status, v->opens, v->reads, v->bytes_read, v->writes,
	   v->bytes_written, v->unlinks);
	if (write_attempts) {
		lg("db: %u write attempt(s) outside the template db directory refused", write_attempts);
		return 7;
	}
	return r;
}
