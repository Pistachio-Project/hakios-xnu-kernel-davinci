/*
 * A serial engine as I2C (drivers/board/geni_i2c.h) and the GT9889 touch driver using it (drivers/board/gt9889.c on a
 * board whose touch-i2c { } names the engine), against a model of the engine in its FIFO mode with a chip on its bus,
 * and of the clock controller's votes it needs.
 *
 * The engine model: registers that hold what is written, a protocol and a FIFO depth that are the firmware's, a command
 * that runs at once on the model's bus (a write is done when the last byte of the FIFO has arrived, a read fills the
 * RX FIFO), the status bits Linux's driver reads (done, NACK, cancel, abort), a TX FIFO that drains a word a look and
 * so is never taken to hold more than its depth, an engine that does not take a cancel, one that never finishes, and
 * clocks that make an access to it a stall: an access to the engine while its bus clocks are off is counted, for on
 * the phone it would not return. The clock controller holds the vote register, the three clocks' status that follows
 * the votes and the rate generator's presets.
 *
 * What is tested: setting up (the registers a command needs, the divider and counters of 400 kHz, the preset of 19.2 MHz
 * and not another); that what was found is given back (the engine's registers, the votes it added and not the ones that
 * were there); the refusals (not I2C, no FIFO, clocks that stay off, no 19.2 MHz) that leave the engine untouched and the
 * votes withdrawn; a write, and a write and a read after a repeated start: the command, the address, the bytes in order,
 * odd lengths, more bytes than the FIFO holds; a NACK (said, recovered) and an engine that does not take the cancel; a look
 * that writes nothing and reads no register of a gated engine. Then the driver: with the keys the report is read through
 * the engine and the bus's two pins are not touched; without a usable engine the pins are driven by hand and the engine
 * as found; touchscreen=-1 writes nothing to the engine or the clock controller; the run's end in a transfer that never
 * ends puts the engine and the votes back.
 *
 *   geni_i2c_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"
#include "tlmm_model.h"

extern char log_text[];

/* ---- the clock controller ----------------------------------------------------------------------------------------- */

#define GCC_AT          0x00100000u
#define SE_AT           0x00a84000u
#define VOTE_REG        0x5200c
#define M_AHB           0x1800c
#define S_AHB           0x18010
#define SE_CLK          0x18864
static const unsigned votes_bits[5] = { 18, 19, 20, 21, 29 };

static uint32_t gcc_regs[0x100000 / 4];
static struct {
	bool stuck_off;                 /* the clocks stay off whatever is voted */
	unsigned vote_writes, writes;
	uint32_t found_vote;
	bool dfs;                       /* the presets are in use */
	uint32_t level_cfg[8];
} clocks;

static bool
clocks_on(void)
{
	uint32_t vote = gcc_regs[VOTE_REG / 4];

	return !clocks.stuck_off && (vote & 1u << 20) && (vote & 1u << 21) && (vote & 1u << 29);
}

static uint32_t
gcc_rd(uint32_t offset)
{
	switch (offset) {
	case M_AHB: case S_AHB: case SE_CLK:
		return clocks_on() ? 0 : 1u << 31;
	}
	return gcc_regs[offset / 4];
}

static void
gcc_wr(uint32_t offset, uint32_t value)
{
	clocks.writes++;
	clocks.vote_writes += offset == VOTE_REG;
	gcc_regs[offset / 4] = value;
}

/* ---- the engine ---------------------------------------------------------------------------------------------------- */

#define DEPTH           16

static uint32_t se_regs[0x4000 / 4];
static struct {
	unsigned protocol, depth;
	bool no_fifo;
	bool deaf;                      /* a cancel and an abort are not taken */
	bool stall;                     /* a command that never finishes */
	bool running;
	uint32_t opcode, address, length, params;
	uint8_t out[256];
	uint32_t got;                   /* bytes of a write that arrived */
	uint32_t tx_words, most_tx_words;
	uint32_t rx[64], rx_head, rx_tail, rx_length;
	uint32_t status;
	unsigned cancels, aborts, writes, reads, gated;
	struct { uint32_t opcode, address, length, params; } msgs[64];
	unsigned count;
} engine = { .protocol = 3, .depth = DEPTH };

/* ---- the chip on the bus: 16-bit registers, the report block at 0x4100 ---------------------------------------------- */

#define REPORT_AT       0x4100
static bool chip_present = true;
static unsigned chip_address = 0x5d;
static uint8_t rep[96];
static volatile bool pending;
static unsigned ptr;
static volatile unsigned reports_read, clears, other_writes, nacks;
static uint8_t scratch[0x100];          /* a register far from the report, to read and write whatever */

static uint8_t
chip_read(void)
{
	uint8_t byte = 0;

	if (ptr == REPORT_AT) {
		reports_read++;
	}
	if (ptr >= REPORT_AT && ptr < REPORT_AT + sizeof(rep)) {
		byte = pending ? rep[ptr - REPORT_AT] : 0;
	} else if (ptr < sizeof(scratch)) {
		byte = scratch[ptr];
	} else {
		byte = (uint8_t)(ptr * 7);      /* something that tells which register it was */
	}
	ptr++;
	return byte;
}

