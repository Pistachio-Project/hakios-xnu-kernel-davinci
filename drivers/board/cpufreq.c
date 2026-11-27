/*
 * The clock speed of the processor cores the kernel runs on: the feature "cpufreq-epss", on every Snapdragon since
 * the SDM845.
 *
 *   cpufreq=1      every group of cores that has a core of ours is asked for the top row of its table
 *
 * The boot loader leaves each group of cores at a modest frequency and expects the operating
 * system to ask for more. That is one register write: the hardware (EPSS on the SM8350 and every
 * chip after it, the OSM's firmware on the SDM845 to the SM8250) has a table of frequencies per
 * group and switches the clock source and the supply voltage by itself when told which row is
 * wanted. There is no reason for us ever to want less than the
 * top row, for every group that has a core of ours in it (one core, unless boot-arg cpus=N
 * says more).
 *
 * Register layouts: board/cpufreq_hw.h (Linux drivers/cpufreq/qcom-cpufreq-hw.c, both of its variants). A group
 * whose hardware is not in charge of its clock (the block's enable bit clear) is left alone, as Linux leaves it.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/board/input.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>  /* xnu-12377: the KMA flags and kmem_alloc moved to the internal header */

#include "kernel.h"
#include "board.h"
#include "cpufreq_hw.h"
#include "kconfig.h"
#include "log.h"
#include "mmio.h"
#include "time.h"

extern void hakios_tl(const char *format, ...) __printflike(1, 2);   /* the kernel's log beside the console (input.c) */

/* (the loader's log as well: this runs early, and the kernel's own log of a run keeps only its end) */
#define tell hakios_say

extern void hakios_pm_idle_init(void);

/*
 * Where the block is, which layout it has and which cores each of its groups ("domains") has is the board's
 * data (board/board.h; soc.dtsi, cpufreq { }): on the two phones this was written for, three EPSS groups of
 * 0x1000 at 0x18591000 -- four small cores, three big, one biggest.
 */
#define CPUFREQ_GROUPS          HAKIOS_CPU_GROUPS

static const struct cpufreq_hw_layout *layout = &cpufreq_hw_layouts[HAKIOS_CPUFREQ_EPSS];  /* (the board's, at the start) */

/* a group's block mapped, or 0 when the hardware is not in charge of the group's clock (Linux refuses such a group too) */
static vm_offset_t
base_of(uint32_t domain)
{
	const struct hakios_board *board = hakios_board();

	return ml_io_map(cpufreq_hw_domain(board, domain), cpufreq_hw_map_size(board));
}

static vm_offset_t
domain_map(uint32_t domain)
{
	vm_offset_t base = base_of(domain);

	return (rd(base + layout->enable) & 1) ? base : 0;
}

/* the row wanted of a group, written to every register that holds a vote of the group's (cpufreq_hw_votes: one, but per-core DCVS's) */
static void
vote(vm_offset_t base, uint32_t domain, uint32_t row)
{
	unsigned votes = cpufreq_hw_votes(hakios_board(), domain, layout->core_ctrl ? rd(base + layout->core_ctrl) : 0);

	for (unsigned core = 0; core < votes; core++) {
		wr(base + layout->perf_state + 4 * core, row);
	}
}

/*
 * the top row of a group's table (it ends by repeating its last row), and its frequency; and, for the log, each row's core
 * count as one digit (CPUFREQ_HW_CORE_COUNT: rows of count 1 are the ones Linux keeps out as turbo), "4441" for four rows
 */
static uint32_t
table_top(vm_offset_t base, uint32_t *top_khz, char counts[CPUFREQ_HW_ROWS + 1])
{
	uint32_t top = 0;

	*top_khz = 0;
	counts[0] = 0;
	for (uint32_t row = 0; row < CPUFREQ_HW_ROWS; row++) {
		uint32_t entry = rd(base + cpufreq_hw_row(layout, row));
		uint32_t khz = cpufreq_hw_khz(entry);

		if (row > 0 && khz <= *top_khz) {
			break;
		}
		top = row;
		*top_khz = khz;
		counts[row] = (char)('0' + CPUFREQ_HW_CORE_COUNT(entry));
		counts[row + 1] = 0;
	}
	return top;
}

