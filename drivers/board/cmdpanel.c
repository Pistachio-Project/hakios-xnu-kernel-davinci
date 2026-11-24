/*
 * A command-mode panel's frames. A DSI command-mode panel (the Pixel 4a's) takes a frame only when it is sent one,
 * where a video-mode panel is scanned out all the time: the framebuffer Darwin draws into is the memory the display
 * engine reads, so without this the screen stays on the last frame the boot loader sent. This turns on the
 * autorefresh of each pingpong block that does tear checks: the engine then sends the frame at every tear-effect
 * pulse of the panel.
 *
 *   panel_autorefresh=1    autorefresh on, at every pulse, for each pingpong whose tear check is on
 *   panel_autorefresh=-1   a look: each pingpong's tear check and autorefresh read and said, nothing written
 *   panel_autorefresh=2    for a loader that left NO tear check on (the Mi 9T's Mu UEFI): pingpong 0's tear check is
 *                          programmed as Linux's dpu_hw_pp_setup_tearcheck() does, the TE pin muxed to mdp_vsync, then
 *                          as 1. Boot-args for the panel's numbers (defaults: the Mi 9T's ss_fhd_ea_f10, phone.dts):
 *                          panel_vdisplay=2340 panel_vtotal=2488 panel_fps=60 panel_te_pin=10 (0: leave the pin alone)
 *                          panel_vsync_hz=19200000 (disp_cc_mdss_vsync_clk)
 *
 * Where the blocks are is the SoC file's display { pingpong } (PINGPONG_0; the others 0x800 apart). Registers, from
 * Linux's DPU driver (dpu_hw_pingpong.c): PP_TEAR_CHECK_EN +0x000 (bit 0), PP_AUTOREFRESH_CONFIG +0x030 (bit 31 on,
 * the low 16 bits the pulses between frames).
 *
 * The display engine has to be on and clocked when this runs, or touching it can stop the phone: the boot loader
 * leaves it so on a phone that shows its own text. No check here can tell in advance; hence the boot argument.
 * Nothing is undone on the way back: the loader draws on the same panel afterwards and needs the frames too.
 *
 * From the Pixel 4a's Darwin 16.5 port (devices/sunfish/drivers/gpu/hakios.cmdpanel.c), where it ran on the phone.
 */
#include <kern/startup.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

#include "board.h"
#include "log.h"
#include "mmio.h"
#include "time.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define PP_BLOCKS               4u
#define PP_STRIDE               0x800u
#define PP_TEAR_CHECK_EN        0x000u
#define PP_SYNC_CONFIG_VSYNC    0x004u  /* dpu_hw_pingpong.c: bit 19 counter on, bit 20 the TE pin, [18:0] clocks a frame */
#define PP_SYNC_CONFIG_HEIGHT   0x008u
#define PP_SYNC_WRCOUNT         0x00cu
#define PP_VSYNC_INIT_VAL       0x010u
#define PP_SYNC_THRESH          0x018u  /* continue << 16 | start */
#define PP_START_POS            0x01cu
#define PP_RD_PTR_IRQ           0x020u
#define PP_LINE_COUNT           0x02cu
#define PP_AUTOREFRESH          0x030u
#define VSYNC_COUNTER_EN        (1u << 19)
#define VSYNC_HW_TE             (1u << 20)
#define TE_SYNC_HEIGHT          0xfff0u /* (Linux: dpu_encoder_phys_cmd.c) */
#define TE_THRESHOLD            4u
#define CBCR_OFF                (1u << 31)
#define GDSC_POWER_ON           (1u << 31)
#define DISPCC_MDSS_VSYNC_CBCR  0x2024u /* disp_cc_mdss_vsync_clk .halt_reg (dispcc-sdmmagpie.c); SM7150 */
#define TLMM_FUNCTION_MDP_VSYNC 1u      /* GPIO 10, 11, 12: pinctrl-sdmmagpie.c PINGROUP(10, NORTH, mdp_vsync, ...) */
#define AUTOREFRESH_ENABLE      0x80000000u
#define AUTOREFRESH_ON          (AUTOREFRESH_ENABLE | 1u)       /* every pulse */