static void
chip_write(const uint8_t *bytes, unsigned count)
{
	unsigned i = 0;

	if (count >= 2) {
		ptr = (unsigned)bytes[0] << 8 | bytes[1];
		i = 2;
	}
	for (; i < count; i++, ptr++) {
		if (ptr == REPORT_AT && bytes[i] == 0) {
			clears++;
			pending = false;
		} else if (ptr < sizeof(scratch)) {
			scratch[ptr] = bytes[i];
			other_writes++;
		} else {
			other_writes++;
		}
	}
}

/* ---- the engine's registers ------------------------------------------------------------------------------------------ */

static pthread_t scenario_thread;
static volatile bool freeze_on_command;         /* the driver's thread stops in the middle of its next command: the run's end finds it so */
static volatile bool frozen;

static void
finish_write(void)
{
	chip_write(engine.out, engine.got);
	engine.running = false;
	engine.status |= 1;                     /* M_CMD_DONE */
}

static uint32_t
se_rd(uint32_t offset)
{
	engine.reads++;
	engine.gated += !clocks_on();
	switch (offset) {
	case 0x068: return engine.protocol << 8 | 0x21;
	case 0x064: return engine.no_fifo;
	case 0xe24: return engine.depth << 16;
	case 0x040: return engine.running;
	case 0x800: {
		uint32_t words = engine.tx_words;

		if (engine.tx_words > 0) {
			engine.tx_words--;      /* (it goes out a word a look) */
		}
		return words;
	}
	case 0x610: return engine.status;
	case 0x804: {
		uint32_t words = engine.rx_tail - engine.rx_head, status = words;

		if (words > 0 && engine.rx_length % 4 != 0) {
			status |= 0x80000000u | (engine.rx_length % 4) << 28;
		}
		return status;
	}
	case 0x780: return engine.rx_head < engine.rx_tail ? engine.rx[engine.rx_head++ % 64] : 0;
	}
	return se_regs[offset / 4];
}

static void
se_wr(uint32_t offset, uint32_t value)
{
	engine.writes++;
	engine.gated += !clocks_on();
	se_regs[offset / 4] = value;
	switch (offset) {
	case 0x600: {                           /* a command */
		uint32_t opcode = value >> 27, address = value >> 9 & 0x7f;

		engine.opcode = opcode;
		engine.address = address;
		engine.params = value & 0x7f;
		engine.running = true;
		engine.got = 0;
		engine.rx_head = engine.rx_tail = 0;
		if (engine.count < 64) {
			engine.msgs[engine.count].opcode = opcode;
			engine.msgs[engine.count].address = address;
			engine.msgs[engine.count].params = value & 0x7f;
			engine.msgs[engine.count].length = opcode == 2 ? se_regs[0x270 / 4] : se_regs[0x26c / 4];
		}
		engine.count++;
		if (freeze_on_command && !pthread_equal(pthread_self(), scenario_thread)) {
			frozen = true;
			for (;;) {
				usleep(100000);
			}
		}
		engine.length = opcode == 2 ? se_regs[0x270 / 4] : se_regs[0x26c / 4];
		if (!chip_present || address != chip_address) {
			engine.status |= 1u << 10;      /* general-purpose interrupt 1: NACK; the command goes on running */
			nacks++;
		} else if (engine.stall) {
			break;
		} else if (opcode == 2) {
			engine.rx_length = engine.length;
			for (uint32_t i = 0; i < engine.length && i < 256; i++) {
				if (i % 4 == 0) {
					engine.rx[engine.rx_tail % 64] = 0;
				}
				engine.rx[engine.rx_tail % 64] |= (uint32_t)chip_read() << (8 * (i % 4));
				if (i % 4 == 3 || i + 1 == engine.length) {
					engine.rx_tail++;
				}
			}
			engine.running = false;
			engine.status |= 1;
		}
		break;
	}
	case 0x618:                             /* status cleared */
		engine.status &= ~value;
		break;
	case 0x604:
		if (!engine.deaf) {
			engine.status |= value & 4 ? 1u << 4 : value & 2 ? 1u << 5 : 0;
			engine.running = false;
		}
		engine.cancels += (value & 4) != 0;
		engine.aborts += (value & 2) != 0;
		break;
	case 0x700:                             /* a word out */
		for (int j = 0; j < 4 && engine.got < engine.length && engine.got < sizeof(engine.out) &&
		    (engine.opcode == 1 && engine.running); j++) {
			engine.out[engine.got++] = (uint8_t)(value >> (8 * j));
		}
		engine.tx_words++;
		if (engine.tx_words > engine.most_tx_words) {
			engine.most_tx_words = engine.tx_words;
		}
		if (engine.opcode == 1 && engine.running && engine.got == engine.length && !engine.stall &&
		    chip_present && engine.address == chip_address) {
			finish_write();
		}
		break;
	}
}

static unsigned tlmm_pin_writes[PINS];

