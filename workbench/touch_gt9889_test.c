/*
 * The driver of a Goodix GT9889 touch controller on I2C (drivers/board/gt9889.c), run as it is against a model of the
 * SoC's pins with the chip on two of them: an I2C device (i2c_device_model.h) with 16-bit registers, a report block at
 * 0x4100 that it puts out as the vendor's driver reads it and takes back when the event byte is written 0, and an
 * interrupt line that is low while a report waits. It answers only while it has its analog supply (RPMh's regulator and
 * the pin), its reset is let go with INT high (the address select) and not for a while after; RPMh and its command
 * database are stood in for, each request noted. The model is the report layout of gt9889_report.h, as sure and as
 * unsure as that file: it holds the driver to what the driver was written for, not to a chip. The board is the one the
 * test phone (devices/phone-gt9889) has: davinci's wiring, on a made-up chip.
 *
 * What is tested: a report into fingers (checksum, counts, ids, the pen, positions off the panel); nothing without the
 * boot argument or the board's wiring; a look (touchscreen=-1) reads the report block and powers, configures and writes
 * nothing; the bring-up's steps in their order, the analog supply's pin before its regulator, INT high when the reset is
 * let go and an input after, and stopped where gt9889_steps says; a supply RPMh refuses; one finger and several with
 * their exact positions and numbers, the second read for more than one finger, the event byte written 0 after each
 * report; mirrored touches; a report whose sum is wrong; a firmware request; a chip that dozes while nobody touches it
 * (not trouble) and one that falls silent with a finger down (lifted, reset, given up); and the run's end
 * (hakios_gt9889_quiesce()).
 *
 *   touch_gt9889_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"

/* which pins the chip is on is the board's: the model is put where the board the driver reads says */
static unsigned sda_pin = 6, scl_pin = 7, int_pin = 9, reset_pin = 8;
static int avdd_pin = 5;
static unsigned chip_address = 0x5d;
#define I2C_DEVICE_SDA          sda_pin
#define I2C_DEVICE_SCL          scl_pin
#define I2C_DEVICE_ADDRESS      chip_address
#include "i2c_device_model.h"

extern char log_text[];

static uint64_t real_ns(void) { return mach_absolute_time(); }

/* ---- what the driver posts ----------------------------------------------------------------------------------------- */

static struct post { uint32_t type, finger, x, y; uint64_t t; } posted[512];
static volatile unsigned posts;
static pthread_mutex_t post_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool in_quiesce;
static volatile unsigned posts_in_quiesce;

void
hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	pthread_mutex_lock(&post_lock);
	if (posts < 512) {
		posted[posts] = (struct post){ type, finger, x, y, real_ns() };
	}
	posts++;
	posts_in_quiesce += in_quiesce;
	pthread_mutex_unlock(&post_lock);
}

/* ---- RPMh and its command database, in place of board/cmd_db.h and board/rpmh.h ------------------------------------- */

static volatile unsigned locks, locks_in_quiesce;
void hakios_rpmh_lock(void) { locks++; locks_in_quiesce += in_quiesce; }
void hakios_rpmh_unlock(void) { }

#define HAKIOS_CMD_DB_H
struct cmd_db_header { uint8_t magic[4]; };
static bool hakios_cmd_db_valid(const struct cmd_db_header *db __unused) { return true; }
/* a regulator's address by its name (the test phone's one); RPMh's regulators are at 0x4xxxx */
static uint32_t hakios_cmd_db_address(const struct cmd_db_header *db __unused, uint32_t s __unused, const char *n)
{
	return strcmp(n, "ldoc6") == 0 ? 0x40600 : 0x30f00;
}

#define ANALOG_SUPPLY   0x40600
static struct { uint32_t address, value; } asked[64];   /* every write asked of RPMh, in order */
static unsigned asks;
static uint32_t refused_supply;                 /* a supply whose switching on RPMh does not answer */
static bool analog_on;                          /* the regulator, as RPMh has it */
static unsigned avdd_high_at_ask = ~0u;         /* the pin's level when the regulator was first asked for (1: high) */
static void chip_state(void);
static bool pin_high(unsigned pin);

#define HAKIOS_RPMH_H
static const char *
hakios_rpmh_send_rsc(vm_offset_t r __unused, uint32_t o __unused, const uint32_t *a, const uint32_t *d, unsigned c)
{
	for (unsigned i = 0; i < c; i++) {
		if (asks < 64) {
			asked[asks].address = a[i];
			asked[asks].value = d[i];
		}
		asks++;
	}
	if (avdd_high_at_ask == ~0u && avdd_pin >= 0) {
		avdd_high_at_ask = pin_high((unsigned)avdd_pin);
	}
	if (refused_supply != 0 && c == 3 && a[0] == refused_supply) {
		return "no answer to the request";
	}
	for (unsigned i = 0; i < c; i++) {
		if (a[i] == ANALOG_SUPPLY + 4) {
			analog_on = d[i] != 0;
		}
	}
	chip_state();
	return NULL;
}

/* the first (or, from the end, the last) time that register was asked to hold that value; -1: never */
static int
ask_of(uint32_t address, uint32_t value, bool last)
{
	int found = -1;

	for (unsigned i = 0; i < asks && i < 64; i++) {
		if (asked[i].address == address && asked[i].value == value && (last || found < 0)) {
			found = (int)i;
		}
	}
	return found;
}

/* ---- the chip -------------------------------------------------------------------------------------------------------- */

#define REPORT_AT       0x4100
#define BOOT_NS         (15 * 1000000ull)       /* how long the chip takes after its reset or its power (the driver's waits
                                                 * are five times shorter here: it gives it 21 ms, 105 of its own) */

