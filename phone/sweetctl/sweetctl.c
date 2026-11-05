/*
 * sweetctl (ws-n-ctl): the root helper behind SweetControl.app. Installed setuid root:
 *   files put out/sweetctl /var/local/bin/sweetctl 755, then on the phone: toybox chown root:wheel ... && chmod 4755 ...
 * The app runs as mobile; every write goes through here. `sweetctl id` shows whether setuid took (euid 0).
 *
 *   sweetctl id                          uid/euid (setuid check)
 *   sweetctl get NAME                    any sysctl (int, quad or string)
 *   sweetctl set NAME VALUE              kern.sweet_* only
 *   sweetctl kv-get FILE KEY             KEY=VALUE files (mtl2-env, mtl2-live, wland.conf, sweethud, ...)
 *   sweetctl kv-set FILE KEY VALUE       replaces the KEY= line or appends one
 *   sweetctl kv-del FILE KEY             removes the KEY= line(s)
 *   sweetctl flag FILE 0|1               1 = create (empty, kept if present), 0 = remove
 *   sweetctl size FILE N                 a file of N zero bytes (SweetSB's size flags); N < 0 removes it
 *   sweetctl write FILE TEXT             whole file = TEXT + newline ("" = empty file)
 *   sweetctl cat FILE                    print a file
 *   sweetctl kill NAME [SIG]             signal every process whose p_comm is NAME (default 9); launchd restarts jobs
 *   sweetctl respring                    kill -9 SpringBoard
 *   sweetctl render                      kill -9 backboardd (render server: Metal env, CA env; SpringBoard and apps follow)
 *   sweetctl userspace-reboot            reboot3(RB2_USERREBOOT): all of userland again, same kernel, live sysctls kept
 *   sweetctl reboot                      full reboot (the RAM-booted image is gone: fastboot boot it again)
 *   sweetctl boot-set NAME VALUE         remember a kern.sweet_* value in /var/local/sweetctl/boot.conf
 *   sweetctl boot-del NAME | boot-list
 *   sweetctl boot-apply [--once]         set every boot.conf value now (--once: only once per kernel boot;
 *                                        SweetFPS.dylib runs it from SpringBoard at every SpringBoard start)
 *
 * FILE must be under /var/local/ (no ".."). The first change to a file copies it to FILE.pre-sweetctl (or makes
 * FILE.pre-sweetctl-absent when it did not exist); every change is one line in /var/local/sweetctl/changes.log.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DIR_LOCAL	"/var/local/"
#define DIR_CTL		"/var/local/sweetctl"
#define BOOT_CONF	DIR_CTL "/boot.conf"
#define CHANGES_LOG	DIR_CTL "/changes.log"
#define BOOT_MARK	"/var/tmp/sweetctl.boot"
#define MAXF		(256 * 1024)

static int
die(const char *fmt, const char *a)
{
	fprintf(stderr, "sweetctl: ");
	fprintf(stderr, fmt, a ? a : "");
	fprintf(stderr, "\n");
	return 1;
}

static void
logchange(int argc, char **argv)
{
	mkdir(DIR_CTL, 0755);
	FILE *f = fopen(CHANGES_LOG, "a");
	if (f == NULL) {
		return;
	}
	time_t t = time(NULL);
	char ts[32];
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
	fprintf(f, "%s uid %d:", ts, (int)getuid());
	for (int i = 1; i < argc; i++) {
		fprintf(f, " '%s'", argv[i]);
	}
	fprintf(f, "\n");
	fclose(f);
}

static int
path_ok(const char *p)
{
	const char *q = p;
	if (strncmp(q, "/private", 8) == 0) {
		q += 8;
	}
	return strncmp(q, DIR_LOCAL, strlen(DIR_LOCAL)) == 0 && strstr(p, "..") == NULL && strlen(p) < 900;
}

/* the whole file, NUL-terminated (NULL + *len 0 if absent) */
static char *
slurp(const char *path, size_t *len)
{
	*len = 0;
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return NULL;
	}
	char *b = malloc(MAXF + 1);
	ssize_t n, tot = 0;
	while (tot < MAXF && (n = read(fd, b + tot, MAXF - tot)) > 0) {
		tot += n;
	}
	close(fd);
	b[tot] = 0;
	*len = (size_t)tot;
	return b;
}

