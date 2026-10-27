/*
 * SWEET process sampler: where is a daemon stuck?
 *
 * The phone has no debugger and daemons log nowhere readable. Every
 * interval a kernel thread prints, for each process named in boot-arg
 * sweetsample (comma separated, default "SpringBoard,backboardd"; "-" = off)
 * and for each of its threads, the user pc/lr/sp and a frame-pointer
 * backtrace as "SMP name[pid] tN pc ... lr ... bt ..." (header line with the
 * shared region slide and the main binary's mach header, to symbolicate
 * with tools/sweet-crash.py). A thread blocked in a syscall shows the
 * syscall stub in pc and its caller in lr/bt.
 *
 * First sample 90 s after start, then every 120 s, 40 samples at most.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

extern task_t proc_task(proc_t);
extern int hz;
extern void sweet_sample_start(void);

static char sweet_sample_names[128];

static bool
sweet_sample_wanted(const char *name)
{
	size_t n = strlen(name);
	for (const char *t = sweet_sample_names; *t;) {
		const char *end = strchr(t, ',');
		size_t tl = end ? (size_t)(end - t) : strlen(t);
		if (tl == n && n > 0 && strncmp(t, name, n) == 0) {
			return true;
		}
		if (!end) {
			break;
		}
		t = end + 1;
	}
	return false;
}

extern void sweet_sample_task(const char *name, int pid, task_t task);  /* osfmk/kern/exception.c */

static int
sweet_sample_proc(proc_t p, __unused void *arg)
{
	char name[32];
	proc_name(proc_pid(p), name, sizeof(name));
	if (sweet_sample_wanted(name)) {
		task_t task = proc_task(p);
		if (task != TASK_NULL) {
			sweet_sample_task(name, proc_pid(p), task);
		}
	}
	return PROC_RETURNED;
}

static void
sweet_sample_thread(__unused void *arg, __unused wait_result_t wr)
{
	static int chan;
	if (!PE_parse_boot_argn("sweetsample", sweet_sample_names, sizeof(sweet_sample_names))) {
		strlcpy(sweet_sample_names, "SpringBoard,backboardd", sizeof(sweet_sample_names));
	}
	if (sweet_sample_names[0] == '-') {
		return;
	}
	(void)tsleep(&chan, PZERO, "sweetsmp", 90 * hz);
	for (int i = 0; i < 40; i++) {
		printf("SMP --- sample %d\n", i);
		proc_iterate(PROC_ALLPROCLIST, sweet_sample_proc, NULL, NULL, NULL);
		(void)tsleep(&chan, PZERO, "sweetsmp", 120 * hz);
	}
}

/*
 * 0991: the task of a process by pid (pid >= 0) or by name (the first match), with a task reference, for the
 * profiler (osfmk/arm64/sweet_prof.c).
 */
task_t sweet_prof_find_task(const char *name, size_t len, int pid, int *pid_out, char *name_out, size_t name_len);

struct sweet_prof_find {
	const char *name;
	size_t      len;
	int         pid;
};

static int
sweet_prof_find_proc(proc_t p, void *arg)
{
	struct sweet_prof_find *f = arg;
	char name[32];
	if (f->pid >= 0) {
		return PROC_RETURNED;
	}
	proc_name(proc_pid(p), name, sizeof(name));
	if (strlen(name) == f->len && strncmp(name, f->name, f->len) == 0) {
		f->pid = proc_pid(p);
		return PROC_RETURNED_DONE;
	}
	return PROC_RETURNED;
}

task_t
sweet_prof_find_task(const char *name, size_t len, int pid, int *pid_out, char *name_out, size_t name_len)
{
	if (pid < 0) {
		struct sweet_prof_find f = { .name = name, .len = len, .pid = -1 };
		proc_iterate(PROC_ALLPROCLIST, sweet_prof_find_proc, &f, NULL, NULL);
		pid = f.pid;
	}
	if (pid < 0) {
		return TASK_NULL;
	}
	proc_t p = proc_find(pid);
	if (p == PROC_NULL) {
		return TASK_NULL;
	}
	task_t task = proc_task(p);
	if (task != TASK_NULL) {
		task_reference(task);
		*pid_out = pid;
		proc_name(pid, name_out, (int)name_len);
	}
	proc_rele(p);
	return task;
}

void
sweet_sample_start(void)
{
	thread_t thread;
	if (kernel_thread_start(sweet_sample_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
