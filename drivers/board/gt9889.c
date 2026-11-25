/*
 * A Goodix GT9889 touch screen on I2C (the "Normandy" part: Xiaomi's tree calls it GT9886 in its file names and
 * firmware, GT9889 in its device tree), as a source of touches for /dev/hakios-touch (sys/board/input.h). Where the
 * chip is, its address, its interrupt and reset, its supplies, how many fingers it reports and the largest
 * coordinates are the board's touch-i2c { } (board/board.h), on a board that lists "touch-gt9889". Written for the
 * Xiaomi Mi 9T (davinci, SM7150), whose node is goodix@5d under the serial engine at 0xa84000 (qupv3_se7_i2c).
 *
 * What the chip says and where this was taken from: board/gt9889_report.h (the vendor driver the davinci build has,
 * drivers/input/touchscreen/goodix_driver_gt9886/, from arch/arm64/configs/vendor/davinci.config:30). The power
 * and reset order is that driver's: goodix_ts_core.c:1016-1055 (goodix_ts_power_on: the analog supply's pin, the
 * regulator "vtouch" set to 3.0 V, 300 ms), :978-979 and :1160-1182 (reset low and INT driven high before power, INT an
 * input after), goodix_ts_i2c.c:1466-1483 (goodix_hw_reset: reset low 2 ms, high, 100 ms).
 *
 * NOT RUN ON A PHONE: run against a model of the chip only (workbench/touch_gt9889_test.c). Begin with touchscreen=-1.
 * Three things are NOT KNOWN to this file, because the vendor driver takes them from a firmware package
 * (goodix_gt9886_cfg_f10.bin) this tree does not have: the registers of the chip's identity, version and firmware
 * requests, and whether the chip reports without the configuration that Android sends it after a reset. So:
 *
 *   identity   not read: "the chip answers" is a read of its report block (0x4100), its event byte, and nothing more
 *   config     NOT SENT. A chip that reports nothing without it is what a phone test will show; say so then
 *   doze       the chip has an I2C low-power state the vendor brackets some accesses with (goodix_ts_i2c.c:522-587);
 *              reads of reports after its interrupt are not bracketed there, and are not here. A look that fails while
 *              no finger is down and the line is high is expected of a dozing chip: it is not counted as trouble
 *
 *   I2C        done by hand (board/i2c_gpio.h) on the board's two pins, taken for each transaction and given back as
 *              they were found. Registers are 16 bits, the high byte first. If the board names the serial engine the
 *              bus is (touch-i2c { serial-engine, clocks, vote-register, votes }) the engine does it instead
 *              (board/geni_i2c.h: its FIFO mode, 400 kHz, the vendor's rate), the pins left to its function: only when
 *              its firmware reads as I2C and its clock gives 19.2 MHz (said, and the pins by hand otherwise; no firmware
 *              is ever loaded into it). Which of the two it is, is said at step 4; the look-only level (below) reads the
 *              engine's state (protocol, clocks) and writes none of it
 *   interrupt  the chip's INT: low for a report (the vendor's trigger is falling-edge, DT goodix,irq-flags = 2, so the line
 *              may only pulse and a look at it every 8 ms may miss one): so the report block is also read every
 *              fourth look whatever the line says, and at every look while a finger is down
 *   power      up to two regulators asked of RPMh by name, and a pin that switches the analog supply, each only where
 *              the board names it. Davinci's "vtouch" is pm6150l_l6 (RPMh "ldoc6", 3.0 V as the vendor asks) behind
 *              the pin gpio5 ("goodix,avdd-gpio"); both are driven here, and if RPMh refuses the regulator the driver
 *              says so and leaves it to the state the firmware left
 *
 * Only the report block is read, and one byte of it written (the event byte, 0, which acknowledges a report as the
 * vendor does, goodix_ts_core.c:905). NO FIRMWARE IS UPLOADED and no configuration is sent: the driver expects a
 * controller that runs the firmware in its own flash.
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   touchscreen=1   the chip powered, reset and looked at, then its reports read and its touches posted
 *   touchscreen=-1  only looks: the pins as the firmware left them, and the report block read if the chip answers as it was
 *                   left (unpowered or held in reset it does not, which is said). Nothing powered, no pin configured
 *                   but the bus's two for the reads, given back as found; nothing written
 *   touch_flip=N    bit 0 mirrors the touches left to right, bit 1 top to bottom (as the other touch drivers)
 *
 * The bring-up is made in steps, each said in the log before it is made. Boot-arg gt9889_steps=N stops after the
 * first N, and what was powered for them is powered off again:
 *   1  the pins as they were found (reads of the pin controller only)
 *   2  the chip held in reset, INT driven high (the address select) and its supplies switched on (RPMh, the pin)
 *   3  the reset let go, INT let go after it, and the chip waited for
 *   4  its report block read (the first transfers on the bus)
 *   5  its reports read, from here on
 * At touchscreen=-1 steps 1 and 4 are made, and 4 with nothing switched on for it.
 *
 * Interrupt: INT falls for each report (the vendor reads it only then: goodix,irq-flags = 2) and this kernel cannot take
 * the interrupt, so the pin controller is asked to latch the falling edge (tlmm.h) and the latch is polled every POLL_MS;
 * it is never routed to the interrupt controller. After an edge the report is read at once, the event byte written 0 and
 * the latch cleared. Without one the chip is not read but once a second (LATCHLESS_TICKS; a failed read then is nothing),
 * and where the latch could not be armed the line's level and the old idle looks are what is used. The pin's interrupt
 * registers are given back as found. gt9889_doze=1 brackets the event byte's write with the vendor's doze handshake.
 *
 * After each reset (and the chip's answer): the watchdog's tick (0xAA into 0x30F3, again every 5 s with a look at 0x3103; a
 * failed check is said, nothing more) and, if the board's touch-i2c names a `config' file of fw/ in the small image, the
 * vendor's configuration package for the chip's sensor id, whole-checked first (gt9889_config.h) and sent by the vendor's
 * command sequence inside its doze handshake. The chip scans nothing without it (found on the phone). A missing or
 * invalid file is said and the chip left as it is.
 *
 * What a report costs: the bus is driven by hand at 100 kHz at most with a core spun for it (i2c_gpio.h). The 12
 * bytes of a look are some 130 bits, about 1.4 ms; idle the chip is looked at every fourth tick (31 times a second:
 * 4 % of a core), with a finger down 125 times a second (a sixth of a core and more with more fingers). A serial
 * engine as I2C (above) costs a hundredth of that, where the board has one and it is I2C.
 *
 * Fingers that fall silent: a finger is lifted once the chip has said nothing believable for a quarter of a second
 * (LIFT_US). After 125 looks in a row that were to find a report and did not the chip is reset and looked at again,
 * at most three times (forgotten after a minute without trouble); the fourth time the polling ends, and the chip is
 * powered off.
 *
 * When bring-up fails, when the polling gives up, when gt9889_steps stops it, and at the run's end (and on a panic's
 * way out, hakios_gt9889_quiesce()) the chip is powered off as far as this driver powered it: held in reset, the
 * votes for its analog supply and then its I/O supply withdrawn, the analog supply's pin and the interrupt's as they
 * were found. The bus's pins are as they were found after every transaction; a transaction the run's end cut off is
 * ended there, and a data line the chip then still holds is clocked free. The fingers still down are lifted when the
 * thread itself stops; at the run's end nothing is posted.
 *
 * Timings: the vendor's (the power-on wait of 300 ms, reset low 2 ms, 100 ms after the reset), the chip then asked
 * every 100 ms for a second more (NOT SURE it is up in 100 ms: a first look that is not answered is no failure).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/board/input.h>
#include <sys/systm.h>

#include "kernel.h"
#include "board.h"
#include "log.h"
#include "gt9889_report.h"
#include "gt9889_config.h"

static vm_offset_t tlmm, rsc, cmd_db;

/*
 * The thread and the run's end (hakios_gt9889_quiesce()): the thread holds `busy' around everything it does with the
 * chip and begins nothing once `leaving' is set; `on_bus' is set from just before it takes the bus's pins until it has
 * given them back, so that the run's end knows a transaction was cut off in its middle.
 */
