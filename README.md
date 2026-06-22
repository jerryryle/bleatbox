# BleatBox — Networked Sound Trigger Firmware

Zephyr RTOS firmware for a network of Adafruit Feather nRF52840 Express boards
that trigger synchronized sound playback over BLE when any device detects
vibration.

## Hardware

- **MCU:** Adafruit Feather nRF52840 Express
- **Audio:** Adafruit Music Maker FeatherWing w/ Amp (VS1053B codec)
- **Trigger:** LIS2DW12 accelerometer on I2C0, INT1 on pin A0 (P0.04)

### Wiring the accelerometer

Connect a LIS2DW12 breakout board to the Feather header:

| Breakout Pin | Feather Pin | Notes |
|---|---|---|
| VIN | 3V | 3.3V supply |
| GND | GND | Ground |
| SDA | SDA | I2C data |
| SCL | SCL | I2C clock |
| SDO/SA0 | 3V | Sets I2C address to 0x19 |
| INT1 | A0 | Wakeup interrupt |

### SD card

Format a microSD card as **FAT32**. Place mp3 files in the root directory named
with a `goat` prefix and zero-padded two-digit indices: `goat00.mp3`,
`goat01.mp3`, ..., up to `goat99.mp3`. The set must be contiguous from
`goat00.mp3` — a gap fails the boot-time scan. An optional second set with a
`misc` prefix (`misc00.mp3`, ...) follows the same rules and is playable via
the shell. `goat00.mp3` is the sound a device plays for its own vibration;
the rest are assigned randomly to peers.

