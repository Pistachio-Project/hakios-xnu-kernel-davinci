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
#include <kern/policy_internal.h>
#include <kern/thread_group.h>
#include <kern/sched_clutch.h>
#include <mach/task_policy.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <os/atomic_private.h>
#include <kern/misc_protos.h>
#include <arm/cpu_data_internal.h>
#include <arm/machine_routines.h>
#include <mach/arm/thread_status.h>
#include <sys/bsdtask_info.h>
#include <string.h>

extern uint32_t hakios_amp_enabled;
extern _Atomic uint64_t hakios_amp_time[2][TH_BUCKET_SCHED_MAX];
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);
extern void loader_log_line(const char *line);
void hakios_amp_start(void);

#define AMP_PERIOD_MS   5000
#define KT_MAX          512             /* kernel threads whose run time is remembered between two lines */
#define KT_TOP          10
#define KT_BT           6
#define AMP_THREADS     64              /* threads of a process remembered between two lines */

static const char *const bucket_name[TH_BUCKET_SCHED_MAX] = {
	[TH_BUCKET_FIXPRI] = "FIX", [TH_BUCKET_SHARE_FG] = "FG", [TH_BUCKET_SHARE_IN] = "IN",
	[TH_BUCKET_SHARE_DF] = "DF", [TH_BUCKET_SHARE_UT] = "UT", [TH_BUCKET_SHARE_BG] = "BG",
};

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

/*
 * The sampler (boot-arg amp_samp=MS, default 10 (was 2: 500 wakeups a second of the kernel's own); 0: none): every MS it looks at what each processor runs and
 * counts, per process (by its task pointer, resolved to a name only when a line is made), the samples by
 * scheduling bucket on P and on E. The thread it reads is never dereferenced beyond its bucket and task.
 */
#define AMP_TASKS 128
static struct { task_t task; uint16_t n[2][TH_BUCKET_SCHED_MAX]; } samp[AMP_TASKS];
static uint32_t samp_ms = 10;
static uint64_t samp_unit_us;   /* the length of a sample of the last 5 s, in us */

static void
sample_once(void)
{
	thread_t self = current_thread();

	for (int cpu = 0; cpu <= ml_get_max_cpu_number() && cpu < MAX_CPUS; cpu++) {
		processor_t pr = processor_array[cpu];
		thread_t th;
		struct thread_ro *tro;
		task_t task;
		unsigned int e, b, i;

		if (pr == PROCESSOR_NULL || pr->processor_set == PROCESSOR_SET_NULL) {
			continue;
		}
		th = pr->active_thread;
		if (th == THREAD_NULL || th == self || (th->state & TH_IDLE) || (tro = th->t_tro) == NULL ||
		    (task = tro->tro_task) == TASK_NULL) {
			continue;
		}
		b = th->th_sched_bucket;
		if (b >= TH_BUCKET_SCHED_MAX) {
			continue;
		}
		e = pr->processor_set->pset_type == PSET_AMP_E;
		for (i = 0; i < AMP_TASKS && samp[i].task != task; i++) {
			if (samp[i].task == TASK_NULL) {
				samp[i].task = task;
				break;
			}
		}
		if (i < AMP_TASKS) {
			samp[i].n[e][b]++;
		}
	}
}


/*
 * kernel_task by thread: every report the kernel threads' P and E run time since the last (the per-thread
 * hk_cluster_time of the scheduler hook), the ten largest:
 *
 *   amp: ktop N 'name' tid T P ms X E ms Y cont C bt a b c d e f     (slide S in the amp: ktopslide line)
 *
 * name: thread_set_thread_name, if the driver gave one; cont: the continuation of a thread blocked with one; bt:
 * the return addresses of a blocked thread's kernel stack (nm the unstripped kernel on the host: addr - slide).
 * A thread that runs at that moment has neither. Switch: boot-arg amp_ktop=0.
 */
static uint32_t ktop_on = 1;
static struct { uint64_t tid, p, e; } kt_prev[KT_MAX];