uint32_t
hakios_test_rd(vm_offset_t address)
{
	if (address >= GCC_AT && address < GCC_AT + 0x100000) {
		return gcc_rd((uint32_t)(address - GCC_AT));
	}
	if (address >= SE_AT && address < SE_AT + 0x4000) {
		return se_rd((uint32_t)(address - SE_AT));
	}
	return tlmm_model_rd(address);
}

void
hakios_test_wr(vm_offset_t address, uint32_t value)
{
	if (address >= GCC_AT && address < GCC_AT + 0x100000) {
		gcc_wr((uint32_t)(address - GCC_AT), value);
		return;
	}
	if (address >= SE_AT && address < SE_AT + 0x4000) {
		se_wr((uint32_t)(address - SE_AT), value);
		return;
	}
	if (address >= model.base && (address - model.base) / 0x1000 < PINS) {
		tlmm_pin_writes[(address - model.base) / 0x1000]++;
	}
	tlmm_model_wr(address, value);
}

/* ---- what the driver posts, RPMh and its command database --------------------------------------------------------------- */

static struct post { uint32_t type, finger, x, y; } posted[512];
static volatile unsigned posts;
static pthread_mutex_t post_lock = PTHREAD_MUTEX_INITIALIZER;

void
hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	pthread_mutex_lock(&post_lock);
	if (posts < 512) {
		posted[posts] = (struct post){ type, finger, x, y };
	}
	posts++;
	pthread_mutex_unlock(&post_lock);
}

void hakios_rpmh_lock(void) { }
void hakios_rpmh_unlock(void) { }

#define HAKIOS_CMD_DB_H
struct cmd_db_header { uint8_t magic[4]; };
static bool hakios_cmd_db_valid(const struct cmd_db_header *db __unused) { return true; }
static uint32_t hakios_cmd_db_address(const struct cmd_db_header *db __unused, uint32_t s __unused, const char *n __unused)
{
	return 0x40600;
}

static unsigned asks;
#define HAKIOS_RPMH_H
static const char *
hakios_rpmh_send_rsc(vm_offset_t r __unused, uint32_t o __unused, const uint32_t *a __unused, const uint32_t *d __unused, unsigned c)
{
	asks += c;
	return NULL;
}

static unsigned mappings;
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { mappings++; return phys; }
vm_offset_t ml_io_map_wcomb(vm_offset_t phys, vm_size_t size __unused) { mappings++; return phys; }

void hakios_gt9889_init(void);
void hakios_gt9889_quiesce(void);

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
#include "board/gt9889.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited_ = 0; !(cond) && waited_ < (ms); waited_ += 2) usleep(2000); } while (0)

/* ---- the phone ------------------------------------------------------------------------------------------------------- */

#define FOUND_VOTE      0x00000001u     /* a vote someone else holds: bit 0 */

static uint32_t se_found[0x4000 / 4];

/* the engine and the clock controller as the firmware leaves them: I2C, clocks off, the presets with 19.2 MHz at level 2 */
static void
phone(void)
{
	memset(&engine, 0, sizeof(engine));
	engine.protocol = 3;
	engine.depth = DEPTH;
	memset(&clocks, 0, sizeof(clocks));
	memset(gcc_regs, 0, sizeof(gcc_regs));
	memset(se_regs, 0, sizeof(se_regs));
	gcc_regs[VOTE_REG / 4] = FOUND_VOTE;
	gcc_regs[(SE_CLK + 0x18) / 4] = 1;                      /* the engine chooses among presets */
	for (unsigned level = 0; level < 8; level++) {
		gcc_regs[(SE_CLK + 0x20 + 4 * level) / 4] = 1u << 8 | 0x1f;     /* GPLL0 / 15.5: 38.7 MHz */
	}
	gcc_regs[(SE_CLK + 0x20 + 4 * 2) / 4] = 0;              /* the crystal, undivided: 19.2 MHz */
	for (unsigned i = 0; i < 0x4000 / 4; i++) {
		se_regs[i] = 0x11110000u + i;                   /* (what the firmware left: something) */
	}
	se_regs[0x40 / 4] = 0;
	memcpy(se_found, se_regs, sizeof(se_found));
	chip_present = true;
	chip_address = 0x5d;
	memset(scratch, 0, sizeof(scratch));
	ptr = 0;
	pending = false;
}

static struct geni_i2c eng;

static void
bare_engine(void)
{
	phone();
	memset(&eng, 0, sizeof(eng));
	eng.se = SE_AT;
	eng.gcc = GCC_AT;
	eng.m_ahb = M_AHB;
	eng.s_ahb = S_AHB;
	eng.se_clock = SE_CLK;
	eng.vote_register = VOTE_REG;
	for (unsigned i = 0; i < 5; i++) {
		eng.vote[i] = (uint8_t)votes_bits[i];
	}
}

