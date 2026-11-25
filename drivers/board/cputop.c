/*
 * Boot-arg cputop=S: every S seconds, the processes that took the most CPU time since the last time, by name and pid,
 * and how many processes came and went meanwhile. (All cores at 98-100 % for the first minute and
 * more after boot -- the governor's own line says how busy each CPU is, not for whom.)
 *
 * A process's time is its task's (recount_task_times: every thread's, the ones that are gone too), taken from each live
 * process in turn (proc_iterate holds a reference to it meanwhile). A process that came and went between two looks is
 * not seen, only counted: what each process had at the last look is kept by pid.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc_internal.h>
#include <kern/recount.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <kern/kalloc.h>
#include <mach_debug/zone_info.h>
#include <vm/vm_kern_xnu.h>

#define CPUTOP_PROCS            640
#define CPUTOP_SHOWN            12
#define CPUTOP_MAX_SECONDS      3600

extern void hakios_tl(const char *format, ...) __printflike(1, 2);
extern task_t proc_task(proc_t);

struct cputop_entry {
	int             pid;
	uint64_t        total;          /* user and system, absolute time */
	uint64_t        took;           /* since the last look */
	char            name[24];
};

static struct cputop_entry cputop_was[CPUTOP_PROCS], cputop_now[CPUTOP_PROCS];
static uint32_t cputop_was_count, cputop_now_count, cputop_new;
static uint32_t cputop_seconds;

static int
cputop_one(proc_t p, void *arg __unused)
{
	struct cputop_entry *e;
	struct recount_times_mach times;
	task_t task = proc_task(p);
	int pid = proc_pid(p);
	uint32_t i;

	if (task == TASK_NULL || cputop_now_count >= CPUTOP_PROCS) {
		return PROC_RETURNED;
	}
	times = recount_task_times(task);
	e = &cputop_now[cputop_now_count++];
	e->pid = pid;
	e->total = times.rtm_user + times.rtm_system;
	e->took = e->total;
	for (i = 0; i < cputop_was_count; i++) {
		if (cputop_was[i].pid == pid) {
			e->took = e->total >= cputop_was[i].total ? e->total - cputop_was[i].total : 0;
			break;
		}
	}
	if (i == cputop_was_count && cputop_was_count != 0) {
		cputop_new++;
	}
	strlcpy(e->name, proc_best_name(p), sizeof(e->name));
	return PROC_RETURNED;
}

static void
cputop_thread(void *arg __unused, wait_result_t w __unused)
{
	for (;;) {
		uint64_t all = 0, ns;
		uint32_t gone = 0;
		char line[640] = "";            /* (said as it is when no process took any time: empty, not whatever was there) */
		int at = 0;

		delay_for_interval(cputop_seconds * 1000, NSEC_PER_MSEC);
		cputop_now_count = 0;
		cputop_new = 0;
		proc_iterate(PROC_ALLPROCLIST, cputop_one, NULL, NULL, NULL);
		for (uint32_t i = 0; i < cputop_was_count; i++) {
			uint32_t j;

			for (j = 0; j < cputop_now_count && cputop_now[j].pid != cputop_was[i].pid; j++) {
			}
			gone += j == cputop_now_count;
		}
		for (uint32_t i = 0; i < cputop_now_count; i++) {
			all += cputop_now[i].took;
		}
		/* the CPUTOP_SHOWN that took most, by selection: a few hundred processes */
		for (uint32_t shown = 0; shown < CPUTOP_SHOWN && shown < cputop_now_count; shown++) {
			uint32_t best = shown;

			for (uint32_t i = shown + 1; i < cputop_now_count; i++) {
				if (cputop_now[i].took > cputop_now[best].took) {
					best = i;
				}
			}
			if (best != shown) {
				struct cputop_entry t = cputop_now[shown];

				cputop_now[shown] = cputop_now[best];
				cputop_now[best] = t;
			}
			if (cputop_now[shown].took == 0) {
				break;
			}
			absolutetime_to_nanoseconds(cputop_now[shown].took, &ns);
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %s[%d] %llu", cputop_now[shown].name,
			    cputop_now[shown].pid, ns / NSEC_PER_MSEC);
			if (at >= (int)sizeof(line) - 1) {
				break;
			}
		}
		absolutetime_to_nanoseconds(all, &ns);
		hakios_tl("cputop: %u s: %llu ms of CPU in %u processes (%u new, %u gone); most, ms:%s\n", cputop_seconds,
		    ns / NSEC_PER_MSEC, cputop_now_count, cputop_new, gone, line);
		bcopy(cputop_now, cputop_was, sizeof(cputop_now[0]) * cputop_now_count);
		cputop_was_count = cputop_now_count;
	}
}

