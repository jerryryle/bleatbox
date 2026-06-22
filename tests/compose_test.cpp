extern "C" {
#include "compose.h"
#include "message.h"

/*
 * Fake sys_rand32_get — returns values from a caller-controlled sequence so
 * tests can predict compose_message output.  compose_message draws two values
 * per slot (sound, then delay).
 */
static uint32_t fake_rand_values[64];
static int fake_rand_index;

uint32_t sys_rand32_get(void)
{
    return fake_rand_values[fake_rand_index++];
}
}

#include <gtest/gtest.h>

class ComposeTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        fake_rand_index = 0;
        memset(fake_rand_values, 0, sizeof(fake_rand_values));
    }
};

TEST_F(ComposeTest, FillsEverySlotAndSetsPlayCommand)
{
    /* 5 sounds, first assignable index 1 → range [1, 4]; delay [100, 500]. */
    compose_init(100, 500, 1, 5);

    /* Two rand draws per slot: sound then delay. */
    for (int s = 0; s < MESSAGE_SLOTS; s++) {
        fake_rand_values[s * 2] = (uint32_t)(1 + s % 4); /* sound */
        fake_rand_values[s * 2 + 1] = (uint32_t)(100 + s * 50); /* delay */
    }

    uint8_t payload[16];
    ASSERT_EQ(compose_message(payload), 0);

    EXPECT_EQ(message_get_command(payload), MESSAGE_CMD_PLAY);
    for (uint8_t s = 0; s < MESSAGE_SLOTS; s++) {
        struct message_slot slot;
        message_parse_slot(payload, s, &slot);
        EXPECT_EQ(slot.sound, (uint8_t)(1 + s % 4)) << "slot " << (int)s;
        EXPECT_EQ(slot.delay_ms, (uint16_t)(100 + s * 50)) << "slot " << (int)s;
    }
}

TEST_F(ComposeTest, SoundAndDelayStayInRange)
{
    compose_init(100, 500, 1, 5); /* sound range [1,4], delay [100,500] */

    /* Extreme rand values that would be out of range without wrapping. */
    for (int i = 0; i < MESSAGE_SLOTS * 2; i++) {
        fake_rand_values[i] = (i % 2 == 0) ? UINT32_MAX : 0;
    }

    uint8_t payload[16];
    ASSERT_EQ(compose_message(payload), 0);

    for (uint8_t s = 0; s < MESSAGE_SLOTS; s++) {
        struct message_slot slot;
        message_parse_slot(payload, s, &slot);
        EXPECT_GE(slot.sound, 1) << "slot " << (int)s;
        EXPECT_LE(slot.sound, 4) << "slot " << (int)s;
        EXPECT_GE(slot.delay_ms, 100) << "slot " << (int)s;
        EXPECT_LE(slot.delay_ms, 500) << "slot " << (int)s;
    }
}

TEST_F(ComposeTest, TooFewSoundsFails)
{
    uint8_t payload[16];

    compose_init(0, 100, 0, 0); /* 0 sounds */
    EXPECT_LT(compose_message(payload), 0);

    compose_init(0, 100, 0, 1); /* 1 sound */
    EXPECT_LT(compose_message(payload), 0);
}

TEST_F(ComposeTest, SilentSlotsArePossibleViaSentinelSound)
{
    /* When the assignable range includes the silent sentinel (0x3F), a slot
     * that lands there must read as silent. */
    compose_init(0, 0, MESSAGE_SOUND_SILENT, MESSAGE_SOUND_SILENT + 1);
    for (int i = 0; i < MESSAGE_SLOTS * 2; i++) {
        fake_rand_values[i] = MESSAGE_SOUND_SILENT;
    }

    uint8_t payload[16];
    ASSERT_EQ(compose_message(payload), 0);

    struct message_slot slot;
    message_parse_slot(payload, 0, &slot);
    EXPECT_EQ(slot.sound, MESSAGE_SOUND_SILENT);
    EXPECT_FALSE(slot.play);
}
