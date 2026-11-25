/*
 * How long each CPU has been idle, from the scheduler's own bookkeeping (recount: every idle entry
 * and exit is stamped), and which cluster a CPU is in (MPIDR Aff2).
 */
#include <stdarg.h>
#include <kern/processor.h>
#include <kern/recount.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/misc_protos.h>
#include <string.h>
#include <machine/atomic.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>

uint64_t hakios_cpu_idle_mach(int cpu);
uint64_t
hakios_cpu_idle_mach(int cpu)
{
	processor_t p = cpu_to_processor(cpu);
	struct recount_usage usage;
	uint64_t idle = 0;

	if (p == PROCESSOR_NULL) {
		return 0;
	}
	recount_processor_usage(&p->pr_recount, &usage, &idle);
	return idle;
}

int hakios_cpu_cluster(int cpu);
int
hakios_cpu_cluster(int cpu)
{
	const ml_topology_info_t *topology = ml_get_topology_info();

	if (topology == NULL || cpu < 0 || (unsigned)cpu >= topology->num_cpus) {
		return -1;
	}
	return (int)((topology->cpus[cpu].phys_id >> 16) & 0xff);
}

#if defined(GENERIC_PSCI_SMP)
/*
 * Idle states beyond WFI (boot-arg pm_idle=N; 0, the default: the core waits in WFI as before):
 *
 *   pm_idle=1   a core with nothing to do and a next timer far enough away asks the firmware for the "pc" state of the vendor's
 *               lpm-levels (PSCI CPU_SUSPEND, the core powered down, its caches kept coherent by the firmware)
 *   pm_idle=2   ... and for "rail-pc" when the wait is long enough, and, when every other core of the kernel is down as well, for
 *               the cluster's "l3-pc" with it
 *
 * Nothing else in the kernel knows. A powered-down core comes back through the reset vector (start.s: the core's cpu data names
 * resume_idle_cpu), arm_init_idle_cpu() -> hakios_idle_resume() puts back what the core lost -- the interrupt controller's CPU
 * interface and the virtual timer's deadline, the rest of what the core needs being set up as for a core started by CPU_ON --
 * and cpu_idle_exit() carries on as after a WFI. The levels' numbers are the Mi 9T's (davinci-data/xiaomi-sdmmagpie.pre.dts,
 * qcom,lpm-levels: psci-mode, exit latency, minimum residency, the cluster's mode shifted by 4); on another board nothing is
 * done and a line says so.
 *
 * What may go wrong is left to find itself, and be said:
 *  - a state the firmware refuses (an answer below 0) is not tried again after PM_REFUSALS refusals in a row;
 *  - a core is only suspended for as long as the kernel expects it to wait (its next timer), at most twice what its last
 *    waits were (a core that is woken by every touch is not put through a millisecond of exit latency to save a microwatt);
 *  - the virtual timer might stop with the core (the vendor's own cpuidle moves the deadline to a timer in the SoC's always-on
 *    domain: "use-broadcast-timer"): the boot processor, which never suspends, looks every PM_WATCH_MS at the cores that are
 *    down past their deadline, wakes them with an interrupt and counts it; PM_LOST_LIMIT of those in a row and the deep states
 *    are off for good (the line says "TIMER LOST");
 *  - the boot processor itself never goes deeper than WFI (it keeps the loader's return path and that watch).
 * Every 5 s a line "pm: idle": for each core the share of the time it spent in each state, the entries, how many came back
 * through the reset vector, how many were refused or came back at once.
 */
#define PM_STATES       4               /* 0 WFI, 1 pc, 2 rail-pc, 3 rail-pc with the cluster's l3-pc */
#define PM_REFUSALS     3
#define PM_WATCH_MS     4
#define PM_SLACK_US     8000            /* how long past its deadline a down core is given before it is woken by hand */
#define PM_LOST_LIMIT   5
#define PM_REPORT_S     5
#define PM_TYPE_POWER_DOWN      (1u << 30)
#define PM_AFFINITY_SHIFT       24

struct pm_level {
	uint32_t state_id;              /* cpu mode | cluster mode << 4 */
	uint32_t aff;                   /* affinity level of the state: 1 when the cluster goes too */
	uint32_t exit_us, min_us;
};

