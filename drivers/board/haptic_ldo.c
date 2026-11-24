/*
 * A vibration motor on a PMIC's vibrator LDO ("qcom,qpnp-vibrator-ldo": the PM6150's vibrator@5300 on the Xiaomi Mi 9T):
 * an eccentric-mass motor that is on while an LDO gives it a supply, and off while not. No chip with a waveform
 * RAM as in board/haptic.c, so this is a driver of its own and not a second kind of that one: nothing
 * of the AW86927's pins, registers or waveforms is shared, and a board that has that chip runs the same bytes as before.
 * What they share is what Darwin sees: /dev/hakios-haptic, written the samples of Darwin's "Actuator" device, and the
 * service that waits for it (HakiosHapticsReady()). Which peripheral the LDO is, and the supply the motor is given, are the board's
 * (vibrator { }, board/board.h); the registers are this driver's, from the phone vendor's open kernel
 * (drivers/leds/leds-qpnp-vibrator-ldo.c of kernel_xiaomi_sm6150, cited as VENDOR:line below) and the device
 * tree of the phone it was written for (node vibrator@5300 under qcom,pm6150@1: compatible
 * "qcom,qpnp-vibrator-ldo", reg 0x5300, "qcom,vib-ldo-volt-uv" 3000000, "qcom,disable-overdrive"):
 *
 *   STATUS1 0x08       [7] the LDO's output is ready                           VENDOR:27-28
 *   VSET    0x40, 0x41 the supply in millivolts, low byte then high byte; the LDO steps by 8 mV,
 *                      from 1.504 V to 3.544 V                                VENDOR:30, 36-38, 98-102
 *   EN_CTL  0x46       [7] the LDO on                                         VENDOR:32-33
 *   sequence           the voltage written, the enable set, STATUS1 polled every 100 us for 1 ms for ready
 *                      (and the enable cleared again if it never is); off is the enable cleared. Nothing else
 *                      of the LDO is touched                                   VENDOR:68-87, 89-148
 *   overdrive          the vendor's driver starts at twice the voltage for 30 ms unless the device tree says
 *                      "qcom,disable-overdrive", which this phone's does: not done here, whatever a board says  VENDOR:150-179
 *   times              the vendor's shortest vibration 50 ms, its default 5 s, its longest 15 s  VENDOR:40-47
 *
 * The type and subtype registers (0x04, 0x05) are the peripheral's own, as every PMIC peripheral has them; the
 * vendor's driver does not read them, so what an LDO of this kind says is not known beforehand: level 1
 * logs them and does not stop at them.
 *
 * Writing to a PMIC is not like writing anywhere else: the PMICs make the phone's supplies. So every write
 * goes through one function that takes only the three registers above (the peripheral the arbiter's channel is checked to
 * be, each write read back), the voltage is never above the board's and never outside the LDO's range, and any
 * surprise ends all writing for the run -- all but the motor's going off, which has a function of its own that
 * takes only the enable and only clears it, whatever went before. Transfers through the arbiter are made
 * one at a time with the buttons' driver (hakios_spmi_lock()).
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   haptic_ldo=1   a look, reading only: the arbiter's channel for the LDO and who may write through it, its type,
 *                  subtype and revision, its status, enable and voltage registers, and the registers 0x00 to 0x4f
 *   haptic_ldo=2   ... and the motor on once at the board's voltage for haptic_ldo_ms (default 200), then off
 *   haptic_ldo=3   ... no vibration at the start; the motor for Darwin until the run ends: what Darwin plays on its
 *                  "Actuator" device is written to /dev/hakios-haptic, and the motor runs while a waveform of Darwin's is
 *                  heard: a waveform that ends in less than the vendor's shortest vibration (50 ms) is that long, a
 *                  longer one is followed until it ends
 *
 *   haptic_ldo_mv=N   the voltage in millivolts, if lower than the board's (default: the board's); never more
 *   haptic_ldo_max_ms=N  the longest the motor runs at a time (default 2000, never more than 5000, the vendor's
 *                  default); after that it rests for as long again
 *
 * Limits, whatever asks: the board's voltage at most, in the LDO's range, on its 8 mV step rounded down (the vendor
 * rounds up, which for 3.0 V, a multiple of the step, is the same); the motor on for haptic_ldo_max_ms at a time at
 * most; ready lost while it runs, a read that fails or a write that does not read back, and the motor
 * goes off and stays off for the rest of the run. At the run's end (and on a panic's way out) it goes off.
 * What this cannot do is stop the motor if the kernel itself stops with it on: the LDO has no timer of its own
 * that the vendor's driver uses.
 *
 * Register layout of the arbiter: board/spmi.h.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/board/audio.h>
#include <sys/board/input.h>
#include <sys/systm.h>
#include <sys/uio.h>

#include "kernel.h"
#include "board.h"
#include "log.h"

extern void HakiosHapticsReady(void);             /* iokit/Kernel/BoardHaptics.cpp */
extern void HakiosHapticsGone(void);

