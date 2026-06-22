#!/usr/bin/env python3
"""Merge several UF2 files into one for drag-drop provisioning.

A UF2 file is a flat sequence of 512-byte blocks, each carrying its own
target address. Merging is therefore just collecting every block from
every input and rewriting the blockNo/numBlocks counters so the combined
file is internally consistent for bootloaders that validate them.

Usage: merge_uf2.py OUT.uf2 IN1.uf2 IN2.uf2 [...]
"""

import struct
import sys

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
BLOCK_SIZE = 512

# Offsets of the two counter fields within a block.
OFF_BLOCK_NO = 0x14
OFF_NUM_BLOCKS = 0x18


def read_blocks(path):
    data = open(path, "rb").read()
    if len(data) % BLOCK_SIZE != 0:
        sys.exit(f"{path}: not a whole number of UF2 blocks")
    blocks = []
    for i in range(0, len(data), BLOCK_SIZE):
        block = bytearray(data[i:i + BLOCK_SIZE])
        start0, start1 = struct.unpack("<II", block[0:8])
        end = struct.unpack("<I", block[0x1FC:0x200])[0]
        if start0 != UF2_MAGIC_START0 or start1 != UF2_MAGIC_START1 or end != UF2_MAGIC_END:
            sys.exit(f"{path}: bad UF2 magic in block {i // BLOCK_SIZE}")
        blocks.append(block)
    return blocks


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    out_path, in_paths = argv[1], argv[2:]

    blocks = []
    for p in in_paths:
        blocks.extend(read_blocks(p))

    total = len(blocks)
    for n, block in enumerate(blocks):
        struct.pack_into("<I", block, OFF_BLOCK_NO, n)
        struct.pack_into("<I", block, OFF_NUM_BLOCKS, total)

    with open(out_path, "wb") as f:
        for block in blocks:
            f.write(block)

    print(f"merged {len(in_paths)} images, {total} blocks -> {out_path}")


if __name__ == "__main__":
    main(sys.argv)
