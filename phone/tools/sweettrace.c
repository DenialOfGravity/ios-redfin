/*
 * sweettrace: record a kdebug trace for the frame-pacing analysis (tools-claude/sweet-trace.py).
 *
 *   sweettrace [-t SECS] [-o FILE] [-b KBUFS] [-m MAXEVENTS] [-r MS] [-d DELAY] [-p PID,...] [-c CLASS[/SUB],...]
 *              [-S] [-I] [-n] [-s SECS]
 *   sweettrace -R      reset kdebug (another owner left it set up)
 *   sweettrace -V      version and features ("rotate" = -s)
 *
 *   -t SECS     how long to record (default 10)
 *   -o FILE     output (default /var/tmp/sweettrace.swtr)
 *   -b KBUFS    kernel buffer in events (default 1048576 = 64 MB; halved until the kernel takes it)
 *   -m MAX      keep at most MAX events in memory (default 4194304 = 256 MB; grown as needed)
 *   -r MS       read the kernel buffer every MS ms (default 100; 0 = only at the end, buffer set to not wrap)
 *   -d DELAY    wait DELAY seconds before starting
 *   -p PIDS     only events of these processes (kdebug's pid filter; 0 = the kernel). Default: everyone, since the
 *               waker of a thread is often in another process. Names work too: -p backboardd,Preferences,0
 *   -c CSC      also these classes (hex or decimal; CLASS = all its subclasses, CLASS/SUB = one), e.g. -c 0x31
 *   -S          also syscalls (BSD and Mach traps: many events)
 *   -I          no interrupts (DBG_MACH_EXCP_INTR)
 *   -n          no thread names (skip the proc_pidinfo walk)
 *   -s SECS     rotate: ONE kdebug session for all of -t, a new output file every SECS seconds (and on SIGUSR1),
 *               with no gap: the kernel buffer is never released, events are split at the rotation time stamp
 *               (file N ends at T, file N+1 starts at T). -o is a printf pattern for the file number (from 1), e.g.
 *               -o /var/tmp/s/c%03d.swtr (no % in it: FILE.001.swtr ...). Each file is written as FILE.part while it
 *               grows (events are streamed to it by a writer thread, so memory stays small) and renamed to FILE
 *               when complete. Per file, stdout gets (line buffered, for pa session's chain.log):
 *                 CHUNK N START <wall s> pid P tries 0 secs SECS rotate
 *                 CHUNK N DONE <bytes> sweettrace: <events> events in <s> s ... -> FILE
 *               Each file has the full header (start/end mach time, wall time at its start, wrapped/dropped for its
 *               own span) and the thread table known at its end (kdebug's current thread map is re-read at
 *               every rotation). Old readers read every file as a normal trace.
 *
 * Default classes: DBG_MACH/DBG_MACH_SCHED (context switches, MAKE_RUNNABLE with the waker as the current thread,
 * blocks, waits), DBG_MACH/DBG_MACH_IPC (mach_msg send/receive, KMSG_INFO sender -> receiver pid), DBG_MACH/
 * DBG_MACH_EXCP_INTR (interrupts), DBG_DRIVERS/0xF5 (0994's display/GPU/touch events), DBG_APPS/0xA6 (hakios-metal's
 * user-space kdebug_trace events, class 0x21 subclass 0xA6), DBG_TRACE (always on).
 *
 * File (little endian): header (struct swt_header), n_events kd_buf (64 bytes each: timestamp, arg1-4, arg5 =
 * thread id, debugid, cpuid, unused), n_threads struct swt_thread (tid, pid, process name, thread name).
 *
 * Needs root (kdebug's ktrace_configure: superuser). Another owner of ktrace (EBUSY): `sweettrace -R` resets it.
 */
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#include <sys/proc.h>

/* bsd/sys/sysctl.h, bsd/sys/kdebug_private.h (not in the iOS SDK) */
#ifndef KERN_KDEBUG
#define KERN_KDEBUG             24
#endif
#define KERN_KDEFLAGS           1
#define KERN_KDENABLE           3
#define KERN_KDSETBUF           4
#define KERN_KDGETBUF           5
#define KERN_KDSETUP            6
#define KERN_KDREMOVE           7
#define KERN_KDREADTR           10
#define KERN_KDPIDTR            11
#define KERN_KDTHRMAP           12
#define KERN_KDREADCURTHRMAP    21
#define KERN_KDSET_TYPEFILTER   22
#define KDEBUG_ENABLE_TRACE     0x1
#define KDBG_NOWRAP             0x0002
#define KDBG_WRAPPED            0x0008
#define KDBG_TYPENONE           0x80000
#define TYPEFILTER_SIZE         8192