static int
copyfile_plain(const char *from, const char *to)
{
	size_t n;
	char *b = slurp(from, &n);
	if (b == NULL) {
		return -1;
	}
	int fd = open(to, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0) {
		free(b);
		return -1;
	}
	ssize_t w = write(fd, b, n);
	close(fd);
	free(b);
	return w == (ssize_t)n ? 0 : -1;
}

/* first change to FILE: keep the original beside it */
static void
backup_once(const char *path)
{
	char bk[1024], ab[1024];
	snprintf(bk, sizeof(bk), "%s.pre-sweetctl", path);
	snprintf(ab, sizeof(ab), "%s.pre-sweetctl-absent", path);
	struct stat st;
	if (stat(bk, &st) == 0 || stat(ab, &st) == 0) {
		return;
	}
	if (stat(path, &st) == 0) {
		copyfile_plain(path, bk);
	} else {
		int fd = open(ab, O_WRONLY | O_CREAT, 0644);
		if (fd >= 0) {
			close(fd);
		}
	}
}

static int
write_atomic(const char *path, const char *data, size_t n)
{
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s.sweetctl-tmp", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		return die("cannot write %s", tmp);
	}
	if (n > 0 && write(fd, data, n) != (ssize_t)n) {
		close(fd);
		unlink(tmp);
		return die("short write %s", tmp);
	}
	fchmod(fd, 0644);
	close(fd);
	if (rename(tmp, path) != 0) {
		unlink(tmp);
		return die("rename to %s failed", path);
	}
	return 0;
}

/* ---- sysctl ---- */

static int
sysctl_kind(const char *name, int *kind)
{
	int mib[CTL_MAXNAME + 2];
	size_t miblen = CTL_MAXNAME;
	if (sysctlnametomib(name, mib + 2, &miblen) != 0) {
		return -1;
	}
	mib[0] = 0;
	mib[1] = 4;	/* oidfmt */
	unsigned char buf[256];
	size_t len = sizeof(buf);
	if (sysctl(mib, (u_int)miblen + 2, buf, &len, NULL, 0) != 0 || len < 4) {
		return -1;
	}
	uint32_t k;
	memcpy(&k, buf, 4);
	*kind = (int)(k & CTLTYPE);
	return 0;
}

static int
cmd_get(const char *name)
{
	int kind = CTLTYPE_INT;
	sysctl_kind(name, &kind);
	if (kind == CTLTYPE_STRING) {
		char s[4096];
		size_t l = sizeof(s) - 1;
		if (sysctlbyname(name, s, &l, NULL, 0) != 0) {
			return die("no sysctl %s", name);
		}
		s[l] = 0;
		printf("%s\n", s);
		return 0;
	}
	uint64_t v = 0;
	size_t l = sizeof(v);
	if (sysctlbyname(name, &v, &l, NULL, 0) != 0) {
		return die("no sysctl %s", name);
	}
	if (l == 4) {
		printf("%d\n", (int)(uint32_t)v);
	} else {
		printf("%lld\n", (long long)v);
	}
	return 0;
}

static int
sysctl_set(const char *name, const char *val)
{
	if (strncmp(name, "kern.sweet_", 11) != 0) {
		return die("set: only kern.sweet_* (%s)", name);
	}
	int kind = CTLTYPE_INT;
	if (sysctl_kind(name, &kind) != 0) {
		return die("no sysctl %s", name);
	}
	int r;
	if (kind == CTLTYPE_STRING) {
		r = sysctlbyname(name, NULL, NULL, (void *)val, strlen(val) + 1);
	} else if (kind == CTLTYPE_QUAD) {
		int64_t q = strtoll(val, NULL, 0);
		r = sysctlbyname(name, NULL, NULL, &q, sizeof(q));
	} else {
		int i = (int)strtol(val, NULL, 0);
		r = sysctlbyname(name, NULL, NULL, &i, sizeof(i));
	}
	if (r != 0) {
		fprintf(stderr, "sweetctl: set %s=%s: %s\n", name, val, strerror(errno));
		return 1;
	}
	return 0;
}

/* ---- KEY=VALUE files ---- */

/* does line [l, l+n) set KEY? (leading blanks, optional "export ") */
static int
line_is_key(const char *l, size_t n, const char *key)
{
	while (n > 0 && (*l == ' ' || *l == '\t')) {
		l++;
		n--;
	}
	if (n > 7 && strncmp(l, "export ", 7) == 0) {
		l += 7;
		n -= 7;
	}
	size_t k = strlen(key);
	return n > k && strncmp(l, key, k) == 0 && l[k] == '=';
}

