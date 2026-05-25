/*
 * BLE subsystem — extended advertising and scanning.
 */

#ifndef BLE_H_
#define BLE_H_

#include <stdint.h>
#include <stdbool.h>

/**
 * Initialize Bluetooth: enable the stack, create the extended
 * advertising set, register the scan callback, and start scanning.
 *
 * @param device_id       This device's FICR-derived ID.
 * @param device_ids      Array of all known device IDs in the network.
 * @param num_devices     Length of @p device_ids.
 * @param triggering_flag Pointer to the am_triggering flag (owned by
 *                        the caller).  The scan callback reads this to
 *                        suppress self-processing while this device is
 *                        the broadcast source.
 * @return 0 on success, negative errno on failure.
 */
int ble_init(uint8_t device_id,
	     const uint8_t *device_ids, uint8_t num_devices,
	     const volatile bool *triggering_flag);

/**
 * Build random sound/delay assignments for every device in the
 * network and broadcast them via BLE extended advertising.
 *
 * @return 0 on success, negative errno on failure.
 */
int ble_advertise_assignments(void);

/**
 * Retrieve this device's assignment from the most recent broadcast.
 *
 * Only valid after a successful ble_advertise_assignments() call.
 *
 * @param device_index  This device's index in KNOWN_DEVICE_IDS[].
 * @param sound         [out] Assigned sound index.
 * @param delay_ms      [out] Assigned delay in milliseconds.
 */
void ble_get_self_assignment(int device_index,
			     uint8_t *sound, uint16_t *delay_ms);

/**
 * Cancel any pending RX playback work.
 *
 * Call this before triggering local playback to prevent a stale
 * scheduled RX playback from firing after the local playback.
 */
void ble_cancel_rx_playback(void);

#endif /* BLE_H_ */
