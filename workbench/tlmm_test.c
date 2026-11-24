/*
 * The SoC's pins as the part drivers share them (drivers/board/tlmm.h), against a
 * model of the pin controller: what is written into a pin's configuration, that an output has its
 * level before it drives, and that only the pin asked for is touched.
 *
 *   tlmm_test SCENARIO
 */
#include "fakekernel.h"

#include "tlmm_model.h"
#include "board/board.h"

/* the board tlmm.h asks for the pins' layout: none (as a kernel that does not know the board), or one a test sets up */
static const struct hakios_board *the_board;
const struct hakios_board *hakios_board(void) { return the_board; }

static uint32_t rd(vm_offset_t address) { return tlmm_model_rd(address); }
static void wr(vm_offset_t address, uint32_t value) { tlmm_model_wr(address, value); }

#define KERNEL 1
static vm_offset_t tlmm;
#define HAKIOS_TLMM tlmm
#include "board/tlmm.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static void
pins_at(uint32_t base)
{
	tlmm_model_at(base);
	tlmm = base;
}

static unsigned
others_touched(unsigned pin)
{
	unsigned touched = 0;

	for (unsigned i = 0; i < PINS; i++) {
		touched += i != pin && (model.cfg[i] != 0 || model.io[i] != 0);
	}
	return touched;
}

/* a pin handed to one of the SoC's own functions: pull, function, drive strength, not an output */
static void
a_function(void)
{
	pins_at(0x0f100000);
	gpio_configure(52, 1, PULL_NONE, 6, false);
	CHECK(model.cfg[52] == (1u << 2 | 2u << 6), "function 1, 6 mA: %#x", model.cfg[52]);
	gpio_configure(81, 0, PULL_UP, 8, false);
	CHECK(model.cfg[81] == (3u | 3u << 6), "a GPIO pulled up, 8 mA: %#x", model.cfg[81]);
	gpio_configure(7, 3, PULL_DOWN, 2, false);
	CHECK(model.cfg[7] == (1u | 3u << 2), "function 3 pulled down, 2 mA: %#x", model.cfg[7]);
	gpio_configure(255, 0, PULL_KEEPER, 16, false);
	CHECK(model.cfg[255] == (2u | 7u << 6), "the last pin, a keeper, 16 mA: %#x", model.cfg[255]);
	CHECK(model.writes == 4 && model.elsewhere == 0, "four writes, each to its pin (%u, %u elsewhere)", model.writes, model.elsewhere);
}

/* an output: its level is set before it drives, and stays what was asked */
static void
an_output(void)
{
	pins_at(0x0f100000);
	gpio_output(105, false, PULL_UP, 8);
	CHECK(model.cfg[105] == (3u | 3u << 6 | 1u << 9) && (model.io[105] & 2) == 0, "low, an output: %#x, %#x", model.cfg[105], model.io[105]);
	CHECK(model.drove_wrong == 0, "the level was there before the pin drove");
	gpio_set(105, true);
	CHECK((model.io[105] & 2) != 0, "then high");
	gpio_output(107, true, PULL_NONE, 2);
	CHECK((model.io[107] & 2) != 0 && (model.cfg[107] & 1u << 9), "another, high from the start");
	CHECK(others_touched(105) == 1 && model.elsewhere == 0, "only those two pins");
}

/* an input: what is outside, whatever was last written to its level */
static void
an_input(void)
{
	pins_at(0x0f100000);
	gpio_configure(81, 0, PULL_UP, 8, false);
	model.input[81] = true;
	CHECK(gpio_get(81), "high outside");
	model.input[81] = false;
	CHECK(!gpio_get(81), "low outside");
	gpio_set(81, true);
	CHECK(!gpio_get(81), "the level written is not what is read");
	CHECK(others_touched(81) == 0 && model.elsewhere == 0, "only that pin");
}

