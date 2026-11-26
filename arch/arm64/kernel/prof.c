/*
 * A sampling profiler for one process (boot-arg hakios_prof=NAME, e.g. backboardd): about 1000 times a second each other
 * CPU running a thread of that process is interrupted (cpu_xcall) and the PC it was at is counted -- the user PC, or
 * the kernel PC when the thread was in the kernel. Every hakios_prof_secs (default 10) the log gets the 60 commonest PCs
 * ("dprof: PC count", user ones first marked u, kernel ones k) and the process's images outside the shared cache with
 * their load addresses and the cache's slide, so the PCs can be named on the host (ipsw dyld a2s for the cache, nm
 * for our own libraries). After Data A Live's pandora_prof.c (ShueiTsuki/data-a-live), without its call stacks.
 */
#include <kern/processor.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/misc_protos.h>
#include <kern/kalloc.h>
#include <arm/cpu_data_internal.h>
#include <arm/machine_routines.h>
#include <mach/arm/thread_status.h>
#include <arm64/proc_reg.h>
#include <kdp/kdp_dyld.h>
#include <sys/bsdtask_info.h>
#include <vm/vm_map.h>
#include <vm/vm_map_xnu.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern char *proc_name_address(void *p);
extern lck_mtx_t tasks_threads_lock;
extern void hakios_tl(const char *format, ...) __printflike(1, 2);
extern kern_return_t cpu_xcall(int cpu_number, broadcastFunc func, void *param);

#define PROF_CPUS       8
#define PROF_SLOTS      8192u           /* distinct PCs a period */

static struct {
	uint64_t pc;
	uint32_t count;
} *prof_pcs;
static uint64_t prof_samples, prof_user, prof_lost;
static task_t prof_task;
static char prof_name[17];
static int prof_param = 1;
static LCK_GRP_DECLARE(prof_grp, "hakios_prof");
static LCK_SPIN_DECLARE(prof_lock, &prof_grp);

static void
prof_count(uint64_t pc)
{
	uint32_t h = (uint32_t)((pc >> 2) * 2654435761u);

	lck_spin_lock(&prof_lock);
	for (uint32_t i = 0; i < 16; i++) {
		uint32_t k = (h + i) % PROF_SLOTS;

		if (prof_pcs[k].pc == pc || prof_pcs[k].pc == 0) {
			prof_pcs[k].pc = pc;
			prof_pcs[k].count++;
			lck_spin_unlock(&prof_lock);
			return;
		}
	}
	prof_lost++;
	lck_spin_unlock(&prof_lock);
}

static void
prof_sample_cpu(void *arg __unused)
{
	thread_t th = current_thread();
	cpu_data_t *cd = getCpuDatap();
	const arm_saved_state_t *st;

	if (th == THREAD_NULL || get_threadtask(th) != prof_task || (st = (const arm_saved_state_t *)cd->cpu_int_state) == NULL) {
		return;
	}
	if (PSR64_IS_USER(get_saved_state_cpsr(st))) {
		prof_count(get_saved_state_pc(st) & 0x0000ffffffffffffull);   /* (the PAC bits off) */
		/* ... and who called the leaf it is in (its return address, LR): marked 2 */
		prof_count((get_saved_state_lr(st) & 0x0000fffffffffffcull) | 2);
		prof_user++;
	} else {
		prof_count(get_saved_state_pc(st) | 1);                         /* (odd: a kernel PC) */
	}
	prof_samples++;
}