static uint32_t level;
static bool broken, leaving;
/*
 * The thread is between registers: set before it looks at `leaving' and writes, cleared after. The run's end sets
 * `leaving' and waits for this to clear, so that it never meets the motor half switched on (enable written,
 * `running' not yet set) and the thread starts no writes after it.
 */
static bool writing;

HAKIOS_DEFINE_SAY(!leaving && ml_get_interrupts_enabled())

#include "mmio.h"
#include "time.h"

#define wait_ms(ms)             hakios_wait_ms((ms), leaving)   /* (spun on the way out) */
#define wait_us(us)             do { if (leaving) { spin_us(us); } else { sleep_us(us); } } while (0)

/* ---- the PMIC arbiter (board/spmi.h) ---------------------------------------------------------------- */

#include "spmi.h"

static struct {
	uint16_t        id;             /* slave << 8 | peripheral: the board's vibrator { } */
	uint16_t        channel;
	bool            found, writable;
	vm_offset_t     write;          /* its write channel, mapped */
} ldo;
static struct hakios_spmi arb;

/* (one transfer at a time with the buttons' driver; at the run's end the others are stopped, perhaps holding it) */
static void
lock(void)
{
	if (!leaving) {
		hakios_spmi_lock();
	}
}

static void
unlock(void)
{
	if (!leaving) {
		hakios_spmi_unlock();
	}
}

/* `count' (1 to 8) registers from `reg' on; NULL, or what went wrong. A dropped transfer is asked for again, twice at most. */
static const char *
spmi_read(uint8_t reg, uint8_t *bytes, uint32_t count)
{
	const char *problem = "no channel";

	for (int tries = 0; ldo.found && tries < 3; tries++) {
		lock();
		problem = hakios_spmi_read(hakios_spmi_observer(&arb, ldo.channel), reg, bytes, count);
		unlock();
		if (problem == NULL || strcmp(problem, SPMI_SAYS_DROPPED) != 0) {
			break;
		}
	}
	return problem;
}

/* ---- the LDO ---------------------------------------------------------------------------------------- */

#define LDO_REVISION1           0x00
#define LDO_TYPE                0x04            /* (the peripheral's own: not read by the vendor's driver) */
#define LDO_SUBTYPE             0x05
#define LDO_STATUS1             0x08            /* VENDOR:27 */
#define  STATUS1_VREG_READY     0x80            /* VENDOR:28 */
#define LDO_VSET_LB             0x40            /* VENDOR:30; the high byte follows it */
#define LDO_VSET_HB             0x41
#define LDO_EN_CTL              0x46            /* VENDOR:32 */
#define  EN_CTL_EN              0x80            /* VENDOR:33 */

#define LDO_VMIN_UV             1504000         /* VENDOR:36 */
#define LDO_VMAX_UV             3544000         /* VENDOR:37 */
#define LDO_STEP_UV             8000            /* VENDOR:38 */
#define LDO_MIN_PLAY_MS         50              /* VENDOR:44 */
#define LDO_MAX_PLAY_MS         5000            /* VENDOR:45: its default, not its 15 s longest */
#define LDO_READY_POLL_US       100             /* VENDOR:75 */
#define LDO_READY_TIMEOUT_US    1000            /* VENDOR:75 */
#define DEFAULT_MAX_MS          2000

static const uint8_t writable_regs[] = { LDO_VSET_LB, LDO_VSET_HB, LDO_EN_CTL };

static bool running;                    /* the enable has been written on (and so the motor may be): until it reads back off */
static uint32_t volt_uv;                /* the supply the motor is given: the board's, lowered by haptic_ldo_mv, in the LDO's range */
static uint32_t max_ms = DEFAULT_MAX_MS;

static void
give_up(const char *why)
{
	if (!broken) {
		broken = true;
		say("haptic_ldo: %s: no more writing to the LDO for the rest of the run\n", why);
	}
}

