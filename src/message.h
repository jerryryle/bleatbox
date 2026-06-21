/*
 * BleatBox message format — the single 16-byte payload every BLE message
 * carries, on the mesh and from macOS alike.
 *
 * Devices exchange it as manufacturer-specific data (with a relay header);
 * macOS can only advertise Service UUIDs, so it sends the same 16 bytes as a
 * 128-bit UUID and the first device that hears it re-emits the manufacturer-
 * data form.  Six positional slots address up to six devices; each device
 * reads only its own slot (a positional index from bleatbox.cfg).
 *
 * Payload layout (canonical = on-air byte order, bit 0 = LSB of byte 0,
 * slots packed LSB-first):
 *
 *   slot i -> bits [i*19 .. i*19+18]   (i = 0..5)
 *     within a slot:  [0..11]  delay_ms  (12 bits, 0..4095)
 *                     [12..18] sound      (7 bits; 0x7F = play nothing)
 *   bits [114..115] command   (2 bits, global; 00 = play)
 *   bits [116..123] seq        (8 bits, relay sequence; macOS path only)
 *   bits [124..127] reserved
 *
 * This module is pure (no Zephyr/BLE deps) so it links into the host test
 * build and pins the bit-packing the Python sender mirrors.
 */

#ifndef MESSAGE_H_
#define MESSAGE_H_

#include <stdint.h>
#include <stdbool.h>

#define MESSAGE_SLOTS        6
#define MESSAGE_SLOT_BITS    19

/* sound == all ones means "this slot plays nothing". */
#define MESSAGE_SOUND_SILENT 0x7F
/* Largest delay the 12-bit slot field can hold, in milliseconds. */
#define MESSAGE_DELAY_MAX    0xFFF
/* command 00 = play; 01/10/11 reserved for future use. */
#define MESSAGE_CMD_PLAY     0x00

/* Reserved originator id stamped on macOS-originated messages so every
 * gateway agrees on one (originator, seq) for mesh dedup.  No device may
 * use this as its own id. */
#define MESSAGE_EXT_ORIGINATOR 0xFE

/* A device's 1-based id is its position: slot = id - 1 for ids 1..MESSAGE_SLOTS.
 * Any other id (0, or above MESSAGE_SLOTS) has no slot — message_slot_for_id()
 * returns MESSAGE_NO_SLOT and the device only relays. */
#define MESSAGE_NO_SLOT 0xFF

/* Bit offsets of the global fields, above the six slots. */
#define MESSAGE_CMD_OFFSET   (MESSAGE_SLOTS * MESSAGE_SLOT_BITS) /* 114 */
#define MESSAGE_SEQ_OFFSET   (MESSAGE_CMD_OFFSET + 2)            /* 116 */

struct message_slot {
    uint8_t  sound;    /* 7-bit sound index */
    uint16_t delay_ms; /* 12-bit, 0..4095 */
    bool     play;     /* false for the silent sentinel */
};

/**
 * Decode @p slot's 19 bits out of the 16-byte canonical payload.
 *
 * @param payload  16-byte payload in on-air byte order.
 * @param slot     Slot index (0..MESSAGE_SLOTS-1).
 * @param[out] out Decoded slot; @c out->play is false (device stays silent)
 *                 for an out-of-range slot or the silent sound sentinel.
 */
void message_parse_slot(const uint8_t payload[16], uint8_t slot,
                        struct message_slot *out);

/** Global 2-bit command from the payload. */
uint8_t message_get_command(const uint8_t payload[16]);

/** 8-bit relay sequence number from the payload. */
uint8_t message_get_seq(const uint8_t payload[16]);

/**
 * Write @p sound (7-bit) and @p delay (12-bit) into @p slot.
 *
 * A @p delay above MESSAGE_DELAY_MAX is clamped to it.  A @p sound that does
 * not fit the 7-bit field is forced to MESSAGE_SOUND_SILENT (no sound) rather
 * than wrapped to a different valid index.
 */
void message_pack_slot(uint8_t payload[16], uint8_t slot,
                       uint8_t sound, uint16_t delay);

/** Set the global 2-bit command. */
void message_set_command(uint8_t payload[16], uint8_t command);

/**
 * Map a 1-based device id to its 0-based slot.
 *
 * @return id - 1 for ids 1..MESSAGE_SLOTS, else MESSAGE_NO_SLOT (the device
 *         has no slot and only relays).
 */
uint8_t message_slot_for_id(uint8_t id);

#endif /* MESSAGE_H_ */