static unsigned chip_contacts = 10;
static bool was_powered, was_released;
static bool running;                            /* it has its supply and its reset is let go */
static bool address_selected;                   /* INT was high when the reset was let go: its address is 0x5d */
static uint64_t ready_at, reset_low_at, shortest_reset_ns = ~0ull;
static unsigned boots, wrong_address_boots;
static unsigned starts_seen, early_starts, starts_while_off;
static volatile bool silent;                    /* it does not acknowledge its address */
static volatile bool dozing;                    /* ... only while nobody touches it (its report block not pending) */
static bool hung;                               /* ... until it is reset */
static unsigned hang_after, reports_since_boot;
static volatile unsigned reports_read;          /* reads of the event byte */
static volatile unsigned second_reads;          /* reads of the fingers after the first */
static volatile unsigned clears;                /* the event byte written 0 */
static unsigned data_writes;                    /* bytes written into the chip but its registers' numbers */
static unsigned writes_elsewhere;               /* ... into registers but the event byte's */

/* the report it puts out, set by the scenario; `pending' while the host has not taken it (and `steady': it is said again) */
static uint8_t rep[96];
static volatile bool pending, steady;
static pthread_mutex_t stage_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned ptr, last_start;                /* the register the next byte is of; the start that set it */

static bool
pin_high(unsigned pin)
{
	if (model.cfg[pin] & 1u << 9) {
		return (model.io[pin] & 2) != 0;
	}
	return (model.cfg[pin] & 3) == 3;       /* (let go: where its pull takes it) */
}

static void
chip_state(void)
{
	bool power = analog_on && (avdd_pin < 0 || pin_high((unsigned)avdd_pin)), released = pin_high(reset_pin);
	uint64_t now = real_ns();

	if (was_released && !released) {
		reset_low_at = now;
	}
	if (power && released && !(was_powered && was_released)) {
		if (was_powered && now - reset_low_at < shortest_reset_ns) {
			shortest_reset_ns = now - reset_low_at;
		}
		boots++;
		address_selected = pin_high(int_pin);
		wrong_address_boots += !address_selected;
		hung = false;
		reports_since_boot = 0;
		ready_at = now + BOOT_NS;
	}
	was_powered = power;
	was_released = released;
	running = power && released;
}

static void
wires_changed(void)
{
	unsigned before = device.starts;

	chip_state();
	device.absent = !running || !address_selected || real_ns() < ready_at || silent || hung || (dozing && !pending);
	changed();
	if (device.starts != before) {
		starts_seen++;
		starts_while_off += !running;
		early_starts += running && real_ns() < ready_at;
	}
}

/* the 16-bit register's high byte arrives as the register of a transfer, the low byte as its first byte "written" */
static void
chip_written(uint8_t high, uint8_t value)
{
	if (device.starts != last_start) {
		last_start = device.starts;
		ptr = (unsigned)high << 8 | value;
		return;
	}
	data_writes++;
	if (ptr == REPORT_AT && value == 0) {
		clears++;
		pthread_mutex_lock(&stage_lock);
		if (!steady) {
			pending = false;
			model.input[int_pin] = true;
		}
		pthread_mutex_unlock(&stage_lock);
	} else {
		writes_elsewhere++;
	}
	ptr++;
}

static uint8_t
chip_read(uint8_t high __unused)
{
	uint8_t byte = 0;

	if (ptr == REPORT_AT) {
		reports_read++;
		if (hang_after != 0 && ++reports_since_boot >= hang_after) {
			hung = true;            /* (from the next transfer on) */
		}
	}
	if (ptr == REPORT_AT + 12) {
		second_reads++;
	}
	pthread_mutex_lock(&stage_lock);
	if (pending && ptr >= REPORT_AT && ptr < REPORT_AT + sizeof(rep)) {
		byte = rep[ptr - REPORT_AT];
	}
	pthread_mutex_unlock(&stage_lock);
	ptr++;
	return byte;
}

struct finger { unsigned id, x, y; };

/* the report with these fingers, its sum right; `again': the chip says it again after it was taken (a finger that stays) */
static void
chip_says(const struct finger *f, unsigned n, bool again)
{
	uint8_t sum = 0;

	pthread_mutex_lock(&stage_lock);
	memset(rep, 0, sizeof(rep));
	rep[0] = 0x80;
	rep[1] = (uint8_t)n;
	for (unsigned i = 0; i < n; i++) {
		uint8_t *s = &rep[2 + 8 * i];

		s[0] = (uint8_t)f[i].id;
		s[1] = (uint8_t)f[i].x;
		s[2] = (uint8_t)(f[i].x >> 8);
		s[3] = (uint8_t)f[i].y;
		s[4] = (uint8_t)(f[i].y >> 8);
		s[5] = 0x20;
		s[7] = 0x10;
	}
	for (unsigned i = 0; i < 3 + 8 * n; i++) {
		sum = (uint8_t)(sum + rep[i]);
	}
	rep[3 + 8 * n] = (uint8_t)-sum;
	steady = again;
	pending = true;
	model.input[int_pin] = false;           /* its interrupt line: low while a report waits */
	pthread_mutex_unlock(&stage_lock);
}

static void
no_finger(void)
{
	pthread_mutex_lock(&stage_lock);
	pending = false;
	steady = false;
	model.input[int_pin] = true;
	pthread_mutex_unlock(&stage_lock);
}

static void
one_finger(unsigned id, unsigned x, unsigned y)
{
	const struct finger f = { id, x, y };

	chip_says(&f, 1, true);
}

/* ---- the registers: the pins' model, with the chip on them ----------------------------------------------------------- */

static unsigned pin_writes[PINS];

uint32_t
hakios_test_rd(vm_offset_t address)
{
	i2c_device_looked_at(address);
	return tlmm_model_rd(address);
}

static pthread_t scenario_thread;
static bool (*volatile stop_after)(vm_offset_t address);
static volatile bool stopped;