static void
prof_images(task_t task)
{
	struct user64_dyld_all_image_infos infos;
	vm_map_t map = get_task_map_reference(task);

	if (map == VM_MAP_NULL) {
		return;
	}
	if (task->all_image_info_addr != 0 &&
	    vm_map_read_user(map, task->all_image_info_addr, &infos, sizeof(infos)) == KERN_SUCCESS && infos.infoArray != 0) {
		hakios_tl("dprof: images %u, shared cache at %#llx slide %#llx, kernel slide %#lx\n", infos.infoArrayCount,
		    infos.sharedCacheBaseAddress, infos.sharedCacheSlide, (unsigned long)vm_kernel_slide);
		for (uint32_t i = 0; i < infos.infoArrayCount && i < 1024; i++) {
			struct user64_dyld_image_info img;
			char path[160];

			if (vm_map_read_user(map, infos.infoArray + i * sizeof(img), &img, sizeof(img)) != KERN_SUCCESS) {
				break;
			}
			if (infos.sharedCacheBaseAddress && img.imageLoadAddress >= infos.sharedCacheBaseAddress &&
			    img.imageLoadAddress < infos.sharedCacheBaseAddress + (8ULL << 30)) {
				continue;
			}
			bzero(path, sizeof(path));
			(void)vm_map_read_user(map, img.imageFilePath, path, sizeof(path) - 1);
			hakios_tl("dprof: image %#llx %s\n", img.imageLoadAddress, path);
		}
	}
	vm_map_deallocate(map);
}

static void
prof_report(void)
{
	static struct {
		uint64_t pc;
		uint32_t count;
	} top[100];
	uint64_t samples, user;
	boolean_t istate;

	bzero(top, sizeof(top));
	/*
	 * (prof_count() takes the lock in a cross-call's interrupt: one that reached this processor while
	 * this thread held it with interrupts on -- sent before the thread moved here -- would spin on it for
	 * good)
	 */
	istate = ml_set_interrupts_enabled(FALSE);
	lck_spin_lock(&prof_lock);
	for (uint32_t k = 0; k < PROF_SLOTS; k++) {
		uint32_t c = prof_pcs[k].count;

		if (c == 0) {
			continue;
		}
		for (unsigned t = 0; t < 100; t++) {
			if (c > top[t].count) {
				memmove(&top[t + 1], &top[t], (99 - t) * sizeof(top[0]));
				top[t].pc = prof_pcs[k].pc;
				top[t].count = c;
				break;
			}
		}
	}
	bzero(prof_pcs, PROF_SLOTS * sizeof(prof_pcs[0]));
	samples = prof_samples;
	user = prof_user;
	prof_samples = prof_user = 0;
	lck_spin_unlock(&prof_lock);
	ml_set_interrupts_enabled(istate);
	hakios_tl("dprof: %s: %llu samples (%llu in user code), %llu lost\n", prof_name, samples, user, prof_lost);
	for (unsigned t = 0; t < 100 && top[t].count; t++) {
		hakios_tl("dprof: %c %#llx %u\n", (top[t].pc & 1) ? 'k' : (top[t].pc & 2) ? 'l' : 'u', top[t].pc & ~3ull,
		    top[t].count);
	}
}

static task_t
prof_find(void)
{
	task_t found = TASK_NULL, task;

	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, task, task_t, tasks) {
		void *p = get_bsdtask_info(task);

		if (p != NULL && strncmp(proc_name_address(p), prof_name, 16) == 0) {
			task_reference(task);
			found = task;
			break;
		}
	}
	lck_mtx_unlock(&tasks_threads_lock);
	return found;
}

static void
prof_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t secs = 10;

	PE_parse_boot_argn("hakios_prof_secs", &secs, sizeof(secs));
	secs = MAX(secs, 1);    /* (0 would be a division by zero below, as a product that wrapped to 0 in 32 bits) */
	for (uint64_t tick = 1;; tick++) {
		delay(1000);
		if (prof_task == TASK_NULL || !prof_task->active) {
			if (tick % 1000 == 0) {
				if (prof_task != TASK_NULL) {
					task_deallocate(prof_task);
				}
				prof_task = prof_find();
			}
			continue;
		}
		for (int cpu = 0; cpu < PROF_CPUS && cpu <= ml_get_max_cpu_number(); cpu++) {
			cpu_data_t *cd = cpu_datap(cpu);
			thread_t t = cd != NULL ? cd->cpu_active_thread : THREAD_NULL;

			if (cpu == cpu_number() || t == THREAD_NULL || get_threadtask(t) != prof_task) {
				continue;
			}
			(void)cpu_xcall(cpu, prof_sample_cpu, &prof_param);
		}
		if (tick % (secs * 1000ull) == 0) {
			prof_images(prof_task);
			prof_report();
		}
	}
}