#define DBG_MACH                1
#define DBG_MACH_EXCP_SC        0x0c
#define DBG_MACH_EXCP_INTR      0x05
#define DBG_MACH_IPC            0x20
#define DBG_MACH_SCHED          0x40
#define DBG_BSD                 4
#define DBG_BSD_EXCP_SC         0x0c
#define DBG_DRIVERS             6
#define DBG_TRACE               7
#define SWEET_SUBCLASS          0xF5
#define DBG_APPS                0x21
#define HAKIOS_METAL_SUBCLASS   0xA6    /* hakios-metal (DBG_APPS) */

typedef struct {
	uint64_t timestamp;
	uint64_t arg1, arg2, arg3, arg4, arg5;
	uint32_t debugid;
	uint32_t cpuid;
	uint64_t unused;
} kd_buf;

typedef struct {
	int nkdbufs;
	int nolog;
	unsigned int flags;
	int nkdthreads;
	int bufid;
} kbufinfo_t;

typedef struct {
	uint64_t thread;
	int valid;
	char command[20];
} kd_threadmap;

typedef struct {
	unsigned int type, value1, value2, value3, value4;
} kd_regtype;

/* libproc (no header in the iOS SDK) */
int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
#define PROC_PIDTHREADID64INFO  15
#define PROC_PIDLISTTHREADIDS   28
struct proc_threadinfo {
	uint64_t pth_user_time, pth_system_time;
	int32_t pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time, pth_curpri, pth_priority,
	    pth_maxpriority;
	char pth_name[64];
};

#define SWT_MAGIC       "SWTRACE1"
struct swt_header {
	char     magic[8];
	uint32_t version, header_size;
	uint32_t tb_numer, tb_denom;    /* mach_timebase_info */
	uint64_t start_abs, end_abs;    /* tracing on / off (mach_absolute_time) */
	uint64_t n_events;
	uint32_t n_threads, ncpu;
	uint32_t kd_flags;              /* kbufinfo flags at the end (KDBG_WRAPPED: the kernel buffer overran) */
	uint32_t kbufs;                 /* kernel buffer size (events) */
	uint32_t reads, dropped;        /* periodic reads; events not kept (-m reached) */
	int32_t  tracer_pid;
	uint32_t read_ms;
	uint64_t start_wall_ns;         /* CLOCK_REALTIME at start */
	char     args[128];
};
struct swt_thread {
	uint64_t tid;
	int32_t  pid;
	uint32_t flags;                 /* 1 from the kdebug map, 2 from proc_pidinfo */
	char     comm[32];
	char     name[64];
};

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

/* kern.kdebug.OP[.VALUE]: KDEFLAGS, KDENABLE and KDSETBUF take VALUE as the next name; the others read or write oldp */
static int kd(int op, int value, void *buf, size_t *len)
{
	int mib[4] = { CTL_KERN, KERN_KDEBUG, op, value };
	size_t l0 = 0;
	int withval = op == KERN_KDEFLAGS || op == KERN_KDENABLE || op == KERN_KDSETBUF;
	return sysctl(mib, withval ? 4 : 3, buf, len ? len : &l0, NULL, 0);
}

static int kd_info(kbufinfo_t *bi)
{
	size_t l = sizeof(*bi);
	memset(bi, 0, sizeof(*bi));
	return kd(KERN_KDGETBUF, 0, bi, &l);
}

/* processes: pid and name, from KERN_PROC_ALL */
struct pinfo { int pid; char comm[32]; };
static struct pinfo *procs(int *n)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
	size_t len = 0;
	*n = 0;
	if (sysctl(mib, 3, NULL, &len, NULL, 0) < 0)
		return NULL;
	len += 64 * sizeof(struct kinfo_proc);
	struct kinfo_proc *kp = malloc(len);
	if (kp == NULL || sysctl(mib, 3, kp, &len, NULL, 0) < 0) {
		free(kp);
		return NULL;
	}
	int c = (int)(len / sizeof(*kp));
	struct pinfo *p = calloc((size_t)c + 1, sizeof(*p));
	for (int i = 0; p != NULL && i < c; i++) {
		p[i].pid = kp[i].kp_proc.p_pid;
		strlcpy(p[i].comm, kp[i].kp_proc.p_comm, sizeof(p[i].comm));
	}
	free(kp);
	*n = p != NULL ? c : 0;
	return p;
}