void
hakios_test_wr(vm_offset_t address, uint32_t value)
{
	uint32_t pin = (uint32_t)(address - model.base) / 0x1000;

	if (address >= model.base && pin < PINS) {
		pin_writes[pin]++;
	}
	tlmm_model_wr(address, value);
	if (stop_after != NULL && !pthread_equal(pthread_self(), scenario_thread) && stop_after(address)) {
		stopped = true;
		for (;;) {
			usleep(100000);
		}
	}
}

static unsigned mappings;
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { mappings++; return phys; }
vm_offset_t ml_io_map_wcomb(vm_offset_t phys, vm_size_t size __unused) { mappings++; return phys; }

void hakios_gt9889_init(void);
void hakios_gt9889_quiesce(void) __attribute__((weak));        /* (a driver without one is shown, not a build that fails) */

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
#include "board/gt9889.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited_ = 0; !(cond) && waited_ < (ms); waited_ += 2) usleep(2000); } while (0)

/* ---- a phone, its pins as the firmware left them ------------------------------------------------------------------ */

#define BUS_AS_FOUND    (1u << 2 | 3u)          /* the bus's pins: the serial engine's function, pulled up */
#define INT_AS_FOUND    (1u)                    /* the interrupt's: an input, pulled down */
#define RESET_AS_FOUND  (1u << 9 | 2u << 6)     /* the reset's: an output of 6 mA, driving low (the chip in reset) */
#define AVDD_AS_FOUND   (1u)                    /* the analog supply's switch: an input, pulled down (off) */

static uint32_t cfg_found[PINS], io_found[PINS];

/* the phone as the firmware leaves it: the chip unpowered and in reset, or (`left_running') powered and let go long ago */
static void
phone(const char *name, bool left_running)
{
	const struct hakios_board *b;

	board = name;
	fake_delay_divisor = 5;
	b = hakios_board();
	tlmm_model_at(b->pins.tlmm);
	if (b->touch_i2c.sda != b->touch_i2c.scl) {
		sda_pin = b->touch_i2c.sda;
		scl_pin = b->touch_i2c.scl;
		int_pin = b->touch_i2c.interrupt;
		reset_pin = b->touch_i2c.reset;
		avdd_pin = b->touch_i2c.analog_enable;
		chip_address = b->touch_i2c.address;
		chip_contacts = b->touch_i2c.contacts;
	}
	model.cfg[sda_pin] = model.cfg[scl_pin] = BUS_AS_FOUND;
	model.cfg[int_pin] = INT_AS_FOUND;
	model.cfg[reset_pin] = RESET_AS_FOUND;
	model.io[reset_pin] = left_running ? 2 : 0;
	model.wanted[reset_pin] = left_running;
	if (avdd_pin >= 0) {
		model.cfg[avdd_pin] = left_running ? (1u << 9) : AVDD_AS_FOUND;
		model.io[avdd_pin] = left_running ? 2 : 0;
		model.wanted[avdd_pin] = left_running;
	}
	analog_on = left_running;
	address_selected = left_running;
	memcpy(cfg_found, model.cfg, sizeof(cfg_found));
	memcpy(io_found, model.io, sizeof(io_found));
	memset(device.reg, 0, sizeof(device.reg));
	device.read = chip_read;
	device.written = chip_written;
	no_finger();
	chip_state();
	address_selected = left_running;         /* (as found, started long ago: chip_state() saw INT as the firmware left it) */
	ready_at = 0;                           /* (as found it started long ago, if it runs) */
	boots = 0;
	model.after_write = wires_changed;
	device.absent = !running;
	lines();
	device.was_sda = device.was_scl = true;
}

/* every pin as the firmware left it; the reset's (`in_reset') with its level low under the configuration it had */
static void
pins_as_found(const char *when, bool in_reset)
{
	CHECK(device.driven_high == 0, "%s: a line of the bus was driven high %u times", when, device.driven_high);
	CHECK(model.elsewhere == 0, "%s: %u accesses outside the pins' registers", when, model.elsewhere);
	CHECK(model.drove_wrong == 0, "%s: a pin was made an output before its level was said, %u times", when, model.drove_wrong);
	for (unsigned i = 0; i < PINS; i++) {
		uint32_t io = i == reset_pin && in_reset ? 0 : io_found[i];

		if (model.cfg[i] != cfg_found[i] || (model.io[i] & 2) != (io & 2)) {
			CHECK(false, "%s: gpio%u is %#x/%#x, was found %#x/%#x", when, i, model.cfg[i], model.io[i], cfg_found[i], io_found[i]);
		}
	}
}

/* nothing but the bus's two pins was written to */
static void
only_the_bus_touched(const char *when)
{
	for (unsigned i = 0; i < PINS; i++) {
		CHECK(pin_writes[i] == 0 || i == sda_pin || i == scl_pin, "%s: gpio%u was written %u times", when, i, pin_writes[i]);
	}
}

/* Powered off: in reset, the regulator's vote withdrawn, the switch's pin as found, without RPMh's lock if at the run's end */
static void
powered_off(const char *when)
{
	int off = ask_of(ANALOG_SUPPLY + 4, 0, true), was_on = ask_of(ANALOG_SUPPLY + 4, 1, true);

	CHECK(off > was_on && !analog_on, "%s: the regulator's vote withdrawn (on %d, off %d)", when, was_on, off);
	CHECK(!pin_high(reset_pin), "%s: the chip is not held in reset", when);
	CHECK(logged("gt9889: powered off"), "%s: said: %s", when, log_text);
	pins_as_found(when, true);
}

static void
start(const char *arguments)
{
	boot_args = arguments;
	hakios_gt9889_init();
}

static void
ready_on(const char *name, const char *arguments)
{
	phone(name, false);
	start(arguments);
	WAIT_FOR(logged("touch screen ready") || logged("touch screen unavailable"), 10000);
	CHECK(logged("touch screen ready"), "the chip brought up: %s", log_text);
}