/*
 * hakios_prof=all: no PCs, only who is on each CPU, a thousand times a second -- the process each CPU's active thread
 * belongs to (the idle thread counted as "idle") -- and every hakios_prof_secs the processes that had the most of them:
 * which process keeps the cores busy, before asking what in it does.
 */
#define ALL_SLOTS       64

static void
prof_all_thread(__unused void *arg, __unused wait_result_t wr)
{
	static struct {
		char name[17];
		uint32_t count;
	} seen[ALL_SLOTS];
	uint32_t secs = 10, total = 0;

	PE_parse_boot_argn("hakios_prof_secs", &secs, sizeof(secs));
	secs = MAX(secs, 1);    /* (0 would be a division by zero below, as a product that wrapped to 0 in 32 bits) */
	for (uint64_t tick = 1;; tick++) {
		delay(1000);
		for (int cpu = 0; cpu < PROF_CPUS && cpu <= ml_get_max_cpu_number(); cpu++) {
			cpu_data_t *cd = cpu_datap(cpu);
			thread_t t = cd != NULL ? cd->cpu_active_thread : THREAD_NULL;
			const char *name = "?";
			char own[17];

			if (cpu == cpu_number() || t == THREAD_NULL) {
				continue;
			}
			if (cpu_to_processor(cpu) != PROCESSOR_NULL && t == cpu_to_processor(cpu)->idle_thread) {
				name = "idle";
			} else {
				task_t task = get_threadtask(t);
				void *p = task != TASK_NULL ? get_bsdtask_info(task) : NULL;

				if (task == kernel_task) {
					name = "kernel_task";
				} else if (p != NULL) {
					strlcpy(own, proc_name_address(p), sizeof(own));
					name = own;
				}
			}
			total++;
			for (unsigned i = 0; i < ALL_SLOTS; i++) {
				if (seen[i].count == 0 || strncmp(seen[i].name, name, 16) == 0) {
					if (seen[i].count == 0) {
						strlcpy(seen[i].name, name, sizeof(seen[i].name));
					}
					seen[i].count++;
					break;
				}
			}
		}
		if (tick % (secs * 1000ull) == 0 && total != 0) {
			char line[400];
			int at = 0;

			for (unsigned n = 0; n < 12; n++) {
				unsigned best = ALL_SLOTS;

				for (unsigned i = 0; i < ALL_SLOTS; i++) {
					if (seen[i].count != 0 && (best == ALL_SLOTS || seen[i].count > seen[best].count)) {
						best = i;
					}
				}
				if (best == ALL_SLOTS) {
					break;
				}
				at += snprintf(line + at, sizeof(line) - (size_t)at, " %s %u%%", seen[best].name,
				    seen[best].count * 100 / total);
				seen[best].count = 0;
			}
			hakios_tl("dprof: who is on the CPUs (%u samples):%s\n", total, line);
			bzero(seen, sizeof(seen));
			total = 0;
		}
	}
}


/*
 * Boot-arg waitprof=NAME[,NAME] (e.g. backboardd,SpringBoard): an off-CPU profiler. About every 2 ms each thread of
 * the named processes is looked at under its thread lock: running, runnable (on a run queue, not on a core) or
 * blocked. A blocked one is named by what it waits on: the wait event (a port, a semaphore, a driver's own object),
 * the block hint, the timeout still to run (thread->wait_timer), the continuation or the return addresses of its
 * kernel stack (the frame chain from the context it was switched out with: symbolize on the host with the
 * unstripped kernel and the kernel slide of the report), and how it entered the kernel: the user PC and LR, the
 * trap number (x16: negative Mach traps, positive BSD syscalls) and the exception class of its saved state (0x15
 * a system call, 0x24 a data abort: a page fault in user code), and for a mach_msg the message id and the first
 * word of its body after the NDR -- an IOConnectCallMethod's selector.
 * An episode is one stay on one wait (the thread's context-switch count does not change); episodes of
 * waitprof_ms (default 30) ms or more are logged when they end ("waitprof: LONG ..."), with the uptime they ended
 * at to be matched with the plug-in's HITCH lines; so are runnable episodes ("RUNNABLE": a thread kept off the CPUs).
 * Every waitprof_secs (default 5) the 8 waits that held threads longest in all, and the threads' time shares.
 */