/* How fast a simple loop runs, in thousands of iterations per millisecond. */
static uint32_t
loop_speed(void)
{
	volatile uint64_t sink = 0;
	uint64_t start = mach_absolute_time(), nanos;

	for (uint32_t i = 0; i < 2000000; i++) {
		sink += i;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	return nanos ? (uint32_t)(2000000ULL * 1000 / nanos) : 0;
}

/*
 * The group of the core with that number in its MPIDR (the device tree's "reg": the number is the second
 * field), by the board's data; -1 for a core the board's groups do not reach.
 */
static int
domain_of(uint32_t phys_id)
{
	return hakios_board_cpu_group_of((phys_id >> 8) & 0xff);
}

static void
raise_domain(uint32_t domain, uint32_t cores)
{
	uint32_t top = 0, top_khz = 0, was;
	char counts[CPUFREQ_HW_ROWS + 1];
	bool turbo = false;
	vm_offset_t base;

	base = domain_map(domain);
	/* (for a phone's log, whichever way it goes: the words that say who is in charge, and the row wanted as the boot loader left it) */
	tell("cpufreq: group %u: enable %#x, per-core control %#x, row wanted %u\n", domain,
	    rd(base_of(domain) + layout->enable), layout->core_ctrl ? rd(base_of(domain) + layout->core_ctrl) : 0,
	    rd(base_of(domain) + layout->perf_state));
	if (base == 0) {
		tell("cpufreq: group %u (%u of our cores): the hardware is not in charge of its clock: left alone\n", domain, cores);
		return;
	}
	top = table_top(base, &top_khz, counts);
	was = rd(base + layout->perf_state);
	vote(base, domain, top);
	sleep_ms(20);
	tell("cpufreq: group %u (%u of our cores): row %u -> %u of the frequency table, %u MHz\n", domain, cores, was, top,
	    top_khz / 1000);
	/* (only looked at: whether the top rows are ones Linux would leave out, which a phone's log is to tell) */
	for (unsigned i = 0; counts[i] != 0; i++) {
		turbo |= counts[i] == '0' + CPUFREQ_HW_TURBO;
	}
	tell("cpufreq: group %u: the rows' core counts %s%s\n", domain, counts, turbo ? " (1: a row Linux leaves out as turbo)" : "");
}

/*
 * The L3 cache's clock, for a look (boot-arg l3=1: raised to its top row; l3_probe=1: its table, the block's words that
 * are not 0, and a load chasing through 2 MB -- past L2, inside L3 -- before and after). Its EPSS block, the device
 * tree's epss_l3_cpu ("qcom,lahaina-epss-l3-cpu", reg 0x18590000 0x4000), sits just before the cores' groups and has
 * their layout: the table at +0x100 (300-1516 MHz here), the row wanted at +0x320 (upstream Linux's sc7280/sm8250
 * epss-l3), which the boot loader leaves at row 0. Raising it changed nothing measured: the chase 7.28 ns against 7.21
 * after, SpringBoard's turn after a tap on an icon 188-193 ms against 190-230 (logs/182753-run.log in hakios-sm7325;
 * a first look, 8.11 against 6.83, did not come again). So it is left where the boot loader put it.
 */
#define L3_EPSS                 0x18590000
#define L3_OSM_SM7150           0x18321000
#define L3_CHASE_BYTES          (2u << 20)
#define L3_CHASE_STEPS          (1u << 20)

static uint64_t
l3_chase_ns(vm_offset_t buffer)
{
	uint64_t began, t = 0;
	volatile uint64_t *at = (volatile uint64_t *)buffer;

	for (uint32_t i = 0; i < L3_CHASE_STEPS / 8; i++) {
		at = (volatile uint64_t *)(uintptr_t)*at;       /* (warm) */
	}
	began = mach_absolute_time();
	for (uint32_t i = 0; i < L3_CHASE_STEPS; i++) {
		at = (volatile uint64_t *)(uintptr_t)*at;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
	(void)at;
	return t * 100 / L3_CHASE_STEPS;        /* hundredths of a ns a load */
}

static void
l3_raise(void)
{
	const struct cpufreq_hw_layout *l3 = &cpufreq_hw_layouts[HAKIOS_CPUFREQ_EPSS];  /* (EPSS's; an OSM chip's below) */
	vm_offset_t l3_pa = L3_EPSS;
	uint32_t enabled = 0, probe = 0, top = 0, top_khz = 0, was, lines = L3_CHASE_BYTES / 64, seed = 12345;
	vm_offset_t base, buffer = 0;
	char line[600];
	int used = 0;

	/* SM7150 (davinci): raised by default (l3=0 leaves it): the boot loader leaves row 7, 1190 MHz; at 1459 a load
	 * through 2 MB took 89.8 ns against 97.1 (logs/135511-run.log) */
	if (!PE_parse_boot_argn("l3", &enabled, sizeof(enabled)) && hakios_board()->cpufreq.block == HAKIOS_CPUFREQ_OSM &&
	    hakios_board()->cpufreq.epss == L3_OSM_SM7150 + 0x2000) {
		enabled = 1;
	}
	PE_parse_boot_argn("l3_probe", &probe, sizeof(probe));
	if (enabled == 0 && probe == 0) {
		return;
	}
	if (hakios_board()->cpufreq.block == HAKIOS_CPUFREQ_OSM && hakios_board()->cpufreq.epss == L3_OSM_SM7150 + 0x2000) {
		/* SM7150 (davinci): the L3's own OSM block 0x2000 before the cores' (Linux osm-l3.c: the table at 0x110, 32 a row, the row wanted at 0x920) */
		l3 = &cpufreq_hw_layouts[HAKIOS_CPUFREQ_OSM];
		l3_pa = L3_OSM_SM7150;
	} else if (hakios_board()->cpufreq.epss != L3_EPSS + 0x1000) {
		tell("l3: not this SoC's layout (the cores' EPSS at %#x): left alone\n", hakios_board()->cpufreq.epss);
		return;
	}
	base = ml_io_map(l3_pa, 0x1000);
	for (uint32_t row = 0; row < CPUFREQ_HW_ROWS; row++) {
		uint32_t khz = cpufreq_hw_khz(*(volatile uint32_t *)(base + cpufreq_hw_row(l3, row)));

		if (row > 0 && khz <= top_khz) {
			break;
		}
		top = row;
		top_khz = khz;
		used += snprintf(line + used, sizeof(line) - (size_t)used, " %u:%u", row, khz / 1000);
	}
	if (probe) {
		tell("l3: frequency table (row:MHz)%s\n", line);
		used = 0;
		for (uint32_t off = 0; off < 0x400 && used < (int)sizeof(line) - 24; off += 4) {
			uint32_t w;

			if (off >= 0x100 && off < 0x300) {
				continue;
			}
			w = *(volatile uint32_t *)(base + off);
			if (w != 0) {
				used += snprintf(line + used, sizeof(line) - (size_t)used, " %x=%x", off, w);
			}
		}
		tell("l3: words not 0:%s\n", line);
		if (kmem_alloc(kernel_map, &buffer, L3_CHASE_BYTES, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) == KERN_SUCCESS) {
			/* a random cycle through every 64-byte line: each load waits for the one before */
			static uint32_t perm[L3_CHASE_BYTES / 64];

			for (uint32_t i = 0; i < lines; i++) perm[i] = i;
			for (uint32_t i = lines - 1; i > 0; i--) {
				uint32_t j, t;

				seed = seed * 1103515245u + 12345u;
				j = (seed >> 8) % (i + 1);
				t = perm[i]; perm[i] = perm[j]; perm[j] = t;
			}
			for (uint32_t i = 0; i < lines; i++) {
				*(uint64_t *)(buffer + 64ull * perm[i]) = buffer + 64ull * perm[(i + 1) % lines];
			}
			tell("l3: a load through 2 MB: %llu/100 ns\n", l3_chase_ns(buffer));
		}
	}
	was = *(volatile uint32_t *)(base + l3->perf_state);
	if (enabled != 0) {
		*(volatile uint32_t *)(base + l3->perf_state) = top;
		delay_for_interval(20, NSEC_PER_MSEC);
		tell("l3: row %u -> %u of its frequency table, %u MHz\n", was, top, top_khz / 1000);
	}
	if (buffer != 0) {
		if (enabled != 0) {
			tell("l3: a load through 2 MB now: %llu/100 ns\n", l3_chase_ns(buffer));
		}
		kmem_free(kernel_map, buffer, L3_CHASE_BYTES);
	}
}

/*
 * The system cache (LLCC: between the cores' L3 and the memory, shared with the GPU and the rest; Linux
 * drivers/soc/qcom/llcc-qcom.c with sc7280's table, the device tree's "qcom,sc7280-llcc" at 0x9200000, 0x9280000
 * and the broadcast block at 0x9600000). Nothing here programmed it: whether the processors' slice is in use is
 * whatever the boot loader left. Boot-arg llcc=1: a look (the block's version, its banks, each slice's attributes
 * and state); llcc=2: the processors' slice (1) and the one of the SMMU's table walks (13) programmed as Linux does
 * and activated, the GPU's two (11, 12) programmed only (activating those is the GPU's own business), and a load
 * chasing through 1-6 MB before and after. The slices activated here are taken back on the way to the loader.
 * What it found here (logs/194716-run.log in hakios-sm7325): version 2.0, two banks, no slice programmed or active.
 * With the processors' slice on, the kernel's chase through 3 MB took 20.7 ns a load against 25.8 (1-2 MB and 4-6 MB
 * the same), but nothing else measured moved: SpringBoard's turn after a tap on an icon 180-190 ms against 196-197,
 * the gap before the opening animation 225-233 ms against 192-242. So it is left off, as the L3's clock is.
 */
#define LLCC_BANK0              0x09200000
#define LLCC_BCAST              0x09600000
#define LLCC_SIZE               0x58000
#define LLCC_HW_INFO            0x30000         /* (broadcast) the version: 0xMMmmpp00 */
#define LLCC_STATUS0            0x3000c         /* (a bank) [31:28] the number of banks */
#define LLCC_ACT_CTRL(n)        ((n) * 0x1000)  /* [0] trigger, [2:1] opcode: 1 activate, 2 deactivate */
#define LLCC_ACT_STATUS(n)      (4 + (n) * 0x1000)      /* [0] active, [1] inactive */
#define LLCC_ATTR0(n)           (0x21000 + 8 * (n))     /* [15:0] reserved ways, [31:16] bonus ways */
#define LLCC_ATTR1(n)           (0x21004 + 8 * (n))     /* [1:0] mode, [2] probe target, [3] fixed, [6:4] priority, [31:16] lines a bank */
#define LLCC_DIS_CAP_ALLOC      0x21f00
#define LLCC_PCB_ACT            0x21f04         /* a bit a slice: kept through a power collapse */
#define LLCC_WRSC_EN            0x21f20
#define LLCC_WRSC_CACHEABLE_EN  0x21f2c
#define LLCC_CHASE_MAX          (6u << 20)

struct llcc_slice {
	uint8_t id, priority, fixed, retain, activate;
	uint16_t kb;
};
static const struct llcc_slice llcc_slices_sc7280[] = {
	{ 1, 1, 0, 1, 1, 768 },         /* CPUSS: the processors */
	{ 11, 1, 1, 1, 0, 256 },        /* GPUHTW: the GPU's SMMU table walks */
	{ 12, 1, 0, 1, 0, 512 },        /* GPU */
	{ 13, 1, 1, 0, 1, 256 },        /* MMUHWT: the system SMMU's table walks */
};
/*
 * SM7150 (davinci): msm-4.14's drivers/soc/qcom/llcc-sdmmagpie.c sdmmagpie_data[] (SCT_ENTRY: usecase = slice id,
 * max_cap KB, priority, fixed_size, bonus 0xF, res 0, cache_mode 0, probe target 0, dis_cap_alloc 0, retain_on_pc 1,
 * activate_on_init). Only the slices of a user here: cpuss 1 (512 KB, activate_on_init), gpuhtw 11 (256 KB, fixed),
 * gpu 12 (256 KB, fixed); modem 8 and npu 23 are not touched. The GPU's two are activated here with the cpuss one
 * (the vendor's kgsl activates "gpu" when the 3D core starts and "gpuhtw" with its SMMU; the registers the GPU
 * needs to use them are written by adreno_a618.c, which asks hakios_llcc_gpu_scids()).
 */
static const struct llcc_slice llcc_slices_sm7150[] = {
	{ 1, 1, 0, 1, 1, 512 },         /* cpuss: the processors */
	{ 11, 1, 1, 1, 1, 256 },        /* gpuhtw: the GPU's SMMU table walks */
	{ 12, 1, 1, 1, 1, 256 },        /* gpu */
};
static const struct llcc_slice *llcc_slices = llcc_slices_sc7280;
static unsigned llcc_slice_count = sizeof(llcc_slices_sc7280) / sizeof(llcc_slices_sc7280[0]);
static bool llcc_sm7150;
static unsigned llcc_gpu_done;          /* bit 0: slice 11 activated, bit 1: slice 12 (SM7150) */
static vm_offset_t llcc_bcast;
static bool llcc_activated;

static void
llcc_chain(vm_offset_t buffer, uint32_t *perm, uint32_t bytes)
{
	uint32_t lines = bytes / 64, seed = 12345;

	for (uint32_t i = 0; i < lines; i++) perm[i] = i;
	for (uint32_t i = lines - 1; i > 0; i--) {
		uint32_t j, t;

		seed = seed * 1103515245u + 12345u;
		j = (seed >> 8) % (i + 1);
		t = perm[i]; perm[i] = perm[j]; perm[j] = t;
	}
	for (uint32_t i = 0; i < lines; i++) {
		*(uint64_t *)(buffer + 64ull * perm[i]) = buffer + 64ull * perm[(i + 1) % lines];
	}
}

static void
llcc_chase_all(vm_offset_t buffer, uint32_t *perm, const char *when)
{
	static const uint32_t sizes[] = { 1u << 20, 2u << 20, 3u << 20, 4u << 20, 6u << 20 };
	char line[200];
	int used = 0;

	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		llcc_chain(buffer, perm, sizes[i]);
		used += snprintf(line + used, sizeof(line) - (size_t)used, " %u KB %llu/100 ns,", sizes[i] >> 10,
		    l3_chase_ns(buffer));
	}
	tell("llcc: a load chasing through%s %s\n", line, when);
}

static bool
llcc_act(uint32_t id, bool activate)
{
	volatile uint32_t *ctrl = (volatile uint32_t *)(llcc_bcast + LLCC_ACT_CTRL(id));
	volatile uint32_t *status = (volatile uint32_t *)(llcc_bcast + LLCC_ACT_STATUS(id));
	uint32_t opcode = activate ? 1u << 1 : 2u << 1, waiting_for = activate ? 2u : 1u;

	*ctrl = opcode | 1;             /* the trigger up, then down (Linux: llcc_update_act_ctrl()) */
	*ctrl = opcode;
	for (int i = 0; i < 2000; i++) {        /* (reads only, no delay: this runs with interrupts off as well) */
		if ((*status & waiting_for) == 0) {
			return true;
		}
	}
	return false;
}

static void
llcc_setup(void)
{
	uint32_t mode = 0, version, banks;
	vm_offset_t bank0, buffer = 0;
	uint32_t *perm = NULL;

	PE_parse_boot_argn("llcc", &mode, sizeof(mode));
	if (mode == 0) {
		return;
	}
	/* (an SM8250's OSM is where an SM7325's EPSS is, and its slices are not these) */
	if (hakios_board()->cpufreq.block == HAKIOS_CPUFREQ_OSM && hakios_board()->cpufreq.epss == L3_OSM_SM7150 + 0x2000) {
		/* SM7150: the same addresses as the SC7280's (sdmmagpie.dtsi qcom,llcc@9200000: reg 0x9200000 0x450000,
		 * banks-off 0x0 0x80000, broadcast-off 0x400000 = 0x9600000), its own slices */
		llcc_slices = llcc_slices_sm7150;
		llcc_slice_count = sizeof(llcc_slices_sm7150) / sizeof(llcc_slices_sm7150[0]);
		llcc_sm7150 = true;
	} else if (hakios_board()->cpufreq.epss != L3_EPSS + 0x1000 || hakios_board()->cpufreq.block != HAKIOS_CPUFREQ_EPSS) {
		tell("llcc: not this SoC (the cores' %s at %#x): left alone\n", layout->name, hakios_board()->cpufreq.epss);
		return;
	}
	llcc_bcast = ml_io_map(LLCC_BCAST, LLCC_SIZE);
	bank0 = ml_io_map(LLCC_BANK0, LLCC_SIZE);
	version = *(volatile uint32_t *)(llcc_bcast + LLCC_HW_INFO);
	banks = *(volatile uint32_t *)(bank0 + LLCC_STATUS0) >> 28;
	tell("llcc: version %#x, %u banks; kept through power collapse %#x, writes' slices %#x/%#x, no capacity %#x\n",
	    version, banks, *(volatile uint32_t *)(llcc_bcast + LLCC_PCB_ACT), *(volatile uint32_t *)(llcc_bcast + LLCC_WRSC_EN),
	    *(volatile uint32_t *)(llcc_bcast + LLCC_WRSC_CACHEABLE_EN), *(volatile uint32_t *)(llcc_bcast + LLCC_DIS_CAP_ALLOC));
	for (uint32_t id = 0; id < 32; id++) {
		uint32_t a0 = *(volatile uint32_t *)(llcc_bcast + LLCC_ATTR0(id)), a1 = *(volatile uint32_t *)(llcc_bcast + LLCC_ATTR1(id));
		uint32_t st = *(volatile uint32_t *)(llcc_bcast + LLCC_ACT_STATUS(id));

		if (a0 != 0 || a1 != 0 || (st & 1) != 0) {
			tell("llcc: slice %2u: %s, ways %#x+%#x, priority %u%s, %u KB\n", id, st & 1 ? "ACTIVE" : st & 2 ? "inactive" : "state 0",
			    a0 & 0xffff, a0 >> 16, (a1 >> 4) & 7, a1 & 8 ? " fixed" : "", (a1 >> 16) * 64 * (banks ? banks : 1) / 1024);
		}
	}
	if (mode != 2) {
		return;
	}
	if (version >= 0x04010000 || banks == 0 || banks > 4) {
		tell("llcc: not the block this was written for: left alone\n");
		return;
	}
	if (kmem_alloc(kernel_map, &buffer, LLCC_CHASE_MAX, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, (vm_offset_t *)&perm, LLCC_CHASE_MAX / 64 * sizeof(uint32_t), KMA_KOBJECT | KMA_DATA,
	    VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		if (buffer != 0) {
			kmem_free(kernel_map, buffer, LLCC_CHASE_MAX);
		}
		buffer = 0;
	}
	if (buffer != 0) {
		llcc_chase_all(buffer, perm, "before");
	}
	for (unsigned i = 0; i < llcc_slice_count; i++) {
		const struct llcc_slice *s = &llcc_slices[i];
		uint32_t lines = s->kb * 1024u / banks / 64, bit = 1u << s->id;

		*(volatile uint32_t *)(llcc_bcast + LLCC_ATTR1(s->id)) = (uint32_t)s->fixed << 3 | (uint32_t)s->priority << 4 | lines << 16;
		*(volatile uint32_t *)(llcc_bcast + LLCC_ATTR0(s->id)) = 0x3fu << 16;
		*(volatile uint32_t *)(llcc_bcast + LLCC_DIS_CAP_ALLOC) &= ~bit;
		*(volatile uint32_t *)(llcc_bcast + LLCC_PCB_ACT) = (*(volatile uint32_t *)(llcc_bcast + LLCC_PCB_ACT) & ~bit) |
		    (s->retain ? bit : 0);
		if (version >= 0x02000000) {
			*(volatile uint32_t *)(llcc_bcast + LLCC_WRSC_EN) &= ~bit;
		}
		if (version >= 0x02010000) {
			*(volatile uint32_t *)(llcc_bcast + LLCC_WRSC_CACHEABLE_EN) &= ~bit;
		}
		if (s->activate) {
			bool done = llcc_act(s->id, true);

			llcc_activated = llcc_activated || done;
			if (llcc_sm7150 && done && (s->id == 11 || s->id == 12)) {
				llcc_gpu_done |= s->id == 11 ? 1u : 2u;
			}
			tell("llcc: slice %u (%u KB) activated: %s, state %#x\n", s->id, s->kb, done ? "yes" : "NO",
			    *(volatile uint32_t *)(llcc_bcast + LLCC_ACT_STATUS(s->id)));
		}
	}
	if (buffer != 0) {
		llcc_chase_all(buffer, perm, "after");
		kmem_free(kernel_map, buffer, LLCC_CHASE_MAX);
		kmem_free(kernel_map, (vm_offset_t)perm, LLCC_CHASE_MAX / 64 * sizeof(uint32_t));
	}
}

/* For adreno_a618.c (the cpufreq steps run first): the GPU's two slices are on (llcc=2 on SM7150), and what they are. */
bool
hakios_llcc_gpu_scids(uint32_t *gpu, uint32_t *gpuhtw)
{
	if (llcc_gpu_done != 3) {
		return false;
	}
	*gpu = 12;
	*gpuhtw = 11;
	return true;
}

/* On the way back to the loader (interrupts off): the slices activated above, deactivated. */
static void
llcc_quiesce(void)
{
	if (!llcc_activated) {
		return;
	}
	llcc_activated = false;
	for (unsigned i = 0; i < llcc_slice_count; i++) {
		if (llcc_slices[i].activate) {
			(void)llcc_act(llcc_slices[i].id, false);
		}
	}
	tell("llcc: the slices activated at boot deactivated for the loader\n");
}

/*
 * The small cores parked while the big ones have room (boot-arg park_small=0: not). The scheduler of this kernel
 * (Clutch, on a configuration without SMT) treats every core alike, so the window server's and SpringBoard's threads
 * landed on the Cortex-A55s as often as anywhere, some two and a half times slower than on a Cortex-A78 -- the 16.5
 * kernel made each small core the SMT sibling of a big one, which Clutch refuses ("The clutch scheduler does not
 * support CONFIG_SCHED_SMT"). Measured on the phone (rc.jobs.fps, the 0.7 s after each gesture): with the three small
 * cores, 47.8 frames a second on the home screen's pages and 19 gaps of 50 ms or more; with cpus=4, the big ones alone,
 * 51.5 and 6 (logs/110921-run.log, logs/112454-run.log). The small cores are not taken away, though: they are left out
 * of the scheduler's recommended cores (as Apple's performance controller does with cores it wants idle) while the big
 * ones have time to spare, and recommended again as soon as every big core was busy for most of PARK_MS -- a build or
 * a burst of compiles gets all seven, a gesture the four. The scheduler's own failsafe gives every core back to a
 * real-time thread that would otherwise wait.
 */
#define PARK_MS                 8               /* how often the big cores' load is looked at */
#define PARK_BUSY               85              /* % busy of the least busy big core: every big core is taken */
static uint32_t park_busy = PARK_BUSY;
#define PARK_CALM               60              /* % busy of the least busy big core under which the small ones go again */
#define PARK_CALM_TIMES         8               /* ... that many looks in a row (64 ms) */
extern void sched_perfcontrol_update_recommended_cores(uint32_t recommended_cores);
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);    /* osfmk/arm64/machine_routines.c (patches/0002) */

static struct {
	uint32_t looks, unparked, parks;        /* since the last line in the log */
} park_stat;
static bool park_wanted;

/*
 * The speed of each group of cores by its load (boot-arg cpu_gov=0: every group at its top row all along, as
 * before). With cpufreq=1 every group was raised to its top row and kept there: idle cores too stood at the top
 * row's voltage, and the SoC ran at 81-86 C on rc.jobs.fps (its thermal lines, logs/140658-run.log), where its
 * own limits hardware held the big cores to 2131 of 2400 MHz and the prime to 2208. Now, at every look (PARK_MS),
 * each group the kernel has all of is set to the lowest row of its table that covers what its busiest core did
 * at the speed it had, with GOV_HEADROOM_PCT to spare: up at once, to the top at once from GOV_UP_BUSY, and down
 * only after GOV_DOWN_LOOKS looks wanted it, to the most any of them wanted. A group with a core the kernel has
 * not got -- the small one, whose core 0 is the loader's, serving the flash while the kernel runs -- stays at
 * its top row. Not with thermal=2, which holds the rows back itself. At the run's end every group is put back
 * at its top row (hakios_ddr_quiesce, on every way back).
 */
#define GOV_HEADROOM_PCT        125
#define GOV_UP_BUSY             90
#define GOV_DOWN_LOOKS          4
#define GOV_ROWS                40
static struct gov_group {
	vm_offset_t base;               /* 0: not governed */
	uint32_t rows, khz[GOV_ROWS], top, row, down_looks, down_most;
	uint32_t cores;                 /* the kernel's cores in it, by cpu number */
	uint64_t looks_at[GOV_ROWS];    /* (for the log: how many looks at each row) */
	/* pm_gov=1 (below): */
	uint32_t cap;                   /* the thermal limiter's row, the most that may be asked (thermal=2) */
	uint32_t want;                  /* what the load asks for, before the caps */
	bool was_boosted;               /* the last look was inside a boost */
	bool foreign;                   /* a core of the group is the boot loader's: the load of it is not seen */
	uint64_t boosts, looks_pm;
} gov[CPUFREQ_GROUPS];
static bool gov_on;

static LCK_GRP_DECLARE(gov_lock_group, "hakios-cpugov");
static LCK_MTX_DECLARE(gov_lock, &gov_lock_group);

static void
gov_set(struct gov_group *g, uint32_t row)
{
	if (row != g->row) {
		vote(g->base, (uint32_t)(g - gov), row);
		g->row = row;
	}
}

/*
 * POWER MANAGEMENT, phase 2 (boot-arg pm_gov=1; default 0: the governor above, or the top rows). The same look every PARK_MS (and
 * every PM_SCREEN_OFF_MS with the screen off) asks for what schedutil asks: the frequency the busiest core of the group needs at
 * its current speed with a quarter to spare, at once when it is more, and, when it is less, only after PM_DOWN_LOOKS looks
 * and then half of the way down (a gentle ramp down, so that a frame after a quiet look does not find the clock at the floor). On top:
 *
 *   - a touch (hakios_touch_last, input.c: every sample of /dev/hakios-touch) holds each group at no less than pm_boost_pct (default
 *     60) of its top frequency for pm_boost_ms (default 120), as Android's input boost does: the first frame after a touch is
 *     the one the finger sees;
 *   - a group whose busiest core is over PM_JUMP_BUSY goes to its top row (the interactive governor's "go hispeed");
 *   - a group with a core of the boot loader's (the small cores' group: core 0 serves the flash while the kernel runs and its load is not
 *     seen) is held at no less than pm_foreign_pct (default 45) of its top frequency;
 *   - the thermal limiter (thermal=2) no longer writes the rows: it hands its row to hakios_cpufreq_pm_cap() as the most allowed;
 *   - the screen off (pm_screen=1, below) caps the groups.
 * Every 5 s a line "pm: gov" with the share of the looks each group spent at each frequency, its mean frequency, the boosts.
 * Without a current sensor in the kernel the mean frequency and the share of the time at the low rows are the numbers to compare
 * runs by, with the temperature lines of thermal.c ("tl: h").
 */
#define PM_DOWN_LOOKS           12              /* (x PARK_MS: ~100 ms of calm before a step down: slow down) */
#define PM_JUMP_BUSY            75
#define PM_HEADROOM_PCT         150             /* the speed asked for is 1.5 x what the busiest core used: it runs at ~65% at most */
#define PM_SCREEN_OFF_MS        25
extern bool hakios_ddr_pm_on(void);
extern void hakios_ddr_pm_look(uint32_t mean, uint32_t big, bool touch, bool gpu);
extern uint32_t hakios_adreno_idle_ms(void);
static bool pm_ddr_on;                  /* pm_ddr=1 and the vote was made (ddr.c) */
static bool pm_gov;
/*
 * Defaults (the phone's UI first): a touch holds both groups at the top row (100%) for 250 ms after the last sample; no group below
 * pm_floor_pct of its top outside the dark screen; and the soft thermal cap (see pm_soft_pct()).
 */
static uint32_t pm_boost_pct = 100, pm_boost_ms = 250, pm_foreign_pct = 45, pm_floor_pct = 30;
static uint32_t pm_soft_start = 600;    /* tenths of a degree: the soft cap begins (boot-arg pm_soft_start; 0: no soft cap) */
static uint32_t pm_soft_big_pct = 75, pm_soft_small_pct = 90;   /* the share of the top frequency left at the trip (pm_soft_big, pm_soft_small) */
static uint64_t pm_boost_edges;         /* the touches that started a boost (the 5 s line) */
static uint32_t pm_soft_last[CPUFREQ_GROUPS];   /* the soft cap's percent of the last look, for the line */
extern int32_t hakios_thermal_group_hottest(uint32_t d);        /* thermal.c */
extern int32_t hakios_thermal_trip(void);
extern uint64_t hakios_touch_last;      /* input.c */
static volatile bool pm_screen_off;     /* pm_screen=1 and the display is dark (hakios_cpufreq_screen()) */
static uint64_t pm_boost_until;         /* (absolute time) a boost asked for by hakios_cpufreq_screen(), on the screen's coming on */
static uint32_t pm_screen_mode;         /* boot-arg pm_screen */

/* the lowest row of the group whose frequency is at least PCT percent of its top */
static uint32_t
pm_row_at_pct(const struct gov_group *g, uint32_t pct)
{
	uint32_t row = 0;

	while (row < g->top && (uint64_t)g->khz[row] * 100 < (uint64_t)g->khz[g->top] * pct) {
		row++;
	}
	return row;
}

/*
 * The soft thermal cap, in percent of the group's top frequency: 100 up to pm_soft_start (default 60.0 C), then falling in a straight
 * line to pm_soft_big_pct (75 for the big group; the small group's, 90) at thermal.c's trip (70.0 C). The limiter (thermal=2) goes on
 * from there as before: row by row a second, each group by its own sensors, and its row reaches us as g->cap (hakios_cpufreq_pm_cap):
 * the two caps are combined with MIN(), so neither undoes the other, and the limiter's steps start from a cooler, already-trimmed
 * clock rather than from the top. The touch boost is held to this cap too.
 */
static uint32_t
pm_soft_pct(uint32_t d)
{
	int32_t hot, trip = hakios_thermal_trip(), start = (int32_t)pm_soft_start;
	uint32_t least = d == 0 ? pm_soft_small_pct : pm_soft_big_pct;

	if (pm_soft_start == 0 || least >= 100) {
		return 100;
	}
	hot = hakios_thermal_group_hottest(d);
	if (hot <= -2000 || hot <= start || trip <= start) {     /* (TH_NONE: not read yet) */
		return 100;
	}
	if (hot >= trip) {
		return least;
	}
	return 100 - (100 - least) * (uint32_t)(hot - start) / (uint32_t)(trip - start);
}

/* what is applied: the want, held to the caps (the thermal limiter's, the screen's) and lifted to the floors (touch, foreign core) */
static void
pm_apply(struct gov_group *g, bool boosted)
{
	uint32_t d = (uint32_t)(g - gov), row = g->want, cap = MIN(g->cap, g->top);

	uint32_t soft = pm_soft_pct(d);

	pm_soft_last[d] = soft;
	row = MAX(row, pm_row_at_pct(g, pm_floor_pct));
	if (boosted) {
		row = MAX(row, pm_row_at_pct(g, pm_boost_pct));
	}
	if (g->foreign) {
		row = MAX(row, pm_row_at_pct(g, pm_foreign_pct));
	}
	if (pm_screen_off) {
		/* the dark display: the big cores' group at its lowest row, the small cores' at half of its top at most */
		cap = MIN(cap, d == 0 ? pm_row_at_pct(g, 50) : 0);
		row = MIN(row, cap);
	}
	if (soft < 100) {
		cap = MIN(cap, pm_row_at_pct(g, soft));
	}
	row = MIN(row, cap);
	gov_set(g, row);
}

/* the thermal limiter's way in (thermal.c apply()): true when the governor takes the row as a cap, false: write it yourself */
bool
hakios_cpufreq_pm_cap(uint32_t domain, uint32_t row)
{
	struct gov_group *g;

	if (!pm_gov || !gov_on || domain >= CPUFREQ_GROUPS || gov[domain].base == 0) {
		return false;
	}
	g = &gov[domain];
	lck_mtx_lock(&gov_lock);
	g->cap = row;
	pm_apply(g, g->was_boosted);
	lck_mtx_unlock(&gov_lock);
	return true;
}

/* the groups the kernel has every core of: their tables, and the governor on (cpufreq=1 has raised them) */
static void
gov_start(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	const struct hakios_board *board = hakios_board();
	uint32_t on = 1, thermal = 1, had[CPUFREQ_GROUPS] = { 0 };

	/* off everywhere unless pm_gov=1: on the Mi 9T it cost smoothness at the same heat (ui_bench, two pairs in both orders, 2026-11-27:
	 * on 36.7/38.7 fps, 101/79 frames over 100 ms; off 41.1/42.6 fps, 71/63; peak 66-69 C either way). It lowers the small cores when
	 * the load drops, and the next animation starts on slow cores. thermal.c's cap still holds the heat. */
	pm_gov = false;
	PE_parse_boot_argn("cpu_gov", &on, sizeof(on));
	PE_parse_boot_argn("thermal", &thermal, sizeof(thermal));
	PE_parse_boot_argn("pm_gov", &pm_gov, sizeof(pm_gov));
	PE_parse_boot_argn("pm_floor_pct", &pm_floor_pct, sizeof(pm_floor_pct));
	PE_parse_boot_argn("pm_soft_start", &pm_soft_start, sizeof(pm_soft_start));
	PE_parse_boot_argn("pm_soft_big", &pm_soft_big_pct, sizeof(pm_soft_big_pct));
	PE_parse_boot_argn("pm_soft_small", &pm_soft_small_pct, sizeof(pm_soft_small_pct));
	PE_parse_boot_argn("pm_boost_pct", &pm_boost_pct, sizeof(pm_boost_pct));
	PE_parse_boot_argn("pm_boost_ms", &pm_boost_ms, sizeof(pm_boost_ms));
	PE_parse_boot_argn("pm_foreign_pct", &pm_foreign_pct, sizeof(pm_foreign_pct));
	PE_parse_boot_argn("pm_screen", &pm_screen_mode, sizeof(pm_screen_mode));
	if (pm_gov) {
		on = 1;
		thermal = 0;            /* (the limiter hands its rows over: hakios_cpufreq_pm_cap()) */
	}
	if (!on || thermal >= 2) {
		printf("cpufreq: every group at its top row (%s)\n", !on ? "cpu_gov=0" : "thermal=2 holds them back itself");
		return;
	}
	for (unsigned int i = 0; i < topology->num_cpus && i < 32; i++) {
		int d = domain_of(topology->cpus[i].phys_id);

		if (d >= 0) {
			gov[d].cores |= 1u << i;
			had[d]++;
		}
	}
	for (uint32_t d = 0; d < CPUFREQ_GROUPS; d++) {
		struct gov_group *g = &gov[d];
		uint32_t top_khz = 0;

		if (had[d] == 0 || (had[d] != board->cpufreq.domain_cores[d] && !pm_gov)) {
			continue;               /* (none of ours, or a core of the loader's in it) */
		}
		g->foreign = had[d] != board->cpufreq.domain_cores[d];
		g->cap = UINT32_MAX;
		g->base = domain_map(d);
		if (g->base == 0) {
			continue;               /* (not the hardware's: raise_domain() has said so) */
		}
		for (uint32_t row = 0; row < GOV_ROWS && row < CPUFREQ_HW_ROWS; row++) {
			uint32_t khz = cpufreq_hw_khz(rd(g->base + cpufreq_hw_row(layout, row)));

			if (row > 0 && khz <= top_khz) {
				break;
			}
			g->khz[row] = top_khz = khz;
			g->rows = row + 1;
		}
		g->top = g->rows - 1;
		/* (an index into khz[] and looks_at[]: never past the table, whatever the register reads; Linux bounds it too) */
		g->row = MIN(rd(g->base + layout->perf_state), g->top);
		g->want = g->row;
		gov_on = true;
		printf("cpufreq: group %u (cores %#x) by its load%s: %u rows, %u to %u MHz%s\n", d, g->cores, pm_gov ? " (pm_gov=1)" : "",
		    g->rows, g->khz[0] / 1000, g->khz[g->top] / 1000, g->foreign ? ", a core of the loader's: held at no less than pm_foreign_pct" : "");
	}
}

/* a look: BUSY the share of the last look each core was busy, in percent */
static void
gov_look(const uint32_t *busy)
{
	for (uint32_t d = 0; d < CPUFREQ_GROUPS; d++) {
		struct gov_group *g = &gov[d];
		uint32_t most = 0, want, need;

		if (g->base == 0) {
			continue;
		}
		for (unsigned int i = 0; i < 32; i++) {
			if ((g->cores >> i) & 1) {
				most = MAX(most, busy[i]);
			}
		}
		/* what the busiest core did, at the speed it had, with room to spare */
		need = (uint32_t)((uint64_t)g->khz[g->row] * most * GOV_HEADROOM_PCT / 10000);
		for (want = 0; want < g->top && g->khz[want] < need; want++) {
		}
		if (most >= GOV_UP_BUSY) {
			want = g->top;
		}
		if (want >= g->row) {
			gov_set(g, want);
			g->down_looks = 0;
			g->down_most = 0;
		} else {
			g->down_most = MAX(g->down_most, want);
			if (++g->down_looks >= GOV_DOWN_LOOKS) {
				gov_set(g, g->down_most);
				g->down_looks = 0;
				g->down_most = 0;
			}
		}
		g->looks_at[g->row]++;
	}
}

/* pm_gov=1: a look (see above). NOW is mach_absolute_time(). */
static void
gov_look_pm(const uint32_t *busy, uint64_t now)
{
	uint64_t boost_ticks;
	bool boosted;

	nanoseconds_to_absolutetime((uint64_t)pm_boost_ms * NSEC_PER_MSEC, &boost_ticks);
	boosted = (hakios_touch_last != 0 && now - hakios_touch_last < boost_ticks) || now < pm_boost_until;
	lck_mtx_lock(&gov_lock);
	for (uint32_t d = 0; d < CPUFREQ_GROUPS; d++) {
		struct gov_group *g = &gov[d];
		uint32_t most = 0, want, need;

		if (g->base == 0) {
			continue;
		}
		for (unsigned int i = 0; i < 32; i++) {
			if ((g->cores >> i) & 1) {
				most = MAX(most, busy[i]);
			}
		}
		/* what the busiest core did, at the speed it ran at (the speed asked for: caps may have kept it lower), with room to spare */
		need = (uint32_t)((uint64_t)g->khz[g->row] * most * PM_HEADROOM_PCT / 10000);
		for (want = 0; want < g->top && g->khz[want] < need; want++) {
		}
		if (most >= PM_JUMP_BUSY) {
			want = g->top;
		}
		if (want >= g->want) {
			g->want = want;
			g->down_looks = 0;
			g->down_most = 0;
		} else {
			g->down_most = MAX(g->down_most, want);
			if (++g->down_looks >= PM_DOWN_LOOKS) {
				/* half of the way down to what the last looks asked for at most */
				g->want = g->want - MAX(1u, (g->want - g->down_most) / 2);
				g->down_looks = 0;
				g->down_most = 0;
			}
		}
		if (boosted && g->row < pm_row_at_pct(g, pm_boost_pct)) {
			g->boosts++;
		}
		if (boosted && !g->was_boosted) {
			pm_boost_edges++;
		}
		g->was_boosted = boosted;
		pm_apply(g, boosted);
		g->looks_at[g->row]++;
		g->looks_pm++;
	}
	lck_mtx_unlock(&gov_lock);
}

/* the 5 s line of pm_gov: each group's share of the looks by frequency, its mean, the boosts; the counters start again */
static void
gov_report_pm(void)
{
	char line[600];
	size_t at = 0;

	for (uint32_t d = 0; d < CPUFREQ_GROUPS && at < sizeof(line) - 100; d++) {
		struct gov_group *g = &gov[d];
		uint64_t sum = 0, khz_sum = 0;
		int32_t hot_now = hakios_thermal_group_hottest(d);

		if (g->base == 0 || g->looks_pm == 0) {
			continue;
		}
		at += (size_t)snprintf(line + at, sizeof(line) - at, " g%u:", d);
		for (uint32_t r = 0; r < g->rows; r++) {
			sum += g->looks_at[r];
			khz_sum += g->looks_at[r] * g->khz[r];
		}
		for (uint32_t r = 0; r < g->rows && sum != 0 && at < sizeof(line) - 40; r++) {
			if (g->looks_at[r] * 100 / sum != 0) {
				at += (size_t)snprintf(line + at, sizeof(line) - at, " %u=%llu%%", g->khz[r] / 1000, g->looks_at[r] * 100 / sum);
			}
		}
		at += (size_t)snprintf(line + at, sizeof(line) - at, " mean %llu MHz, row %u/%u, cap row %u soft %u%% hot %d.%d C, boosted looks %llu%s;",
		    sum ? khz_sum / sum / 1000 : 0, g->row, g->top, MIN(g->cap, g->top), pm_soft_last[d],
		    hot_now / 10, hot_now < 0 ? -(hot_now % 10) : hot_now % 10, g->boosts,
		    g->foreign ? " (floor: loader's core)" : "");
		memset(g->looks_at, 0, sizeof(g->looks_at));
		g->boosts = g->looks_pm = 0;
	}
	tell("pm: gov (MHz=share of looks):%s touch boosts %llu, screen %s\n", line, pm_boost_edges, pm_screen_off ? "OFF" : "on");
	pm_boost_edges = 0;
}

/*
 * PHASE 5 (boot-arg pm_screen=1): the display's state, from the sysctl hw.backlight.power (backlight.c): 0 is dark. Dark:
 * the clocks capped (pm_apply: the big cores' group at its lowest row, the small cores' at half of its top, whatever the load
 * asks), the scheduler given the small cores only (the park thread), the memory vote at its lowest and the GPU at its lowest
 * level (ddr.c, adreno.c), the park thread looking every PM_SCREEN_OFF_MS instead of PARK_MS. Lit again: every cap off, each group
 * asked for its top row at once and held up for 300 ms, all the cores recommended, the memory at its top -- before the sysctl
 * returns, so the first frame finds the machine ready. Needs pm_gov=1 for the processors' part, pm_ddr=1 and pm_gpu=1 for the others.
 */
extern void hakios_ddr_pm_screen(bool off);
extern void hakios_adreno_pm_screen(bool off);
static uint32_t park_all, park_big;
static uint64_t screen_off_since;

void hakios_pm_screen(bool off);
void
hakios_pm_screen(bool off)
{
	uint64_t now = mach_absolute_time(), ticks, since;

	if (pm_screen_mode == 0 || off == pm_screen_off) {
		return;
	}
	if (off) {
		pm_screen_off = true;
		screen_off_since = now;
		lck_mtx_lock(&gov_lock);
		for (uint32_t d = 0; pm_gov && gov_on && d < CPUFREQ_GROUPS; d++) {
			if (gov[d].base != 0) {
				pm_apply(&gov[d], false);
			}
		}
		lck_mtx_unlock(&gov_lock);
		hakios_ddr_pm_screen(true);
		hakios_adreno_pm_screen(true);
		tell("pm: screen: dark: the clocks capped, the small cores only, the memory and the GPU at their lowest\n");
		return;
	}
	nanoseconds_to_absolutetime(300 * NSEC_PER_MSEC, &ticks);
	pm_boost_until = now + ticks;
	pm_screen_off = false;
	lck_mtx_lock(&gov_lock);
	for (uint32_t d = 0; pm_gov && gov_on && d < CPUFREQ_GROUPS; d++) {
		if (gov[d].base != 0) {
			gov[d].want = gov[d].top;
			pm_apply(&gov[d], true);
		}
	}
	lck_mtx_unlock(&gov_lock);
	if (park_big != 0 && park_big != park_all) {
		sched_perfcontrol_update_recommended_cores(park_all);
	}
	hakios_ddr_pm_screen(false);
	hakios_adreno_pm_screen(false);
	absolutetime_to_nanoseconds(now - screen_off_since, &since);
	tell("pm: screen: lit again after %llu ms: top rows asked for, every core recommended\n", since / NSEC_PER_MSEC);
}

/* (every way back to the loader, interrupts off: each governed group at its top row again) */
static void
gov_quiesce(void)
{
	for (uint32_t d = 0; d < CPUFREQ_GROUPS; d++) {
		if (gov[d].base != 0) {
			vote(gov[d].base, d, gov[d].top);
		}
	}
	gov_on = false;
}

static void
park_thread(void *arg __unused, wait_result_t wr __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t idle_before[MAX_CPUS] = { 0 }, then = mach_absolute_time(), said = then, said_pm = then, ten;
	uint32_t big = 0, all = 0, calm = 0;
	bool parked;

	for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS && i < 32; i++) {
		all |= 1u << i;
		if (topology->cpus[i].l2_access_penalty == 0) {
			big |= 1u << i;
		}
	}
	const bool parking = park_wanted && big != all && big != 0;
	bool screen_seen = false;

	pm_ddr_on = hakios_ddr_pm_on();

	park_all = all;
	park_big = big;

	if (!parking && !gov_on && !pm_screen_mode && !pm_ddr_on) {
		return;
	}
	nanoseconds_to_absolutetime(10 * NSEC_PER_SEC, &ten);
	parked = parking;
	if (parking) {
		sched_perfcontrol_update_recommended_cores(big);
		park_stat.parks++;
		printf("cpufreq: small cores %#x parked while the big ones (%#x) have room (park_small=0: not)\n", all & ~big, big);
	}
	for (;;) {
		uint64_t now, least = 100;
		uint32_t busy_of[32] = { 0 };

		delay_for_interval(pm_screen_off ? PM_SCREEN_OFF_MS : PARK_MS, NSEC_PER_MSEC);
		now = mach_absolute_time();
		for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS && i < 32; i++) {
			uint64_t idle = 0, busy;

			if (!ml_cpu_idle_time((int)i, &idle)) {
				continue;
			}
			busy = (now - then) > (idle - idle_before[i]) ? (now - then) - (idle - idle_before[i]) : 0;
			idle_before[i] = idle;
			busy_of[i] = (uint32_t)MIN(100, 100 * busy / (now - then));
			if ((big >> i) & 1) {
				least = MIN(least, 100 * busy / (now - then));
			}
		}
		then = now;
		park_stat.looks++;
		if (pm_ddr_on) {
			uint32_t sum = 0, cores = 0, big_most = 0, idle_ms = hakios_adreno_idle_ms();
			uint64_t touch_ticks;

			for (unsigned int i = 0; i < topology->num_cpus && i < 32; i++) {
				sum += busy_of[i];
				cores++;
				if ((big >> i) & 1) {
					big_most = MAX(big_most, busy_of[i]);
				}
			}
			nanoseconds_to_absolutetime(500 * NSEC_PER_MSEC, &touch_ticks);
			hakios_ddr_pm_look(cores ? sum / cores : 0, big_most, hakios_touch_last != 0 && now - hakios_touch_last < touch_ticks,
			    idle_ms < 100);
		}
		if (pm_screen_off != screen_seen) {
			screen_seen = pm_screen_off;
			if (screen_seen && big != 0 && big != all) {
				sched_perfcontrol_update_recommended_cores(all & ~big);
			} else if (!screen_seen && big != 0 && big != all) {
				sched_perfcontrol_update_recommended_cores(all);
				parked = false;
				calm = 0;
			}
		}
		if (gov_on && pm_gov) {
			gov_look_pm(busy_of, now);
		} else if (gov_on) {
			gov_look(busy_of);
		}
		if (!parking || screen_seen) {
			/* (no parking: the governor alone; the screen dark: the small cores, as set above) */
		} else if (parked && least >= park_busy) {
			sched_perfcontrol_update_recommended_cores(all);
			parked = false;
			calm = 0;
			park_stat.unparked++;
		} else if (!parked) {
			calm = least < PARK_CALM ? calm + 1 : 0;
			if (calm >= PARK_CALM_TIMES) {
				sched_perfcontrol_update_recommended_cores(big);
				parked = true;
				park_stat.parks++;
			}
		}
		if (pm_gov && gov_on && now - said_pm >= ten / 2) {
			gov_report_pm();
			said_pm = now;
		}
		if (now - said >= ten) {
			char rows[96] = "";
			size_t at = 0;

			/* (and how many looks each governed group spent at each row, its top last: "g1 0:12 7:30 ...") */
			for (uint32_t d = 0; gov_on && d < CPUFREQ_GROUPS && at < sizeof(rows) - 16; d++) {
				if (gov[d].base == 0) {
					continue;
				}
				at += (size_t)snprintf(rows + at, sizeof(rows) - at, " g%u", d);
				for (uint32_t r = 0; r < gov[d].rows && at < sizeof(rows) - 12; r++) {
					if (gov[d].looks_at[r] != 0) {
						at += (size_t)snprintf(rows + at, sizeof(rows) - at, " %u:%llu", r, gov[d].looks_at[r]);
					}
				}
				memset(gov[d].looks_at, 0, sizeof(gov[d].looks_at));
			}
			hakios_tl("tl: k %u looks, %u times the small cores taken, %u times parked again;%s\n", park_stat.looks,
			    park_stat.unparked, park_stat.parks, rows);
			memset(&park_stat, 0, sizeof(park_stat));
			said = now;
		}
	}
}

