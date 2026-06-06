extern "C" {
#include "ble_encode.h"
}

#include <gtest/gtest.h>

TEST(BleEncode, GoatTypeHasBit7Clear)
{
    uint8_t encoded = ble_sound_encode(SOUND_TYPE_GOAT, 5);
    EXPECT_EQ(encoded, 0x05);
    EXPECT_EQ(encoded & BLE_SOUND_TYPE_MASK, 0);
}

TEST(BleEncode, MiscTypeHasBit7Set)
{
    uint8_t encoded = ble_sound_encode(SOUND_TYPE_MISC, 5);
    EXPECT_EQ(encoded, 0x85);
    EXPECT_NE(encoded & BLE_SOUND_TYPE_MASK, 0);
}

TEST(BleEncode, IndexZero)
{
    EXPECT_EQ(ble_sound_encode(SOUND_TYPE_GOAT, 0), 0x00);
    EXPECT_EQ(ble_sound_encode(SOUND_TYPE_MISC, 0), 0x80);
}

TEST(BleEncode, MaxIndex)
{
    EXPECT_EQ(ble_sound_encode(SOUND_TYPE_GOAT, 127), 0x7F);
    EXPECT_EQ(ble_sound_encode(SOUND_TYPE_MISC, 127), 0xFF);
}

TEST(BleDecode, RoundTripGoat)
{
    for (uint8_t i = 0; i < 128; i++) {
        uint8_t encoded = ble_sound_encode(SOUND_TYPE_GOAT, i);
        EXPECT_EQ(ble_sound_decode_type(encoded), SOUND_TYPE_GOAT);
        EXPECT_EQ(ble_sound_decode_index(encoded), i);
    }
}

TEST(BleDecode, RoundTripMisc)
{
    for (uint8_t i = 0; i < 128; i++) {
        uint8_t encoded = ble_sound_encode(SOUND_TYPE_MISC, i);
        EXPECT_EQ(ble_sound_decode_type(encoded), SOUND_TYPE_MISC);
        EXPECT_EQ(ble_sound_decode_index(encoded), i);
    }
}

TEST(BleDecode, IndexMaskIgnoresTypeBit)
{
    EXPECT_EQ(ble_sound_decode_index(0x85), 5);
    EXPECT_EQ(ble_sound_decode_index(0x05), 5);
}
