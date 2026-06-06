#ifndef BLE_ENCODE_H_
#define BLE_ENCODE_H_

#include <stdint.h>

#include "sounds.h"

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

#endif /* BLE_ENCODE_H_ */