#define WP_MAXT         2
#define WP_THREADS      192
#define WP_AGG          256
#define WP_BT           6

struct wp_wait {                /* what a thread waits on */
	uint64_t event, cont, upc, ulr, trap;
	uint64_t timeout_ms;    /* (still to run when first seen; ~0: none) */
	uint32_t ec, msgid, word, hint;
	uint8_t has_msg, nbt;
	uint64_t bt[WP_BT];
};

struct wp_agg {
	uint64_t tid, key;
	char name[24];
	struct wp_wait w;
	uint32_t ms, smp, eps, max_ms;
};

struct wp_thr {
	uint64_t tid;
	char name[24];
	uint32_t cs;            /* (c_switch of the episode) */
	uint8_t kind;           /* 0 none, 1 blocked, 2 runnable */
	int agg;
	uint64_t began, last;   /* (ns) */
	uint64_t ev;
	uint32_t ms_run, ms_rdy, ms_blk;
	struct wp_wait w;
};

static struct wp_target {
	char name[17];
	task_t task;
	struct wp_thr *thr;
	struct wp_agg *agg;
	uint32_t nagg, samples, lost;
} wp_t[WP_MAXT];
static unsigned wp_n;
static uint32_t wp_long_ms = 30, wp_secs = 5;

extern void loader_log_line(const char *line);
extern boolean_t machine_thread_on_core(thread_t thread);

#define WPSAY(...) \
	do { \
		char wp_line[480]; \
		snprintf(wp_line, sizeof(wp_line), __VA_ARGS__); \
		loader_log_line(wp_line); \
	} while (0)

static const char *
wp_trap_name(uint64_t t)
{
	switch ((int64_t)t) {
	case -31: return "mach_msg";
	case -47: return "mach_msg2";
	case -36: return "sem_wait";
	case -37: return "sem_wait_signal";
	case -38: return "sem_timedwait";
	case -39: return "sem_timedwait_signal";
	case -59: return "swtch_pri";
	case -61: return "thread_switch";
	case -90: return "mach_wait_until";
	case 93: return "select";
	case 230: case 417: return "poll";
	case 363: return "kevent";
	case 368: return "workq_kernreturn";
	case 374: return "kevent_qos";
	case 375: return "kevent_id";
	case 394: case 395: return "pselect";
	case 515: return "ulock_wait";
	case 544: return "ulock_wait2";
	case 301: return "psynch_mutexwait";
	case 302: return "psynch_mutexdrop";
	case 305: return "psynch_cvwait";
	case 306: return "psynch_rw_rdlock";
	case 307: return "psynch_rw_wrlock";
	case 334: return "usleep/nanosleep";
	case 240: return "nanosleep";
	case 5: return "open";
	case 3: return "read";
	case 4: return "write";
	case 197: return "mmap";
	case 54: return "ioctl";
	case 6: return "close";
	case 361: return "bsdthread_terminate";
	case 478: return "bsdthread_ctl";
	case 336: return "proc_info";
	default: return "";
	}
}

struct wp_pending {
	struct wp_thr *th;
	uint32_t cs;
	uint64_t x0, x4;
};

static void
wp_fill_user(thread_t t, struct wp_wait *w, uint64_t *x0, uint64_t *x4)
{
	const arm_saved_state_t *us = t->machine.upcb;

	if (us == NULL || !is_saved_state64(us)) {
		return;
	}
	w->upc = get_saved_state_pc(us) & 0x0000ffffffffffffull;
	w->ulr = get_saved_state_lr(us) & 0x0000ffffffffffffull;
	w->ec = (uint32_t)(get_saved_state_esr(us) >> 26);
	w->trap = get_saved_state_reg(us, 16);
	*x0 = get_saved_state_reg(us, 0);
	*x4 = get_saved_state_reg(us, 4);
}

