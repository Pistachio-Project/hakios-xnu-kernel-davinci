/*
 * The SoC's own pins (Qualcomm's "TLMM"): a configuration register and a level register for each.
 * What the part drivers that drive pins each had a copy of. Where the pins are comes from the SoC
 * file's pins { } (hakios_board()->pins.tlmm); which pin a part is on is the part driver's.
 *
 * The file that includes this one has rd() and wr() (board/mmio.h), and says where it has the pins
 * mapped before it includes this:
 *
 *   static vm_offset_t tlmm;                   (= ml_io_map(hakios_board()->pins.tlmm, TLMM_SIZE) at its start)
 *   #define HAKIOS_TLMM tlmm
 *   #include "tlmm.h"
 *
 * Register layout (and the tiles): Linux drivers/pinctrl/qcom/pinctrl-msm.c.
 */
#ifndef HAKIOS_TLMM_H
#define HAKIOS_TLMM_H

#include "board.h"

#ifndef HAKIOS_TLMM
#error "say where the pins are mapped: #define HAKIOS_TLMM before including board/tlmm.h"
#endif

/*
 * Where a pin's registers are is the board's to say (a tiled controller has them in blocks at other offsets,
 * board.h's hakios_tlmm_offset()): a driver that computes tlmm + 0x1000 n itself is wrong on such a board.
 * What to map is TLMM_SIZE, how many pins there are TLMM_PINS.
 */
#define TLMM_SIZE               hakios_tlmm_span(hakios_board())
#define TLMM_PINS               hakios_tlmm_pins(hakios_board())
#define GPIO_CFG(n)             ((HAKIOS_TLMM) + hakios_tlmm_offset(hakios_board(), (n)))        /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              ((HAKIOS_TLMM) + hakios_tlmm_offset(hakios_board(), (n)) + 4)    /* bit 0 in, bit 1 out */
#define GPIO_INTR_CFG(n)        ((HAKIOS_TLMM) + hakios_tlmm_offset(hakios_board(), (n)) + 8)    /* the pin's interrupt detection */
#define GPIO_INTR_STATUS(n)     ((HAKIOS_TLMM) + hakios_tlmm_offset(hakios_board(), (n)) + 0xc)  /* bit 0: an edge was seen */
#define PULL_NONE               0
#define PULL_DOWN               1
#define PULL_KEEPER             2
#define PULL_UP                 3
/* the fields of a pin's configuration, for a driver that puts one together itself */
#define CFG_PULL_UP             PULL_UP
#define CFG_FUNCTION(f)         ((uint32_t)(f) << 2)
#define CFG_DRIVE_MA(ma)        ((uint32_t)((ma) / 2 - 1) << 6)
#define CFG_OUTPUT              (1u << 9)

/* `function' 0 is a plain GPIO, the others are the SoC's own uses of the pin; `ma' the drive strength, 2 to 16 in twos */
static inline void __attribute__((unused))
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	wr(GPIO_CFG(pin), pull | CFG_FUNCTION(function) | CFG_DRIVE_MA(ma) | (output ? CFG_OUTPUT : 0));
}

static inline void __attribute__((unused))
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static inline bool __attribute__((unused))
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

/* a plain output at that level */
static inline void __attribute__((unused))
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* the level first, so that the pin never drives the wrong one */
	gpio_configure(pin, 0, pull, ma, true);
}

/*
 * A pin's edge latch, for a pin whose interrupt this kernel cannot take: the controller sees falling edges and
 * remembers one in the pin's status register, and is not told to send it anywhere (the enable and target bits stay
 * as they were), so nothing fires and the driver reads the latch. Bits: drivers/pinctrl/qcom/pinctrl-msm.c
 * msm_gpio_irq_set_type() (raw status enable on, falling edge = detection 2 and polarity 1) with the SM7150's positions
 * (pinctrl-sm6150.c: enable 0, polarity 1, detection 2 (two bits), raw status 4, target 5); the status bit is cleared by
 * writing 0 (msm_gpio_irq_ack()).
 */
#define INTR_ENABLE             (1u << 0)
#define INTR_POLARITY           (1u << 1)
#define INTR_DETECTION_MASK     (3u << 2)
#define INTR_DETECTION_EDGE_FALLING     (2u << 2)
#define INTR_RAW_STATUS_EN      (1u << 4)
#define INTR_DIR_CONN_EN        (1u << 8)

/* the latch armed for falling edges, and cleared (the caller has the registers as found, to give back) */
static inline void __attribute__((unused))
gpio_edge_latch_arm(unsigned pin)
{
	uint32_t cfg = rd(GPIO_INTR_CFG(pin));

	cfg &= ~(INTR_ENABLE | INTR_DETECTION_MASK | INTR_DIR_CONN_EN);   /* (not enabled, not routed anywhere) */
	cfg |= INTR_RAW_STATUS_EN | INTR_DETECTION_EDGE_FALLING | INTR_POLARITY;
	wr(GPIO_INTR_CFG(pin), cfg);
	wr(GPIO_INTR_STATUS(pin), 0);
}

/* an edge was seen since the last clear */
static inline bool __attribute__((unused))
gpio_edge_latched(unsigned pin)
{
	return rd(GPIO_INTR_STATUS(pin)) & 1;
}

static inline void __attribute__((unused))
gpio_edge_clear(unsigned pin)
{
	wr(GPIO_INTR_STATUS(pin), 0);
}

#endif /* HAKIOS_TLMM_H */
