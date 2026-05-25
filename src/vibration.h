/*
 * Vibration switch — debounced GPIO event producer.
 *
 * Configures the vibration switch interrupt and pushes
 * EVENT_VIBRATION into the provided message queue on each
 * debounced trigger.  Has no knowledge of audio, BLE, or
 * any other subsystem.
 */

#ifndef VIBRATION_H_
#define VIBRATION_H_

#include <zephyr/kernel.h>

/**
 * Initialize the vibration switch GPIO and interrupt.
 *
 * @param event_q  Message queue to push EVENT_VIBRATION events into.
 * @return 0 on success, negative errno on failure.
 */
int vibration_init(struct k_msgq *event_q);

#endif /* VIBRATION_H_ */
