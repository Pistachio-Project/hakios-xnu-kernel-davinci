/*
 * The driver of the vibration motor on a PMIC's vibrator LDO (drivers/board/haptic_ldo.c), run as a host program
 * against the model of the PMIC arbiter (spmi_model.h) with an LDO behind it: its enable, its ready flag and its
 * voltage registers as the vendor's driver uses them. The driver writes to a PMIC, which makes the phone's supplies,
 * so what is tested first is what it must never do: write a register that is not the LDO's voltage or enable,
 * give the motor more than the board's voltage, leave it running when something is wrong or when the run ends, or
 * run it past the limit. The board is the test phone's (workbench/devices/phone-keys: slave 2, 2.8 V).
 *
 *   haptic_ldo_driver_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <time.h>
#include <unistd.h>

#include "fakekernel.h"
#include "spmi_model.h"

extern char log_text[];

/* ---- the hardware's model: the LDO ---------------------------------------------------------------- */

#define REG_STATUS1     0x08
#define REG_VSET_LB     0x40
#define REG_VSET_HB     0x41
#define REG_EN_CTL      0x46

static struct spmi_peripheral ldo_model;
static unsigned forbidden_writes;       /* to a register that is not the voltage's or the enable's */
static unsigned highest_mv;             /* the highest voltage the LDO ever had while enabled */
static unsigned times_on;
static bool never_ready;                /* the output does not come up */
static bool stuck;                      /* the LDO does not take what is written */

static bool
is_on(void)
{
	return (ldo_model.reg[REG_EN_CTL] & 0x80) != 0;
}

static void
ldo_written(struct spmi_peripheral *p, uint8_t reg, uint8_t value)
{
	bool was = is_on();
	unsigned mv;

	if (reg != REG_VSET_LB && reg != REG_VSET_HB && reg != REG_EN_CTL) {
		forbidden_writes++;
		fprintf(stderr, "MODEL: a write to register 0x%02x of the vibrator LDO\n", reg);
	}
	if (!stuck) {
		p->reg[reg] = value;
	}
	if (is_on()) {
		mv = p->reg[REG_VSET_LB] | (unsigned)p->reg[REG_VSET_HB] << 8;
		highest_mv = mv > highest_mv ? mv : highest_mv;
		if (!was) {
			times_on++;
		}
		p->reg[REG_STATUS1] = never_ready ? 0x00 : 0x80;
	} else {
		p->reg[REG_STATUS1] = 0x00;
	}
}

static bool
on_now(void)
{
	bool on;

	pthread_mutex_lock(&spmi.lock);
	on = is_on();
	pthread_mutex_unlock(&spmi.lock);
	return on;
}

uint32_t hakios_test_rd(vm_offset_t address) { return spmi_model_rd(address); }
void hakios_test_wr(vm_offset_t address, uint32_t value) { spmi_model_wr(address, value); }
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }

/* the haptic service Darwin waits for (iokit/Kernel/BoardHaptics.cpp) */
static unsigned published, gone;
void HakiosHapticsReady(void);
void HakiosHapticsReady(void) { published++; }
void HakiosHapticsGone(void);
void HakiosHapticsGone(void) { gone++; }

/* the one lock of the bus is the buttons' driver's (bsd/dev/board/buttons.c) */
static pthread_mutex_t bus = PTHREAD_MUTEX_INITIALIZER;
void hakios_spmi_lock(void) { pthread_mutex_lock(&bus); }
void hakios_spmi_unlock(void) { pthread_mutex_unlock(&bus); }

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#define get ldo_get
#include "board/board.c"
#include "board/haptic_ldo.c"
#undef get

static int failures;
/* (to stderr: this driver also prints every line of its log, which main() sends nowhere) */
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

/* a phone with the part: the arbiter where its SoC has it, the LDO as the firmware leaves it */
static void
phone(const char *name)
{
	const struct hakios_board *b;

	board = name;
	b = hakios_board();
	if (b == NULL) {
		return;
	}
	spmi.core = b->spmi.core;
	spmi.cnfg = b->spmi.cnfg;
	spmi.channels = b->spmi.channels;
	spmi.ee = b->spmi.ee;
	spmi.observer = b->spmi.observer + 0x10000 * spmi.ee;
	ldo_model.id = b->vibrator.peripheral;
	ldo_model.written = ldo_written;
	/* the LDO has a channel of another system's first, as real tables do */
	spmi_channel(11, &ldo_model, 3);
	spmi_channel(20, &ldo_model, spmi.ee);
	ldo_model.reg[0x04] = 0x0b;             /* (a made-up type and subtype: the driver only says them) */
	ldo_model.reg[0x05] = 0x01;
	ldo_model.reg[REG_EN_CTL] = 0x01;       /* a bit that is not the enable, to be left as it was */
	ldo_model.reg[REG_VSET_LB] = 0xc0;      /* the firmware's own voltage: 1.5 V */
	ldo_model.reg[REG_VSET_HB] = 0x05;
}

