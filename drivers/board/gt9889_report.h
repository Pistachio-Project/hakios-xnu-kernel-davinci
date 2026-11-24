/*
 * What a Goodix GT9889 (the "Normandy" part of Goodix's GTX8 driver, which Xiaomi's tree calls GT9886) says in its
 * working mode, apart from the bus it says it on: the one register block that holds a report, and a report made into
 * fingers. For board/gt9889.c and the host's tests.
 *
 * Source: Xiaomi's sm6150 kernel (reference/kernel_xiaomi_sm6150), the driver the davinci build has
 * (arch/arm64/configs/vendor/davinci.config:30, CONFIG_TOUCHSCREEN_GOODIX_GTX8 -> drivers/input/touchscreen/
 * goodix_driver_gt9886/, whose DT compatible is "goodix,gt9889", goodix_ts_i2c.c:32). In that driver:
 *
 *   register     16 bits, the most significant byte first; a read is "address, restart, data"
 *                (goodix_i2c_read_trans, goodix_ts_i2c.c:377-449)
 *   0x4100       the report block (reg.coor): the driver's own reads of it are at goodix_ts_i2c.c:1909 and 1776,
 *                and its two other uses of that address -- goodix_ts_tools.c:53, gt9896/goodix_ts_i2c.c:285 (a
 *                one-byte read that a chip is "confirmed" by) -- say the same. THE ADDRESS ITSELF IS NOT IN THE
 *                SOURCE: reg.coor and every other register of this chip are read from the firmware package
 *                "goodix_gt9886_cfg_f10.bin" (goodix_cfg_bin.c:405-424) that Android loads, which this tree does not
 *                have. 0x4100 is what the source's own constants for the block say, not a value read from a package.
 *   [0]          the event: bit 7 touch, bit 6 the firmware asks for something (reset, a configuration), bit 5
 *                gesture (goodix_ts_core.h:76-80). 0 is "nothing new".
 *   [1]          the finger count, low four bits (goodix_ts_i2c.c:1759-1768); bit 4 says a key is down
 *   [2 + 8i ...] eight bytes a finger (goodix_ts_i2c.c:1835-1840): [0] its number in the low four bits (0x80 and
 *                up: the pen, not read here), [1..2] x, [3..4] y, both least significant byte first, [5] its
 *                weight, [6] unknown, [7] its area
 *   then         one byte of keys and one of checksum: all the bytes from [0] to the checksum (8 n + 4 of them)
 *                add up to 0 modulo 256 (goodix_ts_i2c.c:1787-1790)
 *   a report is read as the first 12 bytes, and, with more than one finger, (n - 1) * 8 bytes from 0x410c on
 *   (goodix_ts_i2c.c:1776-1780); after each report the event byte is written 0 (goodix_ts_core.c:905)
 *
 * The chip's other registers (identity, version, configuration, firmware requests, the doze mode that
 * goodix_ts_i2c.c:522-587 brackets some accesses with) are in that package and are not used here.
 *
 * Coordinates are 16 bits in a report; the board says the largest (touch-i2c { } coordinates).
 */
#ifndef HAKIOS_GT9889_REPORT_H
#define HAKIOS_GT9889_REPORT_H

#include <stdbool.h>
#include <stdint.h>

#define GT9889_CONTACTS         10      /* goodix,panel-max-id of davinci's node; a board says its own, up to this */
#define GT9889_REG_REPORT       0x4100
#define GT9889_REPORT_HEAD      12      /* the bytes the first read takes: event, count, one finger, the key and the sum */
#define GT9889_FINGER_BYTES     8
#define GT9889_REPORT_MAX       (4 + GT9889_FINGER_BYTES * GT9889_CONTACTS)
#define GT9889_COORDINATE_MAX   0xffff

#define GT9889_EVENT_TOUCH      0x80
#define GT9889_EVENT_REQUEST    0x40

struct gt9889_frame {
	uint16_t active;                        /* a bit for each finger that is down, by its number */
	uint16_t x[GT9889_CONTACTS], y[GT9889_CONTACTS];
};

enum gt9889_look {
	GT9889_NOTHING,                         /* the event byte says nothing new: nothing changes */
	GT9889_FRAME,                           /* fingers (perhaps none: all lifted) */
	GT9889_REQUEST,                         /* the firmware asks for something this driver does not do; to be cleared */
	GT9889_BAD,                             /* cannot be a report of this chip */
};

/* how many bytes of the report, after its first GT9889_REPORT_HEAD, make it whole: 0 if it is not a touch report */
static inline unsigned
gt9889_more(const uint8_t *head, unsigned contacts)
{
	unsigned n;

	if (!(head[0] & GT9889_EVENT_TOUCH)) {
		return 0;
	}
	n = head[1] & 0x0f;
	return n > 1 && n <= contacts ? (n - 1) * GT9889_FINGER_BYTES : 0;
}

/*
 * A report `bytes' long (the head and what gt9889_more() asked for) made into fingers. A finger of a number that is
 * not the chip's, one twice, a position past the panel, or a sum that is not 0 is a report that cannot be one.
 */
static inline enum gt9889_look
gt9889_decode(const uint8_t *report, unsigned bytes, unsigned contacts, uint32_t max_x, uint32_t max_y,
    struct gt9889_frame *frame)
{
	unsigned n;
	uint8_t sum = 0;

	frame->active = 0;
	if (report[0] & GT9889_EVENT_REQUEST) {
		return GT9889_REQUEST;
	}
	if (!(report[0] & GT9889_EVENT_TOUCH)) {
		return report[0] == 0 ? GT9889_NOTHING : GT9889_BAD;
	}
	n = report[1] & 0x0f;
	if (n > contacts || bytes < 4 + n * GT9889_FINGER_BYTES) {
		return GT9889_BAD;
	}
	for (unsigned i = 0; i < 4 + n * GT9889_FINGER_BYTES; i++) {
		sum = (uint8_t)(sum + report[i]);
	}
	if (sum != 0) {
		return GT9889_BAD;
	}
	for (unsigned i = 0; i < n; i++) {
		const uint8_t *f = &report[2 + i * GT9889_FINGER_BYTES];
		unsigned id = f[0] & 0x0f;
		uint32_t x = (uint32_t)f[1] | (uint32_t)f[2] << 8, y = (uint32_t)f[3] | (uint32_t)f[4] << 8;

		if (f[0] >= 0x80) {
			continue;               /* the pen: not read here */
		}
		if (id >= contacts || (frame->active & (1u << id)) || x > max_x || y > max_y) {
			frame->active = 0;
			return GT9889_BAD;
		}
		frame->active |= (uint16_t)(1u << id);
		frame->x[id] = (uint16_t)x;
		frame->y[id] = (uint16_t)y;
	}
	return GT9889_FRAME;
}

#endif /* HAKIOS_GT9889_REPORT_H */