static void
ready(void)
{
	ready_on("phone-gt9889", "touchscreen=1");
}

/* until the driver has read the chip's report block as it is now, twice */
static void
taken(void)
{
	unsigned before = reports_read;

	WAIT_FOR(reports_read >= before + 2, 3000);
	CHECK(reports_read >= before + 2, "the driver goes on reading reports: %u since: %s", reports_read - before, log_text);
	usleep(2000);                           /* (and has made of the last what it makes of it) */
}

static unsigned
scaled(unsigned raw, unsigned most)
{
	return raw * 65535 / most;
}

/* the post numbered n (from 0) is that one */
static void
post_is(unsigned n, uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	struct post p = { 0 };
	unsigned count;

	pthread_mutex_lock(&post_lock);
	count = posts;
	if (n < count && n < 512) {
		p = posted[n];
	}
	pthread_mutex_unlock(&post_lock);
	CHECK(n < count, "post %u (type %u finger %u at %u, %u) was not made: %u posts", n, type, finger, x, y, count);
	if (n < count) {
		CHECK(p.type == type && p.finger == finger && p.x == x && p.y == y, "post %u is type %u finger %u at %u, %u, not type %u "
		    "finger %u at %u, %u", n, p.type, p.finger, p.x, p.y, type, finger, x, y);
	}
}

static void
quiesce(void)
{
	void (*volatile put_away)(void) = hakios_gt9889_quiesce;

	CHECK(put_away != NULL, "the driver has nothing for the run's end (no hakios_gt9889_quiesce())");
	if (put_away != NULL) {
		in_quiesce = true;
		put_away();
		in_quiesce = false;
	}
}

static int
said_at(const char *text)
{
	const char *at = strstr(log_text, text);

	return at != NULL ? (int)(at - log_text) : -1;
}

/* ---- scenarios: a report into fingers (gt9889_report.h) -------------------------------------------------------------- */

/* a report of n fingers (id, x, y each) with its sum right */
static unsigned
build(uint8_t *out, uint8_t event, const struct finger *f, unsigned n)
{
	uint8_t sum = 0;

	memset(out, 0, 4 + 8 * n);
	out[0] = event;
	out[1] = (uint8_t)n;
	for (unsigned i = 0; i < n; i++) {
		out[2 + 8 * i] = (uint8_t)f[i].id;
		out[3 + 8 * i] = (uint8_t)f[i].x;
		out[4 + 8 * i] = (uint8_t)(f[i].x >> 8);
		out[5 + 8 * i] = (uint8_t)f[i].y;
		out[6 + 8 * i] = (uint8_t)(f[i].y >> 8);
	}
	for (unsigned i = 0; i < 3 + 8 * n; i++) {
		sum = (uint8_t)(sum + out[i]);
	}
	out[3 + 8 * n] = (uint8_t)-sum;
	return 4 + 8 * n;
}

static void
report_decode(void)
{
	uint8_t r[96];
	struct gt9889_frame f = { .active = 0x155 };
	const struct finger two[2] = { { 3, 0x123, 0x456 }, { 9, 1079, 0 } };
	const struct finger twice[2] = { { 3, 1, 1 }, { 3, 2, 2 } };
	const struct finger far_x[1] = { { 0, 1080, 5 } }, far_y[1] = { { 0, 5, 2340 } }, at_edge[1] = { { 0, 1079, 2339 } };
	const struct finger high_id[1] = { { 5, 1, 1 } };
	unsigned len;

	len = build(r, 0x80, two, 2);
	CHECK(gt9889_decode(r, len, 10, 1079, 2339, &f) == GT9889_FRAME && f.active == (1u << 3 | 1u << 9), "two fingers: %#x", f.active);
	CHECK(f.x[3] == 0x123 && f.y[3] == 0x456 && f.x[9] == 1079 && f.y[9] == 0, "where: %u, %u and %u, %u", f.x[3], f.y[3], f.x[9], f.y[9]);
	len = build(r, 0x80, NULL, 0);
	f.active = 0x155;
	CHECK(gt9889_decode(r, len, 10, 1079, 2339, &f) == GT9889_FRAME && f.active == 0, "a touch event of no finger is all lifted");
	r[0] = 0;
	CHECK(gt9889_decode(r, 12, 10, 1079, 2339, &f) == GT9889_NOTHING, "an event byte of 0 is nothing new");
	r[0] = 0x20;
	CHECK(gt9889_decode(r, 12, 10, 1079, 2339, &f) == GT9889_BAD, "a gesture is not a touch this file knows");
	r[0] = 0x40;
	CHECK(gt9889_decode(r, 12, 10, 1079, 2339, &f) == GT9889_REQUEST, "a request is told apart");
	len = build(r, 0x80, two, 2);
	r[len - 1] ^= 1;
	CHECK(gt9889_decode(r, len, 10, 1079, 2339, &f) == GT9889_BAD, "a sum that is not 0 is not a report");
	len = build(r, 0x80, two, 2);
	CHECK(gt9889_decode(r, len - 1, 10, 1079, 2339, &f) == GT9889_BAD, "a report cut short is not one");
	CHECK(gt9889_decode(r, len, 1, 1079, 2339, &f) == GT9889_BAD, "two fingers of a chip of one");
	len = build(r, 0x80, twice, 2);
	CHECK(gt9889_decode(r, len, 10, 1079, 2339, &f) == GT9889_BAD && f.active == 0, "a finger said twice is not a report");
	len = build(r, 0x80, far_x, 1);
	CHECK(gt9889_decode(r, len, 10, 1079, 2339, &f) == GT9889_BAD, "x off the panel");
	len = build(r, 0x80, far_y, 1);
	CHECK(gt9889_decode(r, len, 10, 1079, 2339, &f) == GT9889_BAD, "y off the panel");
	len = build(r, 0x80, at_edge, 1);
	CHECK(gt9889_decode(r, len, 10, 1079, 2339, &f) == GT9889_FRAME && f.x[0] == 1079 && f.y[0] == 2339, "the far corner is taken");
	len = build(r, 0x80, high_id, 1);
	CHECK(gt9889_decode(r, len, 5, 1079, 2339, &f) == GT9889_BAD, "a finger numbered past the chip's");
	len = build(r, 0x80, at_edge, 1);
	r[2] = 0x80;
	r[len - 1] = (uint8_t)(r[len - 1] - 0x80);
	CHECK(gt9889_decode(r, len, 10, 1079, 2339, &f) == GT9889_FRAME && f.active == 0, "the pen (0x80 and up) is not a finger");
	{
		uint8_t head[12] = { 0x80, 3 };

		CHECK(gt9889_more(head, 10) == 16 && (head[1] = 1, gt9889_more(head, 10)) == 0 && (head[1] = 11, gt9889_more(head, 10)) == 0 &&
		    (head[0] = 0, head[1] = 3, gt9889_more(head, 10)) == 0, "the second read: (n - 1) fingers, none for no touch event");
	}
}

