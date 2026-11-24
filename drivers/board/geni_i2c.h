/*
 * I2C through a serial engine (Qualcomm's GENI, in a QUP wrapper) in its FIFO mode: the engine's registers, its
 * clock votes, setting it up as an I2C master at a rate, one transfer (a write, or a write and then a read after a
 * repeated start), and getting it going again after one went wrong. What a part's driver uses in place of
 * board/i2c_gpio.h when the board says which engine its bus is. No DMA and no GSI: a message goes through the FIFO
 * a word at a time and is waited for by looking at the engine's status, never by its interrupt.
 *
 * The engine's firmware is NOT loaded here. Which protocol a serial engine speaks is in a firmware image the boot
 * firmware (XBL/TrustZone) loads into the QUP wrapper before this kernel runs; the vendor's kernel for the same phone
 * (drivers/i2c/busses/i2c-qcom-geni.c, "Invalid proto") only reads it and refuses an engine that is not I2C, and so does
 * this: geni_i2c_open() says why it cannot be used and the driver goes on by hand. Whether the phone's engine is I2C at
 * this point is NOT KNOWN from the files alone; geni_i2c_look() reads it (the protocol is the field [15:8] of
 * GENI_FW_REVISION_RO: 1 SPI, 2 UART, 3 I2C), writing nothing.
 *
 * What it takes of the engine as found, and gives back: the clock votes it had to add (the wrapper's two bus clocks
 * and its core clocks, the engine's own), and the engine's registers it sets (listed in geni_i2c_kept), by
 * geni_i2c_close(). The engine's clock is not reprogrammed: the rate generator must already give 19.2 MHz in one of
 * its presets (or undivided from the crystal), which the I2C counters of geni_i2c_rates are for.
 *
 * The file that includes this one has defined first: rd(), wr() (board/mmio.h), sleep_us() or GENI_I2C_WAIT_US (a wait
 * that may be a spin), now_us() (board/time.h) and GENI_I2C_SAY(), a line to the log.
 * Register layout and protocol: Linux drivers/i2c/busses/i2c-qcom-geni.c and drivers/soc/qcom/qcom-geni-se.c; the
 * vendor's i2c-qcom-geni.c of the sm6150 tree (the clock table, the command's parameters, which status bits are errors).
 */
#ifndef HAKIOS_GENI_I2C_H
#define HAKIOS_GENI_I2C_H

#include "geni_clk.h"

#ifndef GENI_I2C_WAIT_US
#define GENI_I2C_WAIT_US(us)    sleep_us(us)
#endif

#define GENI_FORCE_DEFAULT      0x020
#define GENI_OUTPUT_CTRL        0x024
#define GENI_CGC_CTRL           0x028
#define GENI_STATUS             0x040   /* bit 0: a command is running */
#define GENI_SER_M_CLK_CFG      0x048   /* bit 0 enable, [15:4] divider */
#define GENI_IF_DISABLE_RO      0x064   /* bit 0: no FIFO interface (the engine is for the GSI) */
#define GENI_FW_REVISION_RO     0x068   /* [15:8] protocol: 1 = SPI, 2 = UART, 3 = I2C */
#define GENI_CLK_SEL            0x07c
#define GENI_BYTE_GRAN          0x254
#define GENI_DMA_MODE_EN        0x258
#define GENI_TX_PACKING_CFG0    0x260
#define GENI_TX_PACKING_CFG1    0x264
#define I2C_TX_TRANS_LEN        0x26c
#define I2C_RX_TRANS_LEN        0x270
#define I2C_SCL_COUNTERS        0x278   /* [29:20] high, [19:10] low, [9:0] cycle */
#define GENI_RX_PACKING_CFG0    0x284
#define GENI_RX_PACKING_CFG1    0x288
#define GENI_M_CMD0             0x600   /* [31:27] operation, [26:0] parameters */
#define GENI_M_CMD_CTRL         0x604   /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       0x610
#define GENI_M_IRQ_EN           0x614
#define GENI_M_IRQ_CLEAR        0x618
#define GENI_S_IRQ_CLEAR        0x648
#define GENI_TX_FIFO            0x700
#define GENI_RX_FIFO            0x780
#define GENI_TX_FIFO_STATUS     0x800   /* [27:0] words */
#define GENI_RX_FIFO_STATUS     0x804   /* bit 31 last, [30:28] bytes of the last word, [24:0] words */
#define GENI_TX_WATERMARK       0x80c
#define GENI_RX_WATERMARK       0x810
#define GENI_RX_RFR_WATERMARK   0x814
#define SE_DMA_TX_IRQ_CLR       0xc44
#define SE_DMA_RX_IRQ_CLR       0xd44
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c
#define SE_HW_PARAM_0           0xe24   /* [21:16] FIFO depth in words */
#define SE_DMA_GENERAL_CFG      0xe30

