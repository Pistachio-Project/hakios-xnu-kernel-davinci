/*
 * The battery's live current and voltage, from the PMIC's fuel gauge (Qualcomm "QGauge", qcom,qpnp-qg), so that
 * power-management runs can be measured in mA and mW. Boot-arg batt_live=1; off by default.
 *
 * Where: the board file's qgauge { } (slave << 8 | the high byte): the PM6150's qcom,qgauge@4800 on slave 0 is 0x048
 * (davinci-data/xiaomi-sdmmagpie.pre.dts, pm6150_qg). THIS DRIVER ONLY READS: no charging or QG configuration is
 * written, and not even the burst-average hold bit Linux sets while it reads (QG_DATA_CTL2 bit 0): a value is read
 * twice and taken when both reads agree, which is what the hold is for. Only the arbiter's command word is written.
 *
 * Registers (Linux msm-4.14 drivers/power/supply/qcom/qg-reg.h, qg-util.c, qg-defs.h):
 *   0x04/0x05  type 0x0D (QG_TYPE) / subtype: 0x3 = 5 A ADC, 0x4 = 10 A ADC
 *   0x08       status 1: bit 7 QG_OK, bit 0 battery present
 *   0xC0       LAST_ADC_V, 16 bits little endian, unsigned: V_RAW_TO_UV = raw * 194.637 uV
 *   0xC6       LAST_BURST_AVG_I, 16 bits little endian, signed: raw * 152.588 uA (5 A subtype) or * 305.176 uA (10 A);
 *              what Linux's qg_get_battery_current() returns for current_now
 * Sign: the raw value is negative while the battery charges (qpnp-qg.c tests -ibat for the charge current); this
 * driver reports DISCHARGE POSITIVE, CHARGE NEGATIVE (the raw sign as it is). To be confirmed on the phone.
 *
 * Logged every 5 s from samples every 500 ms:
 *   battery: N mV, I mA (avg over 5 s A mA, min/max), P mW, sample count
 * hw.battery.voltage_mv / current_ma / power_mw (the latest sample), hw.battery.energy_uwh (net energy drawn since
 * the driver started, microwatt-hours, discharge positive) and hakios_battery_qg_energy_uwh() for the kernel's pm_*
 * runs (take it at the start and the end; the difference over 1000 is mWh).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/board/input.h>        /* hakios_spmi_lock() */
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "kernel.h"
#include "board.h"
#include "log.h"

HAKIOS_DEFINE_SAY(true)

#include "mmio.h"
#include "spmi.h"

#define QG_REG_TYPE             0x04
#define QG_REG_STATUS1          0x08
#define QG_TYPE                 0x0D
#define QG_OK_BIT               0x80
#define QG_BATT_PRESENT_BIT     0x01
#define QG_REG_LAST_ADC_V       0xC0
#define QG_REG_LAST_BURST_I     0xC6
#define QG_ADC_IBAT_5A          0x3
#define QG_ADC_IBAT_10A         0x4

#define SAMPLE_MS               500
#define SAMPLES_PER_LINE        10

static struct hakios_spmi arb;
static uint32_t channel;
static uint32_t i_lsb_nua;              /* nanoamperes per raw step */

static volatile int batt_mv, batt_ma, batt_mw;
static int64_t energy_uj;               /* mW * ms = microjoules, net, discharge positive */
static volatile int64_t energy_uwh;
static uint64_t sample_count, read_failures;

SYSCTL_NODE(_hw, OID_AUTO, battery, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "live battery readings (PMIC fuel gauge)");
SYSCTL_INT(_hw_battery, OID_AUTO, voltage_mv, CTLFLAG_RD | CTLFLAG_LOCKED, __DECONST(int *, &batt_mv), 0, "battery voltage, mV");
SYSCTL_INT(_hw_battery, OID_AUTO, current_ma, CTLFLAG_RD | CTLFLAG_LOCKED, __DECONST(int *, &batt_ma), 0,
    "battery current, mA: discharge positive, charge negative");
SYSCTL_INT(_hw_battery, OID_AUTO, power_mw, CTLFLAG_RD | CTLFLAG_LOCKED, __DECONST(int *, &batt_mw), 0, "battery power, mW (discharge positive)");
SYSCTL_QUAD(_hw_battery, OID_AUTO, energy_uwh, CTLFLAG_RD | CTLFLAG_LOCKED, __DECONST(int64_t *, &energy_uwh), "net energy drawn since start, uWh");

int64_t
hakios_battery_qg_energy_uwh(void)
{
	return energy_uwh;
}

static const char *
qg_read(uint8_t reg, uint8_t *bytes, uint32_t n)
{
	const char *problem;

	hakios_spmi_lock();
	problem = hakios_spmi_read(hakios_spmi_observer(&arb, channel), reg, bytes, n);
	hakios_spmi_unlock();
	return problem;
}

/* A 16-bit register read until two reads in a row agree (stands in for Linux's hold bit, which is a write). */
static const char *
qg_read16(uint8_t reg, uint16_t *value)
{
	uint8_t a[2], b[2];
	const char *problem = NULL;

	for (int tries = 0; tries < 4; tries++) {
		problem = qg_read(reg, a, 2);
		if (problem == NULL) {
			problem = qg_read(reg, b, 2);
		}
		if (problem != NULL) {
			return problem;
		}
		*value = (uint16_t)(b[0] | b[1] << 8);
		if (a[0] == b[0] && a[1] == b[1]) {
			return NULL;
		}
	}
	return NULL;    /* (a value that kept moving: the last read is taken) */
}