static int
kv(const char *path, const char *key, const char *val, int op)	/* op 0 get, 1 set, 2 del */
{
	if (!path_ok(path)) {
		return die("path not under /var/local: %s", path);
	}
	if (strchr(key, '=') || strchr(key, '\n') || (val && strchr(val, '\n'))) {
		return die("bad key or value%s", "");
	}
	size_t n;
	char *b = slurp(path, &n);
	char *out = malloc(n + strlen(key) + (val ? strlen(val) : 0) + 4);
	size_t o = 0;
	int found = 0;
	for (size_t i = 0; b != NULL && i < n;) {
		size_t e = i;
		while (e < n && b[e] != '\n') {
			e++;
		}
		size_t ll = e - i;
		if (b[i] != '#' && line_is_key(b + i, ll, key)) {
			if (op == 0) {
				const char *eq = memchr(b + i, '=', ll);
				printf("%.*s\n", (int)(ll - (size_t)(eq + 1 - (b + i))), eq + 1);
				free(b);
				free(out);
				return 0;
			}
			if (op == 1 && !found) {
				o += (size_t)sprintf(out + o, "%s=%s\n", key, val);
			}
			found = 1;
		} else {
			memcpy(out + o, b + i, ll);
			o += ll;
			out[o++] = '\n';
		}
		i = e + 1;
	}
	if (op == 0) {
		free(b);
		free(out);
		return 2;	/* not set: exit 2, no output */
	}
	if (op == 1 && !found) {
		o += (size_t)sprintf(out + o, "%s=%s\n", key, val);
	}
	backup_once(path);
	int r = write_atomic(path, out, o);
	free(b);
	free(out);
	return r;
}

/* ---- processes ---- */

static int
kill_name(const char *name, int sig)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
	size_t len = 0;
	if (sysctl(mib, 3, NULL, &len, NULL, 0) < 0) {
		return die("proc list%s", "");
	}
	len += len / 4;
	struct kinfo_proc *p = malloc(len);
	if (sysctl(mib, 3, p, &len, NULL, 0) < 0) {
		free(p);
		return die("proc list%s", "");
	}
	int n = (int)(len / sizeof(*p)), killed = 0;
	for (int i = 0; i < n; i++) {
		if (strcmp(p[i].kp_proc.p_comm, name) == 0 && p[i].kp_proc.p_pid > 1 && kill(p[i].kp_proc.p_pid, sig) == 0) {
			printf("killed %d %s\n", p[i].kp_proc.p_pid, name);
			killed++;
		}
	}
	free(p);
	return killed ? 0 : die("no process %s", name);
}

static int
do_reboot(int user)
{
	sync();
	if (user) {
		int (*r3)(uint64_t, ...) = (int (*)(uint64_t, ...))dlsym(RTLD_DEFAULT, "reboot3");
		if (r3 == NULL) {
			return die("reboot3 not found%s", "");
		}
		int r = r3(0x2000000000000000ULL);	/* RB2_USERREBOOT */
		fprintf(stderr, "sweetctl: reboot3: %d %s\n", r, strerror(errno));
		return 1;
	}
	int (*rb)(int) = (int (*)(int))dlsym(RTLD_DEFAULT, "reboot");
	if (rb == NULL) {
		return die("reboot not found%s", "");
	}
	rb(0);
	return die("reboot failed: %s", strerror(errno));
}

/* ---- boot.conf (NAME=VALUE) ---- */

