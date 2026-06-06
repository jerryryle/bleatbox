extern "C" {
#include "device_config_parse.h"
}

#include <gtest/gtest.h>
#include <cstring>

/* ------------------------------------------------------------------ */
/* parse_hex_byte                                                     */
/* ------------------------------------------------------------------ */

TEST(ParseHexByte, ValidLowercase)
{
    uint8_t out = 0;
    EXPECT_EQ(device_config_parse_hex_byte("ff", &out), 0);
    EXPECT_EQ(out, 0xFF);
}

TEST(ParseHexByte, ValidUppercase)
{
    uint8_t out = 0;
    EXPECT_EQ(device_config_parse_hex_byte("FF", &out), 0);
    EXPECT_EQ(out, 0xFF);
}

TEST(ParseHexByte, ValidZero)
{
    uint8_t out = 0xFF;
    EXPECT_EQ(device_config_parse_hex_byte("00", &out), 0);
    EXPECT_EQ(out, 0x00);
}

TEST(ParseHexByte, ValidMixed)
{
    uint8_t out = 0;
    EXPECT_EQ(device_config_parse_hex_byte("0A", &out), 0);
    EXPECT_EQ(out, 0x0A);
}

TEST(ParseHexByte, TooLarge)
{
    uint8_t out = 0;
    EXPECT_NE(device_config_parse_hex_byte("100", &out), 0);
}

TEST(ParseHexByte, InvalidChars)
{
    uint8_t out = 0;
    EXPECT_NE(device_config_parse_hex_byte("GG", &out), 0);
}

TEST(ParseHexByte, EmptyString)
{
    uint8_t out = 0;
    EXPECT_NE(device_config_parse_hex_byte("", &out), 0);
}

/* ------------------------------------------------------------------ */
/* parse_line                                                         */
/* ------------------------------------------------------------------ */

class ParseLineTest : public ::testing::Test {
protected:
    struct device_config cfg;
    bool has_id;

    void SetUp() override
    {
        memset(&cfg, 0, sizeof(cfg));
        has_id = false;
    }

    int parse(const char *line)
    {
        char buf[256];
        strncpy(buf, line, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        return device_config_parse_line(buf, &cfg, &has_id);
    }
};

TEST_F(ParseLineTest, CommentLine)
{
    EXPECT_EQ(parse("# this is a comment"), 0);
}

TEST_F(ParseLineTest, BlankLine)
{
    EXPECT_EQ(parse(""), 0);
    EXPECT_EQ(parse("\n"), 0);
    EXPECT_EQ(parse("  \t  "), 0);
}

TEST_F(ParseLineTest, IdValid)
{
    EXPECT_EQ(parse("id 0A"), 0);
    EXPECT_TRUE(has_id);
    EXPECT_EQ(cfg.id, 0x0A);
}

TEST_F(ParseLineTest, IdMissingValue)
{
    EXPECT_NE(parse("id"), 0);
}

TEST_F(ParseLineTest, IdInvalidHex)
{
    EXPECT_NE(parse("id ZZ"), 0);
}

TEST_F(ParseLineTest, PeersSingle)
{
    EXPECT_EQ(parse("peers 01"), 0);
    EXPECT_EQ(cfg.peer_count, 1);
    EXPECT_EQ(cfg.peers[0], 0x01);
}

TEST_F(ParseLineTest, PeersMultiple)
{
    EXPECT_EQ(parse("peers 01 02 FF"), 0);
    EXPECT_EQ(cfg.peer_count, 3);
    EXPECT_EQ(cfg.peers[0], 0x01);
    EXPECT_EQ(cfg.peers[1], 0x02);
    EXPECT_EQ(cfg.peers[2], 0xFF);
}

TEST_F(ParseLineTest, PeersEmpty)
{
    EXPECT_NE(parse("peers"), 0);
}

TEST_F(ParseLineTest, PeersOverflow)
{
    char line[256] = "peers";
    for (int i = 0; i < DEVICE_CONFIG_MAX_PEERS + 1; i++) {
        char hex[4];
        snprintf(hex, sizeof(hex), " %02X", i & 0xFF);
        strncat(line, hex, sizeof(line) - strlen(line) - 1);
    }
    EXPECT_NE(parse(line), 0);
}

TEST_F(ParseLineTest, VolumeValid)
{
    EXPECT_EQ(parse("volume 50"), 0);
    EXPECT_EQ(cfg.volume, 50);
}

TEST_F(ParseLineTest, VolumeBounds)
{
    EXPECT_EQ(parse("volume 0"), 0);
    EXPECT_EQ(cfg.volume, 0);

    EXPECT_EQ(parse("volume 100"), 0);
    EXPECT_EQ(cfg.volume, 100);
}

TEST_F(ParseLineTest, VolumeTooHigh)
{
    EXPECT_NE(parse("volume 101"), 0);
}

TEST_F(ParseLineTest, VolumeMissing)
{
    EXPECT_NE(parse("volume"), 0);
}

TEST_F(ParseLineTest, DelayMinValid)
{
    EXPECT_EQ(parse("delay_min 500"), 0);
    EXPECT_EQ(cfg.delay_min_ms, 500);
}

TEST_F(ParseLineTest, DelayMaxValid)
{
    EXPECT_EQ(parse("delay_max 2000"), 0);
    EXPECT_EQ(cfg.delay_max_ms, 2000);
}

TEST_F(ParseLineTest, DelayMinMissing)
{
    EXPECT_NE(parse("delay_min"), 0);
}

TEST_F(ParseLineTest, AccelThresholdValid)
{
    EXPECT_EQ(parse("accel_threshold 200"), 0);
    EXPECT_EQ(cfg.accel_threshold_mg, 200);
}

TEST_F(ParseLineTest, AccelThresholdZeroInvalid)
{
    EXPECT_NE(parse("accel_threshold 0"), 0);
}

TEST_F(ParseLineTest, AccelThresholdMissing)
{
    EXPECT_NE(parse("accel_threshold"), 0);
}

TEST_F(ParseLineTest, RelayTtlValid)
{
    EXPECT_EQ(parse("relay_ttl 3"), 0);
    EXPECT_EQ(cfg.relay_ttl, 3);
}

TEST_F(ParseLineTest, RelayTtlMax)
{
    EXPECT_EQ(parse("relay_ttl 255"), 0);
    EXPECT_EQ(cfg.relay_ttl, 255);
}

TEST_F(ParseLineTest, RelayTtlTooHigh)
{
    EXPECT_NE(parse("relay_ttl 256"), 0);
}

TEST_F(ParseLineTest, UnknownKeyAccepted)
{
    EXPECT_EQ(parse("unknown_key value"), 0);
}

TEST_F(ParseLineTest, LeadingWhitespace)
{
    EXPECT_EQ(parse("  \t  volume 75"), 0);
    EXPECT_EQ(cfg.volume, 75);
}
