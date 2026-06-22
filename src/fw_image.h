#ifndef FW_IMAGE_H_
#define FW_IMAGE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * On-SD firmware image format for OTA.
 *
 * An OTA artifact is a 12-byte little-endian header followed by the raw
 * main-app image (the bytes that get written to the main_app flash
 * region).  The same format is produced by the host build step, validated
 * by the main app before it reboots to apply, and validated again by the
 * second-stage updater before it touches flash.
 *
 *   offset 0  magic         u32   FW_IMAGE_MAGIC
 *   offset 4  payload_len   u32   bytes of payload following the header
 *   offset 8  crc32         u32   CRC-32/IEEE over the payload bytes
 *
 * There is no version field: any valid image may be applied, so firmware
 * can be freely upgraded or downgraded.
 *
 * The struct is parsed/serialized field-by-field (not memcpy'd over the
 * raw bytes) so layout is independent of host struct packing and endianness.
 */

#define FW_IMAGE_MAGIC       0x584F4242u /* 'B','B','O','X' */
#define FW_IMAGE_HEADER_SIZE 12

/* Where the staged OTA image lives on the SD card (matches SDCARD_MOUNT_POINT). */
#define FW_IMAGE_SD_PATH "/SD:/bleatbox-update.bin"

struct fw_image_header {
    uint32_t magic;
    uint32_t payload_len;
    uint32_t crc32;
};

/* CRC-32/IEEE (poly 0xEDB88320, reflected, init/xorout 0xFFFFFFFF).
 * Matches Python's zlib.crc32 and Zephyr's crc32_ieee. */
uint32_t fw_crc32(const uint8_t *data, size_t len);

/*
 * Streaming form, for CRCing data that doesn't fit in RAM (e.g. a multi-
 * hundred-KB image read from SD in chunks).  Start from FW_CRC32_INIT,
 * feed chunks through fw_crc32_update(), then fw_crc32_final() the result.
 * fw_crc32(d, n) == fw_crc32_final(fw_crc32_update(FW_CRC32_INIT, d, n)).
 */
#define FW_CRC32_INIT 0xFFFFFFFFu
uint32_t fw_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

static inline uint32_t fw_crc32_final(uint32_t crc)
{
    return ~crc;
}

/* Read a 16-byte header from raw little-endian bytes. */
void fw_image_header_parse(const uint8_t raw[FW_IMAGE_HEADER_SIZE],
                           struct fw_image_header *out);

/* Write a 16-byte header as little-endian bytes. */
void fw_image_header_serialize(const struct fw_image_header *hdr,
                               uint8_t raw[FW_IMAGE_HEADER_SIZE]);

/*
 * Validate a parsed header against the payload bytes that follow it.
 * max_payload is the largest payload the caller can accept (e.g. the
 * size of the main_app flash region).  Returns 0 when the header magic
 * is correct, payload_len is non-zero and within max_payload, and the
 * CRC over payload matches.  Returns a negative errno otherwise.
 */
int fw_image_validate(const struct fw_image_header *hdr,
                      const uint8_t *payload, size_t max_payload);

#endif /* FW_IMAGE_H_ */