#define GENI_PROTOCOL_SPI       1
#define GENI_PROTOCOL_I2C       3

#define GENI_I2C_WRITE             1u      /* the operations of the I2C protocol: i2c-qcom-geni.c's I2C_WRITE and I2C_READ */
#define GENI_I2C_READ              2u
#define GENI_I2C_STOP_STRETCH        (1u << 2)       /* no stop after this message: a repeated start follows */
#define GENI_I2C_ADDRESS_SHIFT       9

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define GENI_I2C_NACK                (1u << 10)      /* general-purpose interrupt 1 */
/* overrun, illegal command, failure; NACK, bus protocol (3) and arbitration lost (4); the FIFOs' read and write errors */
#define GENI_I2C_ERRORS              ((1u << 1) | (1u << 2) | (1u << 3) | GENI_I2C_NACK | (1u << 12) | (1u << 13) | (1u << 24) | \
	                         (1u << 25) | (1u << 28) | (1u << 29))

#define GENI_CLOCK_OFF          (1u << 31)

/* the counters for 19.2 MHz into the engine, i2c-qcom-geni.c's geni_i2c_clk_map: SCL's rate, divider, high, low, cycle */
static const struct { uint32_t hz; uint32_t divider, high, low, cycle; } geni_i2c_rates[] = {
	{ 100000, 7, 10, 11, 26 }, { 400000, 2, 5, 12, 24 }, { 1000000, 1, 3, 9, 18 },
};

/* the engine's registers that setting it up changes: given back as found */
static const uint16_t geni_i2c_kept[] = {
	GENI_CGC_CTRL, SE_DMA_GENERAL_CFG, GENI_OUTPUT_CTRL, GENI_DMA_MODE_EN, GENI_RX_WATERMARK, GENI_RX_RFR_WATERMARK,
	GENI_TX_WATERMARK, SE_IRQ_EN, GENI_M_IRQ_EN, SE_GSI_EVENT_EN, GENI_TX_PACKING_CFG0, GENI_TX_PACKING_CFG1,
	GENI_RX_PACKING_CFG0, GENI_RX_PACKING_CFG1, GENI_BYTE_GRAN, GENI_CLK_SEL, GENI_SER_M_CLK_CFG, I2C_SCL_COUNTERS,
};
#define GENI_I2C_KEPT           (sizeof(geni_i2c_kept) / sizeof(geni_i2c_kept[0]))

struct geni_i2c {
	vm_offset_t se, gcc;            /* the engine's registers, and the clock controller's, mapped */
	uint32_t m_ahb, s_ahb, se_clock, vote_register;         /* offsets in the clock controller: the board's */
	uint8_t vote[5];                /* the bits of that register for: core 2x, core, the two bus clocks, the engine */
	uint32_t votes_added;           /* the votes that were not taken when found: withdrawn by geni_i2c_close() */
	bool engine_set;                /* the registers of geni_i2c_kept were changed: found[] has them */
	uint32_t found[GENI_I2C_KEPT];
	uint32_t fifo_depth;
	uint32_t status;                /* the last message's status when it failed */
};

#define GENI_VOTE(e, n)         (1u << (e)->vote[n])

/*
 * An AHB clock in hardware gated mode (bit 1) may say it is off while its bus is idle, even with the vote taken (Linux's
 * clk-branch.c does not wait for it either).
 */
static bool
geni_i2c_ahb_ready(uint32_t cbcr)
{
	return !(cbcr & GENI_CLOCK_OFF) || (cbcr & (1u << 1));
}

static bool
geni_i2c_clocks_running(const struct geni_i2c *e)
{
	return geni_i2c_ahb_ready(rd(e->gcc + e->m_ahb)) && geni_i2c_ahb_ready(rd(e->gcc + e->s_ahb)) &&
	       !(rd(e->gcc + e->se_clock) & GENI_CLOCK_OFF);
}

