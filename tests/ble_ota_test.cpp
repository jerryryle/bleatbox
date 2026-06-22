extern "C" {
#include "ble_ota.h"
}

#include <gtest/gtest.h>

TEST(BleOtaEncode, SetsCommandField)
{
    uint8_t payload[16];
    ble_ota_encode(payload);
    EXPECT_EQ(ble_ota_get_command(payload), BLE_OTA_CMD);
}

TEST(BleOtaIsArm, RoundTrip)
{
    uint8_t payload[16];
    ble_ota_encode(payload);
    EXPECT_TRUE(ble_ota_is_arm(payload));
}

TEST(BleOtaIsArm, RejectsPlayMessage)
{
    /* A play message has command 0; it must not read as an OTA arm. */
    uint8_t payload[16] = {0};
    EXPECT_FALSE(ble_ota_is_arm(payload));
}

TEST(BleOtaEncode, LeavesSeqAndReservedClear)
{
    /* The command field (byte 14, bits 2-3) is the only thing set above the
     * slots, so the seq byte and reserved nibble (byte 15) stay clear. */
    uint8_t payload[16];
    ble_ota_encode(payload);

    EXPECT_EQ(payload[14], BLE_OTA_CMD << 2); /* command bits only */
    EXPECT_EQ(payload[15], 0x00);             /* seq high + reserved */
}
