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

#include "ble_encode.h"

/**
 * Initialize Bluetooth: enable the stack, create the extended
 * advertising set, and register the scan callback.  Does not start
 * scanning — call ble_start() for that.
 *
 * @param event_q    Message queue to push EVENT_BLE_RX events into.
 * @return 0 on success, negative errno on failure.
 */
int ble_init(struct k_msgq *event_q);

/**
 * Start scanning for peer broadcasts.
 *
 * After this call the BLE module pushes EVENT_BLE_RX events into the
 * queue passed to ble_init() and relays packets up to @p relay_ttl hops.
 *
 * @param device_id  This device's 1-based ID from the SD-card config.  It
 *                    stamps our broadcasts and selects our message slot
 *                    (id - 1); ids of 0 or above the slot count only relay.
 * @param relay_ttl  Maximum relay hops.  0 disables relaying.
 * @return 0 on success, negative errno on failure (including -ENODEV
 *         if ble_init() did not succeed).
 */
int ble_start(uint8_t device_id, uint8_t relay_ttl);

/**
 * Broadcast a 16-byte message payload via BLE extended advertising.
 *
 * The BLE module stamps the packet with this device as originator, an
 * auto-incremented sequence number, and the configured relay TTL.  Build the
 * payload with the message_* helpers (e.g. via compose_message()).
 *
 * @param payload  16-byte message payload to send.
 * @return 0 on success, or a negative errno on BLE failure.
 */
int ble_broadcast_message(const uint8_t payload[16]);

#endif /* BLE_H_ */