/* thread map */
static struct swt_thread *thr;
static uint32_t nthr, capthr;
static struct swt_thread *thr_get(uint64_t tid)
{
	for (uint32_t i = 0; i < nthr; i++)
		if (thr[i].tid == tid)
			return &thr[i];
	if (nthr == capthr) {
		capthr = capthr ? 2 * capthr : 4096;
		thr = realloc(thr, capthr * sizeof(*thr));
		if (thr == NULL) {
			perror("realloc");
			exit(1);
		}
	}
	memset(&thr[nthr], 0, sizeof(thr[nthr]));
	thr[nthr].tid = tid;
	thr[nthr].pid = -1;
	return &thr[nthr++];
}

static void map_kdebug(const kd_threadmap *m, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (m[i].thread == 0)
			continue;
		struct swt_thread *t = thr_get(m[i].thread);
		t->flags |= 1;
		if (t->pid < 0)       /* valid: the pid (1 for kernproc, as launchd) */
			t->pid = strncmp(m[i].command, "kernel_task", sizeof(m[i].command)) == 0 ? 0 : m[i].valid;
		if (t->comm[0] == 0) {
			memcpy(t->comm, m[i].command, sizeof(m[i].command) < sizeof(t->comm) ? sizeof(m[i].command) : sizeof(t->comm));
			t->comm[sizeof(t->comm) - 1] = 0;
		}
	}
}

static void map_names(void)
{
	int np = 0;
	struct pinfo *p = procs(&np);
	uint64_t *ids = malloc(4096 * sizeof(uint64_t));
	for (int i = 0; p != NULL && ids != NULL && i < np; i++) {
		int got = proc_pidinfo(p[i].pid, PROC_PIDLISTTHREADIDS, 0, ids, 4096 * (int)sizeof(uint64_t));
		for (int k = 0; got > 0 && k < got / (int)sizeof(uint64_t); k++) {
			struct proc_threadinfo ti;
			memset(&ti, 0, sizeof(ti));
			struct swt_thread *t = thr_get(ids[k]);
			t->flags |= 2;
			t->pid = p[i].pid;
			strlcpy(t->comm, p[i].comm, sizeof(t->comm));
			if (proc_pidinfo(p[i].pid, PROC_PIDTHREADID64INFO, ids[k], &ti, sizeof(ti)) == (int)sizeof(ti) && ti.pth_name[0])
				strlcpy(t->name, ti.pth_name, sizeof(t->name));
		}
	}
	free(ids);
	free(p);
}

static int pid_of(const char *s)
{
	char *e;
	long v = strtol(s, &e, 10);
	if (*s && *e == 0)
		return (int)v;
	int np = 0, pid = -1;
	struct pinfo *p = procs(&np);
	for (int i = 0; p != NULL && i < np; i++)
		if (strcmp(p[i].comm, s) == 0) {
			pid = p[i].pid;
			break;
		}
	free(p);
	return pid;
}

static void tf_allow(uint8_t *tf, unsigned cls, int sub)
{
	if (sub < 0) {
		memset(tf + cls * 32, 0xff, 32);
	} else {
		unsigned csc = cls << 8 | (unsigned)sub;
		tf[csc / 8] |= (uint8_t)(1u << (csc % 8));
	}
}

static kd_buf *ev;
static uint64_t nev, capev, maxev, dropped;
static kd_buf *chunk;
static size_t chunk_n;

/* one KERN_KDREADTR: everything the kernel has, into ev[] */
static int drain(void)
{
	for (int round = 0; round < 64; round++) {
		size_t len = chunk_n * sizeof(kd_buf);
		if (kd(KERN_KDREADTR, 0, chunk, &len) < 0) {
			perror("KERN_KDREADTR");
			return -1;
		}
		size_t got = len;       /* the kernel returns the number of events */
		if (got == 0)
			return 0;
		uint64_t keep = got;
		if (nev + keep > maxev) {
			keep = maxev > nev ? maxev - nev : 0;
			dropped += got - keep;
		}
		if (nev + keep > capev) {
			uint64_t c = capev ? capev : 262144;
			while (c < nev + keep)
				c *= 2;
			if (c > maxev)
				c = maxev;
			kd_buf *n = realloc(ev, c * sizeof(kd_buf));
			if (n == NULL) {
				dropped += keep;
				keep = 0;
			} else {
				ev = n;
				capev = c;
			}
		}
		memcpy(ev + nev, chunk, keep * sizeof(kd_buf));
		nev += keep;
		if (got < chunk_n)
			return 0;
	}
	return 0;
}

static void sleep_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR && !stop) {
	}
}

