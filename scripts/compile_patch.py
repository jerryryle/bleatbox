#!/usr/bin/env python3
"""Compile a VS1053B .cmd patch file into the binary format expected by
    BleatBox.

Usage:
    uv run scripts/compile_patch.py <input.cmd> [<input2.cmd> ...]

Output files are written to build-patches/ with a .bin extension.

Binary format (repeated until EOF):
    [register: 1 byte] [count: 2 bytes BE] [data: count * 2 bytes BE]

Consecutive writes to the same register are merged into a single record.

Patch .cmd files are available from VLSI:
    https://www.vlsi.fi/en/support/software/vs10xxpatches.html
"""

import struct
import sys
from pathlib import Path


def parse_cmd(path: Path) -> list[tuple[int, int]]:
    """Parse a .cmd file into a list of (register, value) pairs."""
    writes: list[tuple[int, int]] = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if parts[0] != "W" or parts[1] != "2":
            continue
        reg = int(parts[2], 16)
        val = int(parts[3], 16)
        writes.append((reg, val))
    return writes


def encode(writes: list[tuple[int, int]]) -> bytes:
    """Run-length encode writes into the binary patch format."""
    out = bytearray()
    i = 0
    while i < len(writes):
        reg = writes[i][0]
        values: list[int] = []
        while i < len(writes) and writes[i][0] == reg:
            values.append(writes[i][1])
            i += 1
        out.append(reg)
        out.extend(struct.pack(">H", len(values)))
        for v in values:
            out.extend(struct.pack(">H", v))
    return bytes(out)


def main() -> None:
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <input.cmd> [...]", file=sys.stderr)
        sys.exit(1)

    out_dir = Path("build-patches")
    out_dir.mkdir(exist_ok=True)

    for arg in sys.argv[1:]:
        src = Path(arg)
        if not src.exists():
            print(f"Error: {src} not found", file=sys.stderr)
            sys.exit(1)

        writes = parse_cmd(src)
        data = encode(writes)

        dest = out_dir / src.with_suffix(".bin").name
        dest.write_bytes(data)
        print(f"{src} -> {dest} ({len(writes)} writes, {len(data)} bytes)")


if __name__ == "__main__":
    main()
