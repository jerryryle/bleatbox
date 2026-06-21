#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

extern "C" {
#include "message.h"
}

namespace {

TEST(Message, AllZeroSlotsPlaySoundZero)
{
    uint8_t payload[16] = {0};
    for (uint8_t s = 0; s < MESSAGE_SLOTS; s++) {
        struct message_slot slot;
        message_parse_slot(payload, s, &slot);
        EXPECT_EQ(slot.sound, 0);
        EXPECT_EQ(slot.delay_ms, 0);
        EXPECT_TRUE(slot.play);
    }
    EXPECT_EQ(message_get_command(payload), MESSAGE_CMD_PLAY);
    EXPECT_EQ(message_get_seq(payload), 0);
}

TEST(Message, EachSlotRoundTripsItsOwnFields)
{
    uint8_t payload[16] = {0};
    const uint8_t sounds[MESSAGE_SLOTS] = {1, 5, 9, 42, 100, 13};
    const uint16_t delays[MESSAGE_SLOTS] = {0, 250, 1000, 1500, 4095, 7};

    for (uint8_t s = 0; s < MESSAGE_SLOTS; s++) {
        message_pack_slot(payload, s, sounds[s], delays[s]);
    }

    for (uint8_t s = 0; s < MESSAGE_SLOTS; s++) {
        struct message_slot slot;
        message_parse_slot(payload, s, &slot);
        EXPECT_EQ(slot.sound, sounds[s]) << "slot " << (int)s;
        EXPECT_EQ(slot.delay_ms, delays[s]) << "slot " << (int)s;
        EXPECT_TRUE(slot.play) << "slot " << (int)s;
    }
}

TEST(Message, SilentSentinelDoesNotPlay)
{
    uint8_t payload[16] = {0};
    message_pack_slot(payload, 3, MESSAGE_SOUND_SILENT, 500);

    struct message_slot slot;
    message_parse_slot(payload, 3, &slot);
    EXPECT_EQ(slot.sound, MESSAGE_SOUND_SILENT);
    EXPECT_FALSE(slot.play);
}

TEST(Message, DelayBoundary)
{
    uint8_t payload[16] = {0};
    message_pack_slot(payload, 5, 2, 4095);

    struct message_slot slot;
    message_parse_slot(payload, 5, &slot);
    EXPECT_EQ(slot.delay_ms, 4095);
    EXPECT_EQ(slot.sound, 2);
    EXPECT_TRUE(slot.play);
}

TEST(Message, AdjacentSlotsAreIndependent)
{
    uint8_t payload[16] = {0};
    /* Fill slot 2 with all-ones in every field; neighbors must stay clean. */
    message_pack_slot(payload, 2, 0x7F, 0xFFF);

    struct message_slot lo, hi;
    message_parse_slot(payload, 1, &lo);
    message_parse_slot(payload, 3, &hi);

    EXPECT_EQ(lo.sound, 0);
    EXPECT_EQ(lo.delay_ms, 0);
    EXPECT_EQ(hi.sound, 0);
    EXPECT_EQ(hi.delay_ms, 0);
}

TEST(Message, OutOfRangeSlotDoesNotPlay)
{
    uint8_t payload[16];
    memset(payload, 0xFF, sizeof(payload));

    struct message_slot slot;
    message_parse_slot(payload, MESSAGE_SLOTS, &slot);
    EXPECT_FALSE(slot.play);

    /* Packing an out-of-range slot must not corrupt the buffer. */
    uint8_t before[16];
    memcpy(before, payload, sizeof(payload));
    message_pack_slot(payload, MESSAGE_SLOTS, 3, 100);
    EXPECT_EQ(memcmp(before, payload, sizeof(payload)), 0);
}

TEST(Message, PackClampsOutOfRangeFields)
{
    uint8_t payload[16] = {0};

    /* delay above the 12-bit max clamps to it. */
    message_pack_slot(payload, 0, 4, 9999);
    struct message_slot s0;
    message_parse_slot(payload, 0, &s0);
    EXPECT_EQ(s0.delay_ms, MESSAGE_DELAY_MAX);
    EXPECT_EQ(s0.sound, 4);
    EXPECT_TRUE(s0.play);

    /* sound that doesn't fit 7 bits becomes silent, not a wrapped index. */
    message_pack_slot(payload, 1, 0x80, 100); /* would wrap to 0 */
    struct message_slot s1;
    message_parse_slot(payload, 1, &s1);
    EXPECT_EQ(s1.sound, MESSAGE_SOUND_SILENT);
    EXPECT_FALSE(s1.play);
    EXPECT_EQ(s1.delay_ms, 100);
}

TEST(Message, SlotForId)
{
    /* 1-based ids 1..MESSAGE_SLOTS map to slots 0..MESSAGE_SLOTS-1. */
    EXPECT_EQ(message_slot_for_id(1), 0);
    EXPECT_EQ(message_slot_for_id(6), MESSAGE_SLOTS - 1);

    /* Id 0 and ids beyond the slot count have no slot (relay only). */
    EXPECT_EQ(message_slot_for_id(0), MESSAGE_NO_SLOT);
    EXPECT_EQ(message_slot_for_id(MESSAGE_SLOTS + 1), MESSAGE_NO_SLOT);
    EXPECT_EQ(message_slot_for_id(255), MESSAGE_NO_SLOT);

    /* A no-slot id parses as a non-playing slot. */
    uint8_t payload[16] = {0};
    struct message_slot s;
    message_parse_slot(payload, message_slot_for_id(0), &s);
    EXPECT_FALSE(s.play);
}

TEST(Message, CommandRoundTripsWithoutDisturbingSlots)
{
    for (uint8_t command = 0; command <= 3; command++) {
        uint8_t payload[16] = {0};
        message_set_command(payload, command);
        /* Loading slot 5 (highest) must not bleed into the command field. */
        message_pack_slot(payload, 5, 0x7F, 0xFFF);

        EXPECT_EQ(message_get_command(payload), command);

        for (uint8_t s = 0; s < 5; s++) {
            struct message_slot slot;
            message_parse_slot(payload, s, &slot);
            EXPECT_EQ(slot.sound, 0) << "slot " << (int)s;
            EXPECT_EQ(slot.delay_ms, 0) << "slot " << (int)s;
        }
    }
}

} // namespace
