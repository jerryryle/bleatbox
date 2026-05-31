/*
 * LIS2DH12 accelerometer — any-motion event producer.
 *
 * Configures the accelerometer in low-power mode with an any-motion
 * interrupt threshold and pushes EVENT_VIBRATION into the provided
 * message queue on each debounced trigger.
 */

#ifndef ACCEL_H_
#define ACCEL_H_

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>

/**
 * Initialize the LIS2DH12 accelerometer and any-motion interrupt.
 *
 * @param event_q       Message queue to push EVENT_VIBRATION events into.
 * @param threshold_mg  Any-motion threshold in milli-g (e.g. 200 = 200 mg).
 * @return 0 on success, negative errno on failure.
 */
int accel_init(struct k_msgq *event_q, uint16_t threshold_mg);

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
