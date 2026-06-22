#ifndef BLE_OTA_H_
#define BLE_OTA_H_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "message.h"

/*
 * OTA arming rides inside the standard 16-byte message payload, exactly like a
 * bleat — so a Mac can open a box's update window the same way it sends a
 * tissue-pull (see scripts/bleatctl.py).  It is marked by a dedicated value in
 * the 2-bit global command field (message.h reserves command values 01/10/11;
 * OTA claims 01).
 *
 * There is nothing else to encode: an armed box becomes connectable for SMP,
 * and the operator drives everything else (upload the image, reboot) over that
 * connection.  Commit/cancel are not on-air commands — an armed box stops
 * scanning, so it could not hear them anyway.
 *
 * Pure and header-only (it only needs MESSAGE_CMD_OFFSET from message.h, a
 * compile-time constant) so it links into the host tests without message.c.
 */

#define BLE_OTA_CMD 0x01 /* message command value: arm for OTA */

/* Read the 2-bit message command field (LSB-first at MESSAGE_CMD_OFFSET). */
static inline uint8_t ble_ota_get_command(const uint8_t payload[16])
{
    uint8_t cmd = 0;
    for (unsigned int i = 0; i < 2; i++) {
        unsigned int bit = MESSAGE_CMD_OFFSET + i;
        cmd |= (uint8_t)(((payload[bit >> 3] >> (bit & 7)) & 1u) << i);
    }
    return cmd;
}

/* Write the 2-bit message command field. */
static inline void ble_ota_set_command(uint8_t payload[16], uint8_t cmd)
{
    for (unsigned int i = 0; i < 2; i++) {
        unsigned int bit = MESSAGE_CMD_OFFSET + i;
        uint8_t mask = (uint8_t)(1u << (bit & 7));
        if ((cmd >> i) & 1u) {
            payload[bit >> 3] |= mask;
        } else {
            payload[bit >> 3] &= (uint8_t)~mask;
        }
    }
}

/* Build an OTA-arm payload: zero the play-slots and set the command field. */
static inline void ble_ota_encode(uint8_t payload[16])
{
    memset(payload, 0, 16);
    ble_ota_set_command(payload, BLE_OTA_CMD);
}

/* True if @p payload is an OTA-arm command rather than a play message. */
static inline bool ble_ota_is_arm(const uint8_t payload[16])
{
    return ble_ota_get_command(payload) == BLE_OTA_CMD;
}

#endif /* BLE_OTA_H_ */