/* the registers of the engine that differ from what the firmware left, among those geni_i2c_kept names */
static unsigned
kept_changed(void)
{
	unsigned n = 0;

	for (unsigned i = 0; i < GENI_I2C_KEPT; i++) {
		n += se_regs[geni_i2c_kept[i] / 4] != se_found[geni_i2c_kept[i] / 4];
	}
	return n;
}

/* ---- scenarios: the engine ------------------------------------------------------------------------------------------- */

static const uint32_t all_votes = 1u << 18 | 1u << 19 | 1u << 20 | 1u << 21 | 1u << 29;

static void
sets_up(void)
{
	const char *why;

	bare_engine();
	why = geni_i2c_open(&eng, 400000);
	CHECK(why == NULL, "ready: %s", why);
	CHECK((gcc_regs[VOTE_REG / 4] & all_votes) == all_votes && (gcc_regs[VOTE_REG / 4] & FOUND_VOTE), "its five votes taken, the one found kept: %#x",
	    gcc_regs[VOTE_REG / 4]);
	CHECK(eng.fifo_depth == DEPTH, "the FIFO's depth: %u", eng.fifo_depth);
	CHECK(se_regs[0x07c / 4] == 2, "the preset of 19.2 MHz (2), not the first: %u", se_regs[0x07c / 4]);
	CHECK(se_regs[0x048 / 4] == (2u << 4 | 1), "divider 2, enabled for 400 kHz: %#x", se_regs[0x048 / 4]);
	CHECK(se_regs[0x278 / 4] == (5u << 20 | 12u << 10 | 24u), "the counters of 400 kHz: %#x", se_regs[0x278 / 4]);
	CHECK(se_regs[0x810 / 4] == DEPTH - 3 && se_regs[0x814 / 4] == DEPTH - 2 && se_regs[0x80c / 4] == 0, "the watermarks");
	CHECK(se_regs[0x260 / 4] == 0x0007f8fe && se_regs[0x284 / 4] == 0x0007f8fe, "four bytes a word, the first lowest");
	CHECK(!(se_regs[0x258 / 4] & 1), "FIFO mode, not DMA: %#x", se_regs[0x258 / 4]);
	CHECK(engine.gated == 0, "no access to the engine while its clocks were off: %u", engine.gated);
	CHECK(kept_changed() > 10, "it set the engine up: %u registers changed", kept_changed());
	geni_i2c_close(&eng);
	CHECK(kept_changed() == 0, "the engine's registers back as found: %u differ", kept_changed());
	CHECK(gcc_regs[VOTE_REG / 4] == FOUND_VOTE, "the votes it added withdrawn, the one found not: %#x", gcc_regs[VOTE_REG / 4]);
	geni_i2c_close(&eng);
	CHECK(gcc_regs[VOTE_REG / 4] == FOUND_VOTE && kept_changed() == 0, "closed twice: no harm");
	CHECK(geni_i2c_open(&eng, 100000) == NULL && se_regs[0x048 / 4] == (7u << 4 | 1) && se_regs[0x278 / 4] == (10u << 20 | 11u << 10 | 26u),
	    "100 kHz: divider 7, counters 10 11 26");
	geni_i2c_close(&eng);
	CHECK(geni_i2c_open(&eng, 123456) != NULL && gcc_regs[VOTE_REG / 4] == FOUND_VOTE, "a rate with no counters: refused, nothing voted");
}

/* votes someone else already holds are not ours to withdraw */
static void
votes_found_stay(void)
{
	bare_engine();
	gcc_regs[VOTE_REG / 4] |= 1u << 20 | 1u << 29;
	CHECK(geni_i2c_open(&eng, 400000) == NULL, "ready");
	geni_i2c_close(&eng);
	CHECK(gcc_regs[VOTE_REG / 4] == (FOUND_VOTE | 1u << 20 | 1u << 29), "only what it added withdrawn: %#x", gcc_regs[VOTE_REG / 4]);
}

static void
refuses_spi(void)
{
	const char *why;

	bare_engine();
	engine.protocol = 1;
	why = geni_i2c_open(&eng, 400000);
	CHECK(why != NULL && strstr(why, "no I2C firmware") != NULL, "refused, and says why: %s", why);
	CHECK(gcc_regs[VOTE_REG / 4] == FOUND_VOTE, "the votes withdrawn: %#x", gcc_regs[VOTE_REG / 4]);
	CHECK(engine.writes == 0 && kept_changed() == 0, "the engine untouched: %u writes", engine.writes);
	CHECK(engine.gated == 0, "no register of a gated engine read");
	engine.protocol = 0;
	CHECK(geni_i2c_open(&eng, 400000) != NULL && engine.writes == 0, "no firmware at all (0): refused");
}

static void
refuses_gsi_and_no_fifo(void)
{
	bare_engine();
	engine.no_fifo = true;
	CHECK(geni_i2c_open(&eng, 400000) != NULL && engine.writes == 0 && gcc_regs[VOTE_REG / 4] == FOUND_VOTE, "FIFO interface disabled: refused");
	engine.no_fifo = false;
	engine.depth = 2;
	CHECK(geni_i2c_open(&eng, 400000) != NULL && engine.writes == 0 && gcc_regs[VOTE_REG / 4] == FOUND_VOTE, "no FIFO: refused");
}