/* a chip whose pins are elsewhere */
static void
another_soc(void)
{
	pins_at(0x01000000);
	gpio_output(3, true, PULL_NONE, 2);
	CHECK((model.io[3] & 2) && (model.cfg[3] & 1u << 9) && model.elsewhere == 0, "pin 3 of the pins at 0x01000000");
	CHECK(GPIO_CFG(3) == 0x01003000 && GPIO_IO(3) == 0x01003004, "its two registers: %#lx, %#lx", (unsigned long)GPIO_CFG(3),
	    (unsigned long)GPIO_IO(3));
}

/* SM7150's TLMM (the vendor's pinctrl-sdmmagpie.c): three tiles, GPIO 0 in the south one, 4 in the north, 31 in the west */
static struct hakios_board sm7150;
static const uint32_t sm7150_tiles[3] = { 0x100000, 0x500000, 0x900000 };
static const uint8_t sm7150_tile_of[119] = {
	2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1,
	2, 0, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
	1, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

static void
tiled_board(void)
{
	sm7150.pins.tlmm = 0x03400000;
	memcpy(sm7150.pins.tile, sm7150_tiles, sizeof(sm7150_tiles));
	sm7150.pins.tiles = 3;
	sm7150.pins.gpios = 119;
	memcpy(sm7150.pins.tile_of, sm7150_tile_of, sizeof(sm7150_tile_of));
	the_board = &sm7150;
	tlmm_model_tiled(0x03400000, sm7150_tiles, sm7150_tile_of, 119);
	tlmm = 0x03400000;
}

/* a pin's registers are in its tile: the addresses the vendor's driver has, and the model (which has its own sum) agrees */
static void
tiled_addresses(void)
{
	tiled_board();
	CHECK(GPIO_CFG(0) == 0x03d00000 && GPIO_IO(0) == 0x03d00004, "GPIO 0 (south): %#lx", (unsigned long)GPIO_CFG(0));
	CHECK(GPIO_CFG(4) == 0x03904000, "GPIO 4 (north): %#lx", (unsigned long)GPIO_CFG(4));
	CHECK(GPIO_CFG(31) == 0x0351f000, "GPIO 31 (west): %#lx", (unsigned long)GPIO_CFG(31));
	CHECK(GPIO_CFG(118) == 0x03976000, "GPIO 118 (north): %#lx", (unsigned long)GPIO_CFG(118));
	CHECK(TLMM_PINS == 119, "119 pins: %u", TLMM_PINS);
	for (unsigned n = 0; n < 119; n++) {
		gpio_output(n, n & 1, PULL_NONE, 2);
		CHECK(model.cfg[n] & 1u << 9 && ((model.io[n] & 2) != 0) == (n & 1), "GPIO %u driven where the model has it", n);
	}
	CHECK(model.elsewhere == 0 && model.writes == 2 * 119, "each write in a pin's registers (%u elsewhere)", model.elsewhere);
}

/* the mapping covers the last pin's registers and no pin is outside it */
static void
tiled_span(void)
{
	uint32_t span;

	tiled_board();
	span = TLMM_SIZE;
	for (unsigned n = 0; n < 119; n++) {
		CHECK(hakios_tlmm_offset(the_board, n) + 0x1000 <= span, "GPIO %u inside what is mapped", n);
	}
	CHECK(span == 0x96e000, "the span ends with the last pin's registers (the south tile's GPIO 109): %#x", span);
	/* a GPIO the controller has not got is nobody else's registers */
	CHECK(hakios_tlmm_offset(the_board, 119) == 0 && hakios_tlmm_offset(the_board, 255) == 0, "no pin, no address of one");
	the_board = NULL;
	CHECK(TLMM_SIZE == 0x100000 && TLMM_PINS == 256, "a board that is not known has the plain 256 pins");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "a_function", a_function }, { "an_output", an_output }, { "an_input", an_input }, { "another_soc", another_soc },
		{ "tiled_addresses", tiled_addresses }, { "tiled_span", tiled_span },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			fprintf(stderr, "%-20s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
