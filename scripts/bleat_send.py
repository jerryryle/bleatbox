#!/usr/bin/env python3
"""
Send a BleatBox message from macOS over BLE.

BleatBox uses one message format on the air: a 16-byte payload of six
positional slots.  Devices exchange it as manufacturer-specific data, but
macOS CoreBluetooth can't advertise that (it silently drops it) — it can only
advertise Service UUIDs.  So this script packs the same 16-byte payload into a
128-bit Service UUID and advertises it alongside a fixed 16-bit marker UUID the
firmware recognizes.

A device in range re-emits it as the native mesh packet and relays it across
the whole herd, so you only need to be near *one* device. Each device plays the
slot matching its id (set `id N`, 1..6, in its bleatbox.cfg) — id N plays slot
N-1. A slot carries a 7-bit sound index and a 12-bit delay (ms); a single global
2-bit command applies to the whole message. Ids left unspecified are sent silent.

The 16-byte payload layout (canonical = on-air byte order the firmware reads,
bit 0 = LSB of byte 0, slots packed LSB-first) mirrors src/message.c exactly;
tests/message_test.cpp pins the two ends together:

    slot i -> bits [i*19 .. i*19+18]   (i = 0..5)
      within a slot:  [0..11]  delay_ms  (12 bits, 0..4095)
                      [12..18] sound      (7 bits; 0x7F = play nothing)
    bits [114..115] command   (2 bits, global; 00 = play)
    bits [116..123] seq        (8 bits, relay sequence)

Usage (with uv — installs dependencies automatically):
    uv run scripts/bleat_send.py --id 1 --sound 1 --delay 1000 \
                                 --id 6 --sound 3 --delay 1500

Each --id must be paired with a matching --sound and --delay (given in the
same order).  Without uv, install the CoreBluetooth framework yourself:
    pip install pyobjc-framework-CoreBluetooth
"""

# /// script
# requires-python = ">=3.9"
# dependencies = [
#     "pyobjc-framework-CoreBluetooth",
# ]
# ///

import argparse
import os
import sys

MESSAGE_SLOTS = 6
MESSAGE_SLOT_BITS = 19
MESSAGE_SOUND_SILENT = 0x7F
MESSAGE_CMD_OFFSET = MESSAGE_SLOTS * MESSAGE_SLOT_BITS  # 114
MESSAGE_SEQ_OFFSET = MESSAGE_CMD_OFFSET + 2             # 116
MARKER_UUID16 = "FB42"

# The relay dedup is keyed on (originator, seq); persist seq across runs so two
# invocations don't collide and get dropped as duplicates.
SEQ_STATE_FILE = os.path.expanduser("~/.bleatbox_msg_seq")

# Advertise for a short burst so passive scanners catch it, then stop.
BURST_SECONDS = 1.5


def write_bits(buf: bytearray, start_bit: int, num_bits: int, value: int) -> None:
    for i in range(num_bits):
        bit = start_bit + i
        if (value >> i) & 1:
            buf[bit >> 3] |= 1 << (bit & 7)


def pack_payload(slots: dict[int, tuple[int, int]], command: int, seq: int) -> bytes:
    """Build the 16-byte canonical payload.

    slots maps slot index -> (sound, delay_ms).  Unspecified slots get the
    silent sentinel.
    """
    buf = bytearray(16)
    for slot in range(MESSAGE_SLOTS):
        sound, delay = slots.get(slot, (MESSAGE_SOUND_SILENT, 0))
        field = (sound & 0x7F) << 12 | (delay & 0xFFF)
        write_bits(buf, slot * MESSAGE_SLOT_BITS, MESSAGE_SLOT_BITS, field)
    write_bits(buf, MESSAGE_CMD_OFFSET, 2, command & 0x3)
    write_bits(buf, MESSAGE_SEQ_OFFSET, 8, seq & 0xFF)
    return bytes(buf)


def payload_to_uuid_string(canonical: bytes) -> str:
    """Format the payload as a 128-bit UUID string.

    BLE transmits a 128-bit UUID least-significant-octet first, so the on-air
    bytes are the reverse of the UUID string bytes.  Reverse here so the
    firmware reads `canonical` directly off the air.
    """
    h = canonical[::-1].hex()
    return f"{h[0:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:32]}"