/*
 * Boot-arg memtop=S (default 5; 0: off): every S seconds a line of where the memory is (hakios_memtop_line, in
 * arch/arm64/kernel/loader_return.c) and the five processes with the biggest footprint, by the kernel's own count
 * (resident and compressed, in MB; the compressed part in brackets).
 */
#define MEMTOP_SHOWN    5

struct memtop_entry {
	int             pid;
	uint64_t        footprint, compressed, wired;
	char            name[24];
};
static struct memtop_entry memtop_best[MEMTOP_SHOWN], memtop_wired[3];
static uint32_t memtop_seconds;

extern uint64_t get_task_phys_footprint(task_t);
extern uint64_t get_task_compressed(task_t);
extern uint64_t get_task_wired_mem(task_t);
extern void hakios_memtop_line(char *buf, size_t len);
extern void hakios_iokit_line(char *buf, size_t len);
extern void hakios_gpu_leaktest(void);

static int
memtop_one(proc_t p, void *arg __unused)
{
	task_t task = proc_task(p);
	struct memtop_entry e;
	uint32_t at = MEMTOP_SHOWN;

	if (task == TASK_NULL) {
		return PROC_RETURNED;
	}
	e.pid = proc_pid(p);
	e.footprint = get_task_phys_footprint(task);
	e.compressed = get_task_compressed(task);
	e.wired = get_task_wired_mem(task);
	{
		uint32_t w = 3;

		while (w > 0 && e.wired > memtop_wired[w - 1].wired) {
			w--;
		}
		if (w < 3) {
			struct memtop_entry c = e;

			strlcpy(c.name, proc_best_name(p), sizeof(c.name));
			for (uint32_t i = 2; i > w; i--) {
				memtop_wired[i] = memtop_wired[i - 1];
			}
			memtop_wired[w] = c;
		}
	}
	strlcpy(e.name, proc_best_name(p), sizeof(e.name));
	while (at > 0 && e.footprint > memtop_best[at - 1].footprint) {
		at--;
	}
	if (at < MEMTOP_SHOWN) {
		for (uint32_t i = MEMTOP_SHOWN - 1; i > at; i--) {
			memtop_best[i] = memtop_best[i - 1];
		}
		memtop_best[at] = e;
	}
	return PROC_RETURNED;
}

/*
 * The kernel's own accounting of what each allocation tag holds (vm_page_diagnose: IOKit, files, kalloc, the
 * kexts' tags ...), the biggest few, in MB. "wired" there says how much of the machine's wired pages the tags
 * together account for; the rest is wired without a tag -- pages a program has wired (see the processes above)
 * or a page table.
 */
