/* englock: QMI LOC engine lock encoding (englock.h). Claude 2026-11-16, ws-gnss-e. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "englock.h"

static uint32_t put(uint8_t *o, uint8_t t, const void *v, uint16_t l)
{
	o[0] = t;
	o[1] = (uint8_t)l;
	o[2] = (uint8_t)(l >> 8);
	memcpy(o + 3, v, l);
	return 3u + l;
}

static uint32_t le32(uint32_t v, uint8_t *b)
{
	for (int i = 0; i < 4; i++)
		b[i] = (uint8_t)(v >> (8 * i));
	return 4;
}

uint32_t lk_tlv_get(uint8_t *out, unsigned subtype)
{
	uint8_t b[4];
	if (!subtype)
		return 0;
	le32(subtype, b);
	return put(out, 0x10, b, 4);
}

uint32_t lk_tlv_set(uint8_t *out, uint32_t type, int with_client, uint64_t client)
{
	uint8_t b[8];
	uint32_t n = 0;
	le32(type, b);
	n += put(out + n, 0x01, b, 4);
	le32(LK_SUB_ALL, b);
	n += put(out + n, 0x10, b, 4);
	if (with_client) {
		for (int i = 0; i < 8; i++)
			b[i] = (uint8_t)(client >> (8 * i));
		n += put(out + n, 0x11, b, 8);
	}
	return n;
}

static uint64_t rd(const uint8_t *p, uint16_t l)
{
	uint64_t v = 0;
	for (int i = l - 1; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

void lk_parse(const struct qmi_msg *m, struct lk_val *v)
{
	const uint8_t *p;
	uint16_t l = 0;
	memset(v, 0, sizeof(*v));
	if ((p = qmi_get(m, 0x01, &l)) && l == 4) {
		v->has_status = 1;
		v->status = (uint32_t)rd(p, 4);
	}
	if ((p = qmi_get(m, 0x10, &l)) && l == 4) {
		v->has_type = 1;
		v->type = (uint32_t)rd(p, 4);
	}
	if ((p = qmi_get(m, 0x11, &l)) && l == 4) {
		v->has_sub = 1;
		v->sub = (uint32_t)rd(p, 4);
	}
	if ((p = qmi_get(m, 0x12, &l)) && l == 8) {
		v->has_client = 1;
		v->client = rd(p, 8);
	}
}

uint64_t lk_mask_of_type(uint32_t type)
{
	switch (type) {
	case LK_TYPE_NONE: return 0;
	case LK_TYPE_MI: return LK_AFW;
	case LK_TYPE_MT: return LK_NFW;
	case LK_TYPE_ALL: return LK_AFW | LK_NFW;
	default: return ~0ull;
	}
}

uint32_t lk_type_of_mask(uint64_t mask)
{
	switch (mask & 3) {
	case 0: return LK_TYPE_NONE;
	case 1: return LK_TYPE_MI;
	case 2: return LK_TYPE_MT;
	default: return LK_TYPE_ALL;
	}
}

int64_t lk_mask(const struct lk_val *v)
{
	if (v->has_client)
		return (int64_t)(v->client & 0x7fffffffffffffffull);
	if (v->has_type && lk_mask_of_type(v->type) != ~0ull)
		return (int64_t)lk_mask_of_type(v->type);
	return -1;
}

const char *lk_str(uint64_t mask, char *b, size_t n)
{
	snprintf(b, n, "0x%llx (MI %s, NI %s%s%s)", (unsigned long long)mask, (mask & LK_AFW) ? "locked" : "open",
		 (mask & LK_NFW) ? "locked" : "open", (mask & LK_PRIV) ? ", privileged locked" : "",
		 (mask & ~7ull) ? ", unknown bits" : "");
	return b;
}

int lk_save_ex(const char *path, const struct lk_file *lf)
{
	char tmp[256], line[192];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	int n = snprintf(line, sizeof(line), "mask=0x%llx tries=%u kind=%u efs=%u m1=%lld m2=%lld\n", (unsigned long long)lf->mask,
			 lf->tries, lf->kind, lf->efs, lf->m1, lf->m2);
	if (write(fd, line, (size_t)n) != n || fsync(fd)) {
		close(fd);
		unlink(tmp);
		return -1;
	}
	close(fd);
	if (rename(tmp, path))
		return -1;
	return 0;
}

int lk_load_ex(const char *path, struct lk_file *lf)
{
	FILE *f = fopen(path, "r");
	unsigned long long m;
	unsigned t = 0, k = 0, e = 0;
	long long m1 = 0, m2 = 0;
	if (!f)
		return -1;
	int r = fscanf(f, "mask=%llx tries=%u kind=%u efs=%u m1=%lld m2=%lld", &m, &t, &k, &e, &m1, &m2);
	fclose(f);
	if (r < 1)
		return -2;		/* present but unreadable */
	memset(lf, 0, sizeof(*lf));
	lf->mask = m;
	lf->tries = r >= 2 ? t : 0;
	lf->kind = r >= 3 ? k : 0;
	if (r >= 6) {
		lf->efs = e;
		lf->m1 = m1;
		lf->m2 = m2;
	}
	return 0;
}

int lk_save(const char *path, uint64_t mask, unsigned tries)
{
	struct lk_file lf = { .mask = mask, .tries = tries };
	return lk_save_ex(path, &lf);
}

int lk_load(const char *path, uint64_t *mask, unsigned *tries)
{
	struct lk_file lf;
	int r = lk_load_ex(path, &lf);
	if (r)
		return r;
	*mask = lf.mask;
	if (tries)
		*tries = lf.tries;
	return 0;
}