static bool
geni_i2c_near_19_2(uint32_t hz)
{
	return hz >= XO_HZ / 100 * 99 && hz <= XO_HZ / 100 * 101;
}

/*
 * Which of the engine's preset clock rates is 19.2 MHz (what GENI_CLK_SEL then says), or, where the engine does not
 * choose among presets itself, whether the rate generator gives it now (level 0). -1: neither, the counters do not
 * fit. Reads of the clock controller only.
 */
static int
geni_i2c_source(const struct geni_i2c *e)
{
	vm_offset_t rcg = e->gcc + e->se_clock;         /* (the generator's registers follow the clock's, as in board/goodix.c) */

	if (rd(rcg + 0x18) & 1) {
		for (uint32_t level = 0; level < 8; level++) {
			if (geni_i2c_near_19_2(rcg_rate(rd(rcg + 0x20 + 4 * level), rd(rcg + 0x60 + 4 * level), rd(rcg + 0xa0 + 4 * level)))) {
				return (int)level;
			}
		}
		return -1;
	}
	return geni_i2c_near_19_2(rcg_rate(rd(rcg + 0x08), rd(rcg + 0x0c), rd(rcg + 0x10))) ? 0 : -1;
}

/*
 * What the engine and its clocks are now, said, and whether it is I2C. Reads of the clock controller, and of the
 * engine only while its clocks run (a read of a gated engine does not come back): nothing is written, nothing voted.
 */
static bool
geni_i2c_look(const struct geni_i2c *e)
{
	uint32_t vote = rd(e->gcc + e->vote_register), protocol;
	uint32_t wanted = GENI_VOTE(e, 0) | GENI_VOTE(e, 1) | GENI_VOTE(e, 2) | GENI_VOTE(e, 3) | GENI_VOTE(e, 4);

	GENI_I2C_SAY("serial engine at %#lx: clock votes %#x (of the %#x it needs), bus clocks %#x %#x, engine's %#x, rate generator "
	    "presets %s, source for 19.2 MHz: %d\n", (unsigned long)e->se, vote, wanted, rd(e->gcc + e->m_ahb), rd(e->gcc + e->s_ahb),
	    rd(e->gcc + e->se_clock), (rd(e->gcc + e->se_clock + 0x18) & 1) ? "on" : "off", geni_i2c_source(e));
	if (!geni_i2c_clocks_running(e)) {
		GENI_I2C_SAY("serial engine: its clocks are off: its registers are not read (the firmware did not leave them on; "
		    "whether it is I2C is for the run that votes for them)\n");
		return false;
	}
	protocol = rd(e->se + GENI_FW_REVISION_RO) >> 8 & 0xff;
	GENI_I2C_SAY("serial engine: protocol %u (%s), firmware revision %#x, FIFO interface %s, FIFO %u words, status %#x, clock "
	    "select %u, divider %#x, SCL counters %#x\n", protocol,
	    protocol == GENI_PROTOCOL_I2C ? "I2C" : protocol == GENI_PROTOCOL_SPI ? "SPI" : protocol == 2 ? "UART" : "none or other",
	    rd(e->se + GENI_FW_REVISION_RO) & 0xff, (rd(e->se + GENI_IF_DISABLE_RO) & 1) ? "disabled" : "enabled",
	    rd(e->se + SE_HW_PARAM_0) >> 16 & 0x3f, rd(e->se + GENI_STATUS), rd(e->se + GENI_CLK_SEL) & 7,
	    rd(e->se + GENI_SER_M_CLK_CFG), rd(e->se + I2C_SCL_COUNTERS));
	return protocol == GENI_PROTOCOL_I2C;
}

/* The votes this added withdrawn, and the engine's registers put back as found. Safe to call again. */
static void
geni_i2c_close(struct geni_i2c *e)
{
	if (e->engine_set) {
		for (unsigned i = 0; i < GENI_I2C_KEPT; i++) {
			wr(e->se + geni_i2c_kept[i], e->found[i]);
		}
		e->engine_set = false;
	}
	if (e->votes_added != 0) {
		wr(e->gcc + e->vote_register, rd(e->gcc + e->vote_register) & ~e->votes_added);
		e->votes_added = 0;
	}
}

static void
geni_i2c_vote(struct geni_i2c *e, uint32_t bits)
{
	uint32_t now = rd(e->gcc + e->vote_register);

	e->votes_added |= bits & ~now;
	wr(e->gcc + e->vote_register, now | bits);
}

