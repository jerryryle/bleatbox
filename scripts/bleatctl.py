#!/usr/bin/env python3
"""
Control BleatBox devices from macOS over BLE — send a bleat or an OTA command.

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
                      [12..17] sound      (6 bits; 0x3F = play nothing)
                      [18]     type       (1 bit; 0 = goat, 1 = misc)
    bits [114..115] command   (2 bits, global; 00 = play)
    bits [116..127] seq        (12 bits, relay sequence)

Usage (with uv — installs dependencies automatically):
    uv run scripts/bleatctl.py --id 1 --sound 1 --delay 1000 \
                               --id 6 --sound 3 --delay 1500 --type misc

Each --id must be paired with a matching --sound and --delay (given in the
same order).  --type (goat|misc, default goat) is optional and applies to all
slots in the message — boxes only ever play goat; a Mac can request misc.
Without uv, install the CoreBluetooth framework yourself:
    pip install pyobjc-framework-CoreBluetooth

The same payload also arms a box for an over-the-air update (see the README,
"Over-the-Air Updates"); the upload and reboot then happen over SMP:
    uv run scripts/bleatctl.py --ota
"""

# /// script
# requires-python = ">=3.9"
# dependencies = [
#     "pyobjc-framework-CoreBluetooth",
# ]
# ///

import argparse
import random
import sys

MESSAGE_SLOTS = 6
MESSAGE_SLOT_BITS = 19
MESSAGE_SOUND_SILENT = 0x3F
MESSAGE_TYPE_GOAT = 0
MESSAGE_TYPE_MISC = 1
MESSAGE_CMD_OFFSET = MESSAGE_SLOTS * MESSAGE_SLOT_BITS  # 114
MESSAGE_SEQ_OFFSET = MESSAGE_CMD_OFFSET + 2             # 116
MESSAGE_SEQ_BITS = 12
MESSAGE_SEQ_MASK = 0x0FFF
MARKER_UUID16 = "FB42"

# OTA arming rides the same payload, marked by command 0b01.  Mirrors
# src/ble_ota.h.  An armed box becomes connectable for SMP; the operator
# drives the upload and reboot over that connection (see the README).
BLE_OTA_CMD = 0x01

# Advertise for a short burst so passive scanners catch it, then stop.
BURST_SECONDS = 1.5


def write_bits(buf: bytearray, start_bit: int, num_bits: int, value: int) -> None:
    for i in range(num_bits):
        bit = start_bit + i
        if (value >> i) & 1:
            buf[bit >> 3] |= 1 << (bit & 7)


def pack_payload(slots: dict[int, tuple[int, int]], type_: int,
                 command: int, seq: int) -> bytes:
    """Build the 16-byte canonical payload.

    slots maps slot index -> (sound, delay_ms).  Unspecified slots get the
    silent sentinel.  type_ (0 = goat, 1 = misc) applies to every slot.
    """
    buf = bytearray(16)
    for slot in range(MESSAGE_SLOTS):
        sound, delay = slots.get(slot, (MESSAGE_SOUND_SILENT, 0))
        field = (type_ & 0x1) << 18 | (sound & 0x3F) << 12 | (delay & 0xFFF)
        write_bits(buf, slot * MESSAGE_SLOT_BITS, MESSAGE_SLOT_BITS, field)
    write_bits(buf, MESSAGE_CMD_OFFSET, 2, command & 0x3)
    write_bits(buf, MESSAGE_SEQ_OFFSET, MESSAGE_SEQ_BITS, seq & MESSAGE_SEQ_MASK)
    return bytes(buf)


def pack_ota_payload(seq: int) -> bytes:
    """Build a 16-byte OTA-arm payload (mirrors ble_ota_encode)."""
    buf = bytearray(16)
    write_bits(buf, MESSAGE_CMD_OFFSET, 2, BLE_OTA_CMD)
    write_bits(buf, MESSAGE_SEQ_OFFSET, MESSAGE_SEQ_BITS, seq & MESSAGE_SEQ_MASK)
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
    """Pick a random 12-bit nonce for the (originator, seq) dedup key.

    The Mac sender is stateless per run, so it can't keep the firmware's
    incrementing counter; a random nonce needs no persisted state (which would
    restart at 0 when lost and collide with entries devices still hold). The
    firmware dedup only checks existence, so a random value is fine, and 12
    bits keeps collisions rare across the dedup ring.
    """
    return random.randint(0, MESSAGE_SEQ_MASK)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Control BleatBox devices from macOS over BLE "
                    "(send a bleat, or an OTA command).")
    p.add_argument("--id", type=int, action="append", dest="ids",
                   help="device id 1..6 (repeatable)")
    p.add_argument("--sound", type=int, action="append",
                   help="sound index 0..62, or 63 for silent (repeatable)")
    p.add_argument("--delay", type=int, action="append",
                   help="delay in ms 0..4095 (repeatable)")
    p.add_argument("--type", choices=("goat", "misc"), default="goat",
                   help="sound bank for all slots (default goat)")
    p.add_argument("--command", type=int, default=0,
                   help="global command 0..3 (0 = play, others reserved)")
    p.add_argument("--ota", action="store_true",
                   help="send an OTA-arm command instead of a bleat")
    args = p.parse_args()

    if args.ota:
        if args.ids or args.sound or args.delay:
            p.error("--ota cannot be combined with --id/--sound/--delay")
        return args

    if not args.ids or not args.sound or not args.delay:
        p.error("give --id/--sound/--delay (a bleat) or --ota (arm an update)")
    if not (len(args.ids) == len(args.sound) == len(args.delay)):
        p.error("--id, --sound and --delay must be given the same number of times")
    if not 0 <= args.command <= 3:
        p.error("--command out of range 0..3")

    slots: dict[int, tuple[int, int]] = {}
    for dev_id, sound, delay in zip(args.ids, args.sound, args.delay):
        if not 1 <= dev_id <= MESSAGE_SLOTS:
            p.error(f"id {dev_id} out of range 1..{MESSAGE_SLOTS}")
        if not 0 <= sound <= 0x3F:
            p.error(f"sound {sound} out of range 0..63")
        if not 0 <= delay <= 0xFFF:
            p.error(f"delay {delay} out of range 0..4095")
        slot = dev_id - 1
        if slot in slots:
            p.error(f"id {dev_id} specified twice")
        slots[slot] = (sound, delay)
    args.slots = slots
    return args


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
                 "Run with uv (auto-installs deps): uv run scripts/bleatctl.py ...\n"
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
    args = parse_args()
    seq = next_seq()

    if args.ota:
        payload = pack_ota_payload(seq)
        print("Sending OTA arm")
    else:
        type_ = MESSAGE_TYPE_MISC if args.type == "misc" else MESSAGE_TYPE_GOAT
        payload = pack_payload(args.slots, type_, args.command, seq)
        active = ", ".join(f"id {s + 1}: sound {snd} @ {d} ms"
                           for s, (snd, d) in sorted(args.slots.items()))
        print(f"Sending [{active}]")
        print(f"  type={args.type}")
        print(f"  command={args.command}")

    payload_uuid = payload_to_uuid_string(payload)
    print(f"  seq={seq}")
    print(f"  marker UUID  : {MARKER_UUID16}")
    print(f"  payload UUID : {payload_uuid}")
    print(f"  advertising for {BURST_SECONDS}s...")
    advertise(payload_uuid)
    print("done.")


if __name__ == "__main__":
    main()
