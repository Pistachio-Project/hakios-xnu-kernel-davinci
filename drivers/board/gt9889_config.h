/*
 * The Goodix GT9886/GT9889 configuration package, a file the vendor's driver loads (goodix_cfg_bin.c:
 * goodix_parse_cfg_bin(), goodix_get_reg_and_cfg()) -- compiled into Android's kernel as firmware (the .ihex), made binary --
 * and checked here in full before any of it goes to the chip. Little-endian throughout.
 *
 *   head      bin_len u32 (the file's size), checksum u8 (the sum of everything from byte 5 on), version[4], pkg_num u8,
 *             6 reserved bytes; then pkg_num u16 offsets of the packages (strictly increasing)
 *   package   56 bytes of constants (pkg_len u32, ic_type[15], cfg_type u8 (1 normal), sensor_id u8, hw_pid[8], hw_vid[8],
 *             fw_mask[9], fw_patch[4], three u16 offsets), 14 registers of 4 bytes (addr u16, a length or mask u8, a
 *             reserved u8) and 9 reserved bytes, then the configuration (pkg_len - 121 bytes)
 *   config    the Normandy's: 4 bytes of head that sum to 0 (byte 2 = the number of bags), then the bags: a
 *             tag byte, a length byte n and n + 1 more, each bag summing to 0 (goodix_ts_i2c.c goodix_check_cfg_valid())
 */
#ifndef HAKIOS_GT9889_CONFIG_H
#define HAKIOS_GT9889_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define GT9889_CFG_HEAD         16      /* the head and its reserved bytes, where the offsets begin */
#define GT9889_PKG_CONSTANTS    56
#define GT9889_PKG_REGISTERS    65
#define GT9889_PKG_HEAD         (GT9889_PKG_CONSTANTS + GT9889_PKG_REGISTERS)
#define GT9889_CFG_MAX          1024    /* the vendor's TS_CFG_MAX_LEN */
#define GT9889_PACKAGES         8

struct gt9889_package {
	uint8_t cfg_type, sensor_id, pid[8], pid_length, sensor_mask, fw_mask[9];
	uint16_t sensor_reg, pid_reg, fw_mask_reg, cfg_reg, esd_reg, command_reg;
	const uint8_t *cfg;
	unsigned length;
};

struct gt9889_config {
	struct gt9889_package package[GT9889_PACKAGES];
	unsigned count;
};

static inline unsigned
gt9889_le16(const uint8_t *p)
{
	return (unsigned)p[0] | (unsigned)p[1] << 8;
}

/* The configuration's own sums: its head, and each bag. NULL if it is whole, else what is wrong. */
static inline const char *
gt9889_config_check(const uint8_t *cfg, unsigned length)
{
	unsigned at = 4, bags;
	uint8_t sum = 0;

	if (length < 4 || length > GT9889_CFG_MAX) {
		return "a configuration of a length that cannot be one";
	}
	for (unsigned i = 0; i < 4; i++) {
		sum = (uint8_t)(sum + cfg[i]);
	}
	if (sum != 0) {
		return "the configuration's head does not add up";
	}
	bags = cfg[2];
	for (unsigned b = 0; b < bags; b++) {
		unsigned end;

		if (at + 1 >= length) {
			return "the configuration's bags run past its end";
		}
		end = at + cfg[at + 1] + 3u;
		if (end > length) {
			return "a bag of the configuration runs past its end";
		}
		sum = 0;
		for (unsigned i = at; i < end; i++) {
			sum = (uint8_t)(sum + cfg[i]);
		}
		if (sum != 0) {
			return "a bag of the configuration does not add up";
		}
		at = end;
	}
	return NULL;
}

/*
 * The file's packages, every one of them whole: NULL, and `out' filled in (pointing into `bin'); else what is wrong, and
 * nothing of it is to be used.
 */
