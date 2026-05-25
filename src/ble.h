/*
 * BLE subsystem — extended advertising and scanning.
 *
 * Produces EVENT_BLE_RX events when a matching broadcast is received.
 * Has no knowledge of the audio subsystem.
 */

#ifndef BLE_H_
#define BLE_H_

#include <zephyr/kernel.h>
#include <stdint.h>

/** A single device assignment in a BLE broadcast. */
struct ble_assignment {
	uint8_t  device_id;
	uint8_t  sound;
	uint16_t delay_ms;
};

/** Maximum assignments that fit in one broadcast packet. */
#define BLE_MAX_ASSIGNMENTS 62

/**
 * Initialize Bluetooth: enable the stack, create the extended
 * advertising set, register the scan callback, and start scanning.
 *
 * @param device_id  This device's FICR-derived ID (used to recognize
 *                   our entry in received broadcasts).
 * @param event_q    Message queue to push EVENT_BLE_RX events into.
 * @return 0 on success, negative errno on failure.
 */
int ble_init(uint8_t device_id, struct k_msgq *event_q);

/**
 * Broadcast the given assignments via BLE extended advertising.
 *
 * @param assignments  Array of assignments to broadcast.
 * @param count        Number of entries in @p assignments.
 *                     Must not exceed BLE_MAX_ASSIGNMENTS.
 * @return 0 on success, -EINVAL if count is too large, or a
 *         negative errno on BLE failure.
 */
int ble_advertise_assignments(const struct ble_assignment *assignments,
			      uint8_t count);

#endif /* BLE_H_ */
