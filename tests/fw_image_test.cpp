extern "C" {
#include "fw_image.h"
}

#include <cstring>
#include <vector>

#include <gtest/gtest.h>

TEST(FwCrc32, MatchesStandardCheckValue)
{
    /* The canonical CRC-32/IEEE check value for "123456789". */
    const char *s = "123456789";
    EXPECT_EQ(fw_crc32(reinterpret_cast<const uint8_t *>(s), 9), 0xCBF43926u);
}

TEST(FwCrc32, EmptyInputIsZero)
{
    EXPECT_EQ(fw_crc32(nullptr, 0), 0u);
}

TEST(FwCrc32, StreamingMatchesOneShot)
{
    std::vector<uint8_t> data;
    for (int i = 0; i < 1000; i++) {
        data.push_back(static_cast<uint8_t>(i * 7 + 3));
    }

    /* Feed it in awkward chunk sizes — the running CRC must match. */
    uint32_t crc = FW_CRC32_INIT;
    size_t off = 0;
    for (size_t chunk : {1u, 7u, 64u, 333u, 595u}) {
        size_t n = std::min(chunk, data.size() - off);
        crc = fw_crc32_update(crc, data.data() + off, n);
        off += n;
    }
    crc = fw_crc32_update(crc, data.data() + off, data.size() - off);

    EXPECT_EQ(fw_crc32_final(crc), fw_crc32(data.data(), data.size()));
}

TEST(FwImageHeader, RoundTripsThroughRawBytes)
{
    struct fw_image_header in = {
        .magic = FW_IMAGE_MAGIC,
        .payload_len = 12345,
        .crc32 = 0xDEADBEEF,
    };
    uint8_t raw[FW_IMAGE_HEADER_SIZE];
    fw_image_header_serialize(&in, raw);

    /* Little-endian on the wire, independent of host layout. */
    EXPECT_EQ(raw[0], 0x42);
    EXPECT_EQ(raw[1], 0x42);
    EXPECT_EQ(raw[2], 0x4F);
    EXPECT_EQ(raw[3], 0x58);

    struct fw_image_header out;
    fw_image_header_parse(raw, &out);
    EXPECT_EQ(out.magic, in.magic);
    EXPECT_EQ(out.payload_len, in.payload_len);
    EXPECT_EQ(out.crc32, in.crc32);
}

static struct fw_image_header make_valid(const std::vector<uint8_t> &payload)
{
    struct fw_image_header h = {
        .magic = FW_IMAGE_MAGIC,
        .payload_len = static_cast<uint32_t>(payload.size()),
        .crc32 = fw_crc32(payload.data(), payload.size()),
    };
    return h;
}

TEST(FwImageValidate, AcceptsWellFormedImage)
{
    std::vector<uint8_t> payload = {1, 2, 3, 4, 5, 6, 7, 8};
    struct fw_image_header h = make_valid(payload);
    EXPECT_EQ(fw_image_validate(&h, payload.data(), 1024), 0);
}

TEST(FwImageValidate, RejectsBadMagic)
{
    std::vector<uint8_t> payload = {1, 2, 3, 4};
    struct fw_image_header h = make_valid(payload);
    h.magic = 0xBADBAD00;
    EXPECT_EQ(fw_image_validate(&h, payload.data(), 1024), -EINVAL);
}

TEST(FwImageValidate, RejectsZeroLength)
{
    std::vector<uint8_t> payload = {1, 2, 3, 4};
    struct fw_image_header h = make_valid(payload);
    h.payload_len = 0;
    EXPECT_EQ(fw_image_validate(&h, payload.data(), 1024), -EFBIG);
}

TEST(FwImageValidate, RejectsPayloadTooLargeForRegion)
{
    std::vector<uint8_t> payload(2048, 0xAB);
    struct fw_image_header h = make_valid(payload);
    EXPECT_EQ(fw_image_validate(&h, payload.data(), 1024), -EFBIG);
}

TEST(FwImageValidate, RejectsCorruptedPayload)
{
    std::vector<uint8_t> payload = {1, 2, 3, 4, 5, 6, 7, 8};
    struct fw_image_header h = make_valid(payload);
    payload[3] ^= 0xFF; /* flip a bit after the CRC was computed */
    EXPECT_EQ(fw_image_validate(&h, payload.data(), 1024), -EIO);
}