static void
refuses_clocks_that_stay_off(void)
{
	bare_engine();
	clocks.stuck_off = true;
	CHECK(geni_i2c_open(&eng, 400000) != NULL, "clocks that stay off: refused");
	CHECK(gcc_regs[VOTE_REG / 4] == FOUND_VOTE, "votes withdrawn: %#x", gcc_regs[VOTE_REG / 4]);
	CHECK(engine.reads == 0 && engine.writes == 0, "no access to the engine: %u, %u", engine.reads, engine.writes);
}

static void
refuses_the_wrong_clock(void)
{
	bare_engine();
	gcc_regs[(SE_CLK + 0x20 + 4 * 2) / 4] = 1u << 8 | 0x1f;         /* no preset gives 19.2 MHz */
	CHECK(geni_i2c_open(&eng, 400000) != NULL, "no 19.2 MHz: refused");
	CHECK(clocks.vote_writes == 0 && engine.reads == 0, "nothing voted, nothing read: %u", clocks.vote_writes);
	/* ... the presets off: the generator itself is looked at (the crystal, undivided is 19.2) */
	gcc_regs[(SE_CLK + 0x18) / 4] = 0;
	gcc_regs[(SE_CLK + 0x08) / 4] = 0;
	CHECK(geni_i2c_open(&eng, 400000) == NULL && se_regs[0x07c / 4] == 0, "no presets, the crystal: level 0");
	geni_i2c_close(&eng);
	gcc_regs[(SE_CLK + 0x08) / 4] = 1u << 8 | 0x1f;
	CHECK(geni_i2c_open(&eng, 400000) != NULL, "no presets, GPLL0: refused");
}

static unsigned
messages(void)
{
	return engine.count;
}

static void
open_engine(void)
{
	bare_engine();
	CHECK(geni_i2c_open(&eng, 400000) == NULL, "ready");
	engine.count = 0;
}

static void
write_goes_out(void)
{
	const uint8_t out[7] = { 0x41, 0x00, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5 };

	open_engine();
	CHECK(geni_i2c_transfer(&eng, 0x5d, out, sizeof(out), NULL, 0), "written");
	CHECK(messages() == 1 && engine.msgs[0].opcode == 1 && engine.msgs[0].address == 0x5d && engine.msgs[0].length == 7 &&
	    !(engine.msgs[0].params & 4), "one write command to 0x5d of 7 bytes with a stop at its end: %u messages", messages());
	CHECK(memcmp(engine.out, out, 7) == 0, "the bytes in order");
	CHECK(ptr == 0x4100 + 5 && clears == 0, "the chip took the register and five bytes: %#x", ptr);
	CHECK(engine.status == 0, "the status cleared: %#x", engine.status);
}

static void
write_then_read(void)
{
	uint8_t in[5] = { 0 };
	const uint8_t at[2] = { 0x00, 0x10 };

	open_engine();
	for (unsigned i = 0; i < 0x20; i++) {
		scratch[i] = (uint8_t)(0xc0 + i);
	}
	CHECK(geni_i2c_transfer(&eng, 0x5d, at, 2, in, sizeof(in)), "written and read");
	CHECK(messages() == 2 && engine.msgs[0].opcode == 1 && (engine.msgs[0].params & 4) && engine.msgs[1].opcode == 2 &&
	    !(engine.msgs[1].params & 4) && engine.msgs[1].length == 5 && engine.msgs[1].address == 0x5d,
	    "a write with no stop and then a read of 5: a repeated start: %u messages", messages());
	CHECK(in[0] == 0xd0 && in[1] == 0xd1 && in[2] == 0xd2 && in[3] == 0xd3 && in[4] == 0xd4, "the bytes read, in order: %02x %02x %02x %02x %02x",
	    in[0], in[1], in[2], in[3], in[4]);
	CHECK(geni_i2c_transfer(&eng, 0x5d, NULL, 0, in, 2) && in[0] == 0xd5 && messages() == 3, "a read alone goes on from where the chip was");
}

static void
odd_lengths(void)
{
	const uint8_t at[2] = { 0x00, 0x00 };

	open_engine();
	for (unsigned i = 0; i < 0x80; i++) {
		scratch[i] = (uint8_t)(i + 1);
	}
	for (unsigned length = 1; length <= 61; length += (length < 9 ? 1 : 13)) {
		uint8_t in[64] = { 0 };

		CHECK(geni_i2c_transfer(&eng, 0x5d, at, 2, in, length), "%u bytes read", length);
		for (unsigned i = 0; i < length; i++) {
			CHECK(in[i] == i + 1, "byte %u of %u is %u", i, length, in[i]);
		}
		CHECK(in[length] == 0, "no byte past them (%u)", length);
	}
}