struct kt_row {
	uint64_t tid, p, e, cont, bt[KT_BT];
	char name[24];
	uint8_t nbt;
	bool running;
};

static void
kt_stack(thread_t t, struct kt_row *r)
{
	vm_offset_t stk = t->kernel_stack;
	const arm_kernel_saved_state_t *ks;
	uint64_t fp;

	if (stk == 0 || t->machine.kstackptr == NULL) {
		return;
	}
	ks = &((thread_kernel_state_t)t->machine.kstackptr)->machine.ss;
	r->bt[r->nbt++] = ks->lr;
	fp = ks->fp;
	while (r->nbt < KT_BT && fp >= stk && fp + 16 <= stk + kernel_stack_size && (fp & 7) == 0) {
		uint64_t ra = ((uint64_t *)fp)[1], next = ((uint64_t *)fp)[0];

		if (ra != 0) {
			r->bt[r->nbt++] = ra;
		}
		if (next <= fp) {
			break;
		}
		fp = next;
	}
}

static void
report_ktop(void)
{
	static struct { uint64_t tid, p, e; } now_t[KT_MAX];
	struct kt_row top[KT_TOP];
	unsigned int ntop = 0, nnow = 0;
	uint64_t tot_p = 0, tot_e = 0;
	thread_t t;
	char line[300];

	bzero(top, sizeof(top));
	task_lock(kernel_task);
	queue_iterate(&kernel_task->threads, t, thread_t, task_threads) {
		uint64_t tid = t->thread_id, p = t->hk_cluster_time[0], e = t->hk_cluster_time[1], dp = p, de = e;
		unsigned int i, at;
		uint64_t ms;
		spl_t s;

		for (i = 0; i < KT_MAX && kt_prev[i].tid != 0; i++) {
			if (kt_prev[i].tid == tid) {
				dp = p - kt_prev[i].p;
				de = e - kt_prev[i].e;
				break;
			}
		}
		if (nnow < KT_MAX) {
			now_t[nnow].tid = tid;
			now_t[nnow].p = p;
			now_t[nnow].e = e;
			nnow++;
		}
		dp = to_ms(dp);
		de = to_ms(de);
		tot_p += dp;
		tot_e += de;
		ms = dp + de;
		if (ms == 0) {
			continue;
		}
		for (at = 0; at < ntop && top[at].p + top[at].e >= ms; at++) {
		}
		if (at >= KT_TOP) {
			continue;
		}
		if (ntop < KT_TOP) {
			ntop++;
		}
		memmove(&top[at + 1], &top[at], (ntop - 1 - at) * sizeof(top[0]));
		bzero(&top[at], sizeof(top[at]));
		top[at].tid = tid;
		top[at].p = dp;
		top[at].e = de;
		s = splsched();
		thread_lock(t);
		top[at].cont = (uintptr_t)t->continuation;
		if ((t->state & (TH_WAIT | TH_RUN)) == TH_WAIT) {
			kt_stack(t, &top[at]);
		} else {
			top[at].running = true;
		}
		thread_unlock(t);
		splx(s);
		if (get_bsdthread_info(t) != NULL) {
			char nm[MAXTHREADNAMESIZE];

			bzero(nm, sizeof(nm));
			bsd_getthreadname(get_bsdthread_info(t), nm);
			strlcpy(top[at].name, nm, sizeof(top[at].name));
		}
	}
	task_unlock(kernel_task);
	bzero(kt_prev, sizeof(kt_prev));
	memcpy(kt_prev, now_t, nnow * sizeof(now_t[0]));
	snprintf(line, sizeof(line), "amp: ktopslide %#lx kernel_task %u threads P ms %llu E ms %llu\n",
	    (unsigned long)vm_kernel_slide, nnow, (unsigned long long)tot_p, (unsigned long long)tot_e);
	amp_say(line);
	for (unsigned int i = 0; i < ntop; i++) {
		int at = snprintf(line, sizeof(line), "amp: ktop %u '%s' tid %llu P ms %llu E ms %llu cont %#llx%s", i, top[i].name,
		    (unsigned long long)top[i].tid, (unsigned long long)top[i].p, (unsigned long long)top[i].e,
		    (unsigned long long)top[i].cont, top[i].running ? " (running)" : " bt");

		for (unsigned int k = 0; k < top[i].nbt; k++) {
			at += snprintf(line + at, sizeof(line) - at, " %#llx", (unsigned long long)top[i].bt[k]);
		}
		snprintf(line + at, sizeof(line) - at, "\n");
		amp_say(line);
	}
}