/* [group][state 1..3]: the vendor's qcom,pm-cpu@0 (the six small cores), @1 (the two big ones), and the cluster level "l3-pc" */
static const struct pm_level pm_levels[2][PM_STATES] = {
	{ { 0, 0, 0, 0 }, { 0x3, 0, 901, 1774 }, { 0x4, 0, 915, 4001 }, { 0x4 | (0x4 << 4), 1, 3048, 6118 } },
	{ { 0, 0, 0, 0 }, { 0x3, 0, 1244, 2207 }, { 0x4, 0, 1854, 5555 }, { 0x4 | (0x4 << 4), 1, 3048, 6118 } },
};
static const char *const pm_state_names[PM_STATES] = { "wfi", "pc", "rail", "clus" };

static struct pm_cpu {
	volatile uint32_t state;        /* the state the core is in now (0: running or in WFI) */
	volatile uint64_t deadline;     /* the next timer when it went down, UINT64_MAX: none */
	uint64_t entered_at;
	uint64_t ticks[PM_STATES];      /* all the time spent in each state (the report takes the difference) */
	uint32_t entries[PM_STATES];
	uint32_t resets, quick, refused, kicks, wakes;
	uint32_t refusals[PM_STATES];
	bool off[PM_STATES];
	uint64_t ewma_ns;               /* the last waits, smoothed */
	uint64_t cntv_cval;             /* the virtual timer's deadline, saved across a power-down */
	uint32_t group;
} pm_cpu[MAX_CPUS] __attribute__((aligned(128)));

static uint32_t pm_mode;                        /* boot-arg pm_idle */
static volatile bool pm_ready, pm_broken;
static uint64_t pm_ticks_per_us_x16;            /* (not 0 once ready) */

extern void gic_v3_irq_signal(uint32_t phys_id);
extern void pe_init_fiq(void);
extern int64_t psci_smp_suspend(uint32_t power_state, uint64_t entry, uint64_t context);
extern int64_t psci_smp_feature(uint32_t function);
extern int hakios_board_cpu_group_of(unsigned core);
extern void loader_log_line(const char *line);

/* (to the loader's log, which the phone's log is made from; longer than hakios_tl()'s 192 bytes) */
#define pm_say(...) \
	do { \
		char pm_line[480]; \
		snprintf(pm_line, sizeof(pm_line), __VA_ARGS__); \
		loader_log_line(pm_line); \
	} while (0)

static inline uint64_t
pm_us_to_ticks(uint64_t us)
{
	return us * pm_ticks_per_us_x16 / 16;
}

static void
pm_account(struct pm_cpu *c, uint64_t now)
{
	uint64_t spent = now - c->entered_at;
	uint32_t state = c->state;

	c->ticks[state] += spent;
	if (state != 0 || spent != 0) {
		uint64_t ns;

		absolutetime_to_nanoseconds(spent, &ns);
		c->ewma_ns = (7 * c->ewma_ns + MIN(ns, 20ULL * NSEC_PER_MSEC)) / 8;
	}
	c->state = 0;
	c->deadline = UINT64_MAX;
	c->entered_at = now;
}

/*
 * The state wanted by a core about to wait: the deepest one whose minimum residency the expected wait covers
 * (state 0: WFI).
 */
static uint32_t
pm_choose(int cpu, struct pm_cpu *c, uint64_t now, uint64_t next)
{
	const struct pm_level *lv = pm_levels[c->group];
	uint64_t expect = next > now ? next - now : 0, mine = pm_us_to_ticks(2 * c->ewma_ns / 1000 + 500);
	uint32_t want = 0;

	if (expect > mine) {
		expect = mine;
	}
	for (uint32_t s = 1; s <= (pm_mode >= 2 ? 3u : 1u); s++) {
		if (!c->off[s] && expect >= pm_us_to_ticks(lv[s].min_us)) {
			want = s;
		}
	}
	if (want == 3) {
		/* the cluster goes too only when every other core of the kernel is down to rail-pc or deeper already */
		for (int other = 0; other <= ml_get_max_cpu_number(); other++) {
			if (other != cpu && other != master_processor->cpu_id && cpu_datap(other) != NULL && pm_cpu[other].state < 2) {
				want = 2;
				break;
			}
		}
	}
	return want;
}

