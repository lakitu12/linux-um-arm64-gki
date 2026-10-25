// SPDX-License-Identifier: GPL-2.0
/*
 * threadscale -- does CPU-bound work scale across threads of ONE process?
 *
 * Two questions, and they need different evidence:
 *
 *  1. wall time: N threads doing the same fixed work each, against one thread
 *     doing it once.  ~1.0 means they took turns; ~N means one per CPU.
 *  2. overlap: every thread records its own [start, end] on CLOCK_MONOTONIC.
 *     If the intervals intersect, the threads were really in guest user mode
 *     at the same time -- wall time alone can be fooled by a run that was
 *     simply faster for another reason.
 *
 * Plus the per-CPU busy-jiffy deltas around the parallel phase, so "the work
 * happened on several CPUs" is visible where the scheduler accounts for it.
 *
 * Build (static, has to run on a glibc image):
 *   $NDK/.../aarch64-linux-android30-clang -static -O2 -pthread \
 *       -o threadscale threadscale.c
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static unsigned long iters = 200000000UL;
static volatile int go;

struct worker {
	pthread_t tid;
	long index;
	uint64_t t_start;
	uint64_t t_end;
	uint64_t cpu_ns;	/* CLOCK_THREAD_CPUTIME_ID -- see the note below */
	long host_tid;
};

static void *spin(void *arg)
{
	struct worker *w = arg;
	unsigned long i, acc = (unsigned long)w->index + 1;

	while (!go)
		__asm__ volatile("" ::: "memory");

	w->host_tid = (long)syscall(SYS_gettid);
	{
		struct timespec ts;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		w->t_start = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	}
	{
		struct timespec ts;

		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
		w->cpu_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	}
	for (i = 0; i < iters; i++)
		__asm__ volatile("" : "+r"(acc) :: "memory");
	{
		struct timespec ts;

		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
		w->cpu_ns = ((uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec) - w->cpu_ns;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		w->t_end = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	}

	return (void *)acc;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static uint64_t cpu_busy_sum(void)
{
	FILE *f = fopen("/proc/stat", "r");
	char line[256];
	unsigned long long busy, total = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		unsigned long long user, nice, sys, idle, iow, irq, sirq;
		int cpu;

		if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu",
			   &cpu, &user, &nice, &sys, &idle, &iow, &irq, &sirq) == 8)
			total += user + nice + sys + irq + sirq;
	}
	fclose(f);
	return total;
}

/* Run nthreads with common start; report wall time and per-thread intervals. */
static uint64_t last_cpu_sum;

static uint64_t run(unsigned long nthreads, int verbose)
{
	struct worker *w;
	uint64_t t0, t1, tmin = ~0ULL, tmax = 0, cpu_sum = 0;
	unsigned long i;

	w = calloc(nthreads, sizeof(*w));
	if (!w) {
		perror("calloc");
		exit(1);
	}

	go = 0;
	for (i = 0; i < nthreads; i++) {
		w[i].index = (long)i;
		if (pthread_create(&w[i].tid, NULL, spin, &w[i])) {
			perror("pthread_create");
			exit(1);
		}
	}

	t0 = now_ns();
	go = 1;
	for (i = 0; i < nthreads; i++)
		pthread_join(w[i].tid, NULL);
	t1 = now_ns();

	for (i = 0; i < nthreads; i++) {
		cpu_sum += w[i].cpu_ns;
		if (w[i].t_start < tmin)
			tmin = w[i].t_start;
		if (w[i].t_end > tmax)
			tmax = w[i].t_end;
		if (verbose)
			printf("THREADSCALE:   thread%lu os_tid=%ld start_off_us=%lld "
			       "dur_us=%lld\n", i, w[i].host_tid,
			       (long long)((w[i].t_start - t0) / 1000),
			       (long long)((w[i].t_end - w[i].t_start) / 1000));
	}
	if (verbose) {
		printf("THREADSCALE:   interval_union_us=%lld wall_us=%lld "
		       "cpu_sum_us=%lld\n",
		       (long long)((tmax - tmin) / 1000), (long long)((t1 - t0) / 1000),
		       (long long)(cpu_sum / 1000));
		/*
		 * cpu_sum is printed for completeness, NOT as a metric: under
		 * UML a guest CLOCK_THREAD_CPUTIME_ID measures the same thing
		 * as the thread's wall clock (measured: cpu_ns == t_end -
		 * t_start for every thread), so it cannot be used to tell
		 * "one CPU each" from "took turns".  The wall-clock speedup is
		 * the honest number here; it is only stable because the work
		 * per thread (200M iterations, ~83ms) dwarfs the ~30ms it can
		 * take a freshly created thread to start.
		 */
	}

	free(w);
	return t1 - t0;
}

int main(int argc, char **argv)
{
	unsigned long nthreads = 4;
	uint64_t serial_ns, par_ns, busy0, busy1;
	double speedup;

	if (argc > 1)
		nthreads = strtoul(argv[1], NULL, 0);
	if (argc > 2)
		iters = strtoul(argv[2], NULL, 0);
	if (!nthreads)
		nthreads = 1;

	printf("THREADSCALE: start pid=%ld threads=%lu iters=%lu\n",
	       (long)syscall(SYS_gettid), nthreads, iters);
	printf("THREADSCALE: nproc=%ld\n", sysconf(_SC_NPROCESSORS_ONLN));

	serial_ns = run(1, 0);
	printf("THREADSCALE: serial_1x_ms=%llu\n",
	       (unsigned long long)(serial_ns / 1000000));

	busy0 = cpu_busy_sum();
	par_ns = run(nthreads, 1);
	busy1 = cpu_busy_sum();
	printf("THREADSCALE: parallel_%lux_ms=%llu\n", nthreads,
	       (unsigned long long)(par_ns / 1000000));
	printf("THREADSCALE: cpu_busy_jiffies_delta=%llu\n",
	       (unsigned long long)(busy1 - busy0));

	speedup = par_ns ? (double)serial_ns * (double)nthreads / (double)par_ns : 0.0;
	printf("THREADSCALE: speedup_%lux=%.2f\n", nthreads, speedup);
	printf("THREADSCALE: done\n");
	return 0;
}
