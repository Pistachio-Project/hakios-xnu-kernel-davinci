/*
 * The rate of a serial engine's clock, as its root clock generator's registers make it: what both of the engine's
 * transports (board/geni_spi.h, board/geni_i2c.h) need to know before they divide it down.
 *
 * The file that includes this one has defined XO_HZ first, the crystal's rate (or lets this give 19.2 MHz).
 */
#ifndef HAKIOS_GENI_CLK_H
#define HAKIOS_GENI_CLK_H

#ifndef XO_HZ
#define XO_HZ                   19200000
#endif

/*
 * The rate a root clock generator (RCG) of the engine's clock gives, from its configuration -- its source
 * ([10:8]: the crystal, GPLL0, GPLL0 halved), its half-integer divider ([4:0]) and its M/N mode ([13:12])
 * -- and its M and NOT(N - M) registers
 */
static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	static const uint32_t parent[8] = { XO_HZ, 600000000, 0, 0, 0, 0, 300000000, 0 };
	uint64_t hz = parent[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		if (n) {
			hz = hz * (m & 0xffff) / n;
		}
	}
	return (uint32_t)hz;
}

#endif /* HAKIOS_GENI_CLK_H */