/* ---- -s: rotating output (one kdebug session, a file every SECS) ---- */
#define SWT_VERSION     "sweettrace 2 (features: rotate -s SECS, SIGUSR1 rotates now)"
struct qitem {
	struct qitem *next;
	int kind;                       /* 0 events, 1 end of this file (rotate), 2 end of the last file */
	kd_buf *ev;
	uint64_t n;
	uint64_t end_abs;               /* kind 1, 2 */
	uint32_t kd_flags, reads, dropped;
	struct swt_thread *thr;
	uint32_t nthr;
};
static struct qitem *qhead, *qtail;
static uint64_t qbytes, qmax;
static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcv = PTHREAD_COND_INITIALIZER;
static volatile sig_atomic_t rotate_now, write_failed;
static void on_usr1(int s) { (void)s; rotate_now = 1; }

static struct {
	const char *pat;
	unsigned idx, secs;
	char path[1024], part[1040];
	FILE *f;
	uint64_t start_abs, n;
	struct swt_header h;            /* template: the fields every file shares */
	uint64_t t0, wall0_ns;
} rf;

static uint64_t abs_ns(uint64_t a) { return a * rf.h.tb_numer / rf.h.tb_denom; }
static uint64_t wall_of(uint64_t a) { return rf.wall0_ns + (a >= rf.t0 ? abs_ns(a - rf.t0) : 0); }

static int rot_open(uint64_t start_abs)
{
	rf.idx++;
	if (strchr(rf.pat, '%'))
		snprintf(rf.path, sizeof(rf.path), rf.pat, rf.idx);
	else {
		size_t l = strlen(rf.pat);
		int sfx = l > 5 && strcmp(rf.pat + l - 5, ".swtr") == 0;
		snprintf(rf.path, sizeof(rf.path), "%.*s.%03u.swtr", (int)(sfx ? l - 5 : l), rf.pat, rf.idx);
	}
	snprintf(rf.part, sizeof(rf.part), "%s.part", rf.path);
	rf.f = fopen(rf.part, "wb");
	if (rf.f == NULL) {
		printf("CHUNK %u FAILED %s: %s\n", rf.idx, rf.part, strerror(errno));
		return -1;
	}
	setvbuf(rf.f, NULL, _IOFBF, 4 << 20);
	struct swt_header z;
	memset(&z, 0, sizeof(z));       /* placeholder (no magic until complete) */
	if (fwrite(&z, sizeof(z), 1, rf.f) != 1)
		return -1;
	rf.start_abs = start_abs;
	rf.n = 0;
	return 0;
}

static void rot_start_line(void)
{
	uint64_t w = wall_of(rf.start_abs);
	printf("CHUNK %u START %llu.%09llu pid %d tries 0 secs %u rotate\n", rf.idx, (unsigned long long)(w / 1000000000ull),
	    (unsigned long long)(w % 1000000000ull), (int)getpid(), rf.secs);
}

static int rot_close(const struct qitem *it)
{
	struct swt_header h = rf.h;
	h.start_abs = rf.start_abs;
	h.end_abs = it->end_abs;
	h.n_events = rf.n;
	h.n_threads = it->nthr;
	h.kd_flags = it->kd_flags;
	h.reads = it->reads;
	h.dropped = it->dropped;
	h.start_wall_ns = wall_of(rf.start_abs);
	int bad = (it->nthr && fwrite(it->thr, sizeof(*it->thr), it->nthr, rf.f) != it->nthr) || fflush(rf.f) != 0 ||
	    fseek(rf.f, 0, SEEK_SET) != 0 || fwrite(&h, sizeof(h), 1, rf.f) != 1;
	if (fclose(rf.f) != 0)
		bad = 1;
	rf.f = NULL;
	if (bad || rename(rf.part, rf.path) != 0) {
		printf("CHUNK %u FAILED writing %s: %s\n", rf.idx, rf.part, strerror(errno));
		return -1;
	}
	struct stat st;
	long long sz = stat(rf.path, &st) == 0 ? (long long)st.st_size : -1;
	double s = (double)abs_ns(h.end_abs - h.start_abs) / 1e9;
	printf("CHUNK %u DONE %lld sweettrace: %llu events in %.2f s (%.0f/s), %u threads, %u reads%s%s -> %s\n", rf.idx, sz,
	    (unsigned long long)h.n_events, s, s > 0 ? h.n_events / s : 0, h.n_threads, h.reads,
	    (h.kd_flags & KDBG_WRAPPED) ? ", KERNEL BUFFER WRAPPED (events lost)" : "",
	    h.dropped ? ", writer behind (events dropped)" : "", rf.path);
	return 0;
}

static void q_push(struct qitem *it)
{
	pthread_mutex_lock(&qmu);
	it->next = NULL;
	if (qtail)
		qtail->next = it;
	else
		qhead = it;
	qtail = it;
	qbytes += it->n * sizeof(kd_buf);
	pthread_cond_signal(&qcv);
	pthread_mutex_unlock(&qmu);
}