static bool leaving, busy, on_bus;
static bool started;                    /* a thread was started: there may be something to put away */

/* every line to the loader's log as well (and to the console when that may be written) */
HAKIOS_DEFINE_SAY(!__atomic_load_n(&leaving, __ATOMIC_SEQ_CST) && ml_get_interrupts_enabled())

#include "mmio.h"
#include "time.h"

#define wait_ms(ms)             hakios_wait_ms((ms), __atomic_load_n(&leaving, __ATOMIC_SEQ_CST))   /* (spun on the way out) */

/* On the way out every wait spins: interrupts are off and the other cores stopped (as in board/fts.c). */
static void __attribute__((unused))    /* (not where a test stands in for board/rpmh.h) */
gt9889_wait_us(uint32_t us)
{
	if (__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
		spin_us(us);
	} else {
		sleep_us(us);
	}
}

#define POLL_MS                 8       /* the interrupt line is looked at 125 times a second */
#define LATCHLESS_TICKS         125     /* with the latch armed and no edge: the chip is asked once a second */
#define WRITE_MAX               254     /* the vendor's transfers: 256 bytes with the register's number */
#define IDLE_LOOKS              4       /* ... and the chip asked on every fourth look whatever the line says (this file's head: interrupt) */
#define LIFT_US                 250000  /* nothing believable for so long: the fingers are lifted */
#define TROUBLE_LOOKS           125     /* so many looks in a row without a report: the chip is reset */
#define TROUBLE_RESETS          3       /* ... so many times, and the next time the polling ends */
#define TROUBLE_FORGOTTEN_US    (60 * 1000000ull)
#define POWER_ON_MS             300     /* after the supplies: the vendor's goodix,power-on-delay-us of 300000 */
#define RESET_LOW_MS            2       /* goodix_hw_reset */
#define RESET_WAIT_MS           100
#define INT_RELEASE_MS          5       /* INT held high through the reset's release (the address select), then let go */
#define IDENTITY_TRIES          10
#define IDENTITY_GAP_MS         100
#define IO_MILLIVOLTS           1800    /* the I/O supply: the bus's level on these phones */

/* ---- pins (TLMM) ------------------------------------------------------------------------------------- */

#define TLMM_PHYS               (hakios_board()->pins.tlmm)     /* the SoC file's pins { } */
#define HAKIOS_TLMM             tlmm
#include "tlmm.h"                    /* a pin's two registers, and the four things done with them */

/* which pins, and where on the bus: the board's touch-i2c { } */
#define WIRED                   (&hakios_board()->touch_i2c)
#define PIN_SDA                 ((unsigned)WIRED->sda)
#define PIN_SCL                 ((unsigned)WIRED->scl)
#define PIN_INT                 ((unsigned)WIRED->interrupt)    /* low = the chip has a report */
#define PIN_RESET               ((unsigned)WIRED->reset)        /* low = in reset */
#define PIN_AVDD                ((unsigned)WIRED->analog_enable)        /* high = analog supply on (if the board has one) */
#define HAS_AVDD                (WIRED->analog_enable >= 0)
#define ADDRESS                 (WIRED->address)
#define HAS_ENGINE              (WIRED->serial_engine != 0)     /* the board names a serial engine as the bus */