static inline const char *
gt9889_config_parse(const uint8_t *bin, uint64_t size, struct gt9889_config *out)
{
	unsigned count, table_end;
	uint8_t sum = 0;
	unsigned previous = 0;

	out->count = 0;
	if (bin == NULL || size < GT9889_CFG_HEAD + 2) {
		return "the file is too short to be a configuration package";
	}
	if ((uint64_t)(bin[0] | bin[1] << 8 | bin[2] << 16 | (uint32_t)bin[3] << 24) != size) {
		return "the length in the file is not the file's";
	}
	if (size > 0xffff) {
		return "the file is too big for 16-bit offsets";
	}
	for (uint64_t i = 5; i < size; i++) {
		sum = (uint8_t)(sum + bin[i]);
	}
	if (sum != bin[4]) {
		return "the file's checksum is wrong";
	}
	count = bin[9];
	if (count == 0 || count > GT9889_PACKAGES) {
		return "the number of packages is 0 or too many";
	}
	table_end = GT9889_CFG_HEAD + 2 * count;
	if (size < table_end) {
		return "the table of offsets runs past the file's end";
	}
	for (unsigned i = 0; i < count; i++) {
		unsigned offset = gt9889_le16(&bin[GT9889_CFG_HEAD + 2 * i]);
		unsigned next = i + 1 < count ? gt9889_le16(&bin[GT9889_CFG_HEAD + 2 * i + 2]) : (unsigned)size;
		const uint8_t *p = &bin[offset], *r;
		struct gt9889_package *k = &out->package[i];
		const char *wrong;

		if (offset < table_end || offset <= previous || offset >= size || next <= offset || next > size) {
			return "the offsets of the packages are not in order, inside the file";
		}
		previous = offset;
		if (next - offset < GT9889_PKG_HEAD + 4) {
			return "a package is shorter than its head and a configuration";
		}
		if ((uint64_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24) != next - offset) {
			return "the length in a package is not the one its offsets give";
		}
		if (p[4] != 'n' || p[5] != 'o' || p[6] != 'r' || p[7] != 'm' || p[8] != 'a' || p[9] != 'n' || p[10] != 'd' ||
		    p[11] != 'y' || p[12] != 0) {
			return "a package is not for a Normandy (ic type)";
		}
		k->cfg_type = p[19];
		k->sensor_id = p[20];
		for (unsigned j = 0; j < 8; j++) {
			k->pid[j] = p[21 + j];
		}
		r = &p[GT9889_PKG_CONSTANTS];
		/* the registers in the order of struct goodix_cfg_pkg_reg_info: send flag, version, pid, vid, sensor id, ... */
		k->pid_reg = (uint16_t)gt9889_le16(&r[2 * 4]);
		k->pid_length = r[2 * 4 + 2];
		k->sensor_reg = (uint16_t)gt9889_le16(&r[4 * 4]);
		k->sensor_mask = r[4 * 4 + 2];
		k->fw_mask_reg = (uint16_t)gt9889_le16(&r[5 * 4]);
		for (unsigned j = 0; j < 9; j++) {
			k->fw_mask[j] = p[37 + j];
		}
		k->cfg_reg = (uint16_t)gt9889_le16(&r[7 * 4]);
		k->esd_reg = (uint16_t)gt9889_le16(&r[8 * 4]);
		k->command_reg = (uint16_t)gt9889_le16(&r[9 * 4]);
		if (k->pid_reg == 0 || k->pid_length == 0 || k->pid_length > 8 || k->sensor_reg == 0 || k->cfg_reg == 0 ||
		    k->esd_reg == 0 || k->command_reg == 0) {
			return "a package lacks a register the driver needs (pid, sensor id, configuration, esd, command)";
		}
		k->cfg = &p[GT9889_PKG_HEAD];
		k->length = next - offset - GT9889_PKG_HEAD;
		wrong = gt9889_config_check(k->cfg, k->length);
		if (wrong != NULL) {
			return wrong;
		}
	}
	out->count = count;
	return NULL;
}

#endif /* HAKIOS_GT9889_CONFIG_H */