/* is this task still there; if so its name, pid and role (the tasks lock held) */
static bool
task_info_locked(task_t want, char *name, size_t len, int *role)
{
	task_t t;

	queue_iterate(&tasks, t, task_t, tasks) {
		if (t == want) {
			const char *n = task_best_name(t);

			strlcpy(name, n ? n : "?", len);
			*role = proc_get_effective_task_policy(t, TASK_POLICY_ROLE);
			return true;
		}
	}
	return false;
}

static void
report_top(uint64_t window)
{
	char line[300];

	for (int rank = 0; rank < 10; rank++) {
		unsigned int best = AMP_TASKS;
		uint32_t best_p = 0;
		char name[20] = "?";
		int role = -1, at;

		for (unsigned int i = 0; i < AMP_TASKS && samp[i].task != TASK_NULL; i++) {
			uint32_t p = 0;

			for (int b = 0; b < TH_BUCKET_SCHED_MAX; b++) {
				p += samp[i].n[0][b];
			}
			if (p > best_p) {
				best_p = p;
				best = i;
			}
		}
		if (best == AMP_TASKS) {
			break;
		}
		lck_mtx_lock(&tasks_threads_lock);
		if (!task_info_locked(samp[best].task, name, sizeof(name), &role)) {
			strlcpy(name, "(gone)", sizeof(name));
		}
		lck_mtx_unlock(&tasks_threads_lock);
		at = snprintf(line, sizeof(line), "amp: top%d %s role %d P ms", rank, name, role);
		for (int w = 0; w < 2; w++) {
			for (int b = 0; b < TH_BUCKET_SCHED_MAX; b++) {
				at += snprintf(line + at, sizeof(line) - at, "%s%llu", b == 0 ? (w ? " | E ms " : " ") : "/", (unsigned long long)samp[best].n[w][b] * samp_unit_us / 1000);
				samp[best].n[w][b] = 0;
			}
		}
		snprintf(line + at, sizeof(line) - at, " (FIX/FG/IN/DF/UT/BG)\n");
		amp_say(line);
	}
	(void)window;
	bzero(samp, sizeof(samp));
}

/*
 * Boot-arg amp_ui=1 (2): the thread groups of processes that are not "the UI" have their DF (default QoS)
 * work prefer the E pset, so that the P cluster is left to UI work (FIX, FG, IN) when it is saturated. The UI
 * is: kernel_task, backboardd, SpringBoard, and (1) a process whose task role is foreground, control or
 * graphics server; with 2 the names and the foreground role only. A thread group of both is UI.
 * Redone every 500 ms (new processes, a new frontmost app). A group the policy once moved is put back by it.
 */
static uint32_t ui_mode;
#define AMP_TGS 192

static bool
task_is_ui(task_t t)
{
	const char *n = task_best_name(t);
	int role = proc_get_effective_task_policy(t, TASK_POLICY_ROLE);

	if (t == kernel_task || (n && (strncmp(n, "backboardd", 16) == 0 || strncmp(n, "SpringBoard", 16) == 0))) {
		return true;
	}
	if (role == TASK_FOREGROUND_APPLICATION) {
		return true;
	}
	return ui_mode == 1 && (role == TASK_CONTROL_APPLICATION || role == TASK_GRAPHICS_SERVER);
}

static uint32_t ui_demoted;     /* thread groups with their DF at E at the last look */