/* the writer: streams event blocks into the current file, closes/opens files at the markers */
static void *writer(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&qmu);
		while (qhead == NULL)
			pthread_cond_wait(&qcv, &qmu);
		struct qitem *it = qhead;
		qhead = it->next;
		if (qhead == NULL)
			qtail = NULL;
		qbytes -= it->n * sizeof(kd_buf);
		pthread_mutex_unlock(&qmu);
		int kind = it->kind;
		if (kind == 0) {
			if (rf.f != NULL && it->n && fwrite(it->ev, sizeof(kd_buf), it->n, rf.f) != it->n) {
				printf("CHUNK %u FAILED writing %s: %s\n", rf.idx, rf.part, strerror(errno));
				fclose(rf.f);
				rf.f = NULL;
				write_failed = 1;
			}
			if (rf.f != NULL)
				rf.n += it->n;
		} else if (rf.f != NULL) {
			if (rot_close(it) < 0)
				write_failed = 1;
			else if (kind == 1) {
				if (rot_open(it->end_abs) < 0)
					write_failed = 1;
				else
					rot_start_line();
			}
		}
		free(it->ev);
		free(it->thr);
		free(it);
		if (kind == 2)
			return NULL;
	}
}

/* hand ev[0..nev) to the writer (events with timestamp >= split go after a kind-1 marker when split != 0) */
static uint64_t seg_dropped;
static void q_events(kd_buf *e, uint64_t n)
{
	if (n == 0) {
		free(e);
		return;
	}
	pthread_mutex_lock(&qmu);
	int full = qbytes + n * sizeof(kd_buf) > qmax;
	pthread_mutex_unlock(&qmu);
	if (full) {             /* the writer is too far behind: keep memory bounded, count the loss */
		seg_dropped += n;
		free(e);
		return;
	}
	struct qitem *it = calloc(1, sizeof(*it));
	if (it == NULL) {
		seg_dropped += n;
		free(e);
		return;
	}
	it->ev = e;
	it->n = n;
	q_push(it);
}

static void q_marker(int kind, uint64_t end_abs, uint32_t reads)
{
	kbufinfo_t bi;
	struct qitem *it = calloc(1, sizeof(*it));
	if (it == NULL) {
		perror("calloc");
		exit(1);
	}
	it->kind = kind;
	it->end_abs = end_abs;
	it->reads = reads;
	it->dropped = (uint32_t)seg_dropped;
	seg_dropped = 0;
	if (kd_info(&bi) == 0)
		it->kd_flags = bi.flags;
	if (nthr) {
		it->thr = malloc(nthr * sizeof(*thr));
		if (it->thr != NULL) {
			memcpy(it->thr, thr, nthr * sizeof(*thr));
			it->nthr = nthr;
		}
	}
	q_push(it);
}

static void read_cur_thrmap(void)
{
	size_t ml = 32768 * sizeof(kd_threadmap);       /* no size query: room for 32768 threads */
	kd_threadmap *m = malloc(ml);
	if (m != NULL && kd(KERN_KDREADCURTHRMAP, 0, m, &ml) == 0)
		map_kdebug(m, ml / sizeof(kd_threadmap));
	free(m);
}

/*
 * everything drained so far -> the writer. rotate_at > 0: start a new file at that time stamp. The split is finished
 * one read later: events stamped before rotate_at can still come in the next read (written on another CPU while
 * this read ran), so events >= rotate_at are held until then, and the next read's events < rotate_at still go to
 * the old file. Returns 1 when a file was closed (its marker queued).
 */
static kd_buf *held;
static uint64_t nheld, pend;
static void split_ev(kd_buf *e, uint64_t n, uint64_t at, uint64_t *na, kd_buf **b, uint64_t *nb)
{
	*b = n ? malloc(n * sizeof(kd_buf)) : NULL;
	uint64_t a = 0, k = 0;
	for (uint64_t i = 0; i < n; i++) {
		if (e[i].timestamp < at || *b == NULL)
			e[a++] = e[i];
		else
			(*b)[k++] = e[i];
	}
	*na = a;
	*nb = k;
}
static int q_drained(uint64_t rotate_at, uint32_t reads)
{
	kd_buf *e = ev, *b;
	uint64_t n = nev, a, nb;
	ev = NULL;
	nev = capev = 0;
	if (pend) {
		split_ev(e, n, pend, &a, &b, &nb);
		q_events(e, a);
		read_cur_thrmap();
		q_marker(1, pend, reads);
		q_events(held, nheld);
		held = NULL;
		nheld = 0;
		q_events(b, nb);
		pend = 0;
		return 1;
	}
	if (rotate_at) {
		split_ev(e, n, rotate_at, &a, &b, &nb);
		q_events(e, a);
		held = b;
		nheld = nb;
		pend = rotate_at;
		return 0;
	}
	q_events(e, n);
	return 0;
}