/* a pin as it is, in words (as board/imu.c says its pins) */
static void
show_pin(unsigned pin, const char *what)
{
	uint32_t cfg = rd(GPIO_CFG(pin)), io = rd(GPIO_IO(pin));

	say("gt9889: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pin, what, (cfg >> 2) & 15, cfg & 3,
	    (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
}

/* step 1, at a look too: reads of the pin controller only */
static void
show_pins(void)
{
	say("gt9889: step 1: the pins as they were found\n");
	show_pin(PIN_SDA, "I2C data");
	show_pin(PIN_SCL, "I2C clock");
	show_pin(PIN_INT, "interrupt");
	show_pin(PIN_RESET, "reset");
	if (HAS_AVDD) {
		show_pin(PIN_AVDD, "analog supply's switch");
	}
}

/* ---- the supplies (RPMh) ---------------------------------------------------------------------------------- */

#define CMD_DB_PHYS             (hakios_board()->rpmh.cmd_db)           /* the SoC file's rpmh { } */
#define CMD_DB_SIZE             (hakios_board()->rpmh.cmd_db_size)
#define RSC_PHYS                (hakios_board()->rpmh.rsc)      /* the application processor's "DRV 2" */
#define RSC_SIZE                0x10000UL
#define sleep_us gt9889_wait_us
#include "rpmh.h"                    /* a slot set's registers, and one request through it */
#undef sleep_us

#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

#include "cmd_db.h"

/* The RPMh address of a regulator by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;

	if (db == NULL || rsc == 0) {
		say("gt9889: RPMh or its command database is not mapped\n");
		return 0;
	}
	if (!hakios_cmd_db_valid(db)) {
		say("gt9889: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1], db->magic[2],
		    db->magic[3]);
		return 0;
	}
	return hakios_cmd_db_address(db, CMD_DB_SIZE, name);
}

/* (without the lock on the way out: the other cores are stopped, one of them perhaps holding it -- as board/fts.c) */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	bool locking = !__atomic_load_n(&leaving, __ATOMIC_SEQ_CST);
	const char *problem;

	if (locking) {
		hakios_rpmh_lock();
	}
	problem = hakios_rpmh_send_rsc(rsc, hakios_board()->rpmh.tcs_offset, address, data, count);
	if (locking) {
		hakios_rpmh_unlock();
	}
	if (problem != NULL) {
		say("gt9889: RPMh: %s\n", problem);
	}
	return problem == NULL;
}

/* each supply's RPMh address once it has been asked for, whether the request went through or not; 0: not asked */
static uint32_t io_supply_at, analog_supply_at;

static bool
supply_on(const char *name, uint32_t millivolts, uint32_t *asked)
{
	uint32_t base = rpmh_address(name);
	uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
	uint32_t data[3] = { millivolts, 1, VRM_MODE_LDO_HPM };

	if (base == 0 || (base >> 16 & 0xf) != 4) {
		say("gt9889: no RPMh regulator %s (address 0x%x)\n", name, base);
		return false;
	}
	say("gt9889: regulator %s at 0x%x: %u mV\n", name, base, millivolts);
	*asked = base;
	return rpmh_write(address, data, 3);
}

/* this driver's vote for a supply withdrawn, if it voted (only the enable: another may want the regulator on) */
static void
supply_off(const char *name, uint32_t *asked)
{
	uint32_t address = *asked + VRM_ENABLE, off = 0;

	if (*asked == 0) {
		return;
	}
	*asked = 0;
	say("gt9889: regulator %s: vote withdrawn\n", name);
	(void)rpmh_write(&address, &off, 1);
}

/* ---- I2C by hand (as in board/imu.c) ------------------------------------------------------------------- */

#include "i2c_gpio.h"                /* the two lines, a bit, a byte, the bus taken and given back */

static struct i2c_gpio i2c_bus;          /* (its two pins: the board's, at the start) */
static const char not_at_rest[] = "the I2C bus is not at rest", not_acknowledged[] = "nobody acknowledged";
static const char engine_failed[] = "the serial engine reports an error";
static const char *failed = not_acknowledged;   /* why the last transfer did not go through: one of the two */
static unsigned refusals_said;

/*
 * The bus's two pins as the run's first transaction found them, which is what every transaction and the run's end give
 * back. Not what each take finds: a take after one that was cut off half way would find them as plain GPIOs.
 */
static uint32_t bus_found_cfg[2], bus_found_io[2];
static bool bus_found;

static void
bus_record(void)
{
	if (__atomic_load_n(&bus_found, __ATOMIC_SEQ_CST)) {
		return;
	}
	bus_found_cfg[0] = rd(GPIO_CFG(i2c_bus.sda));
	bus_found_cfg[1] = rd(GPIO_CFG(i2c_bus.scl));
	bus_found_io[0] = rd(GPIO_IO(i2c_bus.sda));
	bus_found_io[1] = rd(GPIO_IO(i2c_bus.scl));
	__atomic_store_n(&bus_found, true, __ATOMIC_SEQ_CST);
}

static void
bus_give_back(void)
{
	for (unsigned i = 0; i < 2; i++) {
		i2c_bus.found_cfg[i] = bus_found_cfg[i];
		i2c_bus.found_io[i] = bus_found_io[i];
	}
	i2c_gpio_give_back(&i2c_bus);
	__atomic_store_n(&on_bus, false, __ATOMIC_SEQ_CST);
}

static bool
bus_take(void)
{
	bus_record();
	/* (on the bus before the pins are touched: a thread stopped inside the take leaves them to the run's end to give back) */
	__atomic_store_n(&on_bus, true, __ATOMIC_SEQ_CST);
	if (i2c_gpio_take(&i2c_bus)) {
		return true;
	}
	bus_give_back();
	if (refusals_said++ < 4) {
		say("gt9889: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", PIN_SDA,
		    i2c_gpio_level(PIN_SDA), PIN_SCL, i2c_gpio_level(PIN_SCL));
	}
	failed = not_at_rest;
	return false;
}

/* ---- the same bus through a serial engine (board/geni_i2c.h), where the board names one ----------------------------------- */

#define GENI_I2C_SAY            say
#define GENI_I2C_WAIT_US        gt9889_wait_us
#include "geni_i2c.h"

#define GCC_PHYS                (hakios_board()->clocks.gcc)    /* the SoC file's clocks { } */
#define GCC_SIZE                0x100000UL      /* (as far as tools/boards.py lets touch-i2c { } reach) */
#define SE_SIZE                 0x4000UL
#define ENGINE_HZ               400000          /* the vendor's rate for the bus (i2c-qcom-geni.c's default) */

static struct geni_i2c geni;
static bool geni_mapped;
static bool geni_pins;                  /* the bus's two pins given to the engine (their function), to be given back */

/*
 * The engine reaches the bus only through the pins' QUP function: the firmware may leave them as GPIOs (davinci's
 * gpio6/7 were function 0 at entry, and the engine's first transfer failed). Android's active state for the bus
 * (qupv3_se7_i2c_active: function qup11, 2 mA, bias-disable; the board pulls the lines up) is function 1 of those
 * pins in the vendor's pinctrl-sdmmagpie.c; the pins as found are given back when the engine is put back.
 */
#define ENGINE_PIN_CFG          (1u << 2)       /* [5:2] function 1, [1:0] no pull, [8:6] 2 mA */

static void
engine_pins_give(void)
{
	bus_record();
	wr(GPIO_CFG(i2c_bus.sda), ENGINE_PIN_CFG);
	wr(GPIO_CFG(i2c_bus.scl), ENGINE_PIN_CFG);
	__atomic_store_n(&geni_pins, true, __ATOMIC_SEQ_CST);
}

static void
engine_pins_back(void)
{
	if (__atomic_load_n(&geni_pins, __ATOMIC_SEQ_CST)) {
		__atomic_store_n(&geni_pins, false, __ATOMIC_SEQ_CST);
		wr(GPIO_CFG(i2c_bus.sda), bus_found_cfg[0]);
		wr(GPIO_CFG(i2c_bus.scl), bus_found_cfg[1]);
		wr(GPIO_IO(i2c_bus.sda), bus_found_io[0]);
		wr(GPIO_IO(i2c_bus.scl), bus_found_io[1]);
	}
}
static bool geni_on;                    /* the engine is the bus: set up for it, its votes taken */

static bool
engine_map(void)
{
	if (!geni_mapped) {
		if (GCC_PHYS == 0) {
			say("gt9889: the board names a serial engine but the SoC file has no clock controller (clocks { gcc }): by hand\n");
			return false;
		}
		geni.se = ml_io_map(WIRED->serial_engine, SE_SIZE);
		geni.gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
		geni.m_ahb = WIRED->m_ahb_clock;
		geni.s_ahb = WIRED->s_ahb_clock;
		geni.se_clock = WIRED->se_clock;
		geni.vote_register = WIRED->vote_register;
		for (unsigned i = 0; i < 5; i++) {
			geni.vote[i] = WIRED->vote[i];
		}
		geni_mapped = true;
	}
	return true;
}

/* touchscreen=-1: what the engine is, said; nothing written, nothing voted */
static void
engine_look(void)
{
	if (HAS_ENGINE && engine_map()) {
		say("gt9889: the board names the serial engine at %#x as the bus (looked at only; the pins are read by hand below)\n",
		    WIRED->serial_engine);
		if (geni_i2c_look(&geni)) {
			say("gt9889: the engine's firmware is I2C: touchscreen=1 would use it\n");
		} else {
			say("gt9889: the engine is not known to be I2C: touchscreen=1 would drive the pins by hand\n");
		}
	}
}

/* Step 4's bus: the engine if the board names one and it can be used, else the pins by hand. */
static void
engine_take(void)
{
	const char *why;

	uint32_t use = 1;

	if (!HAS_ENGINE || !engine_map()) {
		return;
	}
	/* boot-arg gt9889_geni=0: the pins by hand although the board names an engine (to tell the bus from the chip) */
	if (PE_parse_boot_argn("gt9889_geni", &use, sizeof(use)) && use == 0) {
		say("gt9889: gt9889_geni=0: the serial engine at %#x is not used: the pins by hand\n", WIRED->serial_engine);
		return;
	}
	engine_pins_give();
	why = geni_i2c_open(&geni, ENGINE_HZ);
	if (why != NULL) {
		engine_pins_back();
		say("gt9889: the serial engine at %#x is not used: %s: the pins by hand\n", WIRED->serial_engine, why);
		return;
	}
	__atomic_store_n(&geni_on, true, __ATOMIC_SEQ_CST);
	say("gt9889: the bus is the serial engine at %#x: I2C at %u kHz, FIFO of %u words\n", WIRED->serial_engine, ENGINE_HZ / 1000,
	    geni.fifo_depth);
}

/* The engine's votes withdrawn and its registers as found (power_off()). */
static void
engine_put_back(void)
{
	if (__atomic_load_n(&geni_on, __ATOMIC_SEQ_CST)) {
		__atomic_store_n(&geni_on, false, __ATOMIC_SEQ_CST);
		geni_i2c_close(&geni);
	}
	engine_pins_back();
}

/* one transaction through the engine, its end noted for the run's end */
static bool
engine_transfer(const uint8_t *out, unsigned out_length, uint8_t *in, unsigned in_length)
{
	bool done;

	__atomic_store_n(&on_bus, true, __ATOMIC_SEQ_CST);
	done = geni_i2c_transfer(&geni, ADDRESS, out, out_length, in, in_length);
	__atomic_store_n(&on_bus, false, __ATOMIC_SEQ_CST);
	if (!done) {
		failed = geni_i2c_nacked(&geni) ? not_acknowledged : engine_failed;
	}
	return done;
}

/* a register's 16-bit number, the high byte first, sent to the chip: the start of a read or of a write */
static bool
address_register(uint16_t reg)
{
	i2c_gpio_start(&i2c_bus);
	return i2c_gpio_write(&i2c_bus, (uint8_t)(ADDRESS << 1)) && i2c_gpio_write(&i2c_bus, (uint8_t)(reg >> 8)) &&
	    i2c_gpio_write(&i2c_bus, (uint8_t)reg);
}

/* the chip addressed for reading from `reg' on (it counts the register's number up across a read) */
static bool
read_from(uint16_t reg)
{
	if (!address_register(reg)) {
		return false;
	}
	i2c_gpio_start(&i2c_bus);
	return i2c_gpio_write(&i2c_bus, (uint8_t)(ADDRESS << 1 | 1));
}

/* `count' registers from `reg' on */
static bool
regs_read(uint16_t reg, uint8_t *values, unsigned count)
{
	bool answered;

	if (geni_on) {
		const uint8_t at[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };

		return engine_transfer(at, sizeof(at), values, count);
	}
	if (!bus_take()) {
		return false;
	}
	answered = read_from(reg);
	for (unsigned i = 0; answered && i < count; i++) {
		values[i] = i2c_gpio_read(&i2c_bus, i + 1 < count);
	}
	i2c_gpio_stop(&i2c_bus);
	bus_give_back();
	if (!answered) {
		failed = not_acknowledged;
	}
	return answered;
}

/* `count' bytes into the registers from `reg' on (one transaction) */
static bool
regs_write(uint16_t reg, const uint8_t *values, unsigned count)
{
	bool done;

	if (geni_on) {
		uint8_t out[2 + WRITE_MAX];

		out[0] = (uint8_t)(reg >> 8);
		out[1] = (uint8_t)reg;
		memcpy(&out[2], values, count);
		return engine_transfer(out, 2 + count, NULL, 0);
	}
	if (!bus_take()) {
		return false;
	}
	done = address_register(reg);
	for (unsigned i = 0; done && i < count; i++) {
		done = i2c_gpio_write(&i2c_bus, values[i]);
	}
	i2c_gpio_stop(&i2c_bus);
	bus_give_back();
	if (!done) {
		failed = not_acknowledged;
	}
	return done;
}

/* one byte into a register: the event byte's 0, the doze handshake's and the commands' bytes */
static bool
reg_write(uint16_t reg, uint8_t value)
{
	return regs_write(reg, &value, 1);
}

static struct { bool down; uint32_t x, y; } contact[GT9889_CONTACTS];
volatile uint64_t hakios_touch_down_us;         /* when the last finger came down, now_us() */
static unsigned contacts = GT9889_CONTACTS;     /* how many fingers the board's chip reports */

static unsigned
fingers_down(void)
{
	unsigned down = 0;

	for (unsigned id = 0; id < GT9889_CONTACTS; id++) {
		down += contact[id].down;
	}
	return down;
}

/*
 * The vendor's doze handshake (goodix_ts_i2c.c goodix_set_i2c_doze_mode(), 522-587): 0xAA into 0x30F0, the chip says 0xBB
 * in 0x3100 (asked again every 10 ms, 0xAA again if it says neither), and later 0xCC into 0x30F0. The vendor's reads and
 * writes outside its interrupt handler are each inside one; the report's own read is not.
 */
#define GT9889_REG_DOZE         0x30f0
#define GT9889_REG_DOZE_STATE   0x3100
#define DOZE_TRIES              10
static uint32_t doze;                   /* gt9889_doze=1: the event byte's write inside it as well */

static bool
doze_hold(void)
{
	uint8_t state = 0;

	(void)reg_write(GT9889_REG_DOZE, 0xaa);
	gt9889_wait_us(1000);
	for (unsigned i = 0; i < DOZE_TRIES; i++) {
		state = 0;
		if (regs_read(GT9889_REG_DOZE_STATE, &state, 1) && state == 0xbb) {
			return true;
		}
		if (state != 0xaa) {
			(void)reg_write(GT9889_REG_DOZE, 0xaa);
		}
		wait_ms(10);
	}
	return false;
}

static void
doze_release(void)
{
	for (unsigned i = 0; i < DOZE_TRIES; i++) {
		if (reg_write(GT9889_REG_DOZE, 0xcc)) {
			return;
		}
		gt9889_wait_us(1000);
	}
}

/* the event byte written 0 (the chip waits for that before the next report), inside the handshake if asked for */
static void
doze_ack(void)
{
	bool held = (doze & 1) && doze_hold();

	(void)reg_write(GT9889_REG_REPORT, 0);
	if (held) {
		doze_release();
	}
}

/*
 * A report: its first bytes, and, if they say more than one finger, the rest from the next register on (as the vendor
 * reads it); `bytes' is how many came. A failed read is false. The event byte is written 0 afterwards if the report
 * had one: the chip is waiting for that before it says the next.
 */
static bool
report_read(uint8_t *report, unsigned *bytes)
{
	unsigned more;

	*bytes = 0;
	if (!regs_read(GT9889_REG_REPORT, report, GT9889_REPORT_HEAD)) {
		return false;
	}
	*bytes = GT9889_REPORT_HEAD;
	more = gt9889_more(report, contacts);
	if (more != 0) {
		if (!regs_read((uint16_t)(GT9889_REG_REPORT + GT9889_REPORT_HEAD), &report[GT9889_REPORT_HEAD], more)) {
			return false;
		}
		*bytes += more;
	}
	if (report[0] != 0) {
		doze_ack();
	}
	return true;
}

/* ---- touches ------------------------------------------------------------------------------------------- */

static uint32_t flip, trace;
static uint32_t panel_x, panel_y;       /* the largest coordinates: the board's */
static struct { uint32_t looks, reports, bad, reads_failed, positions, idle_misses, requests, raw_dumped, esd_ticks, esd_failed; } seen;

static void
report(const struct gt9889_frame *frame)
{
	for (uint32_t id = 0; id < GT9889_CONTACTS; id++) {
		uint32_t x, y;

		if (!(frame->active & (1u << id))) {
			if (contact[id].down) {
				hakios_touch_post(HAKIOS_TOUCH_UP, id, contact[id].x, contact[id].y);
				contact[id].down = false;
			}
			continue;
		}
		x = (uint32_t)frame->x[id] * 65535 / panel_x;
		y = (uint32_t)frame->y[id] * 65535 / panel_y;
		if (flip & 1) {
			x = 65535 - x;
		}
		if (flip & 2) {
			y = 65535 - y;
		}
		if (!contact[id].down) {
			if (trace && seen.positions < 40) {
				say("gt9889: finger %u down at %u, %u (of %u x %u)\n", id, frame->x[id], frame->y[id], panel_x, panel_y);
			}
			hakios_touch_down_us = now_us();        /* (haptic_trace=1: board/haptic_ldo.c reports how long after it the motor came) */
			hakios_touch_post(HAKIOS_TOUCH_DOWN, id, x, y);
		} else if (contact[id].x != x || contact[id].y != y) {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, id, x, y);
		}
		contact[id].down = true;
		contact[id].x = x;
		contact[id].y = y;
		seen.positions++;
	}
}

