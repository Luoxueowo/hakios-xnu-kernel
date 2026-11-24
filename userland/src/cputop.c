/*
 * Which processes take the processors: every few seconds, the processor time each process was given since the
 * last look (user and system together), the most first, as one line for the run's log:
 *
 *   cputop: 25 s: 412% in all: backboardd[13] 96% SpringBoard[41] 88% WeatherWidget[131] 40% ...
 *
 * (100% is one core's whole time.) tools/frames.py says when the cores were all busy ("cores busy") while the
 * window server's frames came late; this says with whom it shared them. A process born since the last look counts
 * with all the time it has had.
 *
 *   usage: cputop [SECONDS [LOOKS]]     every SECONDS (default 5), LOOKS times (default: until the run ends)
 */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_info.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* libproc's, which the iPhoneOS SDK does not declare (XNU's bsd/sys/proc_info.h, libsyscall/wrappers/libproc) */
#define PROC_ALL_PIDS           1
#define PROC_PIDTASKINFO        4
struct proc_taskinfo {
	uint64_t pti_virtual_size;
	uint64_t pti_resident_size;
	uint64_t pti_total_user;        /* in mach_absolute_time's units */
	uint64_t pti_total_system;
	uint64_t pti_threads_user;
	uint64_t pti_threads_system;
	int32_t pti_policy;
	int32_t pti_faults;
	int32_t pti_pageins;
	int32_t pti_cow_faults;
	int32_t pti_messages_sent;
	int32_t pti_messages_received;
	int32_t pti_syscalls_mach;
	int32_t pti_syscalls_unix;
	int32_t pti_csw;
	int32_t pti_threadnum;
	int32_t pti_numrunning;
	int32_t pti_priority;
};
int proc_listpids(uint32_t type, uint32_t typeinfo, void *buffer, int buffersize);
int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
int proc_name(int pid, void *buffer, uint32_t buffersize);
#define PROC_PIDTHREADINFO      5
#define PROC_PIDLISTTHREADS     6
struct proc_threadinfo {
	uint64_t        pth_user_time;          /* run time, nanoseconds */
	uint64_t        pth_system_time;
	int32_t         pth_cpu_usage;
	int32_t         pth_policy;
	int32_t         pth_run_state;
	int32_t         pth_flags;
	int32_t         pth_sleep_time;
	int32_t         pth_curpri;
	int32_t         pth_priority;
	int32_t         pth_maxpriority;
	char            pth_name[64];
};

#define MAX_PROCS       2048
#define SHOWN           10

struct sample {
	int pid;
	uint64_t time;                  /* user + system, mach_absolute_time's units */
};

static struct sample last[MAX_PROCS], now[MAX_PROCS];
static int last_count;

struct share {
	int pid;
	uint64_t time;
};

static int
by_time(const void *a, const void *b)
{
	const struct share *x = a, *y = b;

	return x->time < y->time ? 1 : x->time > y->time ? -1 : 0;
}

static uint64_t
time_before(int pid, int *found)
{
	for (int i = 0; i < last_count; i++) {
		if (last[i].pid == pid) {
			*found = 1;
			return last[i].time;
		}
	}
	*found = 0;
	return 0;
}

/*
 * The threads of one process, by the processor time each was given since the look before -- for a process whose
 * share is spread over several cores, which threads they are (CoreAnimation renders in more than one).
 */
#define MAX_THREADS     256
struct thread_share {
	uint64_t tid;
	uint64_t time;                  /* microseconds, user + system */
};
static struct thread_share thread_last[MAX_THREADS], thread_now[MAX_THREADS];
static uint64_t thread_delta[MAX_THREADS];      /* (the difference, kept apart: thread_last holds the totals) */
static int thread_last_count;
static int thread_last_pid;

static int
threads_of(int pid, struct thread_share *out, int max)
{
	uint64_t tids[MAX_THREADS];
	int bytes, n = 0;

	/* (libproc's, not a task port: the daemon this runs as is not allowed one) */
	bytes = proc_pidinfo(pid, PROC_PIDLISTTHREADS, 0, tids, sizeof(tids));
	if (bytes <= 0) {
		return 0;
	}
	for (int i = 0; i < bytes / (int) sizeof(uint64_t) && n < max; i++) {
		struct proc_threadinfo info;

		if (proc_pidinfo(pid, PROC_PIDTHREADINFO, tids[i], &info, sizeof(info)) != sizeof(info)) {
			continue;
		}
		out[n].tid = tids[i];
		out[n].time = info.pth_user_time + info.pth_system_time;       /* nanoseconds */
		n++;
	}
	return n;
}