/* -s: the trace loop of the rotating mode (kdebug is set up and enabled; t0 = enable time) */
static int rotate_main(int argc, char **argv, uint64_t t0, struct timespec wall, mach_timebase_info_data_t tb,
    uint64_t dur, unsigned kbufs, unsigned read_ms, int nonames)
{
	struct swt_header *h = &rf.h;
	memcpy(h->magic, SWT_MAGIC, 8);
	h->version = 1;
	h->header_size = sizeof(*h);
	h->tb_numer = tb.numer;
	h->tb_denom = tb.denom;
	{
		int nc = 0;
		size_t l = sizeof(nc);
		sysctlbyname("hw.ncpu", &nc, &l, NULL, 0);
		h->ncpu = (uint32_t)nc;
	}
	h->kbufs = kbufs;
	h->tracer_pid = getpid();
	h->read_ms = read_ms;
	{
		size_t w = 0;
		for (int i = 1; i < argc && w < sizeof(h->args) - 1; i++)
			w += (size_t)snprintf(h->args + w, sizeof(h->args) - w, "%s%s", i > 1 ? " " : "", argv[i]);
	}
	rf.t0 = t0;
	rf.wall0_ns = (uint64_t)wall.tv_sec * 1000000000ull + (uint64_t)wall.tv_nsec;
	rf.start_abs = t0;
	fprintf(stderr, "sweettrace: %.0f s, a new file every %u s, kernel buffer %u events, reading every %u ms, pid %d -> %s\n",
	    (double)dur * tb.numer / tb.denom / 1e9, rf.secs, kbufs, read_ms, (int)getpid(), rf.pat);
	rot_start_line();
	pthread_t wt;
	if (pthread_create(&wt, NULL, writer, NULL) != 0) {
		perror("pthread_create");
		kd(KERN_KDENABLE, 0, NULL, NULL);
		kd(KERN_KDREMOVE, 0, NULL, NULL);
		return 1;
	}
	uint64_t rdur = (uint64_t)rf.secs * 1000000000ull * tb.denom / tb.numer, seg = t0;
	uint32_t reads = 0;
	int rc = 0;
	while (!stop && !write_failed && mach_absolute_time() - t0 < dur) {
		sleep_ms(read_ms);
		uint64_t now = mach_absolute_time();
		int r = rotate_now || now - seg >= rdur;
		if (r && (now - t0 >= dur || dur - (now - t0) < rdur / 10))
			r = 0;          /* the end is near (< 10% of a file): no file of a few hundred ms */
		if (drain() < 0) {
			rc = 1;
			break;
		}
		reads++;
		if (r && !pend) {
			rotate_now = 0;
			seg = now;
		} else
			r = 0;
		if (q_drained(r ? now : 0, reads))
			reads = 0;
	}
	kd(KERN_KDENABLE, 0, NULL, NULL);
	uint64_t t1 = mach_absolute_time();
	drain();
	if (q_drained(0, reads))
		reads = 0;
	if (held != NULL || nheld) {    /* events of the file after a split pending at the end: the last file */
		q_events(held, nheld);
		held = NULL;
		nheld = 0;
	}
	read_cur_thrmap();
	kd(KERN_KDREMOVE, 0, NULL, NULL);
	if (!nonames)
		map_names();
	q_marker(2, t1, reads);
	pthread_join(wt, NULL);
	return rc || write_failed;
}