/* every finger that is down, lifted */
static void
release(void)
{
	const struct gt9889_frame nobody = { 0 };

	report(&nobody);
}

/* ---- the chip: its power, its reset, its identity ------------------------------------------------------- */

/* The pins bring-up configures, as it found them: what powering off configures them back to. */
#define KEPT_PINS               3
static unsigned pin_number[KEPT_PINS];
static uint32_t pin_found_cfg[KEPT_PINS], pin_found_io[KEPT_PINS];
static unsigned pins_kept;
static uint32_t int_found_intr_cfg;     /* INT's interrupt detection as found, and whether it was changed */
static bool latch_armed;
static bool powered;                    /* bring-up has begun to change things: there is something to put back */

static void
pins_record(void)
{
	pins_kept = 0;
	pin_number[pins_kept++] = PIN_RESET;
	pin_number[pins_kept++] = PIN_INT;
	if (HAS_AVDD) {
		pin_number[pins_kept++] = PIN_AVDD;
	}
	for (unsigned i = 0; i < pins_kept; i++) {
		pin_found_cfg[i] = rd(GPIO_CFG(pin_number[i]));
		pin_found_io[i] = rd(GPIO_IO(pin_number[i]));
	}
	int_found_intr_cfg = rd(GPIO_INTR_CFG(PIN_INT));
	__atomic_store_n(&powered, true, __ATOMIC_SEQ_CST);
}