static int
boot_apply(int once)
{
	struct timeval bt = { 0, 0 };
	size_t l = sizeof(bt);
	sysctlbyname("kern.boottime", &bt, &l, NULL, 0);
	char mark[64];
	snprintf(mark, sizeof(mark), "%ld\n", (long)bt.tv_sec);
	if (once) {
		size_t n;
		char *m = slurp(BOOT_MARK, &n);
		int same = m != NULL && strcmp(m, mark) == 0;
		free(m);
		if (same) {
			return 0;
		}
	}
	size_t n;
	char *b = slurp(BOOT_CONF, &n);
	int bad = 0, done = 0;
	for (char *s = b, *e; s != NULL && *s; s = e) {
		e = strchr(s, '\n');
		if (e != NULL) {
			*e++ = 0;
		} else {
			e = s + strlen(s);
		}
		char *eq = strchr(s, '=');
		if (*s == '#' || eq == NULL) {
			continue;
		}
		*eq = 0;
		if (sysctl_set(s, eq + 1) == 0) {
			printf("%s=%s\n", s, eq + 1);
			done++;
		} else {
			bad++;
		}
	}
	free(b);
	int fd = open(BOOT_MARK, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		write(fd, mark, strlen(mark));
		close(fd);
	}
	printf("boot-apply: %d set, %d failed\n", done, bad);
	return bad ? 1 : 0;
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: see the top of phone/sweetctl/sweetctl.c\n");
		return 1;
	}
	const char *c = argv[1];
	if (geteuid() == 0) {
		setuid(0);	/* real uid too: children and reboot3 see root */
		setgid(0);
	}
	umask(022);
	if (!strcmp(c, "id")) {
		printf("uid %d euid %d\n", (int)getuid(), (int)geteuid());
		return geteuid() == 0 ? 0 : 1;
	}
	if (!strcmp(c, "get") && argc == 3) {
		return cmd_get(argv[2]);
	}
	if (!strcmp(c, "kv-get") && argc == 4) {
		return kv(argv[2], argv[3], NULL, 0);
	}
	if (!strcmp(c, "cat") && argc == 3) {
		if (!path_ok(argv[2])) {
			return die("path not under /var/local: %s", argv[2]);
		}
		size_t n;
		char *b = slurp(argv[2], &n);
		if (b == NULL) {
			return 2;
		}
		fwrite(b, 1, n, stdout);
		free(b);
		return 0;
	}
	if (!strcmp(c, "boot-list")) {
		size_t n;
		char *b = slurp(BOOT_CONF, &n);
		if (b != NULL) {
			fwrite(b, 1, n, stdout);
		}
		free(b);
		return 0;
	}
	/* everything below changes something */
	logchange(argc, argv);
	if (!strcmp(c, "set") && argc == 4) {
		return sysctl_set(argv[2], argv[3]);
	}
	if (!strcmp(c, "kv-set") && argc == 5) {
		return kv(argv[2], argv[3], argv[4], 1);
	}
	if (!strcmp(c, "kv-del") && argc == 4) {
		return kv(argv[2], argv[3], NULL, 2);
	}
	if ((!strcmp(c, "flag") || !strcmp(c, "size") || !strcmp(c, "write")) && argc == 4) {
		const char *p = argv[2];
		if (!path_ok(p)) {
			return die("path not under /var/local: %s", p);
		}
		backup_once(p);
		if (!strcmp(c, "write")) {
			size_t l = strlen(argv[3]);
			char *d = malloc(l + 2);
			memcpy(d, argv[3], l);
			if (l > 0) {
				d[l++] = '\n';
			}
			int r = write_atomic(p, d, l);
			free(d);
			return r;
		}
		long v = strtol(argv[3], NULL, 0);
		if (!strcmp(c, "flag") ? v == 0 : v < 0) {
			return unlink(p) == 0 || errno == ENOENT ? 0 : die("cannot remove %s", p);
		}
		if (!strcmp(c, "flag")) {
			int fd = open(p, O_WRONLY | O_CREAT, 0644);
			return fd >= 0 ? close(fd) : die("cannot create %s", p);
		}
		int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) {
			return die("cannot create %s", p);
		}
		int r = ftruncate(fd, (off_t)v);	/* zero bytes, as `head -c N /dev/zero` */
		close(fd);
		return r == 0 ? 0 : die("truncate %s", p);
	}
	if (!strcmp(c, "kill") && (argc == 3 || argc == 4)) {
		return kill_name(argv[2], argc == 4 ? atoi(argv[3]) : SIGKILL);
	}
	if (!strcmp(c, "respring")) {
		return kill_name("SpringBoard", SIGKILL);
	}
	if (!strcmp(c, "render")) {
		return kill_name("backboardd", SIGKILL);
	}
	if (!strcmp(c, "userspace-reboot")) {
		return do_reboot(1);
	}
	if (!strcmp(c, "reboot")) {
		return do_reboot(0);
	}
	if (!strcmp(c, "boot-set") && argc == 4) {
		if (strncmp(argv[2], "kern.sweet_", 11) != 0) {
			return die("boot-set: only kern.sweet_* (%s)", argv[2]);
		}
		mkdir(DIR_CTL, 0755);
		return kv(BOOT_CONF, argv[2], argv[3], 1);
	}
	if (!strcmp(c, "boot-del") && argc == 3) {
		return kv(BOOT_CONF, argv[2], NULL, 2);
	}
	if (!strcmp(c, "boot-apply")) {
		return boot_apply(argc > 2 && !strcmp(argv[2], "--once"));
	}
	return die("unknown command or wrong arguments: %s", c);
}