/* (no locks held: the task's map is read, which may fault) */
static void
wp_read_msg(struct wp_target *tg, struct wp_pending *pe)
{
	struct wp_thr *th = pe->th;
	struct wp_wait w = th->w;
	vm_map_t map = get_task_map_reference(tg->task);
	uint32_t m[9];

	if (map == VM_MAP_NULL) {
		return;
	}
	bzero(m, sizeof(m));
	if (w.trap == (uint64_t)-31) {
		if (vm_map_read_user(map, pe->x0, m, sizeof(m)) == KERN_SUCCESS) {
			w.msgid = m[5];
			w.word = m[8];
			w.has_msg = 1;
		}
	} else {
		w.msgid = (uint32_t)(pe->x4 >> 32);
		if (vm_map_read_user(map, pe->x0, m, 12) == KERN_SUCCESS) {
			w.word = m[2];
			w.has_msg = 1;
		} else {
			w.has_msg = 2;
		}
	}
	vm_map_deallocate(map);
	if (th->kind == 1 && th->cs == pe->cs) {        /* (still that wait) */
		th->w.msgid = w.msgid;
		th->w.word = w.word;
		th->w.has_msg = w.has_msg;
		if (th->agg >= 0) {
			tg->agg[th->agg].w.msgid = w.msgid;
			tg->agg[th->agg].w.word = w.word;
			tg->agg[th->agg].w.has_msg = w.has_msg;
		}
	}
}

static void
wp_stack(thread_t t, struct wp_wait *w)
{
	vm_offset_t stk = t->kernel_stack;
	const arm_kernel_saved_state_t *ks;
	uint64_t fp;

	if (stk == 0 || t->machine.kstackptr == NULL) {
		return;
	}
	ks = &((thread_kernel_state_t)t->machine.kstackptr)->machine.ss;
	w->bt[w->nbt++] = ks->lr;
	fp = ks->fp;
	while (w->nbt < WP_BT && fp >= stk && fp + 16 <= stk + kernel_stack_size && (fp & 7) == 0) {
		uint64_t ra = ((uint64_t *)fp)[1], next = ((uint64_t *)fp)[0];

		if (ra != 0) {
			w->bt[w->nbt++] = ra;
		}
		if (next <= fp) {
			break;
		}
		fp = next;
	}
}

static int
wp_agg_for(struct wp_target *tg, const struct wp_thr *th, const struct wp_wait *w)
{
	uint64_t key = th->tid * 0x9E3779B97F4A7C15ull ^ (w->event * 31) ^ (w->bt[1] * 17) ^ (w->bt[0] * 7) ^ w->upc ^
	    (w->cont * 13) ^ (w->trap << 40);

	for (uint32_t i = 0; i < tg->nagg; i++) {
		if (tg->agg[i].key == key && tg->agg[i].tid == th->tid) {
			return (int)i;
		}
	}
	if (tg->nagg == WP_AGG) {
		tg->lost++;
		return -1;
	}
	struct wp_agg *a = &tg->agg[tg->nagg];

	bzero(a, sizeof(*a));
	a->key = key;
	a->tid = th->tid;
	strlcpy(a->name, th->name, sizeof(a->name));
	a->w = *w;
	return (int)tg->nagg++;
}

static void
wp_describe(char *out, size_t n, const struct wp_wait *w)
{
	size_t at = 0;

	at += (size_t)snprintf(out + at, n - at, "ec %#x trap %lld %s", w->ec, (long long)w->trap, wp_trap_name(w->trap));
	if (w->has_msg) {
		at += (size_t)snprintf(out + at, n - at, " msgid %u body0 %#x", w->msgid, w->word);
	}
	at += (size_t)snprintf(out + at, n - at, " ev %#llx hint %u", w->event, w->hint);
	if (w->timeout_ms != ~0ull) {
		at += (size_t)snprintf(out + at, n - at, " timeout %llums", w->timeout_ms);
	}
	at += (size_t)snprintf(out + at, n - at, " upc %#llx ulr %#llx", w->upc, w->ulr);
	if (w->cont != 0) {
		at += (size_t)snprintf(out + at, n - at, " cont %#llx", w->cont);
	}
	if (w->nbt != 0) {
		at += (size_t)snprintf(out + at, n - at, " k:");
		for (unsigned i = 0; i < w->nbt; i++) {
			at += (size_t)snprintf(out + at, n - at, " %#llx", w->bt[i]);
		}
	}
}