/*
 * The chip powered off, as far as this driver powered it (this file's head): bounded, and without sleeping on the way
 * out. The reset pin keeps its level low under the configuration it is given back.
 */
static void
power_off(void)
{
	if (!__atomic_load_n(&powered, __ATOMIC_SEQ_CST)) {
		return;                         /* (nothing was changed, or it has been put back already) */
	}
	engine_put_back();
	gpio_output(PIN_RESET, false, PULL_NONE, 2);
	if (WIRED->analog_supply != NULL) {
		supply_off(WIRED->analog_supply, &analog_supply_at);
	}
	if (WIRED->io_supply != NULL) {
		supply_off(WIRED->io_supply, &io_supply_at);
	}
	if (latch_armed) {
		wr(GPIO_INTR_CFG(PIN_INT), int_found_intr_cfg);         /* (then the latch, which putting the detection back may set) */
		gpio_edge_clear(PIN_INT);
		latch_armed = false;
	}
	for (unsigned i = 0; i < pins_kept; i++) {
		if (pin_number[i] != PIN_RESET) {
			wr(GPIO_IO(pin_number[i]), pin_found_io[i] & 2);        /* (the level first, as gpio_output()) */
		}
		wr(GPIO_CFG(pin_number[i]), pin_found_cfg[i]);
	}
	__atomic_store_n(&powered, false, __ATOMIC_SEQ_CST);
	say("gt9889: powered off: in reset, what was switched on for it off again, the pins as found\n");
}

/*
 * The reset held low and let go, and the chip given its time (this file's head: Timings). INT is driven high from
 * before the reset to a moment after its release: the level the chip finds there picks its address, and the vendor's
 * (0x5d) is the high one (goodix_ts_core.c:979); then INT is an input pulled up, as the vendor's pinctrl has it.
 */
static void
reset_pulse(void)
{
	gpio_output(PIN_INT, true, PULL_NONE, 2);
	gpio_set(PIN_RESET, false);
	wait_ms(RESET_LOW_MS);
	if (__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
		return;                         /* (the run's end has the pins: the chip stays in reset) */
	}
	gpio_set(PIN_RESET, true);
	wait_ms(INT_RELEASE_MS);
	if (__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
		return;
	}
	gpio_configure(PIN_INT, 0, PULL_UP, 2, false);
	wait_ms(RESET_WAIT_MS);
	if (!__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
		gpio_edge_latch_arm(PIN_INT);   /* (after the pin is an input: the reset's own edges are not reports) */
		latch_armed = true;
	}
}

/*
 * The chip asked for its report block, `tries' times a tenth of a second apart: that is all it is asked (this file's
 * head: identity). True if it answered with bytes that are a report or no report; said either way. Nothing is written.
 */
static bool
identify(unsigned tries)
{
	uint8_t report[GT9889_REPORT_HEAD] = { 0 };
	bool answered = false;

	for (unsigned attempt = 0; attempt < tries && !answered && !__atomic_load_n(&leaving, __ATOMIC_SEQ_CST); attempt++) {
		if (attempt != 0) {
			wait_ms(IDENTITY_GAP_MS);
		}
		answered = regs_read(GT9889_REG_REPORT, report, sizeof(report));
	}
	if (!answered) {
		if (failed == not_at_rest) {
			say("gt9889: nothing read at 0x%02x: %s\n", ADDRESS, failed);
		} else if (failed == engine_failed) {
			say("gt9889: nothing read at 0x%02x: %s (status %#x)\n", ADDRESS, failed, geni.status);
		} else {
			say("gt9889: nobody answers at 0x%02x: a chip that is not there, not powered or held in reset (or at the other address "
			    "its INT level at reset selects)\n", ADDRESS);
		}
		return false;
	}
	say("gt9889: the chip at 0x%02x answers: its report block (0x%04x) reads %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
	    "%02x\n", ADDRESS, GT9889_REG_REPORT, report[0], report[1], report[2], report[3], report[4], report[5], report[6], report[7],
	    report[8], report[9], report[10], report[11]);
	if (report[0] == 0xff && report[1] == 0xff && report[11] == 0xff) {
		say("gt9889: ... all ones: a bus with nobody driving it, or a chip not yet up: its reports are not read\n");
		return false;
	}
	return true;
}

/* ---- the chip's configuration and its watchdog ------------------------------------------------------------------ */

/*
 * What the vendor's driver does after a reset and the chip does not do without: the watchdog fed (goodix_hw_reset()'s
 * tail, goodix_ts_i2c.c:1495-1520: 0xAA into the esd register, then every GOODIX_ESD_CHECK_INTERVAL = 5 s again, the
 * check reading 0x3103: 0xAA still there, or no answer, is the firmware not running: goodix_ts_core.c:1379-1420), and the
 * configuration package for this chip's sensor sent (goodix_send_config(), goodix_ts_i2c.c:894-1150). Without the
 * package the firmware has no panel configuration and scans nothing (found on the phone: no interrupt edge but the boot
 * report's). All inside the doze handshake, as the vendor's generic reads and writes are.
 */
#define GT9889_REG_ESD          0x30f3
#define GT9889_REG_ESD_CHECK    0x3103
#define GT9889_REG_COMMAND      0x6f68
#define GT9889_REG_CONFIG       0x6f78
#define ESD_TICK                0xaa
#define ESD_TICKS               (5000 / POLL_MS)
#define CMD_READY               0xff
#define CMD_START_SEND          0x80
#define CMD_END_SEND            0x83
#define CMD_PREPARE_OK          0x82
#define CMD_FREE_TRIES          10
#define CMD_READY_TRIES         30

extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);   /* board/image.c */

static struct gt9889_config package_file;
static bool package_tried;
static bool esd_on;
static uint16_t esd_reg = GT9889_REG_ESD;

#define going()                 __atomic_load_n(&leaving, __ATOMIC_SEQ_CST)

/* the board's file of fw/ in the small image, parsed and checked, once */
static void
package_load(void)
{
	const uint8_t *file;
	uint64_t size = 0;
	const char *wrong;

	if (package_tried || WIRED->config == NULL) {
		return;
	}
	package_tried = true;
	file = hakios_image_file("fw", WIRED->config, &size, NULL);
	if (file == NULL) {
		say("gt9889: no fw/%s in the small image: the chip keeps the configuration it has, and may not scan\n", WIRED->config);
		return;
	}
	wrong = gt9889_config_parse(file, size, &package_file);
	if (wrong != NULL) {
		say("gt9889: fw/%s (%u bytes) is not a configuration package: %s; not used\n", WIRED->config, (unsigned)size, wrong);
		package_file.count = 0;
		return;
	}
	say("gt9889: fw/%s: %u packages, each checked\n", WIRED->config, package_file.count);
}