static void
memtop_tags(void)
{
	uint32_t n = vm_page_diagnose_estimate() + 64;
	mach_memory_info_t *info = kalloc_data(n * sizeof(*info), Z_WAITOK | Z_ZERO);
	static char line[640];
	int at = 0;
	uint64_t total = 0;
	struct { uint64_t size, mapped; char name[32]; uint32_t tag; } top[8] = { { 0 } };

	if (info == NULL) {
		return;
	}
	if (vm_page_diagnose(info, n, 0, false) == KERN_SUCCESS) {
		for (uint32_t i = 0; i < n; i++) {
			uint32_t type = info[i].flags & VM_KERN_SITE_TYPE;

			if (info[i].size == 0 || (info[i].flags & (VM_KERN_SITE_ZONE | VM_KERN_SITE_ZONE_VIEW)) ||
			    type == VM_KERN_SITE_COUNTER) {
				continue;
			}
			total += info[i].size;
			uint32_t at_i = 8;

			while (at_i > 0 && info[i].size > top[at_i - 1].size) {
				at_i--;
			}
			if (at_i < 8) {
				for (uint32_t j = 7; j > at_i; j--) {
					top[j] = top[j - 1];
				}
				top[at_i].size = info[i].size;
				top[at_i].mapped = info[i].mapped;
				top[at_i].tag = info[i].tag;
				strlcpy(top[at_i].name, (info[i].flags & VM_KERN_SITE_NAMED) ? info[i].name : "", sizeof(top[at_i].name));
			}
		}
		at = snprintf(line, sizeof(line), "memtags: all tagged allocations %llu MB; biggest:", total >> 20);
		for (uint32_t i = 0; i < 8 && top[i].size != 0 && at < (int)sizeof(line) - 1; i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %s(tag %u) %llu (mapped %llu)", top[i].name, top[i].tag, top[i].size >> 20, top[i].mapped >> 20);
		}
		printf("%s\n", line);
	}
	kfree_data(info, n * sizeof(*info));
}

static void
memtop_thread(void *arg __unused, wait_result_t w __unused)
{
	static char line[1024];

	uint32_t leaktest = 1;      /* (gpu_leaktest=0: not) */
	bool tested = false;

	(void)PE_parse_boot_argn("gpu_leaktest", &leaktest, sizeof(leaktest));
	for (;;) {
		int at;

		delay_for_interval(memtop_seconds * 1000, NSEC_PER_MSEC);
		if (leaktest != 0 && !tested && mach_absolute_time() > 30 * NSEC_PER_SEC * 24 / 1000) {
			tested = true;
			hakios_gpu_leaktest();
		}
		bzero(memtop_best, sizeof(memtop_best));
		bzero(memtop_wired, sizeof(memtop_wired));
		proc_iterate(PROC_ALLPROCLIST, memtop_one, NULL, NULL, NULL);
		hakios_memtop_line(line, sizeof(line));
		at = (int)strlen(line);
		at += snprintf(line + at, sizeof(line) - (size_t)at, "; top by footprint:");
		for (uint32_t i = 0; i < MEMTOP_SHOWN && memtop_best[i].footprint != 0 && at < (int)sizeof(line) - 1; i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %s[%d] %llu (%llu)", memtop_best[i].name,
			    memtop_best[i].pid, memtop_best[i].footprint >> 20, memtop_best[i].compressed >> 20);
		}
		at += snprintf(line + at, sizeof(line) - (size_t)at, "; wired in processes (user wiring: mlock, IOMemoryDescriptor::prepare of a program's pages):");
		for (uint32_t i = 0; i < 3 && memtop_wired[i].wired != 0 && at < (int)sizeof(line) - 1; i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %s[%d] %llu", memtop_wired[i].name, memtop_wired[i].pid,
			    memtop_wired[i].wired >> 20);
		}
		printf("%s\n", line);
		hakios_iokit_line(line, sizeof(line));
		printf("%s\n", line);
		memtop_tags();
	}
}

void hakios_cputop_start(void);
void
hakios_cputop_start(void)
{
	thread_t thread;

	memtop_seconds = 5;
	(void)PE_parse_boot_argn("memtop", &memtop_seconds, sizeof(memtop_seconds));
	if (memtop_seconds != 0) {
		memtop_seconds = memtop_seconds > CPUTOP_MAX_SECONDS ? CPUTOP_MAX_SECONDS : memtop_seconds;
		if (kernel_thread_start(memtop_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
	if (!PE_parse_boot_argn("cputop", &cputop_seconds, sizeof(cputop_seconds)) || cputop_seconds == 0) {
		return;
	}
	/* at most an hour: in milliseconds (the wait's unit) more than 49 days would not fit in 32 bits */
	cputop_seconds = cputop_seconds > CPUTOP_MAX_SECONDS ? CPUTOP_MAX_SECONDS : cputop_seconds;
	if (kernel_thread_start(cputop_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