int main(int argc, char **argv)
{
	unsigned secs = 10, read_ms = 100, delay = 0;
	unsigned kbufs = 1u << 20;
	const char *out = "/var/tmp/sweettrace.swtr", *pids = NULL, *extra = NULL;
	int sysc = 0, nointr = 0, nonames = 0, reset = 0, o;
	unsigned rot = 0;
	maxev = 1u << 22;
	while ((o = getopt(argc, argv, "t:o:b:m:r:d:p:c:SInRs:V")) != -1) {
		switch (o) {
		case 't': secs = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'o': out = optarg; break;
		case 'b': kbufs = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'm': maxev = strtoull(optarg, NULL, 0); break;
		case 'r': read_ms = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'd': delay = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'p': pids = optarg; break;
		case 'c': extra = optarg; break;
		case 'S': sysc = 1; break;
		case 'I': nointr = 1; break;
		case 'n': nonames = 1; break;
		case 'R': reset = 1; break;
		case 's': rot = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'V': printf("%s\n", SWT_VERSION); return 0;
		default:
			fprintf(stderr, "usage: sweettrace [-t SECS] [-o FILE] [-b KBUFS] [-m MAX] [-r MS] [-d DELAY] [-p PIDS] "
			    "[-c CSC] [-S] [-I] [-n] [-s SECS] [-R] [-V]\n");
			return 2;
		}
	}
	if (sizeof(kd_buf) != 64) {
		fprintf(stderr, "kd_buf is %zu bytes, not 64\n", sizeof(kd_buf));
		return 1;
	}
	if (reset) {
		if (kd(KERN_KDREMOVE, 0, NULL, NULL) < 0)
			return perror("KERN_KDREMOVE"), 1;
		printf("kdebug reset\n");
		return 0;
	}
	FILE *f = NULL;
	if (rot) {
		if (secs < 1) {
			fprintf(stderr, "-s: -t must be at least 1 s\n");
			return 2;
		}
		setvbuf(stdout, NULL, _IOLBF, 0);
		if (read_ms == 0)
			read_ms = 100;  /* rotation needs the periodic reads */
		rf.pat = out;
		rf.secs = rot;
		qmax = maxev * sizeof(kd_buf); /* -m: the most the writer may lag behind */
		if (rot_open(0) < 0)
			return perror(rf.part), 1;
	} else {
		f = fopen(out, "wb");
		if (f == NULL)
			return perror(out), 1;
	}
	if (delay)
		sleep(delay);
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGHUP, on_sig);
	if (rot)
		signal(SIGUSR1, on_usr1);

	if (kd(KERN_KDREMOVE, 0, NULL, NULL) < 0) {
		fprintf(stderr, "KERN_KDREMOVE: %s (root needed; EBUSY: someone else traces, sweettrace -R)\n", strerror(errno));
		return 1;
	}
	kbufinfo_t bi;
	for (;;) {
		if (kd(KERN_KDSETBUF, (int)kbufs, NULL, NULL) < 0)
			return perror("KERN_KDSETBUF"), 1;
		if (kd(KERN_KDSETUP, 0, NULL, NULL) == 0)
			break;
		if (kbufs <= 65536)
			return perror("KERN_KDSETUP"), 1;
		kbufs /= 2;
	}
	if (kd_info(&bi) == 0 && bi.nkdbufs > 0)
		kbufs = (unsigned)bi.nkdbufs;

	static uint8_t tf[TYPEFILTER_SIZE];
	tf_allow(tf, DBG_TRACE, -1);
	tf_allow(tf, DBG_MACH, DBG_MACH_SCHED);
	tf_allow(tf, DBG_MACH, DBG_MACH_IPC);
	if (!nointr)
		tf_allow(tf, DBG_MACH, DBG_MACH_EXCP_INTR);
	tf_allow(tf, DBG_DRIVERS, SWEET_SUBCLASS);
	tf_allow(tf, DBG_APPS, HAKIOS_METAL_SUBCLASS);
	if (sysc) {
		tf_allow(tf, DBG_MACH, DBG_MACH_EXCP_SC);
		tf_allow(tf, DBG_BSD, DBG_BSD_EXCP_SC);
	}
	if (extra != NULL) {
		char *s = strdup(extra), *save = NULL;
		for (char *t = strtok_r(s, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
			char *sl = strchr(t, '/');
			unsigned c = (unsigned)strtoul(t, NULL, 0);
			int sub = sl ? (int)strtoul(sl + 1, NULL, 0) : -1;
			if (c < 256 && sub < 256)
				tf_allow(tf, c, sub);
		}
		free(s);
	}
	size_t tl = sizeof(tf);         /* the kernel copies the filter in from oldp, oldlen = its size */
	if (kd(KERN_KDSET_TYPEFILTER, 0, tf, &tl) < 0) {
		perror("KERN_KDSET_TYPEFILTER");
		kd(KERN_KDREMOVE, 0, NULL, NULL);
		return 1;
	}
	if (pids != NULL) {
		char *s = strdup(pids), *save = NULL;
		for (char *t = strtok_r(s, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
			int pid = pid_of(t);
			kd_regtype r = { KDBG_TYPENONE, (unsigned)pid, 1, 0, 0 };
			size_t rl = sizeof(r);
			if (pid < 0 || kd(KERN_KDPIDTR, 0, &r, &rl) < 0)
				fprintf(stderr, "pid filter %s: %s\n", t, pid < 0 ? "no such process" : strerror(errno));
			else
				printf("pid filter: %s = %d\n", t, pid);
		}
		free(s);
	}
	if (read_ms == 0)
		kd(KERN_KDEFLAGS, KDBG_NOWRAP, NULL, NULL);

	chunk_n = kbufs < 262144 ? kbufs : 262144;
	chunk = malloc(chunk_n * sizeof(kd_buf));
	if (chunk == NULL) {
		kd(KERN_KDREMOVE, 0, NULL, NULL);
		return perror("malloc"), 1;
	}
	if (!nonames)
		map_names();    /* before: threads that exit during the trace keep their names */

	struct timespec wall;
	clock_gettime(CLOCK_REALTIME, &wall);
	uint64_t t0 = mach_absolute_time();
	if (kd(KERN_KDENABLE, KDEBUG_ENABLE_TRACE, NULL, NULL) < 0) {
		perror("KERN_KDENABLE");
		kd(KERN_KDREMOVE, 0, NULL, NULL);
		return 1;
	}
	/* the map of every thread, made at enable (read once) */
	if (kd_info(&bi) == 0 && bi.nkdthreads > 0) {
		size_t ml = (size_t)bi.nkdthreads * sizeof(kd_threadmap);
		kd_threadmap *m = malloc(ml);
		if (m != NULL && kd(KERN_KDTHRMAP, 0, m, &ml) == 0)
			map_kdebug(m, ml / sizeof(kd_threadmap));
		free(m);
	}
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	uint64_t dur = (uint64_t)secs * 1000000000ull * tb.denom / tb.numer;
	uint32_t reads = 0;
	if (rot)
		return rotate_main(argc, argv, t0, wall, tb, dur, kbufs, read_ms, nonames);
	printf("sweettrace: %u s, kernel buffer %u events, reading every %u ms, pid %d -> %s\n", secs, kbufs, read_ms,
	    getpid(), out);
	fflush(stdout);
	while (!stop && mach_absolute_time() - t0 < dur) {
		if (read_ms == 0) {
			sleep_ms(100);
			continue;
		}
		sleep_ms(read_ms);
		if (drain() < 0)
			break;
		reads++;
	}
	kd(KERN_KDENABLE, 0, NULL, NULL);
	uint64_t t1 = mach_absolute_time();
	drain();
	kd_info(&bi);
	{
		/* threads that came during the trace */
		size_t ml = 32768 * sizeof(kd_threadmap);       /* no size query: room for 32768 threads */
		kd_threadmap *m = malloc(ml);
		if (m != NULL && kd(KERN_KDREADCURTHRMAP, 0, m, &ml) == 0)
			map_kdebug(m, ml / sizeof(kd_threadmap));
		free(m);
	}
	kd(KERN_KDREMOVE, 0, NULL, NULL);
	if (!nonames)
		map_names();

	struct swt_header h;
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, SWT_MAGIC, 8);
	h.version = 1;
	h.header_size = sizeof(h);
	h.tb_numer = tb.numer;
	h.tb_denom = tb.denom;
	h.start_abs = t0;
	h.end_abs = t1;
	h.n_events = nev;
	h.n_threads = nthr;
	{
		int nc = 0;
		size_t l = sizeof(nc);
		sysctlbyname("hw.ncpu", &nc, &l, NULL, 0);
		h.ncpu = (uint32_t)nc;
	}
	h.kd_flags = bi.flags;
	h.kbufs = kbufs;
	h.reads = reads;
	h.dropped = (uint32_t)dropped;
	h.tracer_pid = getpid();
	h.read_ms = read_ms;
	h.start_wall_ns = (uint64_t)wall.tv_sec * 1000000000ull + (uint64_t)wall.tv_nsec;
	{
		size_t w = 0;
		for (int i = 1; i < argc && w < sizeof(h.args) - 1; i++)
			w += (size_t)snprintf(h.args + w, sizeof(h.args) - w, "%s%s", i > 1 ? " " : "", argv[i]);
	}
	if (fwrite(&h, sizeof(h), 1, f) != 1 || (nev && fwrite(ev, sizeof(kd_buf), nev, f) != nev) ||
	    (nthr && fwrite(thr, sizeof(*thr), nthr, f) != nthr) || fclose(f) != 0) {
		perror(out);
		return 1;
	}
	double s = (double)(t1 - t0) * tb.numer / tb.denom / 1e9;
	printf("sweettrace: %llu events in %.2f s (%.0f/s), %u threads, %u reads%s%s -> %s\n", (unsigned long long)nev, s,
	    s > 0 ? nev / s : 0, nthr, reads, (bi.flags & KDBG_WRAPPED) ? ", KERNEL BUFFER WRAPPED (events lost)" : "",
	    dropped ? ", -m reached (events dropped)" : "", out);
	return 0;
}
