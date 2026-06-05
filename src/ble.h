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

#include "assignments.h"
#include "sounds.h"

/** Maximum assignments that fit in one broadcast packet. */
#define BLE_MAX_ASSIGNMENTS 59

#define BLE_SOUND_TYPE_BIT  7
#define BLE_SOUND_TYPE_MASK (1U << BLE_SOUND_TYPE_BIT)
#define BLE_SOUND_INDEX_MASK 0x7F

static inline uint8_t ble_sound_encode(enum sound_type type, uint8_t index)
{
    return (uint8_t)((type << BLE_SOUND_TYPE_BIT) | (index & BLE_SOUND_INDEX_MASK));
}

static inline enum sound_type ble_sound_decode_type(uint8_t encoded)
{
    return (enum sound_type)((encoded >> BLE_SOUND_TYPE_BIT) & 1);
}

static inline uint8_t ble_sound_decode_index(uint8_t encoded)
{
    return encoded & BLE_SOUND_INDEX_MASK;
}

/**
 * Initialize Bluetooth: enable the stack, create the extended
 * advertising set, register the scan callback, and start scanning.
 *
 * @param device_id  This device's FICR-derived ID (used to recognize
 *                   our entry in received broadcasts).
 * @param event_q    Message queue to push EVENT_BLE_RX events into.
 * @param relay_ttl  Maximum relay hops.  0 disables relaying.
 * @return 0 on success, negative errno on failure.
 */
int ble_init(uint8_t device_id, struct k_msgq *event_q, uint8_t relay_ttl);

/**
 * Broadcast the given assignments via BLE extended advertising.
 *
 * The BLE module stamps the packet with this device as originator,
 * an auto-incremented sequence number, and the configured relay TTL.
 *
 * @param assignments  Array of assignments to broadcast.
 * @param count        Number of entries in @p assignments.
 *                     Must not exceed BLE_MAX_ASSIGNMENTS.
 * @return 0 on success, -EINVAL if count is too large, or a
 *         negative errno on BLE failure.
 */
int ble_advertise_assignments(const struct assignment *assignments,
                              uint8_t count);

#endif /* BLE_H_ */
