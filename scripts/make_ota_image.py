#!/usr/bin/env python3
"""Wrap a raw app image in the BleatBox OTA header.

Produces bleatbox-update.bin = 12-byte header + raw main-app image, the
artifact an operator uploads to a box's SD card with `mcumgr fs upload`.

Header (little-endian, matches src/fw_image.h):
    magic:u32  payload_len:u32  crc32:u32

Usage: make_ota_image.py APP.bin OUT.bin
"""

import struct
import sys
import zlib

FW_IMAGE_MAGIC = 0x584F4242


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)

    app_bin, out_path = argv[1], argv[2]

    payload = open(app_bin, "rb").read()
    if not payload:
        sys.exit(f"{app_bin} is empty")

    crc = zlib.crc32(payload) & 0xFFFFFFFF
    header = struct.pack("<III", FW_IMAGE_MAGIC, len(payload), crc)

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(payload)

    print(f"wrote {out_path}: payload={len(payload)} crc=0x{crc:08x}")


if __name__ == "__main__":
    main(sys.argv)