/*
 * The engine taken for I2C at `hz': its clocks voted for, its protocol and FIFO read, set up. NULL if it is ready;
 * otherwise why it cannot be used, with the votes withdrawn and nothing of the engine changed (the caller goes on by
 * hand, or does without).
 */
static const char *
geni_i2c_open(struct geni_i2c *e, uint32_t hz)
{
	unsigned rate = 0;
	int source;
	uint32_t divider;

	while (rate < sizeof(geni_i2c_rates) / sizeof(geni_i2c_rates[0]) && geni_i2c_rates[rate].hz != hz) {
		rate++;
	}
	if (rate == sizeof(geni_i2c_rates) / sizeof(geni_i2c_rates[0])) {
		return "no counters are known for that rate (100 kHz, 400 kHz or 1 MHz)";
	}
	source = geni_i2c_source(e);
	if (source < 0) {
		return "the engine's clock gives no 19.2 MHz, which the I2C counters are for";
	}
	geni_i2c_vote(e, GENI_VOTE(e, 0) | GENI_VOTE(e, 1) | GENI_VOTE(e, 2) | GENI_VOTE(e, 3));
	geni_i2c_vote(e, GENI_VOTE(e, 4));
	for (int i = 0; i < 1000 && !geni_i2c_clocks_running(e); i++) {
		GENI_I2C_WAIT_US(10);
	}
	if (!geni_i2c_clocks_running(e)) {
		geni_i2c_close(e);
		return "the engine's clocks stay off with the votes taken";
	}
	if ((rd(e->se + GENI_FW_REVISION_RO) >> 8 & 0xff) != GENI_PROTOCOL_I2C) {
		geni_i2c_close(e);
		return "the serial engine has no I2C firmware (its protocol is not 3)";
	}
	if (rd(e->se + GENI_IF_DISABLE_RO) & 1) {
		geni_i2c_close(e);
		return "the serial engine's FIFO interface is disabled (it is for the GSI)";
	}
	e->fifo_depth = rd(e->se + SE_HW_PARAM_0) >> 16 & 0x3f;
	if (e->fifo_depth < 4) {
		geni_i2c_close(e);
		return "the serial engine reports no FIFO";
	}
	for (unsigned i = 0; i < GENI_I2C_KEPT; i++) {
		e->found[i] = rd(e->se + geni_i2c_kept[i]);
	}
	e->engine_set = true;
	wr(e->se + SE_GSI_EVENT_EN, 0);
	wr(e->se + GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(e->se + GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(e->se + SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(e->se + SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(e->se + GENI_CGC_CTRL, rd(e->se + GENI_CGC_CTRL) | 0x7f);
	wr(e->se + SE_DMA_GENERAL_CFG, rd(e->se + SE_DMA_GENERAL_CFG) | 0xf);
	wr(e->se + GENI_OUTPUT_CTRL, 0x7f);
	wr(e->se + GENI_FORCE_DEFAULT, 1);
	wr(e->se + GENI_DMA_MODE_EN, rd(e->se + GENI_DMA_MODE_EN) & ~1u);
	wr(e->se + GENI_RX_WATERMARK, e->fifo_depth - 3);
	wr(e->se + GENI_RX_RFR_WATERMARK, e->fifo_depth - 2);
	wr(e->se + GENI_TX_WATERMARK, 0);
	/* the status bits we poll follow these enables; the interrupt itself stays masked at the GIC */
	wr(e->se + SE_IRQ_EN, rd(e->se + SE_IRQ_EN) | 0xf);
	wr(e->se + GENI_M_IRQ_EN, rd(e->se + GENI_M_IRQ_EN) | 0x7fc0007f);
	/* eight bits, four bytes in a FIFO word, the first lowest (as the SPI's, and Linux's for I2C) */
	wr(e->se + GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(e->se + GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(e->se + GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(e->se + GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(e->se + GENI_BYTE_GRAN, 0);
	divider = geni_i2c_rates[rate].divider;
	wr(e->se + GENI_CLK_SEL, (uint32_t)source & 7);
	wr(e->se + GENI_SER_M_CLK_CFG, divider << 4 | 1);
	wr(e->se + I2C_SCL_COUNTERS, geni_i2c_rates[rate].high << 20 | geni_i2c_rates[rate].low << 10 | geni_i2c_rates[rate].cycle);
	return NULL;
}

/* a command that is running ended: cancelled, and aborted if that is not taken; the FIFOs emptied, the status cleared */
static void
geni_i2c_recover(struct geni_i2c *e)
{
	wr(e->se + GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 2000 && !(rd(e->se + GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		GENI_I2C_WAIT_US(10);
	}
	if (!(rd(e->se + GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(e->se + GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 2000 && !(rd(e->se + GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			GENI_I2C_WAIT_US(10);
		}
	}
	for (int i = 0; i < 64 && (rd(e->se + GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(e->se + GENI_RX_FIFO);
	}
	wr(e->se + GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* A transfer cut off in its middle (the run's end): ended if the engine is running one. */
static void
geni_i2c_abort(struct geni_i2c *e)
{
	if (e->engine_set && (rd(e->se + GENI_STATUS) & 1)) {
		geni_i2c_recover(e);
	}
}

static void
geni_i2c_drain(struct geni_i2c *e, uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint32_t status = rd(e->se + GENI_RX_FIFO_STATUS), words = status & 0x01ffffff, bytes = words * 4;

	if (status & 0x80000000u) {
		uint32_t valid = status >> 28 & 7;

		if (valid && valid < 4) {
			bytes -= 4 - valid;
		}
	}
	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = rd(e->se + GENI_RX_FIFO);

		for (int j = 0; j < 4 && bytes; j++, bytes--) {
			if (*received < length) {
				rx[(*received)++] = (uint8_t)(word >> (8 * j));
			}
		}
	}
}

/* One message: `length' bytes to the device at `address', or from it. `more': a repeated start follows instead of a stop. */
static bool
geni_i2c_message(struct geni_i2c *e, uint32_t address, bool read, uint8_t *buffer, uint32_t length, bool more)
{
	uint32_t sent = 0, received = 0, status = 0;
	uint64_t deadline = now_us() + 100000 + 250ull * length;

	for (int i = 0; rd(e->se + GENI_STATUS) & 1; i++) {
		if (i > 2000) {
			geni_i2c_recover(e);
			break;
		}
		GENI_I2C_WAIT_US(10);
	}
	wr(e->se + GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(e->se + (read ? I2C_RX_TRANS_LEN : I2C_TX_TRANS_LEN), length);
	wr(e->se + GENI_M_CMD0, (read ? GENI_I2C_READ : GENI_I2C_WRITE) << 27 | (address & 0x7f) << GENI_I2C_ADDRESS_SHIFT |
	    (more ? GENI_I2C_STOP_STRETCH : 0));
	while (now_us() < deadline) {
		while (!read && sent < length && (rd(e->se + GENI_TX_FIFO_STATUS) & 0x0fffffff) + 1 < e->fifo_depth) {
			uint32_t word = 0;

			for (int j = 0; j < 4 && sent < length; j++, sent++) {
				word |= (uint32_t)buffer[sent] << (8 * j);
			}
			wr(e->se + GENI_TX_FIFO, word);
		}
		status = rd(e->se + GENI_M_IRQ_STATUS);
		if (read) {
			geni_i2c_drain(e, buffer, length, &received);
		}
		if (status & GENI_I2C_ERRORS) {
			break;
		}
		if (status & M_CMD_DONE) {
			if (read) {
				geni_i2c_drain(e, buffer, length, &received);
			}
			wr(e->se + GENI_M_IRQ_CLEAR, status);
			if (read ? received == length : sent == length) {
				return true;
			}
			break;
		}
		GENI_I2C_WAIT_US(5);
	}
	e->status = status;
	geni_i2c_recover(e);
	return false;
}

/*
 * One transaction with the device at `address': `out_length' bytes written and then, after a repeated start, `in_length'
 * read (either may be 0, not both). False if it failed; geni_i2c_nacked() says whether nobody acknowledged.
 */
static bool
geni_i2c_transfer(struct geni_i2c *e, uint32_t address, const uint8_t *out, uint32_t out_length, uint8_t *in, uint32_t in_length)
{
	e->status = 0;
	if (out_length != 0 && !geni_i2c_message(e, address, false, (uint8_t *)(uintptr_t)out, out_length, in_length != 0)) {
		return false;
	}
	return in_length == 0 || geni_i2c_message(e, address, true, in, in_length, false);
}

static bool
geni_i2c_nacked(const struct geni_i2c *e)
{
	return (e->status & GENI_I2C_NACK) != 0;
}

#endif /* HAKIOS_GENI_I2C_H */
