/*
 * A model of the SoC's pin controller (TLMM): a configuration and a level register for each of 256
 * pins, where a test says they are. For the tests of what drives pins: a write outside a pin's two
 * registers is counted, and so is a pin made an output before anybody said which level it is to have.
 */
#ifndef TLMM_MODEL_H
#define TLMM_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#define PINS            256
static struct {
	uint32_t base;
	unsigned pins;                  /* how many there are (a tiled controller has fewer than 256) */
	uint32_t at[PINS];              /* where a pin's registers are from base: 0x1000 n, or its tile's offset more */
	uint32_t cfg[PINS], io[PINS];
	bool input[PINS];               /* what is outside on the pin */
	unsigned reads, writes, elsewhere;
	unsigned drove_wrong;           /* a pin became an output while its level was not the one asked for last */
	int wanted[PINS];               /* -1: nothing asked yet */
	void (*after_write)(void);      /* for a test with something on the pins: called when a pin's registers changed */
} model;

/* the pin and register an address is, if it is the configuration or the level of one of the pins */
static inline bool
tlmm_model_decode(vm_offset_t address, uint32_t *pin, uint32_t *reg)
{
	uint32_t offset = (uint32_t)(address - model.base);

	if (address < model.base) {
		return false;
	}
	for (uint32_t n = 0; n < model.pins; n++) {
		if (offset >= model.at[n] && offset - model.at[n] < 0x1000) {
			*pin = n;
			*reg = offset - model.at[n];
			return *reg == 0 || *reg == 4;
		}
	}
	return false;
}

static inline uint32_t
tlmm_model_rd(vm_offset_t address)
{
	uint32_t pin, reg;

	if (!tlmm_model_decode(address, &pin, &reg)) {
		model.elsewhere++;
		return 0;
	}
	model.reads++;
	return reg == 0 ? model.cfg[pin] : (model.io[pin] & 2) | (model.input[pin] ? 1 : 0);
}

static inline void
tlmm_model_wr(vm_offset_t address, uint32_t value)
{
	uint32_t pin, reg;

	if (!tlmm_model_decode(address, &pin, &reg)) {
		model.elsewhere++;
		return;
	}
	model.writes++;
	if (reg == 4) {
		model.io[pin] = value;
		model.wanted[pin] = (value & 2) ? 1 : 0;
	} else {
		if ((value & 1u << 9) && !(model.cfg[pin] & 1u << 9) && model.wanted[pin] < 0) {
			model.drove_wrong++;    /* an output before anybody said which level */
		}
		model.cfg[pin] = value;
	}
	if (model.after_write != NULL) {
		model.after_write();
	}
}

/* the pins at that address, nothing asked of any yet */
static inline void
tlmm_model_at(uint32_t base)
{
	model.base = base;
	model.pins = PINS;
	for (unsigned i = 0; i < PINS; i++) {
		model.at[i] = 0x1000 * i;
		model.wanted[i] = -1;
	}
}

/* a tiled controller: pin n is in tile tile_of[n], whose registers start at offset[tile_of[n]] from base, 0x1000 n further */
static inline void
tlmm_model_tiled(uint32_t base, const uint32_t *offset, const uint8_t *tile_of, unsigned pins)
{
	tlmm_model_at(base);
	model.pins = pins;
	for (unsigned i = 0; i < pins; i++) {
		model.at[i] = offset[tile_of[i]] + 0x1000 * i;
	}
}

#endif /* TLMM_MODEL_H */