/* In place of the WFI of cpu_idle() (interrupts masked; what it does afterwards is cpu_idle_exit()'s). */
void hakios_idle_enter(cpu_data_t *cdp);
void
hakios_idle_enter(cpu_data_t *cdp)
{
	int cpu = cdp->cpu_number;
	struct pm_cpu *c = &pm_cpu[cpu];
	uint64_t now = mach_absolute_time();
	uint32_t want = 0;

	if (pm_mode == 0 || !pm_ready || pm_broken || cpu == master_processor->cpu_id) {
		__builtin_arm_wfi();
		return;
	}
	want = pm_choose(cpu, c, now, cdp->rtcPop);
	c->entered_at = now;
	c->deadline = cdp->rtcPop > now ? cdp->rtcPop : UINT64_MAX;
	if (want == 0) {
		c->state = 0;
		__builtin_arm_wfi();
		pm_account(c, mach_absolute_time());
		c->entries[0]++;
		return;
	}
	{
		const struct pm_level *lv = &pm_levels[c->group][want];
		uint32_t power_state = PM_TYPE_POWER_DOWN | (lv->aff << PM_AFFINITY_SHIFT) | lv->state_id;
		int64_t status;

		c->cntv_cval = __builtin_arm_rsr64("CNTV_CVAL_EL0");
		c->entries[want]++;
		CleanPoC_DcacheRegion((vm_offset_t)cdp, sizeof(cpu_data_t));    /* (what the reset vector reads with the MMU off) */
		__builtin_arm_dsb(DSB_SY);
		c->state = want;
		status = psci_smp_suspend(power_state, ml_static_vtop((vm_offset_t)&LowResetVectorBase), 0);
		/* here only when the core did not lose its context: an interrupt was pending, or the firmware said no */
		now = mach_absolute_time();
		if (status < 0) {
			c->refused++;
			if (++c->refusals[want] >= PM_REFUSALS) {
				c->off[want] = true;
			}
			c->state = 0;
			c->entries[want]--;
			c->entered_at = now;
			__builtin_arm_wfi();
			pm_account(c, mach_absolute_time());
			return;
		}
		c->refusals[want] = 0;
		c->quick++;
		pm_account(c, now);
	}
}

/* From arm_init_idle_cpu(): this core came back from a power-down; its MMU is on again and the kernel's vectors set. */
void hakios_idle_resume(void);
void
hakios_idle_resume(void)
{
	cpu_data_t *cdp = getCpuDatap();
	struct pm_cpu *c = &pm_cpu[cdp->cpu_number];

	pe_init_fiq();                  /* the interrupt controller's interface: gic_v3_irq_cpu_init() */
	__builtin_arm_wsr64("CNTV_CVAL_EL0", c->cntv_cval);
	__builtin_arm_isb(ISB_SY);
	c->resets++;
	pm_account(c, mach_absolute_time());
}

static void
pm_report(uint64_t *last_ticks, uint32_t *last_entries, uint64_t span)
{
	char line[420];
	size_t at = 0;
	uint32_t resets = 0, quick = 0, refused = 0, kicks = 0;

	for (int cpu = 0; cpu <= ml_get_max_cpu_number() && at < sizeof(line) - 90; cpu++) {
		struct pm_cpu *c = &pm_cpu[cpu];

		if (cpu_datap(cpu) == NULL) {
			continue;
		}
		at += (size_t)snprintf(line + at, sizeof(line) - at, " c%d", cpu);
		for (uint32_t s = 0; s < PM_STATES; s++) {
			uint64_t t = c->ticks[s] - last_ticks[cpu * PM_STATES + s];
			uint32_t n = c->entries[s] - last_entries[cpu * PM_STATES + s];

			last_ticks[cpu * PM_STATES + s] = c->ticks[s];
			last_entries[cpu * PM_STATES + s] = c->entries[s];
			if (n != 0 || t != 0) {
				at += (size_t)snprintf(line + at, sizeof(line) - at, " %s %llu%%/%u", pm_state_names[s], 100 * t / span, n);
			}
		}
		resets += c->resets;
		quick += c->quick;
		refused += c->refused;
		kicks += c->kicks;
	}
	pm_say("pm: idle (mode %u, share of %us / entries):%s; resumed from reset %u, came back at once %u, refused %u, woken by hand %u%s\n",
	    pm_mode, PM_REPORT_S, line, resets, quick, refused, kicks, pm_broken ? " TIMER LOST: deep states off" : "");
}