static bool
qg_sample(int *mv, int *ma)
{
	uint16_t v, i;

	if (qg_read16(QG_REG_LAST_ADC_V, &v) != NULL || qg_read16(QG_REG_LAST_BURST_I, &i) != NULL) {
		read_failures++;
		return false;
	}
	*mv = (int)(((uint64_t)v * 194637ULL) / 1000000ULL);
	*ma = (int)(((int64_t)(int16_t)i * (int64_t)i_lsb_nua) / 1000000LL);
	return true;
}

static void
qg_thread(__unused void *parameter, __unused wait_result_t result)
{
	int64_t sum_ma = 0, sum_mv = 0;
	int lo = 0, hi = 0;
	unsigned window = 0;

	for (;;) {
		int mv, ma;

		delay_for_interval(SAMPLE_MS, NSEC_PER_MSEC);
		if (!qg_sample(&mv, &ma)) {
			continue;
		}
		int mw = (int)(((int64_t)mv * ma) / 1000);
		batt_mv = mv;
		batt_ma = ma;
		batt_mw = mw;
		sample_count++;
		energy_uj += (int64_t)mw * SAMPLE_MS;
		energy_uwh = energy_uj / 3600;
		if (window == 0 || ma < lo) {
			lo = ma;
		}
		if (window == 0 || ma > hi) {
			hi = ma;
		}
		sum_ma += ma;
		sum_mv += mv;
		if (++window == SAMPLES_PER_LINE) {
			int avg_ma = (int)(sum_ma / window), avg_mv = (int)(sum_mv / window);

			say("battery: %d mV, %d mA (avg over %u s %d mA, min/max %d/%d; discharge positive), %d mW "
			    "(avg %d mW), %llu samples, %lld uWh drawn\n", mv, ma, window * SAMPLE_MS / 1000, avg_ma, lo, hi,
			    mw, (int)(((int64_t)avg_mv * avg_ma) / 1000), sample_count, (long long)energy_uwh);
			sum_ma = sum_mv = 0;
			window = 0;
		}
	}
}

void
hakios_battery_qg_init(void)
{
	const struct hakios_board *board = hakios_board();
	uint32_t enabled = 0;
	uint8_t kind[2], status;
	const char *problem, *name = "the PMIC's fuel gauge";
	uint16_t id;
	bool found = false;
	thread_t thread;

	PE_parse_boot_argn("batt_live", &enabled, sizeof(enabled));
	if (!enabled) {
		return;
	}
	if (board == NULL || board->qgauge.peripheral == 0) {
		say("battery: batt_live: this board's board file names no fuel gauge (qgauge { }): not read\n");
		return;
	}
	id = board->qgauge.peripheral;
	if (board->qgauge.name != NULL) {
		name = board->qgauge.name;
	}
	if (!hakios_spmi_map(&arb)) {
		say("battery: batt_live: no PMIC arbiter this driver knows (version %#x): %s not read\n", arb.version, name);
		return;
	}
	for (uint32_t n = 0; n < arb.channels; n++) {
		if (SPMI_ID_OF(hakios_spmi_channel_map(&arb, n)) != id) {
			continue;
		}
		if (!found || hakios_spmi_channel_owner(&arb, n) == arb.system) {
			found = true;
			channel = n;
		}
	}
	if (!found) {
		say("battery: batt_live: no channel of the arbiter is %s (slave %u, registers %#x): not read\n", name, id >> 8,
		    (id & 0xff) << 8);
		return;
	}
	problem = qg_read(QG_REG_TYPE, kind, 2);
	if (problem == NULL) {
		problem = qg_read(QG_REG_STATUS1, &status, 1);
	}
	if (problem != NULL) {
		say("battery: batt_live: %s (slave %u, registers %#x): %s\n", name, id >> 8, (id & 0xff) << 8, problem);
		return;
	}
	if (kind[0] != QG_TYPE || (kind[1] != QG_ADC_IBAT_5A && kind[1] != QG_ADC_IBAT_10A)) {
		say("battery: batt_live: %s is of type %#x, subtype %#x: not a QGauge (type %#x, subtype 3 or 4): not read\n",
		    name, kind[0], kind[1], QG_TYPE);
		return;
	}
	i_lsb_nua = kind[1] == QG_ADC_IBAT_5A ? 152588 : 305176;
	say("battery: batt_live: %s (slave %u, registers %#x, channel %u), subtype %#x (%s A), status %#x (%s, battery %s): "
	    "sampling every %d ms, a line every %d s: hw.battery.*\n", name, id >> 8, (id & 0xff) << 8, channel, kind[1],
	    kind[1] == QG_ADC_IBAT_5A ? "5" : "10", status, status & QG_OK_BIT ? "ok" : "not ok",
	    status & QG_BATT_PRESENT_BIT ? "present" : "missing", SAMPLE_MS, SAMPLES_PER_LINE * SAMPLE_MS / 1000);
	if (kernel_thread_start(qg_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