#define HAKIOS_TLMM             tlmm
static vm_offset_t tlmm;
#include "tlmm.h"

void hakios_cmdpanel_init(void);

static uint32_t
arg(const char *name, uint32_t fallback)
{
	uint32_t value;

	return PE_parse_boot_argn(name, &value, sizeof(value)) ? value : fallback;
}

/* The display block must be powered and its register clock running: read only, nothing turned on here. */
static bool
display_on(const struct hakios_board *board)
{
	vm_offset_t dispcc;
	uint32_t gdsc, ahb, vsync;

	if (board->clocks.dispcc == 0 || board->clocks.dispcc_mdss_gdscr == 0 || board->clocks.dispcc_mdss_ahb_cbcr == 0) {
		hakios_say("cmdpanel: the SoC file has no dispcc offsets: refused\n");
		return false;
	}
	dispcc = ml_io_map(board->clocks.dispcc, 0x4000);
	if (dispcc == 0) {
		return false;
	}
	gdsc = rd(dispcc + board->clocks.dispcc_mdss_gdscr);
	ahb = rd(dispcc + board->clocks.dispcc_mdss_ahb_cbcr);
	vsync = rd(dispcc + DISPCC_MDSS_VSYNC_CBCR);
	hakios_say("cmdpanel: mdss gdsc %#x, ahb clock %#x, vsync clock %#x\n", gdsc, ahb, vsync);
	if (!(gdsc & GDSC_POWER_ON) || (ahb & CBCR_OFF)) {
		hakios_say("cmdpanel: the display block is off or its registers have no clock: refused\n");
		return false;
	}
	if (vsync & CBCR_OFF) {
		hakios_say("cmdpanel: disp_cc_mdss_vsync_clk is off, the tear check could not count: refused\n");
		return false;
	}
	return true;
}

#define TC_WRITE(offset, value) do {                                                                    \
		uint32_t before_ = rd(block + (offset));                                                \
		wr(block + (offset), (value));                                                          \
		hakios_say("cmdpanel: pingpong 0: +%#x %#x -> %#x (read back %#x)\n", (unsigned)(offset), \
		    before_, (uint32_t)(value), rd(block + (offset)));                                  \
	} while (0)

/* Linux's dpu_hw_pp_setup_tearcheck() and enable_tearcheck(), for pingpong 0, with the panel's own numbers. */
static bool
tearcheck_program(const struct hakios_board *board, vm_offset_t block)
{
	uint32_t vdisplay = arg("panel_vdisplay", 2340), vtotal = arg("panel_vtotal", 2488), fps = arg("panel_fps", 60);
	uint32_t hz = arg("panel_vsync_hz", 19200000), pin = arg("panel_te_pin", 10);
	uint32_t count, line[2];

	if (vdisplay == 0 || vtotal < vdisplay || fps == 0 || hz / (vtotal * fps) == 0 || hz / (vtotal * fps) >= (1u << 19)) {
		hakios_say("cmdpanel: implausible panel numbers (%u of %u lines, %u fps, %u Hz): refused\n", vdisplay, vtotal, fps, hz);
		return false;
	}
	if (!display_on(board)) {
		return false;
	}
	count = hz / (vtotal * fps);
	hakios_say("cmdpanel: tear check: %u lines of %u, %u fps, vsync clock %u Hz: %u clocks a frame\n", vdisplay, vtotal, fps, hz,
	    count);
	if (pin != 0) {
		tlmm = ml_io_map(board->pins.tlmm, TLMM_SIZE);
		if (tlmm == 0 || pin >= TLMM_PINS) {
			hakios_say("cmdpanel: the TLMM could not be used for pin %u: refused\n", pin);
			return false;
		}
		uint32_t cfg = rd(GPIO_CFG(pin));

		hakios_say("cmdpanel: TE pin gpio%u: cfg %#x (function %u, pull %u, drive %u mA), level %u\n", pin, cfg, cfg >> 2 & 15,
		    cfg & 3, ((cfg >> 6 & 7) + 1) * 2, gpio_get(pin));
		if ((cfg >> 2 & 15) != TLMM_FUNCTION_MDP_VSYNC) {
			gpio_configure(pin, TLMM_FUNCTION_MDP_VSYNC, PULL_DOWN, 2, false);
			__asm__ volatile ("dsb sy" ::: "memory");
			hakios_say("cmdpanel: TE pin gpio%u: cfg %#x -> %#x\n", pin, cfg, rd(GPIO_CFG(pin)));
		}
	}
	TC_WRITE(PP_SYNC_CONFIG_VSYNC, VSYNC_COUNTER_EN | (pin != 0 ? VSYNC_HW_TE : 0) | count);
	TC_WRITE(PP_SYNC_CONFIG_HEIGHT, TE_SYNC_HEIGHT);
	TC_WRITE(PP_VSYNC_INIT_VAL, vdisplay);
	TC_WRITE(PP_RD_PTR_IRQ, vdisplay + 1);
	TC_WRITE(PP_START_POS, vdisplay);
	TC_WRITE(PP_SYNC_THRESH, TE_THRESHOLD << 16 | TE_THRESHOLD);
	TC_WRITE(PP_SYNC_WRCOUNT, vdisplay + TE_THRESHOLD + 1);
	TC_WRITE(PP_TEAR_CHECK_EN, 1);
	__asm__ volatile ("dsb sy" ::: "memory");
	line[0] = rd(block + PP_LINE_COUNT);
	spin_us(5000);
	line[1] = rd(block + PP_LINE_COUNT);
	hakios_say("cmdpanel: tear check on: its line counter %u, 5 ms later %u%s\n", line[0], line[1],
	    line[0] != line[1] ? " (COUNTING)" : " (STILL: no counting, no TE pulses seen)");
	return (rd(block + PP_TEAR_CHECK_EN) & 1u) != 0;
}