static void
wp_end_episode(struct wp_target *tg, struct wp_thr *th)
{
	uint64_t ms = (th->last - th->began) / NSEC_PER_MSEC + 2;

	if (th->kind == 1 && th->agg >= 0) {
		struct wp_agg *a = &tg->agg[th->agg];

		a->eps++;
		a->max_ms = MAX(a->max_ms, (uint32_t)ms);
	}
	if (th->kind != 0 && ms >= wp_long_ms) {
		char d[300];

		d[0] = 0;
		if (th->kind == 1) {
			wp_describe(d, sizeof(d), &th->w);
		}
		WPSAY("waitprof: %s %s at %llu.%03llu s: tid %llu '%s' %llu ms %s", tg->name, th->kind == 1 ? "LONG" : "RUNNABLE",
		    th->last / NSEC_PER_SEC, th->last % NSEC_PER_SEC / NSEC_PER_MSEC, th->tid, th->name, ms, d);
	}
	th->kind = 0;
}

static void
wp_sample(struct wp_target *tg, uint64_t now_ns, uint64_t dt_ms)
{
	thread_t t;
	task_t task = tg->task;
	struct wp_pending pend[16];
	unsigned npend = 0;

	tg->samples++;
	task_lock(task);
	queue_iterate(&task->threads, t, thread_t, task_threads) {
		struct wp_thr *th = NULL;
		struct wp_wait w;
		uint32_t state, cs, hint;
		uint64_t tid = t->thread_id;
		uint64_t ev, to_abs = 0;
		uintptr_t cont;
		bool timer_armed;
		int kind;
		spl_t s;

		for (unsigned i = 0; i < WP_THREADS; i++) {
			if (tg->thr[i].tid == tid) {
				th = &tg->thr[i];
				break;
			}
			if (th == NULL && tg->thr[i].tid == 0) {
				th = &tg->thr[i];
			}
		}
		if (th == NULL) {
			continue;
		}
		bzero(&w, sizeof(w));
		w.timeout_ms = ~0ull;
		s = splsched();
		thread_lock(t);
		state = t->state;
		cs = t->c_switch;
		ev = (uint64_t)t->wait_event;
		hint = t->block_hint;
		cont = (uintptr_t)t->continuation;
		timer_armed = t->wait_timer != NULL && t->wait_timer_armed;
		if (timer_armed) {
			to_abs = t->wait_timer->tc_soft_deadline;
		}
		if ((state & (TH_WAIT | TH_RUN)) == TH_WAIT) {
			kind = 1;
		} else if ((state & TH_RUN) && !machine_thread_on_core(t)) {
			kind = 2;
		} else {
			kind = 0;
		}
		if (kind == 1) {
			w.event = ev;
			w.hint = hint;
			w.cont = cont;
			if (timer_armed) {
				uint64_t nowabs = mach_absolute_time(), ns;

				absolutetime_to_nanoseconds(to_abs > nowabs ? to_abs - nowabs : 0, &ns);
				w.timeout_ms = ns / NSEC_PER_MSEC;
			}
		}
		thread_unlock(t);
		splx(s);

		if (th->tid == 0) {
			th->tid = tid;
			th->kind = 0;
			th->agg = -1;
			bzero(th->name, sizeof(th->name));
			if (get_bsdthread_info(t) != NULL) {
				char nm[MAXTHREADNAMESIZE];

				bzero(nm, sizeof(nm));
				bsd_getthreadname(get_bsdthread_info(t), nm);
				strlcpy(th->name, nm, sizeof(th->name));
			}
		}
		if (th->name[0] == 0 && get_bsdthread_info(t) != NULL) {
			char nm[MAXTHREADNAMESIZE];

			bzero(nm, sizeof(nm));
			bsd_getthreadname(get_bsdthread_info(t), nm);
			strlcpy(th->name, nm, sizeof(th->name));
		}
		/* the same episode: same kind, same dispatch count, same event */
		if (th->kind != 0 && !(th->kind == kind && th->cs == cs && (kind != 1 || th->ev == ev))) {
			wp_end_episode(tg, th);
		}
		if (kind == 0) {
			th->ms_run += (uint32_t)dt_ms;
			continue;
		}
		if (kind == 2) {
			th->ms_rdy += (uint32_t)dt_ms;
			if (th->kind == 0) {
				th->kind = 2;
				th->cs = cs;
				th->began = now_ns;
				bzero(&th->w, sizeof(th->w));
			}
			th->last = now_ns;
			continue;
		}
		th->ms_blk += (uint32_t)dt_ms;
		if (th->kind == 0) {
			/* a new wait: what made it, read once (the thread is blocked: its state is still) */
			wp_stack(t, &w);
			uint64_t x0 = 0, x4 = 0;

			wp_fill_user(t, &w, &x0, &x4);
			if ((t->state & (TH_WAIT | TH_RUN)) != TH_WAIT || t->c_switch != cs) {
				continue;       /* (it woke meanwhile: the reading is not of this wait) */
			}
			th->kind = 1;
			th->cs = cs;
			th->ev = ev;
			th->began = now_ns;
			th->w = w;
			th->agg = wp_agg_for(tg, th, &w);
			if (w.ec == 0x15 && (w.trap == (uint64_t)-31 || w.trap == (uint64_t)-47) && npend < 16) {
				pend[npend].th = th;
				pend[npend].cs = cs;
				pend[npend].x0 = x0;
				pend[npend].x4 = x4;
				npend++;
			}
		}
		th->last = now_ns;
		if (th->agg >= 0) {
			tg->agg[th->agg].ms += (uint32_t)dt_ms;
			tg->agg[th->agg].smp++;
		}
	}
	task_unlock(task);
	for (unsigned i = 0; i < npend; i++) {
		wp_read_msg(tg, &pend[i]);
	}
}