static bool
get(uint8_t reg, uint8_t *value)
{
	const char *problem = spmi_read(reg, value, 1);

	if (problem != NULL) {
		say("haptic_ldo: reading 0x%02x: %s\n", reg, problem);
		return false;
	}
	return true;
}

/* One byte to the LDO, made (a dropped write has not been: asked for again); NULL, or what went wrong. */
static const char *
send(uint8_t reg, uint8_t value)
{
	const char *problem = NULL;

	for (int tries = 0; tries < 3; tries++) {
		lock();
		problem = hakios_spmi_write(ldo.write, reg, value);
		unlock();
		if (problem == NULL || strcmp(problem, SPMI_SAYS_DROPPED) != 0) {
			break;
		}
	}
	return problem;
}

/* the arbiter's channel is still the LDO's: its entry in the table is looked at again before a write */
static bool
still_the_ldo(void)
{
	return SPMI_ID_OF(hakios_spmi_channel_map(&arb, ldo.channel)) == ldo.id;
}

/*
 * One register of the three above written and read back (`mask' of it: the enable's other bits are not ours and are
 * kept as found); false, and no more writing, if it cannot be or does not read back.
 */
static bool
put_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	const char *problem;
	uint8_t now = 0, back = 0xff;
	bool allowed = false;

	for (size_t i = 0; i < sizeof(writable_regs); i++) {
		allowed |= writable_regs[i] == reg;
	}
	if (broken || !allowed || !ldo.writable || ldo.write == 0 || ldo.id == 0) {
		give_up(allowed ? "a write that may not be made" : "a register that is not to be written");
		return false;
	}
	if (!still_the_ldo()) {
		give_up("the arbiter's channel is no longer the LDO's");
		return false;
	}
	if (mask != 0xff && !get(reg, &now)) {
		give_up("a read that failed");
		return false;
	}
	value = (uint8_t)((now & ~mask) | (value & mask));
	if ((problem = send(reg, value)) != NULL || (problem = spmi_read(reg, &back, 1)) != NULL) {
		say("haptic_ldo: 0x%02x <- 0x%02x: %s\n", reg, value, problem);
		give_up("a write that failed");
		return false;
	}
	if (back != value) {
		say("haptic_ldo: 0x%02x <- 0x%02x, but it reads 0x%02x\n", reg, value, back);
		give_up("a register that did not read back as written");
		return false;
	}
	return true;
}

/*
 * The voltage to be written for `uv': the board's at most (which the start has kept in the LDO's range), the LDO's
 * least at least, the step rounded down; in millivolts.
 * (Done once at the start, and again at the write, so that no path to the register has not been through it.)
 */
static uint32_t
vset_mv(uint32_t uv)
{
	uint32_t top = hakios_board()->vibrator.volt_uv;

	uv = uv > top ? top : uv;
	uv = uv < LDO_VMIN_UV ? LDO_VMIN_UV : uv;
	uv = uv / LDO_STEP_UV * LDO_STEP_UV;
	return uv / 1000;
}

/*
 * The motor on: the voltage, then the enable, then wait for the LDO to say it is ready (VENDOR:150-171, 120-143).
 * The enable counts as written from the first write that may switch it on: one that lands and then does not read
 * back leaves it on all the same, and whatever ends this switches it off only if `running' is set.
 */
static bool
motor_on(void)
{
	uint32_t mv = vset_mv(volt_uv);
	uint8_t status = 0;

	if (!ldo.writable || !put_bits(LDO_VSET_LB, 0xff, (uint8_t)(mv & 0xff)) || !put_bits(LDO_VSET_HB, 0xff, (uint8_t)(mv >> 8))) {
		return false;
	}
	running = true;
	if (!put_bits(LDO_EN_CTL, EN_CTL_EN, EN_CTL_EN)) {
		return false;
	}
	for (uint32_t waited = 0; waited <= LDO_READY_TIMEOUT_US; waited += LDO_READY_POLL_US) {
		if (get(LDO_STATUS1, &status) && (status & STATUS1_VREG_READY)) {
			return true;
		}
		wait_us(LDO_READY_POLL_US);
	}
	say("haptic_ldo: the LDO is not ready within %u us (status 0x%02x): off\n", LDO_READY_TIMEOUT_US, status);
	return false;       /* (the caller puts it off, and gives up: the vendor's "Keep VIB_LDO disabled") */
}

#define OFF_TRIES               5