/* ---- scenarios: the driver -------------------------------------------------------------------------------------------- */

/* without its boot argument, or with touchscreen=0: nothing */
static void
off_by_default(void)
{
	phone("phone-gt9889", false);
	start("");
	start("touchscreen=0");
	usleep(50000);
	CHECK(model.writes == 0 && model.reads == 0 && mappings == 0 && !logged("gt9889:"), "nothing touched, nothing said: %s", log_text);
	quiesce();
	CHECK(model.writes == 0 && model.reads == 0 && asks == 0 && posts == 0, "and nothing at the run's end");
}

/* on a board that does not say where the chip is: said, nothing touched */
static void
no_wiring(void)
{
	phone("phone-video", false);
	start("touchscreen=1");
	usleep(50000);
	CHECK(logged("gt9889: disabled: the board file does not say how the controller is wired (touch-i2c { })"), "said: %s", log_text);
	CHECK(model.writes == 0 && model.reads == 0 && mappings == 0, "nothing touched");
	quiesce();
	CHECK(model.writes == 0 && model.reads == 0, "nor at the run's end");
}

/* touchscreen=-1 on a chip the firmware left running: its report block read and said, nothing powered, configured or written */
static void
look_only(void)
{
	phone("phone-gt9889", true);
	one_finger(0, 100, 100);                /* (a finger on it: a look reads the block and does not take the report) */
	start("touchscreen=-1");
	WAIT_FOR(logged("nothing powered, configured or written"), 5000);
	CHECK(logged("gt9889: touchscreen=-1: looking only") && logged("gt9889: step 1: the pins as they were found"), "said: %s", log_text);
	CHECK(logged("gt9889: gpio6 (I2C data): function 1, pull 3, 2 mA, input") &&
	    logged("gt9889: gpio8 (reset): function 0, pull 0, 6 mA, drives high") &&
	    logged("gt9889: gpio9 (interrupt): function 0, pull 1, 2 mA, input, reads 0") &&
	    logged("gt9889: gpio5 (analog supply's switch): function 0, pull 0, 2 mA, drives high"), "each pin as it was found: %s", log_text);
	CHECK(logged("gt9889: the chip at 0x5d answers: its report block (0x4100) reads 80 01 00 64 00 64 00 20 00 10"), "the block: %s", log_text);
	CHECK(!logged("step 2") && !logged("step 3") && !logged("step 5") && !logged("held in reset"), "no other step: %s", log_text);
	CHECK(data_writes == 0 && clears == 0, "%u bytes written into the chip", data_writes);
	CHECK(device.starts >= 2 && device.starts == 2 * device.stops, "read in whole transfers: %u starts, %u stops", device.starts, device.stops);
	CHECK(asks == 0 && locks == 0, "RPMh asked %u times", asks);
	CHECK(mappings == 1, "only the pins mapped: %u mappings", mappings);
	CHECK(posts == 0, "nothing posted (%u)", posts);
	only_the_bus_touched("a look");
	pins_as_found("a look", false);
	usleep(30000);
	CHECK(device.starts == starts_seen && posts == 0 && clears == 0, "nor later");
	quiesce();
	CHECK(data_writes == 0 && asks == 0 && pin_high(reset_pin), "nor anything at the run's end: the chip left running");
	only_the_bus_touched("a look, at the run's end");
	pins_as_found("a look, at the run's end", false);
	CHECK(!logged("powered off"), "nothing was powered: %s", log_text);
}

/* touchscreen=-1 on a chip the firmware left in reset and unpowered: said, and left so */
static void
look_at_a_chip_in_reset(void)
{
	phone("phone-gt9889", false);
	start("touchscreen=-1");
	WAIT_FOR(logged("nothing powered, configured or written"), 5000);
	CHECK(logged("gt9889: the chip is held in reset as found (gpio8 drives low): it is not expected to answer"), "said: %s", log_text);
	CHECK(logged("gt9889: nobody answers at 0x5d"), "said: %s", log_text);
	CHECK(device.starts == 1, "asked once: %u", device.starts);
	CHECK(data_writes == 0 && asks == 0 && boots == 0, "nothing written, powered or reset");
	only_the_bus_touched("a look");
	pins_as_found("a look", false);
	quiesce();
	pins_as_found("a look, at the run's end", false);
}