static void
never(void)
{
	CHECK(forbidden_writes == 0, "%u writes to registers that are not the voltage's or the enable's", forbidden_writes);
	CHECK(highest_mv <= hakios_board()->vibrator.volt_uv / 1000, "the LDO ran at %u mV, above the board's %u uV", highest_mv,
	    hakios_board()->vibrator.volt_uv);
	CHECK(spmi.elsewhere == 0, "%u accesses where nothing of the arbiter is", spmi.elsewhere);
}

static void
off_at_the_end(void)
{
	hakios_haptic_ldo_quiesce();
	CHECK(!on_now(), "the motor is on at the run's end");
	CHECK((spmi_get(&ldo_model, REG_EN_CTL) & 0x7f) == 0x01, "the enable's neighbours are not as they were found: %#x",
	    spmi_get(&ldo_model, REG_EN_CTL));
}

/* what Darwin writes to /dev/hakios-haptic: `n' samples at `peak' */
static void
play(int16_t peak, unsigned n)
{
	int16_t samples[256];

	for (unsigned i = 0; i < n && i < 256; i++) {
		samples[i] = (i & 1) ? (int16_t)-peak : peak;
	}
	CHECK(fake_dev_write("hakios-haptic", samples, n * sizeof(samples[0])) == 0, "/dev/hakios-haptic takes samples");
}