static bool
command_send(uint16_t reg, uint8_t command)
{
	const uint8_t bytes[3] = { command, 0, (uint8_t)(0 - command) };

	return regs_write(reg, bytes, sizeof(bytes));
}

/* until the command register says it is free (0xff) */
static bool
command_free(uint16_t reg)
{
	uint8_t value = 0;

	for (unsigned i = 0; i < CMD_FREE_TRIES && !going(); i++) {
		if (regs_read(reg, &value, 1) && value == CMD_READY) {
			return true;
		}
		wait_ms(10);
	}
	return false;
}

/* until the chip answers a command with `right' (goodix_wait_cfg_cmd_ready()): the command sent again if it says something else */
static bool
command_answered(uint16_t reg, uint8_t right, uint8_t sent)
{
	uint8_t value[3];

	for (unsigned i = 0; i < CMD_READY_TRIES && !going(); i++) {
		if (!regs_read(reg, value, sizeof(value))) {
			return false;
		}
		if (value[0] == right) {
			return true;
		}
		if (value[0] != sent && !command_send(reg, sent)) {
			return false;
		}
		wait_ms(10);
	}
	return false;
}

/* the package the chip's sensor id, hardware id (and firmware mask) say (goodix_get_reg_and_cfg()); NULL if none */
static const struct gt9889_package *
package_select(void)
{
	for (unsigned i = 0; i < package_file.count && !going(); i++) {
		const struct gt9889_package *k = &package_file.package[i];
		uint8_t sensor = 0, pid[8] = { 0 }, mask[9] = { 0 };

		if (!regs_read(k->sensor_reg, &sensor, 1)) {
			say("gt9889: package %u: the sensor id (0x%04x) not read: %s\n", i, k->sensor_reg, failed);
			return NULL;
		}
		if (k->sensor_mask != 0) {
			sensor &= k->sensor_mask;
		}
		if (sensor != k->sensor_id) {
			say("gt9889: package %u is for sensor id %u, the chip says %u\n", i, k->sensor_id, sensor);
			continue;
		}
		if (k->fw_mask_reg != 0 && k->fw_mask[0] != 0) {
			if (!regs_read(k->fw_mask_reg, mask, sizeof(mask))) {
				return NULL;
			}
			if (memcmp(mask, k->fw_mask, sizeof(mask)) != 0) {
				say("gt9889: package %u: the firmware mask is not the chip's\n", i);
				continue;
			}
		}
		if (!regs_read(k->pid_reg, pid, k->pid_length)) {
			say("gt9889: package %u: the hardware id (0x%04x) not read: %s\n", i, k->pid_reg, failed);
			return NULL;
		}
		if (memcmp(pid, k->pid, k->pid_length) != 0) {
			say("gt9889: package %u is for hardware id %.4s, the chip says %.4s\n", i, (const char *)k->pid, (const char *)pid);
			continue;
		}
		if (k->cfg_type == 1) {
			return k;
		}
	}
	return NULL;
}

/* the configuration into the chip (goodix_send_large_config()): the command register free, 0x80, its 0x82, the bytes, 0x83, free */
static bool
package_send(const struct gt9889_package *k)
{
	if (!command_free(k->command_reg)) {
		say("gt9889: the command register (0x%04x) is not free (0xff)\n", k->command_reg);
		return false;
	}
	if (!command_send(k->command_reg, CMD_START_SEND) || !command_answered(k->command_reg, CMD_PREPARE_OK, CMD_START_SEND)) {
		say("gt9889: the chip does not take the configuration (0x%04x does not say 0x82)\n", k->command_reg);
		return false;
	}
	for (unsigned at = 0; at < k->length; at += WRITE_MAX) {
		unsigned n = k->length - at < WRITE_MAX ? k->length - at : WRITE_MAX;

		if (!regs_write((uint16_t)(k->cfg_reg + at), &k->cfg[at], n)) {
			say("gt9889: the configuration not written at 0x%04x: %s\n", k->cfg_reg + at, failed);
			return false;
		}
	}
	if (!command_send(k->command_reg, CMD_END_SEND) || !command_free(k->command_reg)) {
		say("gt9889: the chip does not finish with the configuration (0x%04x not 0xff after 0x83)\n", k->command_reg);
		return false;
	}
	return true;
}

/* the watchdog's first tick, after a reset */
static void
esd_init(void)
{
	if (!reg_write(esd_reg, ESD_TICK)) {
		say("gt9889: the watchdog (0x%04x) not fed: %s\n", esd_reg, failed);
	}
	esd_on = true;
}

/* every five seconds (goodix_esd_check(), goodix_ts_esd_work()): looked at, said if it did not run, fed again */
static void
esd_tick(void)
{
	uint8_t value = 0;
	bool read = regs_read(GT9889_REG_ESD_CHECK, &value, 1);

	seen.esd_ticks++;
	if (!read || value == ESD_TICK) {
		if (seen.esd_failed++ < 4) {
			say("gt9889: the watchdog check %s (0x%04x%s%02x): the firmware may not be running\n", read ? "found the tick still there" : "was not answered",
			    GT9889_REG_ESD_CHECK, read ? " = 0x" : "", value);
		}
	}
	(void)reg_write(esd_reg, ESD_TICK);
}

/* After a reset and an answer from the chip: the watchdog, then the configuration if the board has one and the file is whole. */
static void
chip_start(void)
{
	const struct gt9889_package *k;
	bool held, sent;

	esd_init();
	package_load();
	if (package_file.count == 0 || going()) {
		return;
	}
	held = doze_hold();
	if (!held) {
		say("gt9889: the chip does not say it is awake (0xbb in 0x%04x): going on\n", GT9889_REG_DOZE_STATE);
	}
	k = package_select();
	if (k == NULL) {
		say("gt9889: no package for this chip: nothing sent\n");
	} else {
		esd_reg = k->esd_reg;
		sent = package_send(k);
		say("gt9889: configuration package for sensor id %u (%u bytes, version 0x%02x) %s\n", k->sensor_id, k->length, k->cfg[0],
		    sent ? "sent" : "NOT sent");
	}
	if (held) {
		doze_release();
	}
}

static uint32_t steps = 5;              /* gt9889_steps=N */

/* Steps 1 to 4 (this file's head): the chip ready to be read, not there (for a reason said), or the steps asked for made. */
enum bring_up { READY, NO_CHIP, STEPS_MADE };

static enum bring_up
bring_up(void)
{
	show_pins();
	if (steps < 2) {
		return STEPS_MADE;
	}
	say("gt9889: step 2: the chip held in reset, INT driven high, its supplies switched on (I/O %s, analog %s%s)\n",
	    WIRED->io_supply != NULL ? WIRED->io_supply : "not ours to switch",
	    WIRED->analog_supply != NULL ? WIRED->analog_supply : HAS_AVDD ? "behind its pin" : "not ours to switch",
	    WIRED->analog_supply != NULL && HAS_AVDD ? " and its pin" : "");
	pins_record();
	gpio_output(PIN_RESET, false, PULL_NONE, 2);
	gpio_output(PIN_INT, true, PULL_NONE, 2);
	if (HAS_AVDD) {
		gpio_output(PIN_AVDD, true, PULL_NONE, 2);      /* (the vendor's first: goodix_ts_power_on) */
	}
	wait_ms(1);
	if (WIRED->io_supply != NULL && !supply_on(WIRED->io_supply, IO_MILLIVOLTS, &io_supply_at)) {
		return NO_CHIP;
	}
	if (WIRED->analog_supply != NULL && !supply_on(WIRED->analog_supply, WIRED->analog_mv, &analog_supply_at)) {
		return NO_CHIP;
	}
	wait_ms(POWER_ON_MS);
	if (__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
		return NO_CHIP;                 /* (the run's end has the pins and the supplies: nothing more is done) */
	}
	if (steps < 3) {
		return STEPS_MADE;
	}
	say("gt9889: step 3: the reset low %u ms and let go, INT let go after it, the chip given %u ms\n", RESET_LOW_MS, RESET_WAIT_MS);
	reset_pulse();
	if (steps < 4) {
		return STEPS_MADE;
	}
	say("gt9889: step 4: the first transfers on the I2C bus (gpio%u, gpio%u): the chip's report block\n", PIN_SDA, PIN_SCL);
	engine_take();
	if (!identify(IDENTITY_TRIES)) {
		return NO_CHIP;
	}
	return steps < 5 ? STEPS_MADE : READY;
}