/*
 * Off: the enable cleared, kept as found in its other bits when they can be read and, when not, written 0 (off never
 * waits on a read that may fail); read back, OFF_TRIES times at most. Not refused once writing has been given up
 * on, and only the enable: it takes nothing else. `running' is cleared as soon as it reads back off.
 */
static bool
motor_off(void)
{
	uint8_t now = 0x00, back = 0xff;
	bool off = false;

	if (!ldo.writable || ldo.write == 0 || ldo.id == 0 || !still_the_ldo()) {
		give_up("a write that may not be made");
		return false;
	}
	for (int tries = 0; tries < OFF_TRIES && !off; tries++) {
		if (tries > 0) {
			wait_ms(5);
		}
		if (spmi_read(LDO_EN_CTL, &now, 1) != NULL) {
			now = 0x00;
		}
		if (send(LDO_EN_CTL, (uint8_t)(now & ~EN_CTL_EN)) == NULL) {
			off = spmi_read(LDO_EN_CTL, &back, 1) == NULL && (back & EN_CTL_EN) == 0;
		}
	}
	if (!off) {
		say("haptic_ldo: THE MOTOR IS NOT SEEN TO GO OFF after %d tries\n", OFF_TRIES);
		give_up("the enable does not read back off");
		return false;
	}
	running = false;
	return true;
}

/* What would make the motor go off now, or NULL: the LDO says it is not ready, or cannot be asked. */
static const char *
trouble(void)
{
	uint8_t status = 0;

	if (!get(LDO_STATUS1, &status)) {
		return "a read of the status that failed";
	}
	return (status & STATUS1_VREG_READY) ? NULL : "the LDO's output is no longer ready";
}

static void
look(void)
{
	uint8_t r[8], id[2] = { 0, 0 }, revision = 0;
	char line[100];

	if (spmi_read(LDO_TYPE, id, 2) != NULL || spmi_read(LDO_REVISION1, &revision, 1) != NULL) {
		say("haptic_ldo: the LDO cannot be read\n");
		return;
	}
	say("haptic_ldo: type %#x, subtype %#x, revision %#x (what an LDO of this kind says is not known beforehand: "
	    "the vendor's driver does not read it)\n", id[0], id[1], revision);
	for (unsigned base = 0; base < 0x50; base += 8) {
		int n = snprintf(line, sizeof(line), "haptic_ldo: 0x%02x:", base);

		if (spmi_read((uint8_t)base, r, 8) != NULL) {
			say("%s (unreadable)\n", line);
			continue;
		}
		for (unsigned j = 0; j < 8; j++) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %02x", r[j]);
		}
		say("%s\n", line);
	}
	say("haptic_ldo: the supply the motor would be given: %u mV (the board's %u uV; the LDO's range %u to %u uV)\n",
	    vset_mv(volt_uv), hakios_board()->vibrator.volt_uv, LDO_VMIN_UV, LDO_VMAX_UV);
}

static void
find_channel(void)
{
	for (uint32_t n = 0; n < arb.channels; n++) {
		uint32_t map = hakios_spmi_channel_map(&arb, n), owner;

		if (map == 0 || SPMI_ID_OF(map) != ldo.id) {
			continue;
		}
		owner = hakios_spmi_channel_owner(&arb, n);
		say("haptic_ldo: channel %u is the vibrator LDO (%#x); written by system %u%s\n", n, map, owner,
		    owner == arb.system ? ", which is us" : "");
		if (!ldo.found || owner == arb.system) {
			ldo.found = true;
			ldo.channel = (uint16_t)n;
			ldo.writable = owner == arb.system;
		}
	}
	if (!ldo.found) {
		say("haptic_ldo: no channel of the arbiter is the vibrator LDO\n");
	}
}

/* ---- haptic_ldo=3: what Darwin plays (as board/haptic.c hears it) ------------------------------------ */

#define ACTIVE_US               30000           /* a write that was not silence counts for this long */
#define SILENT_BELOW            256             /* of 32768: samples no larger are silence */

static volatile uint64_t heard_us;              /* when the last write that was not silence came */
static volatile uint64_t quiet_us;              /* when the last write that was silence came */
static volatile uint64_t onset_us;              /* when Darwin's last waveform began: the first write not silence after a pause */
static uint64_t written_frames;
static unsigned vibrations, capped;

