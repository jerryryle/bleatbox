/*
 * LIS2DW12 accelerometer — wakeup event producer.
 *
 * Configures the accelerometer in low-power mode with a wakeup
 * interrupt threshold and pushes EVENT_VIBRATION into the provided
 * message queue on each trigger.
 */

#ifndef ACCEL_H_
#define ACCEL_H_

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>

/**
 * Initialize the LIS2DW12 accelerometer module.
 *
 * Stores the event queue and verifies the device is ready.  Does not
 * configure or arm the wakeup interrupt — call accel_start() for that.
 *
 * @param event_q       Message queue to push EVENT_VIBRATION events into.
 * @return 0 on success, negative errno on failure.
 */
int accel_init(struct k_msgq *event_q);

/**
 * Configure the wakeup threshold and arm the motion trigger.
 *
 * After this call the accelerometer begins pushing EVENT_VIBRATION
 * events into the queue passed to accel_init().
 *
 * @param threshold_mg  Wakeup threshold in milli-g (e.g. 200 = 200 mg).
 * @return 0 on success, negative errno on failure (including -ENODEV
 *         if accel_init() did not succeed).
 */
int accel_start(uint16_t threshold_mg);

/**
 * Update the wakeup threshold on the live accelerometer.
 *
 * Applies immediately without re-arming the trigger; does not persist
 * to the SD-card config.
 *
 * @param threshold_mg  Wakeup threshold in milli-g (e.g. 200 = 200 mg).
 * @return 0 on success, negative errno on failure (including -ENODEV
 *         if accel_init() did not succeed).
 */
int accel_set_threshold(uint16_t threshold_mg);

/**
 * Get the most recently applied wakeup threshold.
 *
 * @param[out] threshold_mg  Current threshold in milli-g.
 * @return 0 on success, -ENODEV if accel_init() did not succeed.
 */
int accel_get_threshold(uint16_t *threshold_mg);

/**
 * Read a single XYZ acceleration sample.
 *
 * @param[out] x  X-axis acceleration.
 * @param[out] y  Y-axis acceleration.
 * @param[out] z  Z-axis acceleration.
 * @return 0 on success, negative errno on failure.
 */
int accel_sample_xyz(struct sensor_value *x,
                     struct sensor_value *y,
                     struct sensor_value *z);

/**
 * Temporarily set the ODR to 100 Hz for interactive sampling.
 *
 * @param[out] prev_ctrl1  Previous CTRL1 register value (for restore).
 * @return 0 on success, negative errno on failure.
 */
int accel_odr_boost(uint8_t *prev_ctrl1);

/**
 * Restore the CTRL1 register to a previous value.
 *
 * @param prev_ctrl1  Value returned by accel_odr_boost().
 * @return 0 on success, negative errno on failure.
 */
int accel_odr_restore(uint8_t prev_ctrl1);

#endif /* ACCEL_H_ */