/*
 * touchscreen=-1: steps 1 and 4 alone. The chip is asked as the firmware left it -- once: nothing was switched on that
 * it could be waited for -- and nothing but the bus's two pins is touched, those given back as found.
 */
static void
look(void)
{
	uint32_t reset_cfg = rd(GPIO_CFG(PIN_RESET)), reset_io = rd(GPIO_IO(PIN_RESET));

	say("gt9889: touchscreen=-1: looking only\n");
	show_pins();
	if ((reset_cfg & CFG_OUTPUT) && !(reset_io & 2)) {
		say("gt9889: the chip is held in reset as found (gpio%u drives low): it is not expected to answer\n", PIN_RESET);
	}
	engine_look();
	say("gt9889: step 4: the chip's report block, as it was left (steps 2 and 3, its power and its reset, are not made)\n");
	(void)identify(1);
	say("gt9889: touchscreen=-1: nothing powered, configured or written\n");
}

/* ---- the thread ------------------------------------------------------------------------------------------- */

static bool looking_only;              /* touchscreen=-1 */

/* The chip is the thread's from here until it calls done(); false once the run's end has it. */
static bool
begin(void)
{
	__atomic_store_n(&busy, true, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&leaving, __ATOMIC_SEQ_CST)) {
		__atomic_store_n(&busy, false, __ATOMIC_SEQ_CST);
		return false;
	}
	return true;
}

static void
done(void)
{
	__atomic_store_n(&busy, false, __ATOMIC_SEQ_CST);
}

/* looks in a row without a report, resets made for that, and when the chip last said something believable and last did not */
static struct { unsigned failures, resets; uint64_t last_good_us, last_failure_us; } trouble;

static void
heard(uint64_t now)
{
	trouble.failures = 0;
	trouble.last_good_us = now;
	if (trouble.resets != 0 && now - trouble.last_failure_us >= TROUBLE_FORGOTTEN_US) {
		trouble.resets = 0;
	}
}

/* a finger with nothing believable for LIFT_US is lifted: the chip has lost it, or the report that lifted it */
static void
lift_if_silent(uint64_t now)
{
	if (fingers_down() != 0 && now - trouble.last_good_us >= LIFT_US) {
		say("gt9889: nothing believable for %u ms with a finger down: lifted\n", (unsigned)((now - trouble.last_good_us) / 1000));
		release();
	}
}

/* One look at the chip; false: it is given up. */
static bool
poll_once(unsigned tick)
{
	uint8_t report_bytes[GT9889_REPORT_MAX];
	struct gt9889_frame frame;
	unsigned bytes = 0;
	enum gt9889_look look = GT9889_BAD;
	bool armed = __atomic_load_n(&latch_armed, __ATOMIC_SEQ_CST), edge = false, down = fingers_down() != 0, asked, got;
	uint64_t now;

	if (esd_on && tick % ESD_TICKS == 0) {
		esd_tick();
	}
	if (armed) {
		/* the latch is cleared before the read, so that an edge during it is the next report's */
		edge = gpio_edge_latched(PIN_INT);
		if (edge) {
			gpio_edge_clear(PIN_INT);
		}
		asked = edge;
		if (!edge) {
			if (tick != 1 && tick % LATCHLESS_TICKS != 0) {
				lift_if_silent(now_us());
				return true;
			}
		}
	} else {
		/* (no latch: the line may only pulse for a report, and a board may have it wrong: asked now and then whatever it says) */
		asked = !gpio_get(PIN_INT) || down;
		if (!asked && tick % IDLE_LOOKS != 0) {
			return true;
		}
	}
	seen.looks++;
	got = report_read(report_bytes, &bytes);
	now = now_us();
	if (got && trace && seen.raw_dumped < 60) {
		/* gt9889_trace=1: each read as it came, before it is judged (the first 60), to tell the bus from the chip's format */
		char hex[3 * GT9889_REPORT_MAX + 1];
		unsigned at = 0;

		seen.raw_dumped++;
		for (unsigned i = 0; i < bytes && at + 3 < sizeof(hex); i++) {
			at += (unsigned)snprintf(hex + at, sizeof(hex) - at, "%02x ", report_bytes[i]);
		}
		say("gt9889: read %u bytes (%s, line %s, latch %s): %s\n", bytes, asked ? "asked" : "unasked", gpio_get(PIN_INT) ? "high" : "low",
		    armed ? (edge ? "set" : "clear") : "off", hex);
	}
	if (got) {
		look = gt9889_decode(report_bytes, bytes, contacts, panel_x, panel_y, &frame);
	}
	if (!asked && (!got || look == GT9889_BAD)) {
		/* a look that was not asked for by the line or a finger, and the chip did not answer: it may be dozing (this file's head) */
		seen.idle_misses++;
		lift_if_silent(now);
		return true;
	}
	if (look == GT9889_FRAME) {
		heard(now);
		seen.reports++;
		report(&frame);
	} else if (look == GT9889_NOTHING || look == GT9889_REQUEST) {
		if (look == GT9889_REQUEST && seen.requests++ < 4) {
			say("gt9889: the firmware asks for something (event byte 0x%02x): this driver does nothing for it, and says it was taken\n",
			    report_bytes[0]);
		}
		if (!down || look == GT9889_REQUEST) {
			heard(now);
		}
		lift_if_silent(now);
	} else {
		seen.bad++;
		seen.reads_failed += !got;
		trouble.last_failure_us = now;
		if (seen.bad <= 4) {
			if (got) {
				say("gt9889: a report that cannot be one: %u bytes, %02x %02x %02x %02x %02x %02x\n", bytes, report_bytes[0],
				    report_bytes[1], report_bytes[2], report_bytes[3], report_bytes[4], report_bytes[5]);
			} else {
				say("gt9889: a report not read: %s\n", failed);
			}
		}
		lift_if_silent(now);
		if (++trouble.failures >= TROUBLE_LOOKS) {
			release();
			say("gt9889: %u looks in a row without a report (%u reads failed in all); resets made for that before: %u of %u\n",
			    trouble.failures, seen.reads_failed, trouble.resets, TROUBLE_RESETS);
			if (trouble.resets++ >= TROUBLE_RESETS) {
				return false;
			}
			reset_pulse();
			if (!identify(IDENTITY_TRIES)) {
				return false;
			}
			chip_start();
			trouble.failures = 0;
		}
	}
	if (tick % (60 * 1000 / POLL_MS) == 0) {
		say("gt9889: so far %u reports in %u looks (%u without one, %u reads failed, %u unasked looks unanswered, %u firmware "
		    "requests), %u finger positions; the interrupt line reads %s\n", seen.reports, seen.looks, seen.bad, seen.reads_failed,
		    seen.idle_misses, seen.requests, seen.positions, gpio_get(PIN_INT) ? "high" : "low");
	}
	return true;
}