The firmware scans the card at boot and auto-discovers available sounds. The SD
card also holds the device configuration file (see
[Device Configuration](#device-configuration) below) and an optional VS1053B
codec patch (`patch.bin`, compiled by `scripts/compile_patch.py`, applied on
every codec power-up if present). Insert into the Music Maker FeatherWing's
card slot.

## macOS Development Setup

### 1. Install Homebrew dependencies

```bash
brew install cmake ninja gperf python3 python-tk ccache qemu dtc libmagic wget openocd uv
```

### 2. Create a Python virtual environment

West and all Zephyr Python dependencies live in a venv to avoid polluting
your system Python. From inside the project folder:

```bash
uv venv
source .venv/bin/activate
uv pip install west
uv pip install pip
```

> **Note:** You must `source .venv/bin/activate` (from the project root) in
> every new terminal session before running `west` directly. The `make`
> targets activate it for you.

### 3. Initialize Zephyr workspace

```bash
west init
west update
west zephyr-export
west packages pip --install
```

### 4. Install Zephyr SDK

The SDK (cross-compiler toolchains, host tools) is installed via `west`:

```bash
west sdk install
```

### 5. Build

```bash
make build
```

To start fresh:

```bash
make clean
```

### Tests

Unit tests build and run on the host with CMake/CTest (no hardware needed):

```bash
make test
```

### 6. Flash

The Adafruit Feather nRF52840 Express ships with a UF2 bootloader, not a
J-Link, so flashing is over USB:

1. Enter the UF2 bootloader, either way:
   - **Double-tap the RESET button** on the Feather, or
   - if the box is already running BleatBox firmware and connected over USB,
     run `bootloader` at the [serial shell](#serial-console-from-vs-code)
     (handy once the enclosure is sealed and the button is hard to reach).

   The onboard NeoPixel turns green and a `FTHR840BOOT` USB drive appears.
2. Run:
   ```bash
   make flash
   ```

`make flash` builds the firmware and copies the merged provisioning image
(`build/bleatbox-provision.uf2`) onto the mounted drive; the board then
auto-resets and runs it. That image bundles **both** the second-stage updater
and the app — flashing the app alone would brick the box, since the bootloader
would jump to a missing updater (see
[Over-the-Air Updates](#over-the-air-updates) for the flash layout).

Once a box has been provisioned over USB this way, you can update it **over the
air** with no cable at all — see [Over-the-Air Updates](#over-the-air-updates).

If the `uf2` runner can't find the drive, copy the image by hand instead:

```bash
cp build/bleatbox-provision.uf2 /Volumes/FTHR840BOOT/
```

**Known macOS issue:** Spotlight indexing can interfere with the UF2 bootloader
volume. If the `FTHR840BOOT` drive ejects immediately after mounting, disable
Spotlight indexing for it:

```bash
sudo mdutil -i off /Volumes/FTHR840BOOT
```

If the volume still misbehaves, try using a USB 2.0 hub (some USB 3.0
controllers on Macs cause timing issues with the nRF52840 USB bootloader).

### 7. Serial console

Log output and an interactive shell are available over USB CDC ACM. Connect the Feather via USB and open the serial port:

```bash
# Find the device:
ls /dev/cu.usbmodem*

# Connect at 115200 baud:
screen /dev/cu.usbmodem* 115200
```

If no USB host is attached, log output is silently discarded until a terminal connects.

See [Shell Commands](#shell-commands) below for the available interactive commands.

## VS Code Setup

### Recommended extensions

Install the **nRF Connect for VS Code Extension Pack** from the VS Code
marketplace. This single pack installs:

- **nRF Connect for VS Code** — build system integration, board/SOC selection
- **nRF Terminal** — integrated serial terminal
- **nRF DeviceTree** — syntax highlighting and validation for `.dts`/`.overlay`
- **Cortex-Debug** — ARM debugger integration (if using a debug probe)

### Configure nRF Connect extension

1. Open VS Code settings (`Cmd+,`) and search for `nrf-connect`.
2. Set **Nrf Connect: Toolchain Path** to the SDK installed by `west sdk install`.
   The default location is:
   ```
   ~/zephyr-sdk-1.0.1
   ```
   (Run `west sdk list` to confirm the path on your machine.)
3. Set **Nrf Connect: Zephyr Base** (if not auto-detected) to the `zephyr/`
   directory inside this repo's west workspace:
   ```
   <repo root>/zephyr
   ```

### Build from VS Code

1. Open the nRF Connect sidebar panel (Nordic logo in the activity bar).
2. Click **Add Build Configuration**.
3. Select board: `adafruit_feather_nrf52840`.
4. Select the application directory: `app/` (not the repo root). Enable
   **sysbuild** so the updater image is built alongside the app.
5. Click **Build Configuration** — this runs `west build --sysbuild` internally.

### Serial console from VS Code

1. Open the nRF Terminal panel (bottom bar or via the command palette:
   `nRF Terminal: Start Terminal`).
2. Select the USB CDC ACM port.
3. Log output and the interactive shell appear in the integrated terminal.

## Device Configuration

Each device reads its identity from a config file on the SD card at boot. This
means every device runs the same firmware — only the SD card contents differ.

### Config file format

Create a plain text file named `bleatbox.cfg` in the root of the SD card:

```
# This device's ID (one hex byte; 0xFE is reserved)
id 01

# Playback volume (0 = silent, 100 = max, default 80)
volume 80

# Random delay range in ms before assigned playback (defaults 0-2000)
delay_min 0
delay_max 2000

# Accelerometer wakeup threshold in milli-g (default 200)
accel_threshold 150

# Max relay hops for mesh rebroadcast, 0 disables relaying (default 2)
relay_ttl 2
```

- **`id`** — a single hex byte that uniquely identifies this device; it must
  differ across devices, and `0xFE` is reserved. It serves two roles: it stamps
  the device's own broadcasts so the mesh can deduplicate them, and it is the
  device's **1-based position** — ids `1`–`6` play that slot (`id - 1`) in every
  BLE message. An id of `0` or above `6` plays nothing it receives, but still
  relays. Up to six devices play.
- **`volume`** *(optional)* — playback volume, 0–100. Defaults to 80 if
  omitted.
- **`delay_min`** / **`delay_max`** *(optional)* — bounds in milliseconds for
  the random playback delay assigned to each slot when this device triggers.
  Default 0–2000; `delay_min` must not exceed `delay_max`.
- **`accel_threshold`** *(optional)* — wakeup detection threshold in
  milli-g. Defaults to 200 if omitted. Use the `accel` shell command to find
  the right value for your setup.
- **`relay_ttl`** *(optional)* — maximum hops a broadcast is rebroadcast by
  receiving devices, extending range beyond a single radio hop. 0 disables
  relaying. Defaults to 2.
- Lines starting with `#` are comments.
- Hex values accept a `0x` prefix or bare hex digits.

Only `id` is required. All other fields are optional. If the file is missing or
malformed, the firmware logs an error and halts.

### Provisioning workflow

1. Give each device a unique 1-byte ID. The playing devices are `0x01`–`0x06`
   (id `N` plays slot `N - 1`); extra relay-only devices can use any other id
   except the reserved `0xFE`.
2. Copy the file to the device's SD card alongside the mp3 files.
3. Boot the device. The log output confirms the loaded configuration:
   ```
   [00:00:00.xxx,xxx] <inf> device_config: Device ID: 0x01
   [00:00:00.xxx,xxx] <inf> ble: BLE slot: 0
   ```

To change a device's configuration, update `bleatbox.cfg` on its SD card and
reboot.

### Messaging format and sending from macOS

Every BLE message is the same 16-byte payload of six positional slots — a 6-bit
sound index, a 1-bit goat/misc type, and a 12-bit delay each, plus a global
2-bit command and a 12-bit sequence number. A tissue pull fills the slots with
random goat sounds and broadcasts it; each device plays the slot matching its id
(`id - 1`). Up to six devices are addressed.

Devices exchange this as manufacturer-specific data, which macOS CoreBluetooth
cannot send (it silently drops it). It *can* advertise 128-bit Service UUIDs, so
the same payload is sent from a Mac as a 128-bit UUID alongside a fixed 16-bit
marker UUID. The first device to hear it re-broadcasts it as the normal mesh
packet, so it **relays across the whole herd** — you only need to be near one
device:

```
uv run scripts/bleatctl.py --id 1 --sound 1 --delay 1000 \
                           --id 6 --sound 3 --delay 1500
```

The script declares its dependency inline ([PEP 723](https://peps.python.org/pep-0723/)),
so [`uv run`](https://docs.astral.sh/uv/) fetches PyObjC into an ephemeral
environment automatically — no manual install or virtualenv needed. (Without uv:
`pip install pyobjc-framework-CoreBluetooth` then run with `python`.)

A `--sound` of `63` — or any id you don't name — stays silent. Add `--type misc`
to play from the misc bank instead of goat (boxes only ever compose goat
themselves; misc is a Mac-only trigger). The sender advertises a short burst
with a random per-message sequence nonce, so repeated sends fire once each.

## Shell Commands

An interactive shell is available over USB serial (see
[Serial console](#7-serial-console)). These commands work regardless of whether
device provisioning succeeds.

| Command | Description |
|---------|-------------|
| `play goat\|misc <index>` | Play sound `goatNN.mp3` / `miscNN.mp3` from the SD card |
| `volume [0-100]` | Get the playback volume, or set it (runtime only, does not persist) |
| `accel [count]` | Sample accelerometer at 100 Hz and print XYZ in milli-g (default 200 samples) |
| `sinetest on\|off` | Play the VS1053B's built-in sine test tone |
| `ota status\|arm\|cancel` | Inspect or drive an over-the-air update window (see [Over-the-Air Updates](#over-the-air-updates)) |

## Over-the-Air Updates

Deployed boxes update over BLE — no USB, no MCUboot, and without touching the
Adafruit UF2 bootloader. Three parts work together:

1. **A second-stage updater** (`updater/`) that the UF2 bootloader launches at
   `0x26000`. On every boot it checks the SD card for a staged image, flashes it
   to the main app region, and chain-loads the app at `0x3e000`.
2. **SMP-over-BLE file transfer** (the SMP File Management group) drops a new
   image onto a box's SD card over the air.
3. **A BLE arm trigger** — the same 16-byte message a bleat uses — opens an
   update window on a box in range, making it connectable for SMP. (It is *not*
   relayed across the mesh: an armed box stops scanning and relaying so its
   connectable advertisement is stable, so you update one box at a time within
   radio range.)

### Flash layout

| Region   | Address  | Size   |
|----------|----------|--------|
| updater  | 0x26000  | 96 KB  |
| main_app | 0x3e000  | 696 KB |
| storage  | 0xec000  | 32 KB  |
| UF2 boot | 0xf4000  | 48 KB  |

The UF2 boot partition is never written, so double-press-reset → USB drag-drop
always recovers a box.

### Build artifacts

`make` produces, under `build/`:

- `bleatbox-provision.uf2` — updater + app merged, for **initial** flashing via
  UF2 drag-drop (or `make flash`). Flashing only the app would brick the box,
  since `0x26000` would hold no updater.
- `bleatbox-update.bin` — app image plus a 12-byte header (`common/fw_image.h`), the
  artifact pushed over the air. There is no version field: any valid image is
  applied, so firmware can be freely upgraded **or downgraded**.

### Updating a deployed box

The operator tool is [`smpmgr`](https://github.com/intercreate/smpmgr) (an SMP
client with BLE support that works on macOS). It needs no install — `uvx` runs
it on demand.

The quick path, standing within BLE range of the box, is one command:

```
make ota 4        # build, arm, upload, and reboot device 4
```

That runs the steps below; here they are spelled out:

1. Build the firmware you want to install with `make`.
2. **Arm** the box — from a Mac:
   ```
   uv run scripts/bleatctl.py --ota
   ```
   It opens a ~5-minute window and advertises connectably as `bleatbox-dfu-<id>`
   (its 1-based device id, 1–6). (The `ota arm` shell command does the same
   locally, for bench testing.)
3. **Upload** the image onto the box's SD card over BLE (here device 4):
   ```
   uvx smpmgr --timeout 30 --ble bleatbox-dfu-4 \
       file upload build/bleatbox-update.bin /SD:/bleatbox-update.bin
   ```
4. **Commit** — reboot it so the updater applies the staged image:
   ```
   uvx smpmgr --ble bleatbox-dfu-4 os reset
   ```

Repeat per box (an armed box doesn't relay, so there's no swarm-wide broadcast).
The updater validates the image CRC **before** erasing the old app and deletes
the staged file only after a verified write, so an interrupted flash just retries
on the next boot — and a power loss never bricks the box, since double-press-reset
→ USB drag-drop always recovers it.

**macOS name caching:** macOS caches a peripheral's name, so a box you connected
to before setting names may still show as `Zephyr`. `--ble` accepts the
peripheral's address too; find it by scanning (e.g. `BleakScanner.discover()`),
or clear the cache with `sudo pkill bluetoothd`.

### Trigger format

The OTA-arm command rides inside the standard 16-byte message payload, so a Mac
sends it exactly like a bleat — it just claims one of the reserved values of the
global 2-bit command field (see `app/src/ble_ota.h`):

```
command field (bits 114..115) = 0b01   OTA arm
```

It is acted on locally and **never relayed** — a relay burst would disrupt the
box's own connectable advertising — so it only reaches boxes in direct range.

> **Security:** the trigger and the uploaded image are unauthenticated, matching
> the project's connectionless-mesh stance. A hostile broadcaster could open a
> window and push firmware; the image-header CRC guards against corruption, not
> malice, and USB drag-drop is the backstop.