static void
more_than_the_fifo_holds(void)
{
	uint8_t out[130];

	open_engine();
	out[0] = 0;
	out[1] = 0x20;
	for (unsigned i = 2; i < sizeof(out); i++) {
		out[i] = (uint8_t)i;
	}
	CHECK(geni_i2c_transfer(&eng, 0x5d, out, 66, NULL, 0), "66 bytes: 17 words of a 16 word FIFO");
	CHECK(engine.most_tx_words <= DEPTH, "never more than the FIFO holds: %u of %u", engine.most_tx_words, DEPTH);
	CHECK(engine.got == 66 && memcmp(engine.out, out, 66) == 0, "all of it, in order: %u", engine.got);
	CHECK(scratch[0x20 + 63] == 65 && other_writes == 64, "the chip has 64 registers written: %u", other_writes);
}

static void
nack_is_recovered(void)
{
	uint8_t in[2];
	const uint8_t at[2] = { 0, 0 };

	open_engine();
	CHECK(!geni_i2c_transfer(&eng, 0x38, at, 2, in, 2), "nobody at 0x38");
	CHECK(geni_i2c_nacked(&eng), "said to be a NACK (%#x)", eng.status);
	CHECK(engine.cancels == 1 && engine.aborts == 0 && !engine.running, "the command cancelled, not aborted: %u, %u", engine.cancels, engine.aborts);
	CHECK(engine.status == 0 && engine.rx_head == engine.rx_tail, "the status cleared, the FIFO empty");
	CHECK(messages() == 1, "the read was not tried after the write failed: %u", messages());
	CHECK(geni_i2c_transfer(&eng, 0x5d, at, 2, in, 2), "and the next one goes through");
	CHECK(!geni_i2c_nacked(&eng), "not a NACK now");
	chip_present = false;
	CHECK(!geni_i2c_transfer(&eng, 0x5d, NULL, 0, in, 2) && geni_i2c_nacked(&eng), "a read from nobody: NACK too");
}

static void
deaf_engine_is_aborted(void)
{
	uint8_t in[2];

	open_engine();
	chip_present = false;
	engine.deaf = true;
	CHECK(!geni_i2c_transfer(&eng, 0x5d, NULL, 0, in, 2), "a failed read");
	CHECK(engine.cancels == 1 && engine.aborts == 1, "a cancel that was not taken, then an abort: %u, %u", engine.cancels, engine.aborts);
}

static void
abort_ends_a_running_command(void)
{
	uint8_t in[2];

	open_engine();
	engine.stall = true;
	engine.running = false;
	CHECK(!geni_i2c_transfer(&eng, 0x5d, NULL, 0, in, 2), "a read that never ends is given up (100 ms)");
	engine.running = true;
	geni_i2c_abort(&eng);
	CHECK(!engine.running && engine.cancels >= 2, "an abort at the run's end cancels a running command: %u", engine.cancels);
	engine.cancels = 0;
	geni_i2c_abort(&eng);
	CHECK(engine.cancels == 0, "and does nothing to an idle one");
	geni_i2c_close(&eng);
	geni_i2c_abort(&eng);
	CHECK(engine.cancels == 0, "nor to one given back");
}

static void
look_writes_nothing(void)
{
	bare_engine();
	CHECK(!geni_i2c_look(&eng), "clocks off: not known to be I2C");
	CHECK(engine.reads == 0 && engine.gated == 0, "the engine's registers not read while it is gated: %u", engine.reads);
	CHECK(clocks.writes == 0 && engine.writes == 0, "nothing written, nothing voted");
	CHECK(logged("its clocks are off: its registers are not read"), "said: %s", log_text);
	gcc_regs[VOTE_REG / 4] |= all_votes;
	CHECK(geni_i2c_look(&eng), "clocks on, I2C: known to be");
	CHECK(logged("protocol 3 (I2C), firmware revision 0x21, FIFO interface enabled, FIFO 16 words"), "said: %s", log_text);
	CHECK(logged("source for 19.2 MHz: 2"), "which preset: %s", log_text);
	CHECK(clocks.writes == 0 && engine.writes == 0, "still nothing written: %u, %u", clocks.writes, engine.writes);
	engine.protocol = 1;
	CHECK(!geni_i2c_look(&eng) && logged("protocol 1 (SPI)"), "SPI: said so, and not I2C");
}

/* ---- scenarios: the driver ---------------------------------------------------------------------------------------------- */

#define BUS_AS_FOUND    (1u << 2 | 3u)
#define RESET_AS_FOUND  (1u << 9 | 2u << 6)

static void
board_phone(void)
{
	phone();
	board = "phone-gt9889-geni";
	fake_delay_divisor = 5;
	tlmm_model_at(hakios_board()->pins.tlmm);
	model.cfg[6] = model.cfg[7] = BUS_AS_FOUND;
	model.cfg[9] = 1;
	model.cfg[8] = RESET_AS_FOUND;
	model.cfg[5] = 1;
	nacks = 0;
}

