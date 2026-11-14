/*
 * sweetstate: the parts of the state dump (phone/tools/sweetstate.sh) a shell cannot do. Read-only.
 *   sweetstate -v               version
 *   sweetstate -b               H boottime / H uptime records (kern.boottime)
 *   sweetstate -S PREFIX        S NAME VALUE for every sysctl whose name starts with PREFIX (e.g. kern.sweet_):
 *                               walks the kernel's OID tree with {0,2} "next" + {0,1} "name" (what sctl cannot list)
 *   sweetstate -P               P PID PPID UID START COMM for every process (START = epoch seconds; psx has none)
 *   sweetstate -M NAME,NAME     M PID COMM PATH: files mapped by those processes whose path has Sweet/Hakios/Redwood/
 *                               /var/local (which Metal / audio bundle a running process really uses)
 * Options combine: sweetstate -S kern.sweet_ -P -M backboardd,mediaserverd. Output is TAB separated, one record a line.
 * Built by phone/tools/build.sh; installed through the manifest (state/redfin.toml: /var/local/bin/sweetstate).
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#define SWEETSTATE_VERSION "sweetstate 1"

/* libproc: not in the iOS SDK headers (psx.c declares proc_pid_rusage the same way) */
int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
#if __has_include(<sys/proc_info.h>)
#include <sys/proc_info.h>
#endif
#ifndef PROC_PIDREGIONPATHINFO
#define PROC_PIDREGIONPATHINFO 8
#endif
struct st_vinfo_stat {	/* struct vinfo_stat */
	uint32_t vst_dev; uint16_t vst_mode; uint16_t vst_nlink; uint64_t vst_ino; uint32_t vst_uid; uint32_t vst_gid;
	int64_t vst_atime, vst_atimensec, vst_mtime, vst_mtimensec, vst_ctime, vst_ctimensec, vst_birthtime, vst_birthtimensec;
	int64_t vst_size, vst_blocks; int32_t vst_blksize; uint32_t vst_flags, vst_gen, vst_rdev; int64_t vst_qspare[2];
};
struct st_regionpath {	/* struct proc_regionwithpathinfo = proc_regioninfo + vnode_info_path */
	uint32_t pri_protection, pri_max_protection, pri_inheritance, pri_flags;
	uint64_t pri_offset;
	uint32_t pri_behavior, pri_user_wired_count, pri_user_tag, pri_pages_resident, pri_pages_shared_now_private,
		pri_pages_swapped_out, pri_pages_dirtied, pri_ref_count, pri_shadow_depth, pri_share_mode,
		pri_private_pages_resident, pri_shared_pages_resident, pri_obj_id, pri_depth;
	uint64_t pri_address, pri_size;
	struct { struct st_vinfo_stat vi_stat; int vi_type; int vi_pad; int32_t vi_fsid[2]; } vip_vi;
	char vip_path[1024];
};

static void clean(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r')
			*s = ' ';
}

static void boottime(void)
{
	struct timeval tv;
	size_t l = sizeof(tv);
	if (sysctlbyname("kern.boottime", &tv, &l, NULL, 0) == 0) {
		printf("H\tboottime\t%ld\nH\tuptime\t%ld\n", (long)tv.tv_sec, (long)(time(NULL) - tv.tv_sec));
	} else {
		printf("E\tkern.boottime: %s\n", strerror(errno));
	}
}

/* value of one OID as text: ints/quads decimal, strings cleaned, opaque as hex (first 64 bytes) */
static void print_oid(const char *name, int *oid, size_t n)
{
	int fq[2 + CTL_MAXNAME] = { 0, 4 };
	unsigned char fmt[256];
	size_t fl = sizeof(fmt);
	unsigned kind = 0;
	memcpy(fq + 2, oid, n * sizeof(int));
	if (sysctl(fq, (u_int)(n + 2), fmt, &fl, NULL, 0) == 0 && fl >= sizeof(unsigned))
		memcpy(&kind, fmt, sizeof(kind));
	kind &= 0xf;	/* CTLTYPE */
	if (kind == 1)	/* CTLTYPE_NODE */
		return;
	size_t l = 0;
	if (sysctl(oid, (u_int)n, NULL, &l, NULL, 0) != 0) {
		printf("E\t%s: %s\n", name, strerror(errno));
		return;
	}
	l += 64;
	unsigned char *buf = malloc(l + 1);
	if (!buf)
		return;
	if (sysctl(oid, (u_int)n, buf, &l, NULL, 0) != 0) {
		printf("E\t%s: %s\n", name, strerror(errno));
		free(buf);
		return;
	}
	const char *f = (const char *)fmt + sizeof(unsigned);
	if (kind == 3) {	/* string */
		buf[l] = 0;
		clean((char *)buf);
		printf("S\t%s\t%s\n", name, buf);
	} else if (kind == 2 && l == 4) {	/* int */
		int v;
		memcpy(&v, buf, 4);
		if (f[0] == 'I' && f[1] == 'U')
			printf("S\t%s\t%u\n", name, (unsigned)v);
		else
			printf("S\t%s\t%d\n", name, v);
	} else if ((kind == 4 || kind == 2) && l == 8) {	/* quad */
		long long v;
		memcpy(&v, buf, 8);
		printf("S\t%s\t%lld\n", name, v);
	} else {
		printf("S\t%s\t", name);
		for (size_t i = 0; i < l && i < 64; i++)
			printf("%02x", buf[i]);
		printf("\n");
	}
	free(buf);
}

