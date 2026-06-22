#include "message.h"

/*
 * Read @p num_bits starting at @p start_bit (LSB-first) out of the byte
 * array.  Bit-at-a-time keeps it obviously correct and avoids any
 * out-of-bounds read at the high slots; the fields are tiny.
 */
static uint32_t read_bits(const uint8_t *p, unsigned int start_bit,
                          unsigned int num_bits)
{
    uint32_t result = 0;

    for (unsigned int i = 0; i < num_bits; i++) {
        unsigned int bit = start_bit + i;
        uint32_t b = (p[bit >> 3] >> (bit & 7)) & 1u;
        result |= b << i;
    }

    return result;
}

static void write_bits(uint8_t *p, unsigned int start_bit,
                       unsigned int num_bits, uint32_t value)
{
    for (unsigned int i = 0; i < num_bits; i++) {
        unsigned int bit = start_bit + i;
        uint8_t mask = (uint8_t)(1u << (bit & 7));
        if ((value >> i) & 1u) {
            p[bit >> 3] |= mask;
        } else {
            p[bit >> 3] &= (uint8_t)~mask;
        }
    }
}

void message_parse_slot(const uint8_t payload[16], uint8_t slot,
                        struct message_slot *out)
{
    if (slot >= MESSAGE_SLOTS) {
        out->sound = MESSAGE_SOUND_SILENT;
        out->delay_ms = 0;
        out->type = MESSAGE_TYPE_GOAT;
        out->play = false;
        return;
    }

    uint32_t field = read_bits(payload, (unsigned int)slot * MESSAGE_SLOT_BITS,
                               MESSAGE_SLOT_BITS);

    out->delay_ms = (uint16_t)(field & 0xFFF);    /* bits 0..11  */
    out->sound = (uint8_t)((field >> 12) & 0x3F); /* bits 12..17 */
    out->type = (uint8_t)((field >> 18) & 0x1);   /* bit  18     */
    out->play = (out->sound != MESSAGE_SOUND_SILENT);
}

uint8_t message_get_command(const uint8_t payload[16])
{
    return (uint8_t)read_bits(payload, MESSAGE_CMD_OFFSET, 2);
}

uint16_t message_get_seq(const uint8_t payload[16])
{
    return (uint16_t)read_bits(payload, MESSAGE_SEQ_OFFSET, MESSAGE_SEQ_BITS);
}

void message_set_seq(uint8_t payload[16], uint16_t seq)
{
    write_bits(payload, MESSAGE_SEQ_OFFSET, MESSAGE_SEQ_BITS, seq);
}

void message_pack_slot(uint8_t payload[16], uint8_t slot,
                       uint8_t sound, uint8_t type, uint16_t delay)
{
    if (slot >= MESSAGE_SLOTS) {
        return;
    }

    /* Force an unrepresentable sound to silent rather than wrapping it to a
     * different valid index; clamp delay to the field's range. */
    if (sound > MESSAGE_SOUND_SILENT) {
        sound = MESSAGE_SOUND_SILENT;
    }
    if (delay > MESSAGE_DELAY_MAX) {
        delay = MESSAGE_DELAY_MAX;
    }

    uint32_t field = (uint32_t)(type & 0x1) << 18 |
                     (uint32_t)sound << 12 | (uint32_t)delay;
    write_bits(payload, (unsigned int)slot * MESSAGE_SLOT_BITS,
               MESSAGE_SLOT_BITS, field);
}

void message_set_command(uint8_t payload[16], uint8_t command)
{
    write_bits(payload, MESSAGE_CMD_OFFSET, 2, command & 0x3);
}

uint8_t message_slot_for_id(uint8_t id)
{
    if (id < 1 || id > MESSAGE_SLOTS) {
        return MESSAGE_NO_SLOT;
    }
    return (uint8_t)(id - 1);
}