static void
wp_report(struct wp_target *tg)
{
	bool used[WP_AGG];

	bzero(used, sizeof(used));
	WPSAY("waitprof: %s: %u samples in %us, kernel slide %#lx, %u waits not kept", tg->name, tg->samples, wp_secs,
	    (unsigned long)vm_kernel_slide, tg->lost);
	for (unsigned n = 0; n < 8; n++) {
		int best = -1;
		char d[300];

		for (uint32_t i = 0; i < tg->nagg; i++) {
			if (!used[i] && tg->agg[i].ms != 0 && (best < 0 || tg->agg[i].ms > tg->agg[best].ms)) {
				best = (int)i;
			}
		}
		if (best < 0) {
			break;
		}
		used[best] = true;
		wp_describe(d, sizeof(d), &tg->agg[best].w);
		WPSAY("waitprof: %s #%u tid %llu '%s' blocked %u ms (%u eps, max %u ms) %s", tg->name, n, tg->agg[best].tid,
		    tg->agg[best].name, tg->agg[best].ms, tg->agg[best].eps, tg->agg[best].max_ms, d);
	}
	/* the threads' shares: the ones that ran or waited for a CPU for 3% or more of the period */
	for (unsigned i = 0; i < WP_THREADS; i++) {
		struct wp_thr *th = &tg->thr[i];
		uint32_t all = th->ms_run + th->ms_rdy + th->ms_blk;

		if (th->tid != 0 && all != 0 && (th->ms_run + th->ms_rdy) * 100 / all >= 3) {
			WPSAY("waitprof: %s tid %llu '%s' run %u%% runnable %u%% blocked %u%%", tg->name, th->tid, th->name,
			    th->ms_run * 100 / all, th->ms_rdy * 100 / all, th->ms_blk * 100 / all);
		}
		th->ms_run = th->ms_rdy = th->ms_blk = 0;
	}
	bzero(tg->agg, WP_AGG * sizeof(tg->agg[0]));
	tg->nagg = tg->samples = tg->lost = 0;
}