/* From the audio server's I/O thread: never waits. */
static int
haptic_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t samples[256];
	int error = 0;

	while (uio_resid(uio) >= 2) {
		user_ssize_t bytes = uio_resid(uio);
		uint32_t peak = 0;

		if (bytes > (user_ssize_t)sizeof(samples)) {
			bytes = sizeof(samples);
		}
		bytes &= ~(user_ssize_t)1;
		if ((error = uiomove((caddr_t)samples, (int)bytes, uio)) != 0) {
			break;
		}
		for (unsigned i = 0; i < bytes / 2; i++) {
			uint32_t v = samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];

			peak = v > peak ? v : peak;
		}
		written_frames += (uint64_t)bytes / 2;
		if (peak > SILENT_BELOW) {
			uint64_t now = now_us();

			if (heard_us == 0 || quiet_us > heard_us || now - heard_us >= ACTIVE_US) {
				onset_us = now;
			}
			heard_us = now;
		} else {
			quiet_us = now_us();
		}
	}
	return error;
}

static int
haptic_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw haptic_cdevsw = {
	.d_open = haptic_open, .d_close = haptic_open, .d_read = eno_rdwrt, .d_write = haptic_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static void
serve(void)
{
	enum { IDLE, ON, REST } state = IDLE;
	uint64_t since = 0, rest_until = 0, last_look = 0, handled = 0;
	int major = cdevsw_add(-1, &haptic_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-haptic") == NULL) {
		say("haptic_ldo: /dev/hakios-haptic cannot be made\n");
		return;
	}
	say("haptic_ldo: THE MOTOR RUNS WHAT DARWIN PLAYS ON ITS ACTUATOR (/dev/hakios-haptic) from here on, until the run ends: "
	    "at %u mV, at most %u ms at a time\n", vset_mv(volt_uv), max_ms);
	HakiosHapticsReady();           /* the service Darwin's haptic server waits for (iokit/Kernel/BoardHaptics.cpp) */
	while (!broken) {
		uint64_t now = now_us();
		bool active = heard_us != 0 && now - heard_us < ACTIVE_US;

		__atomic_store_n(&writing, true, __ATOMIC_SEQ_CST);
		if (__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
			break;                  /* (the run's end has the motor now: hakios_haptic_ldo_quiesce()) */
		}
		switch (state) {
		case IDLE:
			/* a waveform of Darwin's that has begun and not been run yet (after a pause: a tap of a series is a tap of its own) */
			if (active && onset_us != handled) {
				handled = onset_us;
				if (motor_on()) {
					since = now;
					state = ON;
					vibrations++;
				} else {
					(void)motor_off();
					give_up("the motor did not start");
				}
			}
			break;
		case ON:
			if (now - since >= (uint64_t)max_ms * 1000) {
				capped++;
				say("haptic_ldo: on for %u ms, the limit: off, and at rest for as long\n", max_ms);
				if (!motor_off()) {
					break;
				}
				rest_until = now + (uint64_t)max_ms * 1000;
				state = REST;
			} else if (!active && now - since >= LDO_MIN_PLAY_MS * 1000ull) {
				if (!motor_off()) {
					break;
				}
				state = IDLE;
			} else if (now - last_look >= 20000) {
				const char *why;

				last_look = now;
				if ((why = trouble()) != NULL) {
					say("haptic_ldo: OFF: %s\n", why);
					(void)motor_off();
					give_up("trouble while running");
				}
			}
			break;
		case REST:
			if (now >= rest_until) {
				state = IDLE;
			}
			break;
		}
		__atomic_store_n(&writing, false, __ATOMIC_SEQ_CST);
		wait_ms(5);
	}
	if (running && !__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
		(void)motor_off();
	}
	__atomic_store_n(&writing, false, __ATOMIC_SEQ_CST);
	if (!leaving) {
		HakiosHapticsGone();
	}
	say("haptic_ldo: the motor is given up on (%u vibrations, %u cut at %u ms)\n", vibrations, capped, max_ms);
}

/* ---- the start --------------------------------------------------------------------------------------- */

static void
haptic_ldo_thread(__unused void *parameter, __unused wait_result_t result)
{
	say("haptic_ldo: level %u: %s\n", level, level == 1 ? "a look, reading only" : level == 2 ? "the motor on once" :
	    "the motor for Darwin");
	if (!hakios_spmi_map(&arb)) {
		say("haptic_ldo: PMIC arbiter version %#x, not 5, 7 or 8: nothing done\n", arb.version);
		goto out;
	}
	find_channel();
	if (!ldo.found) {
		goto out;
	}
	look();
	if (level < 2) {
		goto out;
	}
	if (!ldo.writable) {
		say("haptic_ldo: the channel is not ours to write through: nothing written\n");
		goto out;
	}
	ldo.write = hakios_spmi_map_write(&arb, ldo.channel);
	if (level == 2) {
		uint32_t ms = 200;
		const char *why = NULL;

		PE_parse_boot_argn("haptic_ldo_ms", &ms, sizeof(ms));
		ms = ms < LDO_MIN_PLAY_MS ? LDO_MIN_PLAY_MS : ms > max_ms ? max_ms : ms;
		say("haptic_ldo: the motor ON at %u mV for %u ms\n", vset_mv(volt_uv), ms);
		if (motor_on()) {
			for (uint32_t waited = 0; waited < ms && (why = trouble()) == NULL; waited += 10) {
				wait_ms(10);
			}
			if (why != NULL) {
				say("haptic_ldo: OFF early: %s\n", why);
			}
		}
		if (running) {
			/* (also when the start failed half way: put out all the same, not left on with no limit) */
			say("haptic_ldo: the motor is %s\n", motor_off() ? "off again" : "NOT SEEN TO GO OFF");
		}
	} else {
		serve();
	}
out:
	say("haptic_ldo: done\n");
	thread_terminate(current_thread());
}

void
hakios_haptic_ldo_init(void)
{
	uint32_t mv = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("haptic_ldo", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (!hakios_board_has(HAKIOS_HAS_HAPTIC_PMIC_LDO)) {
		/* every address below is one phone's: not on a board that does not say it is wired the same */
		say("haptic_ldo: disabled: this board's board.dts lists no \"haptic-pmic-ldo\"\n");
		level = 0;
		return;
	}
	if (hakios_board()->spmi.core == 0 || hakios_board()->spmi.channels == 0) {
		say("haptic_ldo: disabled: the SoC file's spmi { } does not say where the arbiter and its channels are\n");
		level = 0;
		return;
	}
	if (hakios_board()->vibrator.peripheral == 0 || hakios_board()->vibrator.volt_uv < LDO_VMIN_UV ||
	    hakios_board()->vibrator.volt_uv > LDO_VMAX_UV) {
		say("haptic_ldo: disabled: the board file does not say which peripheral the LDO is, or gives a voltage outside "
		    "%u to %u uV (vibrator { })\n", LDO_VMIN_UV, LDO_VMAX_UV);
		level = 0;
		return;
	}
	ldo.id = hakios_board()->vibrator.peripheral;
	volt_uv = hakios_board()->vibrator.volt_uv;
	if (PE_parse_boot_argn("haptic_ldo_mv", &mv, sizeof(mv)) && mv != 0) {
		if (mv * 1000ull > volt_uv) {
			say("haptic_ldo: haptic_ldo_mv=%u is above the board's %u uV: not more than that\n", mv, volt_uv);
		} else {
			volt_uv = mv * 1000;
		}
	}
	PE_parse_boot_argn("haptic_ldo_max_ms", &max_ms, sizeof(max_ms));
	max_ms = max_ms < LDO_MIN_PLAY_MS ? LDO_MIN_PLAY_MS : max_ms > LDO_MAX_PLAY_MS ? LDO_MAX_PLAY_MS : max_ms;
	level = level > 3 ? 3 : level;
	if (kernel_thread_start(haptic_ldo_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: the motor off, if it was ever on. */
void
hakios_haptic_ldo_quiesce(void)
{
	uint8_t enable = 0xff;

	if (level < 2 || ldo.write == 0) {
		return;
	}
	__atomic_store_n(&leaving, true, __ATOMIC_SEQ_CST);
	for (unsigned i = 0; i < 100 && __atomic_load_n(&writing, __ATOMIC_SEQ_CST); i++) {
		wait_ms(1);             /* (the thread finishing what it writes: 100 ms at most, a stopped CPU's never ends) */
	}
	if (running) {
		/* (off is not refused for `broken': one last time, whatever went before) */
		say("haptic_ldo: at the run's end: the motor %s\n", motor_off() ? "switched off" : "COULD NOT BE SWITCHED OFF");
	}
	(void)spmi_read(LDO_EN_CTL, &enable, 1);
	say("haptic_ldo: at the run's end: %u vibrations (%u cut at %u ms) from %llu frames Darwin wrote; the enable reads 0x%02x%s\n",
	    vibrations, capped, max_ms, written_frames, enable, (enable & EN_CTL_EN) == 0 ? " (off)" : " (NOT OFF)");
}