def next_seq() -> int:
    """Read, increment (mod 256), and persist the relay sequence number."""
    try:
        with open(SEQ_STATE_FILE) as f:
            seq = (int(f.read().strip()) + 1) & 0xFF
    except (OSError, ValueError):
        seq = 0
    try:
        with open(SEQ_STATE_FILE, "w") as f:
            f.write(str(seq))
    except OSError as e:
        print(f"warning: could not persist seq ({e})", file=sys.stderr)
    return seq


def parse_args() -> tuple[dict[int, tuple[int, int]], int]:
    p = argparse.ArgumentParser(description="Send a BleatBox message from macOS over BLE.")
    p.add_argument("--id", type=int, action="append", required=True, dest="ids",
                   help="device id 1..6 (repeatable)")
    p.add_argument("--sound", type=int, action="append", required=True,
                   help="sound index 0..127, or 127 for silent (repeatable)")
    p.add_argument("--delay", type=int, action="append", required=True,
                   help="delay in ms 0..4095 (repeatable)")
    p.add_argument("--command", type=int, default=0,
                   help="global command 0..3 (0 = play, others reserved)")
    args = p.parse_args()

    if not (len(args.ids) == len(args.sound) == len(args.delay)):
        p.error("--id, --sound and --delay must be given the same number of times")
    if not 0 <= args.command <= 3:
        p.error("--command out of range 0..3")

    slots: dict[int, tuple[int, int]] = {}
    for dev_id, sound, delay in zip(args.ids, args.sound, args.delay):
        if not 1 <= dev_id <= MESSAGE_SLOTS:
            p.error(f"id {dev_id} out of range 1..{MESSAGE_SLOTS}")
        if not 0 <= sound <= 0x7F:
            p.error(f"sound {sound} out of range 0..127")
        if not 0 <= delay <= 0xFFF:
            p.error(f"delay {delay} out of range 0..4095")
        slot = dev_id - 1
        if slot in slots:
            p.error(f"id {dev_id} specified twice")
        slots[slot] = (sound, delay)
    return slots, args.command


def advertise(payload_uuid: str) -> None:
    try:
        import objc
        from CoreBluetooth import (
            CBPeripheralManager,
            CBUUID,
            CBAdvertisementDataServiceUUIDsKey,
        )
        from Foundation import NSRunLoop, NSDate
    except ImportError as e:
        sys.exit(f"PyObjC CoreBluetooth not available ({e}).\n"
                 "Run with uv (auto-installs deps): uv run scripts/bleat_send.py ...\n"
                 "or install manually: pip install pyobjc-framework-CoreBluetooth")

    service_uuids = [
        CBUUID.UUIDWithString_(MARKER_UUID16),
        CBUUID.UUIDWithString_(payload_uuid),
    ]
    adv_data = {CBAdvertisementDataServiceUUIDsKey: service_uuids}

    class Delegate(objc.lookUpClass("NSObject")):
        def peripheralManagerDidUpdateState_(self, manager):
            if manager.state() == 5:  # CBManagerStatePoweredOn
                manager.startAdvertising_(adv_data)
            else:
                print(f"Bluetooth not ready (state={manager.state()}); "
                      "is BLE powered on and is this app allowed to use it?",
                      file=sys.stderr)

    delegate = Delegate.alloc().init()
    manager = CBPeripheralManager.alloc().initWithDelegate_queue_(delegate, None)

    deadline = NSDate.dateWithTimeIntervalSinceNow_(BURST_SECONDS)
    NSRunLoop.currentRunLoop().runUntilDate_(deadline)
    manager.stopAdvertising()


def main() -> None:
    slots, command = parse_args()
    seq = next_seq()
    payload = pack_payload(slots, command, seq)
    payload_uuid = payload_to_uuid_string(payload)

    active = ", ".join(f"id {s + 1}: sound {snd} @ {d} ms"
                       for s, (snd, d) in sorted(slots.items()))
    print(f"Sending [{active}]")
    print(f"  command={command} seq={seq}")
    print(f"  marker UUID  : {MARKER_UUID16}")
    print(f"  payload UUID : {payload_uuid}")
    print(f"  advertising for {BURST_SECONDS}s...")
    advertise(payload_uuid)
    print("done.")


if __name__ == "__main__":
    main()