static void
finger_down(unsigned x, unsigned y)
{
	uint8_t sum = 0;

	memset(rep, 0, sizeof(rep));
	rep[0] = 0x80;
	rep[1] = 1;
	rep[2] = 0;
	rep[3] = (uint8_t)x;
	rep[4] = (uint8_t)(x >> 8);
	rep[5] = (uint8_t)y;
	rep[6] = (uint8_t)(y >> 8);
	rep[7] = 0x20;
	rep[9] = 0x10;
	for (unsigned i = 0; i < 11; i++) {
		sum = (uint8_t)(sum + rep[i]);
	}
	rep[11] = (uint8_t)-sum;
	pending = true;
	model.input[9] = false;
}

static void
start(const char *arguments)
{
	boot_args = arguments;
	hakios_gt9889_init();
}

static bool
posted_at(uint32_t x, uint32_t y)
{
	bool found = false;

	pthread_mutex_lock(&post_lock);
	for (unsigned i = 0; i < posts && i < 512; i++) {
		found |= posted[i].x == x && posted[i].y == y;
	}
	pthread_mutex_unlock(&post_lock);
	return found;
}

static void
pins_not_touched(const char *when)
{
	CHECK(tlmm_pin_writes[6] == 0 && tlmm_pin_writes[7] == 0, "%s: the bus's pins left to the engine's function: %u, %u writes", when,
	    tlmm_pin_writes[6], tlmm_pin_writes[7]);
	CHECK(model.cfg[6] == BUS_AS_FOUND && model.cfg[7] == BUS_AS_FOUND, "%s: the bus's pins as found", when);
}

/* touchscreen=1 on a board that names the engine: the report read through it, the engine and the votes given back at the end */
static void
driver_uses_the_engine(void)
{
	board_phone();
	finger_down(100, 200);
	start("touchscreen=1");
	WAIT_FOR(logged("touch screen ready") || logged("touch screen unavailable"), 10000);
	CHECK(logged("touch screen ready"), "the chip brought up: %s", log_text);
	CHECK(logged("gt9889: the bus is the serial engine at 0xa84000: I2C at 400 kHz, FIFO of 16 words"), "said: %s", log_text);
	CHECK(!logged("the pins by hand"), "not by hand: %s", log_text);
	CHECK(logged("gt9889: the chip at 0x5d answers: its report block (0x4100) reads 80 01 00 64 00 c8 00 20 00 10"), "the block: %s", log_text);
	WAIT_FOR(posted_at(100u * 65535 / 1079, 200u * 65535 / 2339) && clears > 0, 3000);
	CHECK(posted_at(100u * 65535 / 1079, 200u * 65535 / 2339), "the finger posted from what the engine read");
	CHECK(clears >= 1 && other_writes == 0, "the event byte written 0 and nothing else: %u, %u", clears, other_writes);
	CHECK((gcc_regs[VOTE_REG / 4] & all_votes) == all_votes, "the votes held while it runs: %#x", gcc_regs[VOTE_REG / 4]);
	CHECK(engine.gated == 0, "no access to the engine with its clocks off: %u", engine.gated);
	CHECK(engine.msgs[0].opcode == 1 && engine.msgs[0].length == 2 && (engine.msgs[0].params & 4) && engine.msgs[0].address == 0x5d,
	    "the first message is the register's two bytes, no stop");
	pins_not_touched("running");
	hakios_gt9889_quiesce();
	CHECK(gcc_regs[VOTE_REG / 4] == FOUND_VOTE, "at the end the votes withdrawn: %#x", gcc_regs[VOTE_REG / 4]);
	CHECK(kept_changed() == 0, "and the engine's registers as found: %u differ", kept_changed());
	CHECK(logged("powered off"), "said: %s", log_text);
	pins_not_touched("at the end");
}

/* an engine that is not I2C: said, the engine and the votes as found, the pins driven by hand (and nobody there to answer) */
static void
driver_falls_back_on_spi(void)
{
	board_phone();
	engine.protocol = 1;
	start("touchscreen=1");
	WAIT_FOR(logged("touch screen unavailable") || logged("nobody answers") || logged("nothing read at"), 10000);
	CHECK(logged("gt9889: the serial engine at 0xa84000 is not used: the serial engine has no I2C firmware (its protocol is not 3): the pins by hand"),
	    "said: %s", log_text);
	CHECK(!logged("the bus is the serial engine"), "not said to be the bus");
	CHECK(engine.writes == 0 && kept_changed() == 0 && gcc_regs[VOTE_REG / 4] == FOUND_VOTE, "the engine and the votes as found");
	CHECK(logged("nothing read at") || logged("nobody answers"), "and the pins were driven by hand: %s", log_text);
	hakios_gt9889_quiesce();
	CHECK(engine.writes == 0 && gcc_regs[VOTE_REG / 4] == FOUND_VOTE, "still as found at the end");
}

/* a clock that is not 19.2 MHz: the same */
static void
driver_falls_back_on_the_clock(void)
{
	board_phone();
	gcc_regs[(SE_CLK + 0x20 + 4 * 2) / 4] = 1u << 8 | 0x1f;
	start("touchscreen=1");
	WAIT_FOR(logged("the pins by hand"), 10000);
	CHECK(logged("the engine's clock gives no 19.2 MHz"), "said: %s", log_text);
	CHECK(clocks.vote_writes == 0 && engine.reads == 0, "nothing voted, the engine not read");
	hakios_gt9889_quiesce();
}