static task_t
wp_find(const char *name)
{
	task_t found = TASK_NULL, task;

	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, task, task_t, tasks) {
		void *p = get_bsdtask_info(task);

		if (p != NULL && strncmp(proc_name_address(p), name, 16) == 0) {
			task_reference(task);
			found = task;
			break;
		}
	}
	lck_mtx_unlock(&tasks_threads_lock);
	return found;
}

static void
waitprof_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint64_t last_ns = 0, report_ns = 0, find_ns = 0;

	for (;;) {
		uint64_t abs = mach_absolute_time(), now, dt_ms;

		absolutetime_to_nanoseconds(abs, &now);
		dt_ms = last_ns != 0 ? MIN((now - last_ns) / NSEC_PER_MSEC, 20) : 2;
		last_ns = now;
		for (unsigned i = 0; i < wp_n; i++) {
			struct wp_target *tg = &wp_t[i];

			if (tg->task == TASK_NULL || !tg->task->active) {
				if (tg->task != TASK_NULL) {
					task_deallocate(tg->task);
					tg->task = TASK_NULL;
					bzero(tg->thr, WP_THREADS * sizeof(tg->thr[0]));
				}
				if (now - find_ns >= NSEC_PER_SEC) {
					tg->task = wp_find(tg->name);
				}
				continue;
			}
			wp_sample(tg, now, dt_ms);
			if (now - report_ns >= (uint64_t)wp_secs * NSEC_PER_SEC && report_ns != 0) {
				wp_report(tg);
			}
		}
		if (now - find_ns >= NSEC_PER_SEC) {
			find_ns = now;
		}
		if (report_ns == 0 || now - report_ns >= (uint64_t)wp_secs * NSEC_PER_SEC) {
			report_ns = now;
		}
		delay_for_interval(2, NSEC_PER_MSEC);
	}
}

static void
waitprof_start(void)
{
	char arg[40];
	thread_t thread;
	char *p, *q;

	if (!PE_parse_boot_arg_str("waitprof", arg, sizeof(arg)) || arg[0] == 0) {
		return;
	}
	PE_parse_boot_argn("waitprof_ms", &wp_long_ms, sizeof(wp_long_ms));
	PE_parse_boot_argn("waitprof_secs", &wp_secs, sizeof(wp_secs));
	wp_secs = MAX(wp_secs, 1);
	for (p = arg; *p != 0 && wp_n < WP_MAXT; p = q) {
		struct wp_target *tg = &wp_t[wp_n];
		size_t len;

		for (q = p; *q != 0 && *q != ','; q++) {
		}
		len = MIN((size_t)(q - p), sizeof(tg->name) - 1);
		memcpy(tg->name, p, len);
		if (*q == ',') {
			q++;
		}
		tg->thr = kalloc_data(WP_THREADS * sizeof(tg->thr[0]), Z_WAITOK | Z_ZERO);
		tg->agg = kalloc_data(WP_AGG * sizeof(tg->agg[0]), Z_WAITOK | Z_ZERO);
		if (tg->thr == NULL || tg->agg == NULL || len == 0) {
			return;
		}
		wp_n++;
	}
	if (wp_n != 0 && kernel_thread_start_priority(waitprof_thread, NULL, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

void hakios_prof_start(void);

void
hakios_prof_start(void)
{
	thread_t thread;

	waitprof_start();       /* (boot-arg waitprof=NAME[,NAME]: below) */
	if (!PE_parse_boot_arg_str("hakios_prof", prof_name, sizeof(prof_name)) || prof_name[0] == 0) {
		return;
	}
	if (strcmp(prof_name, "all") == 0) {
		if (kernel_thread_start_priority(prof_all_thread, NULL, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
		return;
	}
	prof_pcs = kalloc_data(PROF_SLOTS * sizeof(prof_pcs[0]), Z_WAITOK | Z_ZERO);
	if (prof_pcs != NULL && kernel_thread_start_priority(prof_thread, NULL, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