static double
seconds(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + t.tv_nsec / 1e9;
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

static void
off_by_default(void)
{
	phone("phone-keys");
	boot_args = "";
	hakios_haptic_ldo_init();
	usleep(200 * 1000);
	CHECK(spmi.transfers == 0 && log_text[0] == 0, "without haptic_ldo=N: not a read, not a word");
	hakios_haptic_ldo_quiesce();
	CHECK(spmi.transfers == 0, "... at the run's end either");
}

/* haptic_ldo=1: the LDO is found and read, and nothing is written */
static void
look_only(void)
{
	phone("phone-keys");
	boot_args = "haptic_ldo=1";
	hakios_haptic_ldo_init();
	WAIT_FOR(logged("haptic_ldo: done"), 5000);
	CHECK(logged("channel 20 is the vibrator LDO (0x25300); written by system 2, which is us"), "the channel that is ours: %s", log_text);
	CHECK(logged("type 0xb, subtype 0x1, revision 0 ("), "its type is said: %s", log_text);
	CHECK(logged("haptic_ldo: 0x40: c0 05"), "its registers are listed: %s", log_text);
	CHECK(logged("2800 mV"), "the supply it would be given: %s", log_text);
	CHECK(spmi.writes == 0 && spmi.denied == 0 && !on_now(), "a look writes nothing (%u writes)", spmi.writes);
	never();
}

/* haptic_ldo=2: on once at the board's voltage, the enable alone changed, then off as it was found */
static void
once(void)
{
	double on;

	phone("phone-keys");
	boot_args = "haptic_ldo=2 haptic_ldo_ms=300";
	hakios_haptic_ldo_init();
	WAIT_FOR(on_now(), 5000);
	on = seconds();
	CHECK(on_now(), "the motor comes on: %s", log_text);
	CHECK(spmi_get(&ldo_model, REG_VSET_LB) == 0xf0 && spmi_get(&ldo_model, REG_VSET_HB) == 0x0a, "at 2800 mV (0x0af0): %#x %#x",
	    spmi_get(&ldo_model, REG_VSET_HB), spmi_get(&ldo_model, REG_VSET_LB));
	CHECK((spmi_get(&ldo_model, REG_EN_CTL) & 0x7f) == 0x01, "the enable's neighbours are left as they were");
	WAIT_FOR(!on_now(), 5000);
	CHECK(seconds() - on > 0.2, "on for the time asked for, not much less: %.2f s", seconds() - on);
	WAIT_FOR(logged("haptic_ldo: done"), 5000);
	CHECK(logged("the motor is off again") && times_on == 1, "off again, on once (%u)", times_on);
	never();
	off_at_the_end();
}

/* the board's voltage is the most; a lower one asked for is taken, in the LDO's range */
static void
voltage_never_above_the_boards(void)
{
	phone("phone-keys");
	boot_args = "haptic_ldo=2 haptic_ldo_mv=3500";
	hakios_haptic_ldo_init();
	WAIT_FOR(logged("haptic_ldo: done"), 5000);
	CHECK(logged("is above the board's 2800000 uV") && highest_mv == 2800, "still the board's 2800 mV (%u)", highest_mv);
	never();
	off_at_the_end();
}

static void
voltage_lower_is_taken(void)
{
	phone("phone-keys");
	boot_args = "haptic_ldo=2 haptic_ldo_mv=2000";
	hakios_haptic_ldo_init();
	WAIT_FOR(logged("haptic_ldo: done"), 5000);
	CHECK(highest_mv == 2000, "2000 mV (%u)", highest_mv);
	never();
	off_at_the_end();
}

static void
voltage_not_below_the_ldos_least(void)
{
	phone("phone-keys");
	boot_args = "haptic_ldo=2 haptic_ldo_mv=100";
	hakios_haptic_ldo_init();
	WAIT_FOR(logged("haptic_ldo: done"), 5000);
	CHECK(highest_mv == 1504, "the LDO's least, 1504 mV (%u)", highest_mv);
	never();
	off_at_the_end();
}

/* haptic_ldo=3: a short waveform of Darwin's is a run of the motor of the vendor's least time, no longer */
static void
a_tap(void)
{
	double on;

	phone("phone-keys");
	boot_args = "haptic_ldo=3";
	hakios_haptic_ldo_init();
	WAIT_FOR(published == 1, 5000);
	CHECK(published == 1 && logged("THE MOTOR RUNS WHAT DARWIN PLAYS"), "the service is published: %s", log_text);
	usleep(100 * 1000);
	CHECK(!on_now() && times_on == 0, "no vibration until Darwin plays one");
	play(20000, 64);
	on = seconds();
	WAIT_FOR(on_now(), 1000);
	CHECK(on_now(), "the motor comes on with Darwin's waveform: %s", log_text);
	WAIT_FOR(!on_now(), 2000);
	CHECK(!on_now() && seconds() - on > 0.04 && seconds() - on < 0.5, "and goes off after the vendor's 50 ms, not before: %.3f s", seconds() - on);
	usleep(150 * 1000);
	CHECK(times_on == 1, "the tail of one waveform is not a second tap (%u)", times_on);
	never();
	off_at_the_end();
}

/* silence is not a waveform */
static void
silence_is_not_played(void)
{
	phone("phone-keys");
	boot_args = "haptic_ldo=3";
	hakios_haptic_ldo_init();
	WAIT_FOR(published == 1, 5000);
	play(100, 64);
	usleep(200 * 1000);
	CHECK(times_on == 0 && spmi.writes == 0, "a whisper does not run the motor (%u, %u writes)", times_on, spmi.writes);
	never();
}

/* a waveform that goes on is followed until it ends, and cut at the limit; the motor then rests */
static void
a_vibration_cut_at_the_limit(void)
{
	double start;

	phone("phone-keys");
	boot_args = "haptic_ldo=3 haptic_ldo_max_ms=200";
	hakios_haptic_ldo_init();
	WAIT_FOR(published == 1, 5000);
	start = seconds();
	for (int i = 0; i < 60 && seconds() - start < 0.6; i++) {
		play(20000, 64);
		usleep(10 * 1000);
	}
	CHECK(logged("on for 200 ms, the limit: off, and at rest for as long"), "cut at the limit: %s", log_text);
	CHECK(!on_now(), "off while Darwin still plays (it rests)");
	CHECK(times_on == 1, "it did not start again within the rest (%u)", times_on);
	never();
	off_at_the_end();
}

/* the run ends with the motor on: it is put off, whoever has stopped */
static void
run_ends_while_on(void)
{
	phone("phone-keys");
	boot_args = "haptic_ldo=3";
	hakios_haptic_ldo_init();
	WAIT_FOR(published == 1, 5000);
	for (int i = 0; i < 20; i++) {
		play(20000, 64);
		usleep(5 * 1000);
	}
	CHECK(on_now(), "the motor is on: %s", log_text);
	hakios_haptic_ldo_quiesce();
	CHECK(!on_now(), "the run's end puts it off");
	CHECK(logged("the motor switched off"), "and says so: %s", log_text);
	never();
	off_at_the_end();
}

/* the output never comes up: off at once (the vendor's "keep it disabled"), and nothing more written */
static void
never_ready_stays_off(void)
{
	phone("phone-keys");
	never_ready = true;
	boot_args = "haptic_ldo=3";
	hakios_haptic_ldo_init();
	WAIT_FOR(published == 1, 5000);
	play(20000, 64);
	WAIT_FOR(logged("not ready within"), 2000);
	WAIT_FOR(logged("no more writing"), 2000);
	CHECK(logged("not ready within") && !on_now(), "off after ready never came: %s", log_text);
	CHECK(logged("no more writing to the LDO"), "and given up on: %s", log_text);
	usleep(100 * 1000);
	CHECK(times_on == 1, "no second try (%u)", times_on);
	never();
	off_at_the_end();
}

/* ready lost while the motor runs: off, and given up on */
static void
ready_lost_while_on(void)
{
	phone("phone-keys");
	boot_args = "haptic_ldo=3";
	hakios_haptic_ldo_init();
	WAIT_FOR(published == 1, 5000);
	for (int i = 0; i < 4; i++) {
		play(20000, 64);
		usleep(10 * 1000);
	}
	CHECK(on_now(), "the motor is on");
	pthread_mutex_lock(&spmi.lock);
	ldo_model.reg[REG_STATUS1] = 0x00;
	pthread_mutex_unlock(&spmi.lock);
	for (int i = 0; i < 20 && on_now(); i++) {
		play(20000, 64);
		usleep(10 * 1000);
	}
	CHECK(!on_now() && logged("the LDO's output is no longer ready"), "off when ready is lost: %s", log_text);
	never();
	off_at_the_end();
}

/* a write that does not read back: given up on, no more writes, and the motor not left on */
static void
write_does_not_stick(void)
{
	phone("phone-keys");
	stuck = true;
	boot_args = "haptic_ldo=2";
	hakios_haptic_ldo_init();
	WAIT_FOR(logged("haptic_ldo: done"), 5000);
	CHECK(logged("but it reads") && logged("no more writing to the LDO"), "said: %s", log_text);
	CHECK(!on_now() && times_on == 0, "nothing was switched on (%u)", times_on);
	never();
}

/* the channel is another system's: nothing is written */
static void
not_ours_to_write(void)
{
	phone("phone-keys");
	spmi.owner[20] = 3;
	boot_args = "haptic_ldo=2";
	hakios_haptic_ldo_init();
	WAIT_FOR(logged("haptic_ldo: done"), 5000);
	CHECK(logged("not ours to write through") && spmi.writes == 0, "no write: %s", log_text);
	never();
}

/* what is written is the board's at most, the LDO's least at least, on the LDO's 8 mV step rounded down */
static void
vset_clamps(void)
{
	phone("phone-keys");
	CHECK(vset_mv(9000000) == 2800 && vset_mv(2800000) == 2800, "never above the board's 2800 mV: %u", vset_mv(9000000));
	CHECK(vset_mv(0) == 1504 && vset_mv(1504000) == 1504, "never below the LDO's 1504 mV: %u", vset_mv(0));
	CHECK(vset_mv(2007999) == 2000 && vset_mv(2000000) == 2000, "rounded down to the step: %u", vset_mv(2007999));
}

/* the board does not list the feature */
static void
board_without_the_part(void)
{
	phone("phone-cmd");
	boot_args = "haptic_ldo=3";
	hakios_haptic_ldo_init();
	usleep(200 * 1000);
	CHECK(logged("lists no \"haptic-pmic-ldo\""), "said in the log: %s", log_text);
	CHECK(spmi.transfers == 0 && spmi.elsewhere == 0, "nothing read or written");
	hakios_haptic_ldo_quiesce();
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "off_by_default", off_by_default }, { "look_only", look_only }, { "once", once },
		{ "voltage_never_above_the_boards", voltage_never_above_the_boards }, { "voltage_lower_is_taken", voltage_lower_is_taken },
		{ "voltage_not_below_the_ldos_least", voltage_not_below_the_ldos_least },
		{ "a_tap", a_tap }, { "silence_is_not_played", silence_is_not_played },
		{ "a_vibration_cut_at_the_limit", a_vibration_cut_at_the_limit }, { "run_ends_while_on", run_ends_while_on },
		{ "never_ready_stays_off", never_ready_stays_off }, { "ready_lost_while_on", ready_lost_while_on },
		{ "write_does_not_stick", write_does_not_stick }, { "not_ours_to_write", not_ours_to_write },
		{ "board_without_the_part", board_without_the_part }, { "vset_clamps", vset_clamps },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			if (freopen("/dev/null", "w", stdout) == NULL) {
				return 2;
			}
			scenarios[i].run();
			fprintf(stderr, "%-20s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