/* touchscreen=1: the steps in their order, the pin before the regulator, INT high at the reset's release, nothing configured */
static void
brought_up_in_steps(void)
{
	static const char *const steps_said[] = { "gt9889: step 1: the pins as they were found",
		"gt9889: step 2: the chip held in reset, INT driven high, its supplies switched on (I/O not ours to switch, analog ldoc6 and its pin)",
		"gt9889: regulator ldoc6 at 0x40600: 3000 mV",
		"gt9889: step 3: the reset low 2 ms and let go, INT let go after it, the chip given 100 ms",
		"gt9889: step 4: the first transfers on the I2C bus (gpio6, gpio7): the chip's report block",
		"gt9889: the chip at 0x5d answers: its report block (0x4100) reads 00 00",
		"gt9889: step 5: touch screen ready, 1079 x 2339, 10 fingers; the interrupt line (gpio9) is looked at every 8 ms" };
	int before = -1;

	ready();
	for (unsigned i = 0; i < sizeof(steps_said) / sizeof(steps_said[0]); i++) {
		CHECK(said_at(steps_said[i]) > before, "said, and after the one before: \"%s\": %s", steps_said[i], log_text);
		before = said_at(steps_said[i]);
	}
	CHECK(asks == 3 && asked[0].address == ANALOG_SUPPLY && asked[0].value == 3000 && asked[1].address == ANALOG_SUPPLY + 4 &&
	    asked[1].value == 1 && asked[2].address == ANALOG_SUPPLY + 8 && asked[2].value == 7, "the regulator at the board's 3000 mV: %u asks", asks);
	CHECK(avdd_high_at_ask == 1, "the switch's pin high before the regulator was asked for: %u", avdd_high_at_ask);
	CHECK(locks == 1, "under RPMh's lock: %u", locks);
	CHECK(boots == 1 && wrong_address_boots == 0, "the chip started once, INT high when the reset was let go: %u, %u", boots, wrong_address_boots);
	CHECK(starts_while_off == 0, "%u transfers begun while the chip was unpowered or in reset", starts_while_off);
	CHECK(early_starts == 0, "%u transfers begun before the chip had its time after the reset", early_starts);
	CHECK(pin_high(reset_pin) && (model.cfg[reset_pin] & 1u << 9), "the reset let go, driven high");
	CHECK((model.cfg[int_pin] & (1u << 9 | 0x3c | 3u)) == 3u, "the interrupt's pin an input, pulled up: %#x", model.cfg[int_pin]);
	CHECK(pin_high((unsigned)avdd_pin) && (model.cfg[avdd_pin] & 1u << 9), "the analog supply's pin driven high");
	CHECK(data_writes == 0, "%u bytes written into the chip before anything was reported", data_writes);
	CHECK(device.driven_high == 0 && model.drove_wrong == 0 && model.elsewhere == 0, "the pins used as they are meant");
	CHECK(mappings == 3, "the pins, RPMh and its command database mapped: %u", mappings);
	CHECK(posts == 0, "nothing posted of a chip nobody touches: %u", posts);
}

static void
stops_after_step_1(void)
{
	phone("phone-gt9889", false);
	start("touchscreen=1 gt9889_steps=1");
	WAIT_FOR(logged("stopping here"), 5000);
	CHECK(logged("gt9889: step 1") && logged("gt9889: stopping here (gt9889_steps=1)") && !logged("step 2"), "said: %s", log_text);
	CHECK(model.writes == 0 && asks == 0 && device.starts == 0, "only looked at: %u writes", model.writes);
	quiesce();
	CHECK(model.writes == 0 && asks == 0, "nor anything at the run's end");
}

static void
stops_after_step_2(void)
{
	phone("phone-gt9889", false);
	start("touchscreen=1 gt9889_steps=2");
	WAIT_FOR(logged("stopping here"), 5000);
	CHECK(logged("gt9889: step 2") && logged("gt9889: stopping here (gt9889_steps=2)") && !logged("step 3"), "said: %s", log_text);
	CHECK(ask_of(ANALOG_SUPPLY + 4, 1, false) == 1, "the regulator was switched on");
	CHECK(boots == 0 && device.starts == 0, "the reset never let go, the bus never used");
	WAIT_FOR(logged("powered off"), 2000);
	powered_off("stopped after step 2");
}

static void
stops_after_step_4(void)
{
	phone("phone-gt9889", false);
	start("touchscreen=1 gt9889_steps=4");
	WAIT_FOR(logged("stopping here"), 5000);
	CHECK(logged("answers: its report block") && logged("gt9889: stopping here (gt9889_steps=4)") && !logged("step 5"), "said: %s", log_text);
	WAIT_FOR(logged("powered off"), 2000);
	CHECK(posts == 0 && clears == 0, "no report taken, nothing written");
	powered_off("stopped after step 4");
}

/* a chip that finds INT low at its reset's release has another address: nobody answers at 0x5d, and it is said so */
static void
nobody_answers(void)
{
	phone("phone-gt9889", false);
	silent = true;
	start("touchscreen=1");
	WAIT_FOR(logged("powered off"), 20000);
	CHECK(logged("gt9889: nobody answers at 0x5d: a chip that is not there, not powered or held in reset (or at the other address") &&
	    logged("touch screen unavailable"), "said: %s", log_text);
	powered_off("nobody answering");
}

/* a regulator RPMh does not answer for: said, powered off, the reset never let go */
static void
supply_refused(void)
{
	phone("phone-gt9889", false);
	refused_supply = ANALOG_SUPPLY;
	start("touchscreen=1");
	WAIT_FOR(logged("powered off"), 10000);
	CHECK(logged("gt9889: RPMh: no answer to the request") && logged("unavailable") && !logged("step 3"), "said: %s", log_text);
	CHECK(boots == 0 && device.starts == 0, "the reset not let go, the bus not used");
	CHECK(ask_of(ANALOG_SUPPLY + 4, 0, true) > ask_of(ANALOG_SUPPLY + 4, 1, true), "the vote withdrawn, the one that may have gone through too");
	CHECK(!pin_high(reset_pin), "in reset");
	pins_as_found("a supply refused", true);
}

