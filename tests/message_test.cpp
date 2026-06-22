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
    const uint8_t sounds[MESSAGE_SLOTS] = {1, 5, 9, 42, 62, 13};
    const uint16_t delays[MESSAGE_SLOTS] = {0, 250, 1000, 1500, 4095, 7};
    const uint8_t types[MESSAGE_SLOTS] = {MESSAGE_TYPE_GOAT, MESSAGE_TYPE_MISC,
                                          MESSAGE_TYPE_GOAT, MESSAGE_TYPE_MISC,
                                          MESSAGE_TYPE_MISC, MESSAGE_TYPE_GOAT};

    for (uint8_t s = 0; s < MESSAGE_SLOTS; s++) {
        message_pack_slot(payload, s, sounds[s], types[s], delays[s]);
    }

    for (uint8_t s = 0; s < MESSAGE_SLOTS; s++) {
        struct message_slot slot;
        message_parse_slot(payload, s, &slot);
        EXPECT_EQ(slot.sound, sounds[s]) << "slot " << (int)s;
        EXPECT_EQ(slot.delay_ms, delays[s]) << "slot " << (int)s;
        EXPECT_EQ(slot.type, types[s]) << "slot " << (int)s;
        EXPECT_TRUE(slot.play) << "slot " << (int)s;
    }
}

TEST(Message, SilentSentinelDoesNotPlay)
{
    uint8_t payload[16] = {0};
    message_pack_slot(payload, 3, MESSAGE_SOUND_SILENT, MESSAGE_TYPE_GOAT, 500);

    struct message_slot slot;
    message_parse_slot(payload, 3, &slot);
    EXPECT_EQ(slot.sound, MESSAGE_SOUND_SILENT);
    EXPECT_FALSE(slot.play);
}

TEST(Message, TypeBitRoundTrips)
{
    uint8_t payload[16] = {0};

    /* Same sound/delay, different type, in adjacent slots. */
    message_pack_slot(payload, 0, 5, MESSAGE_TYPE_GOAT, 100);
    message_pack_slot(payload, 1, 5, MESSAGE_TYPE_MISC, 100);

    struct message_slot g, m;
    message_parse_slot(payload, 0, &g);
    message_parse_slot(payload, 1, &m);

    EXPECT_EQ(g.type, MESSAGE_TYPE_GOAT);
    EXPECT_EQ(m.type, MESSAGE_TYPE_MISC);
    EXPECT_EQ(g.sound, 5);
    EXPECT_EQ(m.sound, 5);
    EXPECT_TRUE(g.play);
    EXPECT_TRUE(m.play);
}

TEST(Message, DelayBoundary)
{
    uint8_t payload[16] = {0};
    message_pack_slot(payload, 5, 2, MESSAGE_TYPE_GOAT, 4095);

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
    message_pack_slot(payload, 2, MESSAGE_SOUND_SILENT, MESSAGE_TYPE_MISC, 0xFFF);

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
    message_pack_slot(payload, MESSAGE_SLOTS, 3, MESSAGE_TYPE_GOAT, 100);
    EXPECT_EQ(memcmp(before, payload, sizeof(payload)), 0);
}

TEST(Message, PackClampsOutOfRangeFields)
{
    uint8_t payload[16] = {0};

    /* delay above the 12-bit max clamps to it. */
    message_pack_slot(payload, 0, 4, MESSAGE_TYPE_GOAT, 9999);
    struct message_slot s0;
    message_parse_slot(payload, 0, &s0);
    EXPECT_EQ(s0.delay_ms, MESSAGE_DELAY_MAX);
    EXPECT_EQ(s0.sound, 4);
    EXPECT_TRUE(s0.play);

    /* sound that doesn't fit 6 bits becomes silent, not a wrapped index. */
    message_pack_slot(payload, 1, 0x40, MESSAGE_TYPE_GOAT, 100); /* would wrap to 0 */
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

TEST(Message, SeqRoundTripsAcrossFull12Bits)
{
    for (uint16_t seq : {0, 1, 255, 256, 4094, 4095}) {
        uint8_t payload[16] = {0};
        message_set_seq(payload, seq);
        EXPECT_EQ(message_get_seq(payload), seq) << "seq " << (int)seq;
    }
}

TEST(Message, SeqDoesNotDisturbSlotsOrCommand)
{
    uint8_t payload[16] = {0};
    message_pack_slot(payload, 5, 7, MESSAGE_TYPE_MISC, 4095);
    message_set_command(payload, MESSAGE_CMD_PLAY);
    message_set_seq(payload, 0xFFF);

    EXPECT_EQ(message_get_seq(payload), 0xFFF);
    EXPECT_EQ(message_get_command(payload), MESSAGE_CMD_PLAY);

    struct message_slot s;
    message_parse_slot(payload, 5, &s);
    EXPECT_EQ(s.sound, 7);
    EXPECT_EQ(s.type, MESSAGE_TYPE_MISC);
    EXPECT_EQ(s.delay_ms, 4095);

    /* The other slots stay clean. */
    for (uint8_t i = 0; i < 5; i++) {
        message_parse_slot(payload, i, &s);
        EXPECT_EQ(s.sound, 0) << "slot " << (int)i;
        EXPECT_EQ(s.delay_ms, 0) << "slot " << (int)i;
    }
}

TEST(Message, CommandRoundTripsWithoutDisturbingSlots)
{
    for (uint8_t command = 0; command <= 3; command++) {
        uint8_t payload[16] = {0};
        message_set_command(payload, command);
        /* Loading slot 5 (highest) must not bleed into the command field;
         * its type bit (113) sits right below the command field (114). */
        message_pack_slot(payload, 5, MESSAGE_SOUND_SILENT, MESSAGE_TYPE_MISC,
                          0xFFF);

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
