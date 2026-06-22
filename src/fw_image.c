/*
 * Firmware image header + CRC-32 helpers (see fw_image.h).
 *
 * Deliberately free of Zephyr dependencies so the exact same code links
 * into the main app, the second-stage updater, and the host unit tests.
 */

#include <errno.h>

#include "fw_image.h"

uint32_t fw_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }

    return crc;
}

uint32_t fw_crc32(const uint8_t *data, size_t len)
{
    return fw_crc32_final(fw_crc32_update(FW_CRC32_INIT, data, len));
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

void fw_image_header_parse(const uint8_t raw[FW_IMAGE_HEADER_SIZE],
                           struct fw_image_header *out)
{
    out->magic = read_le32(&raw[0]);
    out->payload_len = read_le32(&raw[4]);
    out->crc32 = read_le32(&raw[8]);
}

void fw_image_header_serialize(const struct fw_image_header *hdr,
                               uint8_t raw[FW_IMAGE_HEADER_SIZE])
{
    write_le32(&raw[0], hdr->magic);
    write_le32(&raw[4], hdr->payload_len);
    write_le32(&raw[8], hdr->crc32);
}

int fw_image_validate(const struct fw_image_header *hdr,
                      const uint8_t *payload, size_t max_payload)
{
    if (hdr->magic != FW_IMAGE_MAGIC) {
        return -EINVAL;
    }

    if (hdr->payload_len == 0 || hdr->payload_len > max_payload) {
        return -EFBIG;
    }

    if (fw_crc32(payload, hdr->payload_len) != hdr->crc32) {
        return -EIO;
    }

    return 0;
}