static void
ui_policy_apply(void)
{
	static struct { struct thread_group *tg; bool ui; } tgs[AMP_TGS];
	unsigned int n = 0;
	pset_id_t e_pset = PSET_ID_INVALID, p_pset = PSET_ID_INVALID;
	task_t t;

	for (pset_id_t id = 0; id < sched_num_psets; id++) {
		if (pset_array[id] != PROCESSOR_SET_NULL) {
			if (pset_array[id]->pset_type == PSET_AMP_E && e_pset == PSET_ID_INVALID) {
				e_pset = id;
			} else if (pset_array[id]->pset_type == PSET_AMP_P && p_pset == PSET_ID_INVALID) {
				p_pset = id;
			}
		}
	}
	if (e_pset == PSET_ID_INVALID || p_pset == PSET_ID_INVALID) {
		return;
	}
	bzero(tgs, sizeof(tgs));
	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, t, task_t, tasks) {
		bool ui = task_is_ui(t);
		thread_t th;

		task_lock(t);
		queue_iterate(&t->threads, th, thread_t, task_threads) {
			struct thread_group *tg = thread_group_get(th);
			unsigned int i;

			if (tg == NULL) {
				continue;
			}
			for (i = 0; i < n && tgs[i].tg != tg; i++) {
			}
			if (i == n) {
				if (n >= AMP_TGS) {
					continue;
				}
				tgs[n].tg = thread_group_retain(tg);
				tgs[n].ui = false;
				n++;
			}
			tgs[i].ui |= ui;
		}
		task_unlock(t);
	}
	lck_mtx_unlock(&tasks_threads_lock);

	ui_demoted = 0;
	for (unsigned int i = 0; i < n; i++) {
		pset_id_t want[TH_BUCKET_SCHED_MAX];

		for (int b = 0; b < TH_BUCKET_SCHED_MAX; b++) {
			want[b] = (b == TH_BUCKET_SHARE_UT || b == TH_BUCKET_SHARE_BG || (b == TH_BUCKET_SHARE_DF && !tgs[i].ui)) ? e_pset : p_pset;
		}
		ui_demoted += !tgs[i].ui;
		sched_edge_tg_preferred_pset_change(tgs[i].tg, want, 0);
		thread_group_release(tgs[i].tg);
	}
}

static void
amp_thread(void *arg __unused, wait_result_t wr __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t idle_before[MAX_CPUS] = { 0 }, then = mach_absolute_time();
	uint64_t bucket_before[2][TH_BUCKET_SCHED_MAX] = { { 0 } }, ui_ticks;

	nanoseconds_to_absolutetime(500 * NSEC_PER_MSEC, &ui_ticks);

	for (;;) {
		char line[300];
		uint64_t now, ns, busy[MAX_CPU_TYPES] = { 0 }, total[MAX_CPU_TYPES] = { 0 };
		int at;

		uint64_t loops = 0, last_ui = 0, end_ns, now_ns;

		clock_interval_to_deadline(AMP_PERIOD_MS, NSEC_PER_MSEC, &end_ns);
		do {
			delay_for_interval(samp_ms ? samp_ms : 250, NSEC_PER_MSEC);
			loops++;
			if (samp_ms) {
				sample_once();
			}
			now_ns = mach_absolute_time();
			if (ui_mode && now_ns - last_ui >= ui_ticks) {
				last_ui = now_ns;
				ui_policy_apply();
			}
		} while (now_ns < end_ns);
		samp_unit_us = loops ? (uint64_t)AMP_PERIOD_MS * 1000 / loops : 0;
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
		at += snprintf(line + at, sizeof(line) - at, " ui=%u demoted_tgs=%u", ui_mode, ui_demoted);
		snprintf(line + at, sizeof(line) - at, "\n");
		amp_say(line);
		if (samp_ms) {
			report_top(now - then);
		}
		if (ktop_on) {
			report_ktop();
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
	PE_parse_boot_argn("amp_samp", &samp_ms, sizeof(samp_ms));
	PE_parse_boot_argn("amp_ktop", &ktop_on, sizeof(ktop_on));
	PE_parse_boot_argn("amp_ui", &ui_mode, sizeof(ui_mode));
	if (kernel_thread_start(amp_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