/* one finger: down, moved, not said again where it stays, lifted -- each with the finger's number and its exact place */
static void
one_finger_down_moves_up(void)
{
	unsigned clears_before;

	ready();
	taken();
	CHECK(posts == 0 && clears == 0, "nobody touches: %u posts, %u clears", posts, clears);
	one_finger(4, 0, 0);
	taken();
	CHECK(posts == 1, "one post for a finger that landed: %u", posts);
	post_is(0, HAKIOS_TOUCH_DOWN, 4, 0, 0);
	CHECK(clears >= 1 && second_reads == 0, "the event byte written 0 after the report, no second read for one finger: %u, %u", clears, second_reads);
	clears_before = clears;
	taken();
	CHECK(posts == 1 && clears > clears_before, "a finger that stays where it is is not said again: %u posts", posts);
	one_finger(4, 1079, 2339);
	taken();
	CHECK(posts == 2, "one post for a move: %u", posts);
	post_is(1, HAKIOS_TOUCH_MOVE, 4, 65535, 65535);
	one_finger(4, 271, 1999);
	taken();
	post_is(2, HAKIOS_TOUCH_MOVE, 4, scaled(271, 1079), scaled(1999, 2339));
	chip_says(NULL, 0, false);               /* lifted: a touch event of no finger, said once */
	taken();
	CHECK(posts == 4, "one post for the lift: %u", posts);
	post_is(3, HAKIOS_TOUCH_UP, 4, scaled(271, 1079), scaled(1999, 2339));
	taken();
	CHECK(posts == 4, "and nothing more: %u posts", posts);
	CHECK(writes_elsewhere == 0 && device.driven_high == 0, "only the event byte written, the bus used as one: %u", writes_elsewhere);
	CHECK(seen.bad == 0 && seen.requests == 0, "%u reports not believed", seen.bad);
}

/* several fingers: the first twelve bytes, then the rest from the next register; their numbers are the chip's, not their slots' */
static void
several_fingers(void)
{
	const struct finger three[3] = { { 9, 1079, 2339 }, { 0, 540, 1170 }, { 3, 1, 2 } };
	const struct finger two[2] = { { 9, 1079, 2339 }, { 3, 1, 2 } };

	ready();
	chip_says(three, 3, true);
	taken();
	CHECK(posts == 3, "three fingers landed: %u posts", posts);
	post_is(0, HAKIOS_TOUCH_DOWN, 0, scaled(540, 1079), scaled(1170, 2339));       /* (by number, not by slot) */
	post_is(1, HAKIOS_TOUCH_DOWN, 3, scaled(1, 1079), scaled(2, 2339));
	post_is(2, HAKIOS_TOUCH_DOWN, 9, 65535, 65535);
	CHECK(second_reads >= 1, "the rest of the report read from the register after the first twelve bytes: %u", second_reads);
	chip_says(two, 2, true);
	taken();
	CHECK(posts == 4, "one lifted: %u posts", posts);
	post_is(3, HAKIOS_TOUCH_UP, 0, scaled(540, 1079), scaled(1170, 2339));
}

/* touch_flip=3 mirrors both ways */
static void
mirrored(void)
{
	phone("phone-gt9889", false);
	start("touchscreen=1 touch_flip=3");
	WAIT_FOR(logged("touch screen ready"), 10000);
	one_finger(1, 270, 1000);
	taken();
	post_is(0, HAKIOS_TOUCH_DOWN, 1, 65535 - scaled(270, 1079), 65535 - scaled(1000, 2339));
}

/* a report whose sum is wrong is not believed and nothing is posted; the next good one is */
static void
reports_not_believed(void)
{
	ready();
	one_finger(2, 10, 20);
	pthread_mutex_lock(&stage_lock);
	rep[11] ^= 0x5a;                         /* (the sum) */
	pthread_mutex_unlock(&stage_lock);
	taken();
	CHECK(posts == 0 && seen.bad >= 1, "nothing posted of a report that cannot be one: %u posts, %u bad", posts, seen.bad);
	CHECK(logged("gt9889: a report that cannot be one: 12 bytes, 80 01 02 0a 00 14"), "said: %s", log_text);
	one_finger(2, 10, 20);
	taken();
	post_is(0, HAKIOS_TOUCH_DOWN, 2, scaled(10, 1079), scaled(20, 2339));
}

/* the firmware asks for something (0x40): said once, the event byte written 0, nothing posted, nothing reset */
static void
firmware_request(void)
{
	ready();
	pthread_mutex_lock(&stage_lock);
	memset(rep, 0, sizeof(rep));
	rep[0] = 0x40;
	steady = false;
	pending = true;
	model.input[int_pin] = false;
	pthread_mutex_unlock(&stage_lock);
	WAIT_FOR(clears >= 1, 3000);
	taken();
	CHECK(clears >= 1 && posts == 0 && logged("gt9889: the firmware asks for something (event byte 0x40)"), "said: %s", log_text);
	CHECK(boots == 1 && !logged("looks in a row"), "nothing reset");
}

/* a chip that dozes while nobody touches it does not answer a look nobody asked for: not trouble, and it still works when touched */
static void
dozing_is_not_trouble(void)
{
	ready();
	dozing = true;
	usleep(400000);
	CHECK(boots == 1 && !logged("looks in a row") && !logged("a report not read"), "no trouble: %s", log_text);
	CHECK(seen.idle_misses >= 3, "the looks were made and went unanswered: %u", seen.idle_misses);
	one_finger(0, 100, 200);
	WAIT_FOR(posts >= 1, 3000);
	post_is(0, HAKIOS_TOUCH_DOWN, 0, scaled(100, 1079), scaled(200, 2339));
	dozing = false;
}