static void
say_threads(int pid, const char * name, double span, mach_timebase_info_data_t timebase)
{
	int n = threads_of(pid, thread_now, MAX_THREADS), shown = 0;
	uint64_t busiest = 0;

	if (thread_last_pid != pid) {
		thread_last_count = 0;
		thread_last_pid = pid;
	}
	if (n == 0) {
		thread_last_count = 0;
		printf("threads: %s[%d]: no task port (not root?)\n", name, pid);
		return;
	}
	/* (sorted by the time since the look before, one pass per thread is enough for a dozen) */
	for (int k = 0; k < n; k++) {
		uint64_t then = thread_now[k].time;

		for (int i = 0; i < thread_last_count; i++) {
			if (thread_last[i].tid == thread_now[k].tid) {
				then = thread_last[i].time;
				break;
			}
		}
		thread_delta[k] = thread_now[k].time > then ? thread_now[k].time - then : 0;
		if (thread_delta[k] > busiest) {
			busiest = thread_delta[k];
		}
	}
	printf("threads: %s[%d]:", name, pid);
	for (int round = 0; round < 8 && shown < 8; round++) {
		int pick = -1;

		for (int k = 0; k < n; k++) {
			if (thread_delta[k] == 0) {
				continue;
			}
			if (pick < 0 || thread_delta[k] > thread_delta[pick]) {
				pick = k;
			}
		}
		if (pick < 0) {
			break;
		}
		double percent = span > 0 ? 100.0 * (double) thread_delta[pick] /
		    ((double) span * (double) timebase.numer / (double) timebase.denom) : 0;
		if (percent < 1) {
			break;
		}
		{
			struct proc_threadinfo info;
			char tname[33] = "";

			if (proc_pidinfo(pid, PROC_PIDTHREADINFO, thread_now[pick].tid, &info, sizeof(info)) == sizeof(info)) {
				memcpy(tname, info.pth_name, sizeof(tname) - 1);
			}
			printf(" %s%.0f%%", tname[0] ? tname : "?", percent);
		}
		thread_delta[pick] = 0;
		shown++;
	}
	printf("\n");
	memcpy(thread_last, thread_now, sizeof(thread_now[0]) * (size_t) n);
	thread_last_count = n;
}

int
main(int argc, char **argv)
{
	unsigned seconds = argc > 1 ? (unsigned)atoi(argv[1]) : 5, looks = argc > 2 ? (unsigned)atoi(argv[2]) : 0;
	static int pids[MAX_PROCS];
	static struct share shares[MAX_PROCS];
	mach_timebase_info_data_t timebase;
	uint64_t start = mach_absolute_time(), before = start;

	if (seconds == 0) {
		seconds = 5;
	}
	mach_timebase_info(&timebase);
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (unsigned look = 0; looks == 0 || look <= looks; look++) {
		int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids)), count = 0, shown = 0;
		uint64_t at = mach_absolute_time(), all = 0;

		for (int i = 0; i < bytes / (int)sizeof(int) && count < MAX_PROCS; i++) {
			struct proc_taskinfo info;

			if (pids[i] <= 0 || proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &info, sizeof(info)) != sizeof(info)) {
				continue;
			}
			now[count].pid = pids[i];
			now[count].time = info.pti_total_user + info.pti_total_system;
			count++;
		}
		if (look > 0) {
			int n = 0;

			for (int i = 0; i < count; i++) {
				int found;
				uint64_t then = time_before(now[i].pid, &found);

				if (now[i].time > then) {
					shares[n].pid = now[i].pid;
					shares[n].time = now[i].time - then;
					all += shares[n].time;
					n++;
				}
			}
			qsort(shares, (size_t)n, sizeof(shares[0]), by_time);
			/* the shares as percent of one core over the time between the looks (both in mach_absolute_time's units) */
			double span = (double)(at - before);
			printf("cputop: %llu s: %.0f%% in all:", (unsigned long long)((at - start) * timebase.numer / timebase.denom / 1000000000ull),
			    span > 0 ? 100.0 * (double)all / span : 0);
			for (int i = 0; i < n && shown < SHOWN; i++) {
				char name[64] = "?";
				double percent = span > 0 ? 100.0 * (double)shares[i].time / span : 0;

				if (percent < 1) {
					break;
				}
				proc_name(shares[i].pid, name, sizeof(name));
				printf(" %s[%d] %.0f%%", name, shares[i].pid, percent);
				shown++;
			}
			printf("\n");
			if (n > 0 && all != 0 && shares[0].time * 4 > all) {
				char name[64] = "?";

				proc_name(shares[0].pid, name, sizeof(name));
				say_threads(shares[0].pid, name, span, timebase);
			} else {
				thread_last_count = 0;
			}
		}
		memcpy(last, now, sizeof(now[0]) * (size_t)count);
		last_count = count;
		before = at;
		if (looks != 0 && look == looks) {
			break;
		}
		sleep(seconds);
	}
	return 0;
}