static void
pm_watch_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint64_t ticks[MAX_CPUS * PM_STATES] = { 0 }, last_report, period;
	uint32_t entries[MAX_CPUS * PM_STATES] = { 0 }, lost = 0;

	thread_bind(master_processor);
	thread_block(THREAD_CONTINUE_NULL);
	nanoseconds_to_absolutetime((uint64_t)PM_REPORT_S * NSEC_PER_SEC, &period);
	last_report = mach_absolute_time();
	for (;;) {
		uint64_t now;

		delay_for_interval(PM_WATCH_MS, NSEC_PER_MSEC);
		now = mach_absolute_time();
		for (int cpu = 0; cpu <= ml_get_max_cpu_number(); cpu++) {
			struct pm_cpu *c = &pm_cpu[cpu];
			uint64_t dl = c->deadline;

			if (c->state != 0 && dl != UINT64_MAX && now > dl + pm_us_to_ticks(PM_SLACK_US) && cpu_datap(cpu) != NULL) {
				c->kicks++;
				c->deadline = UINT64_MAX;       /* (once for each wait) */
				gic_v3_irq_signal(cpu_datap(cpu)->cpu_phys_id);
				if (++lost >= PM_LOST_LIMIT && !pm_broken) {
					pm_broken = true;
					pm_say("pm: idle: TIMER LOST: cores that went down did not wake at their deadline %u times in a row: "
					    "the deep states are off\n", lost);
				}
			}
		}
		if (now - last_report >= period) {
			uint64_t span = now - last_report;

			if (lost != 0 && lost < PM_LOST_LIMIT) {
				lost = 0;
			}
			pm_report(ticks, entries, span ? span : 1);
			last_report = now;
		}
	}
}

/* From the board's init, after the cores are up: reads pm_idle=, asks whether the firmware has CPU_SUSPEND, starts the watch. */
void hakios_pm_idle_init(void);
void
hakios_pm_idle_init(void)
{
	DTEntry chosen;
	void const *prop = NULL;
	unsigned size = 0;
	bool davinci = false;
	uint32_t mode = 0;
	const ml_topology_info_t *topology = ml_get_topology_info();
	thread_t thread;

	if (!PE_parse_boot_argn("pm_idle", &mode, sizeof(mode)) || mode == 0) {
		return;
	}
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	    SecureDTGetProperty(chosen, "darwin-board", &prop, &size) == kSuccess && size >= 8) {
		davinci = strncmp((const char *)prop, "davinci", 7) == 0;
	}
	if (!davinci) {
		pm_say("pm: idle: levels are known only for the davinci (Mi 9T): WFI only\n");
		return;
	}
	if (psci_smp_feature(0xC4000001u) < 0) {
		pm_say("pm: idle: the firmware has no CPU_SUSPEND: WFI only\n");
		return;
	}
	if (topology == NULL || ml_get_cpu_count() < 2) {
		return;
	}
	pm_ticks_per_us_x16 = (uint64_t)gPEClockFrequencyInfo.timebase_frequency_hz * 16 / 1000000;
	for (unsigned i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
		int g = hakios_board_cpu_group_of((topology->cpus[i].phys_id >> 8) & 0xff);

		pm_cpu[i].group = g == 1 ? 1 : 0;
		pm_cpu[i].deadline = UINT64_MAX;
		pm_cpu[i].ewma_ns = 0;      /* (a core that has not waited yet: WFI until it has) */
	}
	pm_mode = mode;
	if (kernel_thread_start(pm_watch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "pm-idle");
		thread_deallocate(thread);
	}
	pm_ready = true;
	pm_say("pm: idle: mode %u: cores wait in PSCI CPU_SUSPEND states (pc %#x, rail-pc %#x, cluster l3-pc %#x), WFI below their minimum residency; "
	    "the boot processor %d stays in WFI and watches\n", mode, pm_levels[0][1].state_id, pm_levels[0][2].state_id,
	    pm_levels[0][3].state_id, master_processor->cpu_id);
}
#endif /* defined(GENERIC_PSCI_SMP) */
