/*
 * Battery monitor — reads VBAT through the on-board voltage divider.
 *
 * The Feather nRF52840 wires VBAT to AIN5 (P0.29) through a 100k/100k
 * divider; this module samples it on demand via the SAADC.  Sampling is
 * one-shot — the ADC is only powered for the brief conversion, so the
 * module adds no continuous draw beyond the divider's own bleed current.
 */

#ifndef BATTERY_H_
#define BATTERY_H_

#include <stdint.h>

/**
 * Initialize the battery monitor.
 *
 * Verifies the ADC controller is ready and configures the battery-sense
 * channel.  Does not start sampling; call battery_read_mv() on demand.
 *
 * @return 0 on success, negative errno on failure.
 */
int battery_init(void);

/**
 * Read the battery voltage.
 *
 * Performs a single SAADC conversion and applies the divider scaling.
 *
 * @param[out] millivolts  Battery voltage in millivolts.
 * @return 0 on success, negative errno on failure (including -ENODEV if
 *         battery_init() did not succeed).
 */
int battery_read_mv(int32_t *millivolts);

#endif /* BATTERY_H_ */