static int sysctls(const char *prefix)
{
	/* start at the node of the prefix's first component ("kern" -> {1}) */
	int cur[CTL_MAXNAME];
	size_t cn = CTL_MAXNAME;
	char top[64];
	snprintf(top, sizeof(top), "%s", prefix);
	char *dot = strchr(top, '.');
	if (dot)
		*dot = 0;
	if (sysctlnametomib(top, cur, &cn) != 0)
		return printf("E\tsysctl %s: %s\n", top, strerror(errno)), 1;
	int found = 0;
	for (int guard = 0; guard < 100000; guard++) {
		int q[2 + CTL_MAXNAME] = { 0, 2 }, next[CTL_MAXNAME];
		memcpy(q + 2, cur, cn * sizeof(int));
		size_t nl = sizeof(next);
		if (sysctl(q, (u_int)(cn + 2), next, &nl, NULL, 0) != 0)
			break;	/* ENOENT: end of the tree */
		cn = nl / sizeof(int);
		memcpy(cur, next, nl);
		int nq[2 + CTL_MAXNAME] = { 0, 1 };
		memcpy(nq + 2, cur, cn * sizeof(int));
		char name[512];
		size_t sl = sizeof(name);
		if (sysctl(nq, (u_int)(cn + 2), name, &sl, NULL, 0) != 0)
			continue;
		name[sizeof(name) - 1] = 0;
		if (strncmp(name, top, strlen(top)) != 0 || name[strlen(top)] != '.')
			break;	/* past the top node */
		if (strncmp(name, prefix, strlen(prefix)) == 0) {
			print_oid(name, cur, cn);
			found++;
		}
	}
	if (!found)
		printf("E\tno sysctl under %s (no {0,2} next in this kernel?)\n", prefix);
	return 0;
}

static struct kinfo_proc *procs(int *n)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
	size_t len = 0;
	if (sysctl(mib, 3, NULL, &len, NULL, 0) < 0)
		return NULL;
	len += len / 4;
	struct kinfo_proc *p = malloc(len);
	if (!p || sysctl(mib, 3, p, &len, NULL, 0) < 0)
		return free(p), NULL;
	*n = (int)(len / sizeof(*p));
	return p;
}

static int interesting(const char *path)
{
	return strstr(path, "/var/local/") || strstr(path, "Sweet") || strstr(path, "Hakios") || strstr(path, "Redwood");
}

static void maps(struct kinfo_proc *p, int n, const char *names)
{
	for (int i = 0; i < n; i++) {
		const char *c = p[i].kp_proc.p_comm;
		size_t cl = strlen(c);
		const char *s = names;
		int want = 0;
		while ((s = strstr(s, c)) != NULL) {
			if ((s == names || s[-1] == ',') && (s[cl] == ',' || s[cl] == 0)) {
				want = 1;
				break;
			}
			s += cl;
		}
		if (!want)
			continue;
		pid_t pid = p[i].kp_proc.p_pid;
		char last[1024] = "";
		uint64_t addr = 0;
		for (int guard = 0; guard < 200000; guard++) {
			struct st_regionpath r;
			memset(&r, 0, sizeof(r));
			int got = proc_pidinfo(pid, PROC_PIDREGIONPATHINFO, addr, &r, sizeof(r));
			if (got <= 0)
				break;
			if (r.vip_path[0] && interesting(r.vip_path) && strcmp(r.vip_path, last) != 0) {
				snprintf(last, sizeof(last), "%s", r.vip_path);
				clean(last);
				printf("M\t%d\t%s\t%s\n", pid, c, last);
			}
			if (r.pri_address + r.pri_size <= addr)
				break;
			addr = r.pri_address + r.pri_size;
		}
	}
}

int main(int argc, char **argv)
{
	int np = 0;
	struct kinfo_proc *p = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) {
			printf("%s\n", SWEETSTATE_VERSION);
		} else if (!strcmp(argv[i], "-b")) {
			boottime();
		} else if (!strcmp(argv[i], "-S") && i + 1 < argc) {
			sysctls(argv[++i]);
		} else if (!strcmp(argv[i], "-P") || (!strcmp(argv[i], "-M") && i + 1 < argc)) {
			if (!p && !(p = procs(&np))) {
				printf("E\tKERN_PROC_ALL: %s\n", strerror(errno));
				continue;
			}
			if (!strcmp(argv[i], "-M")) {
				maps(p, np, argv[++i]);
				continue;
			}
			for (int k = 0; k < np; k++) {
				char comm[sizeof(p[k].kp_proc.p_comm) + 1];
				snprintf(comm, sizeof(comm), "%s", p[k].kp_proc.p_comm);
				clean(comm);
				printf("P\t%d\t%d\t%u\t%ld\t%s\n", p[k].kp_proc.p_pid, p[k].kp_eproc.e_ppid,
				       p[k].kp_eproc.e_ucred.cr_uid, (long)p[k].kp_proc.p_starttime.tv_sec, comm);
			}
		} else {
			fprintf(stderr, "usage: sweetstate [-v] [-b] [-S PREFIX] [-P] [-M NAME,NAME]\n");
			return 2;
		}
	}
	free(p);
	return 0;
}
