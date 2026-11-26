/*
 * Boot-arg amp=1 statistics: how the asymmetric scheduler (the Edge one, with the A76 cores as the
 * P cluster and the A55 cores as the E cluster) placed the work. Every 5 s, to the loader's log and
 * the kernel's:
 *
 *   amp: at MS P NN% E NN% | FG p/e IN p/e DF p/e UT p/e BG p/e   (utilisation of each cluster; the CPU ms
 *                                                                  each scheduling bucket ran on P / on E)
 *   amp: backboardd tid:p/e tid:p/e tid:p/e                        (its three busiest threads: ms on P / on E)
 *   amp: SpringBoard tid:p/e ...
 *
 * What a thread ran is counted when it is switched away from (sched_prim.c: hakios_amp_account), so a
 * thread that never gives up its core shows up late.
 */
#include <kern/kern_types.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/processor.h>
#include <kern/clock.h>
#include <kern/sched.h>
#include <kern/queue.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <os/atomic_private.h>
#include <kern/misc_protos.h>
#include <string.h>

extern uint32_t hakios_amp_enabled;
extern _Atomic uint64_t hakios_amp_time[2][TH_BUCKET_SCHED_MAX];
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);
extern void loader_log_line(const char *line);
void hakios_amp_start(void);

#define AMP_PERIOD_MS   5000
#define AMP_THREADS     64              /* threads of a process remembered between two lines */

static const char *const bucket_name[TH_BUCKET_SCHED_MAX] = {
	[TH_BUCKET_FIXPRI] = "FIX", [TH_BUCKET_SHARE_FG] = "FG", [TH_BUCKET_SHARE_IN] = "IN",
	[TH_BUCKET_SHARE_DF] = "DF", [TH_BUCKET_SHARE_UT] = "UT", [TH_BUCKET_SHARE_BG] = "BG",
};

static const char *const targets[2] = { "backboardd", "SpringBoard" };

struct snap {
	uint64_t tid;
	uint64_t t[2];
};
static struct snap snaps[2][AMP_THREADS];

static void
amp_say(const char *line)
{
	loader_log_line(line);
	printf("%s", line);
}

static uint64_t
to_ms(uint64_t abs)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(abs, &ns);
	return ns / 1000000;
}

static task_t
find_task(const char *name)
{
	task_t found = TASK_NULL, t;

	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, t, task_t, tasks) {
		const char *n = task_best_name(t);

		if (n != NULL && strncmp(n, name, 16) == 0) {
			task_reference(t);
			found = t;
			break;
		}
	}
	lck_mtx_unlock(&tasks_threads_lock);
	return found;
}

/* the line of a process's busiest threads since the last one */
static void
report_task(int which, uint64_t window)
{
	struct { uint64_t tid, cum[2], d[2], total; } cur[AMP_THREADS];
	unsigned int n = 0;
	task_t task = find_task(targets[which]);
	char line[240];
	int at;

	if (task == TASK_NULL) {
		return;
	}
	task_lock(task);
	thread_t th;
	queue_iterate(&task->threads, th, thread_t, task_threads) {
		if (n >= AMP_THREADS) {
			break;
		}
		cur[n].tid = th->thread_id;
		cur[n].cum[0] = th->hk_cluster_time[0];
		cur[n].cum[1] = th->hk_cluster_time[1];
		n++;
	}
	task_unlock(task);
	task_deallocate(task);

	for (unsigned int i = 0; i < n; i++) {
		const struct snap *s = NULL;

		for (unsigned int j = 0; j < AMP_THREADS; j++) {
			if (snaps[which][j].tid == cur[i].tid) {
				s = &snaps[which][j];
				break;
			}
		}
		cur[i].d[0] = cur[i].cum[0] - (s ? s->t[0] : 0);
		cur[i].d[1] = cur[i].cum[1] - (s ? s->t[1] : 0);
		cur[i].total = cur[i].d[0] + cur[i].d[1];
	}
	bzero(snaps[which], sizeof(snaps[which]));
	for (unsigned int i = 0; i < n; i++) {
		snaps[which][i].tid = cur[i].tid;
		snaps[which][i].t[0] = cur[i].cum[0];
		snaps[which][i].t[1] = cur[i].cum[1];
	}

	at = snprintf(line, sizeof(line), "amp: %s ms tid:P/E in %llu ms:", targets[which], to_ms(window));
	for (int rank = 0; rank < 3; rank++) {
		unsigned int best = n;
		uint64_t best_total = 0;

		for (unsigned int i = 0; i < n; i++) {
			if (cur[i].total > best_total) {
				best_total = cur[i].total;
				best = i;
			}
		}
		if (best == n) {
			break;
		}
		at += snprintf(line + at, sizeof(line) - at, " %llx:%llu/%llu", cur[best].tid & 0xffffff, to_ms(cur[best].d[0]),
		    to_ms(cur[best].d[1]));
		cur[best].total = 0;
	}
	snprintf(line + at, sizeof(line) - at, "\n");
	amp_say(line);
}

static void
amp_thread(void *arg __unused, wait_result_t wr __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t idle_before[MAX_CPUS] = { 0 }, then = mach_absolute_time();
	uint64_t bucket_before[2][TH_BUCKET_SCHED_MAX] = { { 0 } };

	for (;;) {
		char line[300];
		uint64_t now, ns, busy[MAX_CPU_TYPES] = { 0 }, total[MAX_CPU_TYPES] = { 0 };
		int at;

		delay_for_interval(AMP_PERIOD_MS, NSEC_PER_MSEC);
		now = mach_absolute_time();
		for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
			uint64_t idle = 0, b;
			cluster_type_t type = topology->cpus[i].cluster_type;

			if (!ml_cpu_idle_time((int)i, &idle)) {
				continue;
			}
			b = (now - then) > (idle - idle_before[i]) ? (now - then) - (idle - idle_before[i]) : 0;
			idle_before[i] = idle;
			busy[type] += b;
			total[type] += now - then;
		}
		absolutetime_to_nanoseconds(now, &ns);
		at = snprintf(line, sizeof(line), "amp: at %llu P %llu%% E %llu%% |", ns / 1000000,
		    total[CLUSTER_TYPE_P] ? 100 * busy[CLUSTER_TYPE_P] / total[CLUSTER_TYPE_P] : 0,
		    total[CLUSTER_TYPE_E] ? 100 * busy[CLUSTER_TYPE_E] / total[CLUSTER_TYPE_E] : 0);
		for (int b = 0; b < TH_BUCKET_SCHED_MAX; b++) {
			uint64_t p = os_atomic_load(&hakios_amp_time[0][b], relaxed), e = os_atomic_load(&hakios_amp_time[1][b], relaxed);

			at += snprintf(line + at, sizeof(line) - at, " %s %llu/%llu", bucket_name[b], to_ms(p - bucket_before[0][b]),
			    to_ms(e - bucket_before[1][b]));
			bucket_before[0][b] = p;
			bucket_before[1][b] = e;
		}
		snprintf(line + at, sizeof(line) - at, "\n");
		amp_say(line);
		for (int w = 0; w < 2; w++) {
			report_task(w, now - then);
		}
		then = now;
	}
}

/* boot-arg amp=1 (osfmk/arm64/machine_routines.c): the statistics thread */
void
hakios_amp_start(void)
{
	thread_t thread;

	if (!hakios_amp_enabled) {
		return;
	}
	if (kernel_thread_start(amp_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