/* touchscreen=-1: the engine's state said, nothing written to the engine or the clock controller */
static void
driver_look(void)
{
	board_phone();
	gcc_regs[VOTE_REG / 4] |= all_votes;
	start("touchscreen=-1");
	WAIT_FOR(logged("nothing powered, configured or written"), 5000);
	CHECK(logged("gt9889: the board names the serial engine at 0xa84000 as the bus (looked at only"), "said: %s", log_text);
	CHECK(logged("serial engine: protocol 3 (I2C)"), "the protocol: %s", log_text);
	CHECK(logged("gt9889: the engine's firmware is I2C: touchscreen=1 would use it"), "the verdict: %s", log_text);
	CHECK(engine.writes == 0 && clocks.writes == 0, "nothing written to the engine (%u) or the clock controller (%u)", engine.writes, clocks.writes);
	CHECK(asks == 0 && !logged("step 2"), "nothing powered");
	hakios_gt9889_quiesce();
	CHECK(engine.writes == 0 && clocks.writes == 0, "nor at the run's end");
}

/* the same with the engine's clocks off, as the firmware most likely leaves them: its registers are not read */
static void
driver_look_with_clocks_off(void)
{
	board_phone();
	start("touchscreen=-1");
	WAIT_FOR(logged("nothing powered, configured or written"), 5000);
	CHECK(logged("its clocks are off: its registers are not read"), "said: %s", log_text);
	CHECK(logged("the engine is not known to be I2C"), "said: %s", log_text);
	CHECK(engine.reads == 0 && engine.writes == 0 && engine.gated == 0 && clocks.writes == 0, "the engine not touched at all");
	hakios_gt9889_quiesce();
}

/* the run ends while a transfer never finishes: the engine's command ended, the votes withdrawn, the registers as found */
static void
run_ends_in_a_stalled_transfer(void)
{
	board_phone();
	finger_down(100, 200);
	start("touchscreen=1");
	WAIT_FOR(logged("touch screen ready"), 10000);
	CHECK(logged("touch screen ready"), "up: %s", log_text);
	freeze_on_command = true;               /* (the thread stops at its next command, as a core stopped by the run's end would) */
	WAIT_FOR(frozen, 3000);
	CHECK(frozen && engine.running, "a command is running that the thread never sees the end of");
	hakios_gt9889_quiesce();
	CHECK(logged("a transfer cut off: the serial engine's command ended"), "said: %s", log_text);
	CHECK(engine.cancels >= 1, "cancelled: %u", engine.cancels);
	CHECK(gcc_regs[VOTE_REG / 4] == FOUND_VOTE, "the votes withdrawn: %#x", gcc_regs[VOTE_REG / 4]);
	CHECK(kept_changed() == 0, "the registers as found: %u differ", kept_changed());
	pins_not_touched("at the end");
}

/* nobody at the address: the NACK said as nobody answering, as by hand */
static void
driver_nobody_answers(void)
{
	board_phone();
	chip_present = false;
	start("touchscreen=1");
	WAIT_FOR(logged("touch screen unavailable"), 10000);
	CHECK(logged("gt9889: nobody answers at 0x5d"), "said: %s", log_text);
	CHECK(nacks >= 1, "the engine said NACK: %u", nacks);
	CHECK(gcc_regs[VOTE_REG / 4] == FOUND_VOTE && kept_changed() == 0, "powered off: the votes withdrawn, the registers as found");
	pins_not_touched("nobody");
	hakios_gt9889_quiesce();
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "sets_up", sets_up }, { "votes_found_stay", votes_found_stay }, { "refuses_spi", refuses_spi },
		{ "refuses_gsi_and_no_fifo", refuses_gsi_and_no_fifo }, { "refuses_clocks_that_stay_off", refuses_clocks_that_stay_off },
		{ "refuses_the_wrong_clock", refuses_the_wrong_clock }, { "write_goes_out", write_goes_out },
		{ "write_then_read", write_then_read }, { "odd_lengths", odd_lengths }, { "more_than_the_fifo_holds", more_than_the_fifo_holds },
		{ "nack_is_recovered", nack_is_recovered }, { "deaf_engine_is_aborted", deaf_engine_is_aborted },
		{ "abort_ends_a_running_command", abort_ends_a_running_command }, { "look_writes_nothing", look_writes_nothing },
		{ "driver_uses_the_engine", driver_uses_the_engine }, { "driver_falls_back_on_spi", driver_falls_back_on_spi },
		{ "driver_falls_back_on_the_clock", driver_falls_back_on_the_clock }, { "driver_look", driver_look },
		{ "driver_look_with_clocks_off", driver_look_with_clocks_off },
		{ "run_ends_in_a_stalled_transfer", run_ends_in_a_stalled_transfer }, { "driver_nobody_answers", driver_nobody_answers },
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
			fprintf(stderr, "%-34s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
