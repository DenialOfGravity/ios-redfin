/*
 * sctl NAME [VALUE]: read or write one sysctl (toybox has no sysctl; Apple's is not on the image).
 *   read:  "NAME: VALUE"            (int/quad in decimal; strings as-is; opaque data as hex bytes)
 *   write: "NAME: OLD -> NEW"       (VALUE decimal or 0x hex, optional leading '-'; a string for string sysctls)
 * The output is parsed by tools-claude/sweetmon/collector.py ("NAME: INT") and agent scripts; keep its shape.
 * Rewrite of the original int-only sctl (2026-10-29, source not in git): long string sysctls (kern.sweet_bb_stats,
 * kern.sweet_ssr_state, ...) were read into a 4-byte buffer and failed with ENOMEM; "0x10" was atoi()'d to 0 and
 * written silently. Now the buffer is sized by a first sysctlbyname(NULL) call and retried on ENOMEM, the type comes
 * from the kernel's OID format (sysctl {0,3} name2oid + {0,4} oidfmt), and a bad VALUE is an error (exit 2).
 * Exit: 0 ok, 1 sysctl failed (perror), 2 usage / bad value.
 */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

#define K_INT 2
#define K_STRING 3
#define K_QUAD 4
#define K_OPAQUE 5

static int kind_of(const char *name, size_t *len, int *uns)
{
	int qoid[2 + CTL_MAXNAME] = { 0, 3 };
	size_t ol = sizeof(qoid) - 2 * sizeof(int);
	*uns = 0;
	if (sysctl(qoid, 2, qoid + 2, &ol, (void *)name, strlen(name)) == 0 && ol >= sizeof(int)) {
		int fq[2 + CTL_MAXNAME];
		unsigned char fmt[256];
		size_t fl = sizeof(fmt) - 1;
		fq[0] = 0;
		fq[1] = 4;
		memcpy(fq + 2, qoid + 2, ol);
		if (sysctl(fq, 2 + (u_int)(ol / sizeof(int)), fmt, &fl, NULL, 0) == 0 && fl > sizeof(u_int)) {
			fmt[fl] = 0;
			u_int k;
			memcpy(&k, fmt, sizeof(k));
			const char *f = (const char *)fmt + sizeof(u_int);
			*uns = strchr(f, 'U') != NULL;
			k &= CTLTYPE;
			if (k == K_INT || k == K_STRING || k == K_QUAD) {
				return (int)k;
			}
			return K_OPAQUE;
		}
	}
	/* no OID format: guess from the size */
	return *len == 4 ? K_INT : *len == 8 ? K_QUAD : K_STRING;
}

/* read NAME into a malloc'd buffer sized by a NULL probe; ENOMEM (the value grew) = bigger buffer, again */
static char *get(const char *name, size_t *outlen)
{
	size_t len = 0;
	if (sysctlbyname(name, NULL, &len, NULL, 0) != 0) {
		return NULL;
	}
	for (int i = 0; i < 6; i++) {
		size_t cap = len + len / 2 + 64;
		char *b = malloc(cap + 1);
		if (b == NULL) {
			errno = ENOMEM;
			return NULL;
		}
		size_t l = cap;
		if (sysctlbyname(name, b, &l, NULL, 0) == 0) {
			b[l] = 0;
			*outlen = l;
			return b;
		}
		int e = errno;
		free(b);
		if (e != ENOMEM) {
			errno = e;
			return NULL;
		}
		len = cap * 2;
	}
	errno = ENOMEM;
	return NULL;
}

static void show(int kind, int uns, const char *b, size_t l)
{
	if (kind == K_INT && l >= 4) {
		int32_t v;
		memcpy(&v, b, 4);
		uns ? printf("%u", (uint32_t)v) : printf("%d", v);
	} else if (kind == K_QUAD && l >= 8) {
		int64_t v;
		memcpy(&v, b, 8);
		uns ? printf("%llu", (unsigned long long)v) : printf("%lld", (long long)v);
	} else if (kind == K_STRING) {
		while (l && (b[l - 1] == 0 || b[l - 1] == '\n')) {
			l--;
		}
		fwrite(b, 1, l, stdout);
	} else {
		for (size_t i = 0; i < l; i++) {
			printf("%02x", (unsigned char)b[i]);
		}
	}
}

/* decimal or 0x hex, optional '-', nothing after it */
static int parse(const char *s, long long *v)
{
	const char *p = s;
	int neg = 0;
	if (*p == '-') {
		neg = 1;
		p++;
	}
	int base = 10;
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		base = 16;
		p += 2;
	}
	if (*p == 0 || *p == '-' || *p == '+') {
		return -1;
	}
	char *end;
	errno = 0;
	unsigned long long u = strtoull(p, &end, base);
	if (errno || *end) {
		return -1;
	}
	if (neg && u > (unsigned long long)LLONG_MAX + 1) {
		return -1;
	}
	*v = neg ? (long long)(0 - u) : (long long)u;
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: sctl NAME [INT|0xHEX|STRING]\n");
		return 2;
	}
	const char *name = argv[1];
	size_t l = 0;
	char *old = get(name, &l);
	if (old == NULL) {
		perror("sysctlbyname");
		return 1;
	}
	int uns;
	int kind = kind_of(name, &l, &uns);
	if (argc == 2) {
		printf("%s: ", name);
		show(kind, uns, old, l);
		printf("\n");
		return 0;
	}
	const void *nv;
	size_t nl;
	int32_t i32;
	int64_t i64;
	if (kind == K_STRING) {
		nv = argv[2];
		nl = strlen(argv[2]) + 1;
	} else {
		long long v;
		if (kind == K_OPAQUE || parse(argv[2], &v) != 0) {
			fprintf(stderr, "sctl: bad value '%s' for %s (decimal or 0x hex%s)\n", argv[2], name,
			    kind == K_OPAQUE ? "; opaque sysctls are read-only here" : "");
			return 2;
		}
		if (kind == K_INT) {
			if (v < INT32_MIN || v > (long long)UINT32_MAX) {
				fprintf(stderr, "sctl: %s is a 32-bit sysctl; '%s' does not fit\n", name, argv[2]);
				return 2;
			}
			i32 = (int32_t)(uint32_t)v;
			nv = &i32;
			nl = 4;
		} else {
			i64 = v;
			nv = &i64;
			nl = 8;
		}
	}
	if (sysctlbyname(name, NULL, NULL, (void *)nv, nl) != 0) {
		perror("sysctlbyname");
		return 1;
	}
	printf("%s: ", name);
	show(kind, uns, old, l);
	printf(" -> ");
	size_t l2 = 0;
	char *now = get(name, &l2);
	if (now) {
		show(kind, uns, now, l2);
	} else {
		printf("?");
	}
	printf("\n");
	return 0;
}