/* a finger whose chip falls silent is lifted after a quarter of a second: not sooner, not much later, once */
static void
silent_finger_lifted(void)
{
	uint64_t fell_silent, lifted_ms;
	int up = -1;

	phone("phone-gt9889", false);
	fake_delay_divisor = 1;
	start("touchscreen=1");
	WAIT_FOR(logged("touch screen ready"), 20000);
	CHECK(logged("touch screen ready"), "the chip brought up: %s", log_text);
	one_finger(6, 300, 400);
	taken();
	CHECK(posts == 1, "the finger landed: %u posts", posts);
	fell_silent = real_ns();
	silent = true;
	WAIT_FOR(posts >= 2, 3000);
	for (unsigned i = 1; i < posts && i < 512; i++) {
		up = posted[i].type == HAKIOS_TOUCH_UP && posted[i].finger == 6 ? (int)i : up;
	}
	CHECK(up == 1, "the finger lifted: %u posts: %s", posts, log_text);
	post_is(1, HAKIOS_TOUCH_UP, 6, scaled(300, 1079), scaled(400, 2339));
	lifted_ms = up >= 0 ? (posted[up].t - fell_silent) / 1000000 : 0;
	CHECK(lifted_ms >= 230 && lifted_ms <= 600, "lifted %u ms after the chip fell silent, not about 250", (unsigned)lifted_ms);
	CHECK(logged("with a finger down: lifted"), "said: %s", log_text);
	silent = false;
}

/* a chip that hangs with a finger down and that its reset brings back: reset, looked at again, read again */
static void
reset_brings_it_back(void)
{
	ready();
	one_finger(2, 10, 20);
	taken();
	hung = true;
	WAIT_FOR(boots >= 2 && logged("125 looks in a row"), 20000);
	CHECK(boots == 2, "the chip was reset: %u starts", boots);
	CHECK(wrong_address_boots == 0, "INT high at every release: %u", wrong_address_boots);
	CHECK(shortest_reset_ns >= 300000, "its reset held low %u us, not the 2 ms the driver means (0.4 ms here)",
	    (unsigned)(shortest_reset_ns / 1000));
	WAIT_FOR(posts >= 3, 5000);
	CHECK(early_starts == 0, "%u transfers begun before the chip had its time after the reset", early_starts);
	post_is(1, HAKIOS_TOUCH_UP, 2, scaled(10, 1079), scaled(20, 2339));
	post_is(2, HAKIOS_TOUCH_DOWN, 2, scaled(10, 1079), scaled(20, 2339));
	CHECK(!logged("stopped") && !logged("powered off"), "and goes on: %s", log_text);
}

/* a chip that hangs again and again: reset three times, and given up the fourth */
static void
four_troubles_end_it(void)
{
	ready();
	one_finger(2, 10, 20);
	hang_after = 3;
	hung = true;
	WAIT_FOR(logged("powered off"), 60000);
	CHECK(boots == 4, "reset three times: %u starts", boots);
	CHECK(logged("resets made for that before: 3 of 3") && logged("gt9889: stopped: the chip does not come back"), "said: %s", log_text);
	powered_off("troubles without end");
}

/* the run's end while polling: no more reads, nothing posted, the chip powered off and the pins as found */
static void
run_ends_while_polling(void)
{
	unsigned starts, lock_count;

	ready();
	one_finger(2, 10, 20);
	taken();
	CHECK(posts == 1, "the finger landed");
	lock_count = locks;
	quiesce();
	starts = device.starts;
	CHECK(logged("gt9889: at the run's end:") && logged("gt9889: powered off"), "said: %s", log_text);
	CHECK(locks == lock_count && locks_in_quiesce == 0, "RPMh asked without its lock at the run's end");
	CHECK(posts_in_quiesce == 0 && posts == 1, "nothing posted from the run's end: %u posts", posts);
	powered_off("the run's end");
	usleep(60000);
	CHECK(device.starts == starts && posts == 1, "and no transfer after it: %u more", device.starts - starts);
	quiesce();
	CHECK(asks == 4, "a second run's end finds nothing to do: %u asks", asks);
}

/* the run's end before the chip is ready: the bring-up stops where it is, and what it powered is powered off */
static void
run_ends_in_bring_up(void)
{
	phone("phone-gt9889", false);
	fake_delay_divisor = 1;
	start("touchscreen=1");
	WAIT_FOR(logged("step 2"), 5000);
	quiesce();
	CHECK(analog_on == false && !pin_high(reset_pin), "powered off");
	CHECK(posts == 0 && locks_in_quiesce == 0, "nothing posted, no lock taken");
	usleep(500000);
	CHECK(!logged("touch screen ready") && reports_read == 0, "and no report read after it: %s", log_text);
	pins_as_found("the run's end in bring-up", true);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "report_decode", report_decode },
		{ "off_by_default", off_by_default }, { "no_wiring", no_wiring },
		{ "look_only", look_only }, { "look_at_a_chip_in_reset", look_at_a_chip_in_reset },
		{ "brought_up_in_steps", brought_up_in_steps }, { "stops_after_step_1", stops_after_step_1 },
		{ "stops_after_step_2", stops_after_step_2 }, { "stops_after_step_4", stops_after_step_4 },
		{ "nobody_answers", nobody_answers }, { "supply_refused", supply_refused },
		{ "one_finger_down_moves_up", one_finger_down_moves_up }, { "several_fingers", several_fingers },
		{ "mirrored", mirrored }, { "reports_not_believed", reports_not_believed }, { "firmware_request", firmware_request },
		{ "dozing_is_not_trouble", dozing_is_not_trouble }, { "silent_finger_lifted", silent_finger_lifted },
		{ "reset_brings_it_back", reset_brings_it_back }, { "four_troubles_end_it", four_troubles_end_it },
		{ "run_ends_while_polling", run_ends_while_polling }, { "run_ends_in_bring_up", run_ends_in_bring_up },
	};

	scenario_thread = pthread_self();
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
			fprintf(stderr, "%-28s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