void
hakios_cmdpanel_init(void)
{
	int32_t level = 0;
	const struct hakios_board *board;
	vm_offset_t pp;
	unsigned found = 0;

	if (!PE_parse_boot_argn("panel_autorefresh", &level, sizeof(level)) || level == 0) {
		return;
	}
	board = hakios_board();
	if (board == NULL || board->display.pingpong == 0) {
		hakios_say("cmdpanel: no display { pingpong } in this board's SoC file: nothing done\n");
		return;
	}
	pp = ml_io_map(board->display.pingpong, PP_BLOCKS * PP_STRIDE);
	if (pp == 0) {
		hakios_say("cmdpanel: the pingpong blocks at %#x could not be mapped\n", board->display.pingpong);
		return;
	}
	if (level == 2) {
		/* (read first: a loader that left tear checks on is not touched) */
		bool any = false;

		for (unsigned n = 0; n < PP_BLOCKS; n++) {
			any = any || (rd(pp + n * PP_STRIDE + PP_TEAR_CHECK_EN) & 1u);
		}
		if (any) {
			hakios_say("cmdpanel: a tear check is on already: not programmed\n");
		} else if (!tearcheck_program(board, pp)) {
			hakios_say("cmdpanel: the tear check was not turned on: nothing more done\n");
			return;
		}
	}
	for (unsigned n = 0; n < PP_BLOCKS; n++) {
		vm_offset_t block = pp + n * PP_STRIDE;
		uint32_t was, now;

		if (!(rd(block + PP_TEAR_CHECK_EN) & 1u)) {
			continue;       /* not driving a command-mode panel */
		}
		found++;
		was = rd(block + PP_AUTOREFRESH);
		if (level > 0 && !(was & AUTOREFRESH_ENABLE)) {
			wr(block + PP_AUTOREFRESH, AUTOREFRESH_ON);
			__asm__ volatile ("dsb sy" ::: "memory");
		}
		now = rd(block + PP_AUTOREFRESH);
		hakios_say("cmdpanel: pingpong %u: autorefresh %#x -> %#x%s\n", n, was, now,
		    level < 0 ? " (a look)" : (now & AUTOREFRESH_ENABLE) ? "" : " (DID NOT TAKE: the screen will not follow)");
	}
	if (found == 0) {
		hakios_say("cmdpanel: no pingpong does tear checks: the panel was not left in command mode, nothing done\n");
	}
}

/* As early as the device tree is there: the console's first lines are on the screen only once this has run. */
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hakios_cmdpanel_init);