static void
gt9889_thread(__unused void *parameter, __unused wait_result_t result)
{
	bool supplied = WIRED->io_supply != NULL || WIRED->analog_supply != NULL;
	enum bring_up made;

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	if (tlmm == 0) {
		say("gt9889: the pins could not be mapped\n");
		thread_terminate(current_thread());
		return;
	}
	if (looking_only) {
		if (begin()) {
			look();
			done();
		}
		thread_terminate(current_thread());
		return;
	}
	if (supplied) {
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
		cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	}
	if (!begin()) {
		thread_terminate(current_thread());
		return;
	}
	if (supplied && (rsc == 0 || cmd_db == 0)) {
		say("gt9889: RPMh or its command database could not be mapped\n");
		made = NO_CHIP;
	} else {
		made = bring_up();
	}
	if (made != READY) {
		if (made == STEPS_MADE) {
			say("gt9889: stopping here (gt9889_steps=%u)\n", steps);
		} else {
			say("gt9889: touch screen unavailable\n");
		}
		power_off();
		done();
		thread_terminate(current_thread());
		return;
	}
	chip_start();
	done();
	say("gt9889: step 5: touch screen ready, %u x %u, %u fingers; the interrupt line (gpio%u) is looked at every %u ms (%s)\n", panel_x,
	    panel_y, contacts, PIN_INT, POLL_MS, latch_armed ? "its falling edges latched" : "by its level");
	trouble.last_good_us = now_us();
	for (unsigned tick = 1; begin(); tick++) {
		if (!poll_once(tick)) {
			release();
			say("gt9889: stopped: the chip does not come back\n");
			power_off();
			done();
			break;
		}
		done();
		wait_ms(POLL_MS);
	}
	/* (or the run's end has the chip: hakios_gt9889_quiesce() powers it off) */
	thread_terminate(current_thread());
}

void
hakios_gt9889_init(void)
{
	const struct hakios_board *b = hakios_board();
	int32_t enabled = 0;
	thread_t thread;

	/* boot-arg touchscreen=1: the chip started, its touches posted to /dev/hakios-touch; touchscreen=-1: only looked at */
	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	looking_only = enabled == -1;
	/* (called only on a board that lists "touch-gt9889": bsd/dev/board/drivers.c) */
	if (b == NULL || b->pins.tlmm == 0) {
		say("gt9889: disabled: the SoC file does not say where the pins are (pins { })\n");
		return;
	}
	if (WIRED->sda == WIRED->scl || WIRED->address == 0 || WIRED->interrupt == WIRED->reset) {
		say("gt9889: disabled: the board file does not say how the controller is wired (touch-i2c { })\n");
		return;
	}
	if ((WIRED->io_supply != NULL || WIRED->analog_supply != NULL) && (b->rpmh.cmd_db == 0 || b->rpmh.rsc == 0)) {
		say("gt9889: disabled: touch-i2c { } names a supply, and the SoC file does not say where RPMh is (rpmh { })\n");
		return;
	}
	if (WIRED->analog_supply != NULL && WIRED->analog_mv == 0) {
		say("gt9889: disabled: touch-i2c { } names an analog supply without its voltage (analog-millivolts)\n");
		return;
	}
	/* (a report carries 16 bits of a coordinate, and the vendor's panel-max-id of davinci is 10: more is a board file that is wrong) */
	contacts = WIRED->contacts;
	panel_x = WIRED->max_x;
	panel_y = WIRED->max_y;
	if (contacts == 0 || contacts > GT9889_CONTACTS || panel_x == 0 || panel_y == 0 || panel_x > GT9889_COORDINATE_MAX ||
	    panel_y > GT9889_COORDINATE_MAX) {
		say("gt9889: disabled: touch-i2c { } contacts %u and coordinates %u x %u: 1 to %u fingers, and coordinates of 1 to %u, "
		    "are what a report can say\n", contacts, panel_x, panel_y, GT9889_CONTACTS, GT9889_COORDINATE_MAX);
		return;
	}
	i2c_bus.sda = PIN_SDA;
	i2c_bus.scl = PIN_SCL;
	/*
	 * touch_flip=N: bit 0 mirrors the touches left to right, bit 1 top to bottom.
	 * gt9889_trace=1: the first 40 touches' positions as the controller gives them, in the log.
	 * gt9889_doze=1: the event byte's write inside the vendor's doze handshake (this file's head).
	 * gt9889_steps=N: the bring-up stops after its first N steps (this file's head), and powers off what it powered.
	 */
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	PE_parse_boot_argn("gt9889_trace", &trace, sizeof(trace));
	PE_parse_boot_argn("gt9889_steps", &steps, sizeof(steps));
	PE_parse_boot_argn("gt9889_doze", &doze, sizeof(doze));
	if (steps == 0) {
		say("gt9889: gt9889_steps=0: nothing is done\n");
		return;
	}
	__atomic_store_n(&started, true, __ATOMIC_SEQ_CST);
	if (kernel_thread_start(gt9889_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		__atomic_store_n(&started, false, __ATOMIC_SEQ_CST);
		say("gt9889: cannot start the touch screen's thread\n");
	}
}

/*
 * At the run's end and on a panic's way out (osfmk/arm64/loader_return.c): no more polling, a transaction cut off in
 * its middle ended and the bus's pins given back, and the chip powered off (this file's head). Spins, with interrupts
 * off, a bounded time.
 */
void
hakios_gt9889_quiesce(void)
{
	unsigned clocks = 0;

	if (!__atomic_load_n(&started, __ATOMIC_SEQ_CST)) {
		return;
	}
	__atomic_store_n(&leaving, true, __ATOMIC_SEQ_CST);
	for (unsigned i = 0; i < 100 && __atomic_load_n(&busy, __ATOMIC_SEQ_CST); i++) {
		spin_us(1000);                  /* (the thread finishing a transaction: 100 ms at most, a stopped core's never ends) */
	}
	if (tlmm == 0) {
		return;                         /* (the thread never came as far as the pins) */
	}
	if (__atomic_load_n(&on_bus, __ATOMIC_SEQ_CST) && __atomic_load_n(&geni_on, __ATOMIC_SEQ_CST)) {
		/* the engine was in the middle of a message: cancelled, which ends it with a stop */
		geni_i2c_abort(&geni);
		__atomic_store_n(&on_bus, false, __ATOMIC_SEQ_CST);
		say("gt9889: at the run's end: a transfer cut off: the serial engine's command ended\n");
	} else if (__atomic_load_n(&on_bus, __ATOMIC_SEQ_CST)) {
		/* the thread stopped in the middle of a transaction, or of taking the pins: ended, the pins as the run found them */
		i2c_gpio_stop(&i2c_bus);
		bus_give_back();
		if (!i2c_gpio_level(PIN_SDA) || !i2c_gpio_level(PIN_SCL)) {
			/* a chip cut off in a byte it was sending holds the data line down until it has sent it: clocked free */
			bool came_back = i2c_gpio_unwedge(&i2c_bus, &clocks);

			bus_give_back();
			if (came_back) {
				say("gt9889: at the run's end: a transfer cut off: the bus came back after %u clocks\n", clocks);
			} else {
				say("gt9889: at the run's end: a transfer cut off: THE I2C BUS IS NOT AT REST (gpio%u reads %u, gpio%u reads %u)\n",
				    PIN_SDA, i2c_gpio_level(PIN_SDA), PIN_SCL, i2c_gpio_level(PIN_SCL));
			}
		} else {
			say("gt9889: at the run's end: a transfer cut off: ended, the bus at rest\n");
		}
	}
	if (__atomic_load_n(&powered, __ATOMIC_SEQ_CST)) {
		say("gt9889: at the run's end: %u reports in %u looks\n", seen.reports, seen.looks);
	}
	power_off();
}