static void
park_start(void)
{
	/*
	 * boot-arg park_busy=N: how busy (%) the least busy big core must be for the small ones to be given back (default
	 * PARK_BUSY; 101: never, but for the scheduler's failsafe for a real-time thread). A phone with two big cores only
	 * (the Mi 9T's two A76) has both past 85% in every gesture, and the window server's draws then cost three to four
	 * times as much on an A55 (logs/113203-run.log: 28 us a draw on an A76, 80 to 110 on an A55).
	 */
	PE_parse_boot_argn("park_busy", &park_busy, sizeof(park_busy));
	extern uint32_t hakios_amp_enabled;
	uint32_t on = hakios_amp_enabled ? 0 : 1;       /* (amp=1: the scheduler places work on the E cluster itself; park_small=1 still parks) */
	thread_t thread;

	PE_parse_boot_argn("park_small", &on, sizeof(on));
	park_wanted = on != 0;
	if ((park_wanted || gov_on || pm_screen_mode || hakios_ddr_pm_on()) && kernel_thread_start(park_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* every group back at its top row and the cache slices as the boot loader left them, for the loader (hakios_ddr_quiesce) */
void
hakios_cpufreq_quiesce(void)
{
	if (gov_on) {
		gov_quiesce();
	}
	llcc_quiesce();
}

void
hakios_cpufreq_init(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint32_t enabled = 0, cores[CPUFREQ_GROUPS] = { 0 }, before, here;

	hakios_cpuload_start();
	hakios_amp_start();             /* (boot-arg amp=1: osfmk/kern/hakios_amp.c) */
	hakios_pm_idle_init();          /* (boot-arg pm_idle=: the cores' idle states; arch/arm64/kernel/cpu_idle.c) */
	PE_parse_boot_argn("pm_screen", &pm_screen_mode, sizeof(pm_screen_mode));
	PE_parse_boot_argn("cpufreq", &enabled, sizeof(enabled));
	if (CONFIG_QCOM_DDR_VOTE) {
		hakios_ddr_init(enabled != 0);  /* (the memory first, as before: bsd/dev/board/ddr.c) */
	}
	if (!enabled) {
		park_start();           /* (the small cores parked all the same: no clocks are touched for that) */
		return;
	}
	if (!hakios_board_has(HAKIOS_HAS_CPUFREQ_EPSS)) {
		tell("cpufreq: disabled: this board's board.dts lists no \"cpufreq-epss\"\n");
		park_start();
		return;
	}
	layout = cpufreq_hw_layout(hakios_board());
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		int domain = domain_of(topology->cpus[i].phys_id);

		if (domain >= 0) {
			cores[domain]++;
		}
	}
	/* (whichever core this thread happens to be on, and stays on or not: it only goes to the log) */
	here = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	before = loop_speed();
	for (uint32_t domain = 0; domain < CPUFREQ_GROUPS; domain++) {
		if (cores[domain] != 0) {
			raise_domain(domain, cores[domain]);
		}
	}
	tell("cpufreq: on core %u a loop ran at %u, now at %u\n", here, before, loop_speed());
	hakios_membench();                      /* (after the cores' clocks: bsd/dev/board/bench.c) */
	l3_raise();
	llcc_setup();
	/* the groups by their load from here on (cpu_gov), and the small cores parked (park_small): one thread looks */
	gov_start();
	park_start();
}
