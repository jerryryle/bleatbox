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
with zero-padded two-digit indices: `00.mp3`, `01.mp3`, ..., up to `99.mp3`.
The firmware scans the card at boot and auto-discovers available sounds. The SD
card also holds the device configuration file (see
[Device Configuration](#device-configuration) below). Insert into the Music
Maker FeatherWing's card slot.

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

> **Note:** You must `source ~/zephyrproject/.venv/bin/activate` in every new
> terminal session before running `west`. Add it to your `~/.zshrc` if you
> prefer it automatic.

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
J-Link. There are two ways to flash:

#### Option A: UF2 drag-and-drop (recommended, no extra tools)

1. Double-tap the RESET button on the Feather. The onboard NeoPixel turns
   green and a `FTHR840BOOT` USB drive appears.
2. Copy the UF2 file:
   ```bash
   cp build/zephyr/zephyr.uf2 /Volumes/FTHR840BOOT/
   ```
   The board auto-resets and runs the new firmware.

**Known macOS issue:** Spotlight indexing can interfere with the UF2 bootloader
volume. If the `FTHR840BOOT` drive ejects immediately after mounting, disable
Spotlight indexing for it:

```bash
sudo mdutil -i off /Volumes/FTHR840BOOT
```

If the volume still misbehaves, try using a USB 2.0 hub (some USB 3.0
controllers on Macs cause timing issues with the nRF52840 USB bootloader).

#### Option B: west flash with a debug probe

If you have a J-Link or CMSIS-DAP probe connected to the SWD header:

```bash
west flash
```

This requires `nrfjprog` (Nordic's CLI tools) or `openocd`.

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
   ~/.local/zephyr-sdk-1.0.1
   ```
   (Run `west sdk list` to confirm the path on your machine.)
3. Set **Nrf Connect: Zephyr Base** (if not auto-detected):
   ```
   ~/zephyrproject/zephyr
   ```

### Build from VS Code

1. Open the nRF Connect sidebar panel (Nordic logo in the activity bar).
2. Click **Add Build Configuration**.
3. Select board: `adafruit_feather_nrf52840`.
4. Select the project directory (this repo root).
5. Click **Build Configuration** — this runs `west build` internally.

### Flash from VS Code

1. In the nRF Connect panel, click the **Flash** button on your build
   configuration.
2. For UF2 bootloader boards, the extension may prompt you to double-tap
   reset. Follow the on-screen instructions.

### Serial console from VS Code

1. Open the nRF Terminal panel (bottom bar or via the command palette:
   `nRF Terminal: Start Terminal`).
2. Select the USB CDC ACM port.
3. Log output and the interactive shell appear in the integrated terminal.

## Device Configuration

Each device reads its identity and peer list from a config file on the SD card
at boot. This means every device runs the same firmware — only the SD card
contents differ.

### Config file format

Create a plain text file named `bleatbox.cfg` in the root of the SD card:

```
# This device's ID (one hex byte)
id 0xa3

# IDs of the other devices in the network
peers 0x01 0x42 0xb7 0x05 0x9c

# Playback volume (0 = silent, 100 = max, default 80)
volume 80

# Accelerometer wakeup threshold in milli-g (default 200)
accel_threshold 150
```

- **`id`** — a single hex byte (`0x00`–`0xFF`) that uniquely identifies this
  device on the network.
- **`peers`** — space-separated hex bytes for every *other* device in the
  network. Up to 30 peers are supported.
- **`volume`** *(optional)* — playback volume, 0–100. Defaults to 80 if
  omitted.
- **`accel_threshold`** *(optional)* — wakeup detection threshold in
  milli-g. Defaults to 200 if omitted. Use the `accel` shell command to find
  the right value for your setup.
- Lines starting with `#` are comments.
- Hex values accept a `0x` prefix or bare hex digits.

Both `id` and `peers` are required. All other fields are optional. If the file
is missing or malformed, the firmware logs an error and halts.

### Provisioning workflow

1. Choose a unique 1-byte ID for each device (e.g. `0x01` through `0x05` for a
   five-device network).
2. For each device, create a `bleatbox.cfg` with that device's `id` and the IDs
   of all the *other* devices as `peers`.
3. Copy the file to the device's SD card alongside the mp3 files.
4. Boot the device. The log output confirms the loaded configuration:
   ```
   [00:00:00.xxx,xxx] <inf> device_config: Device ID: 0xa3
   [00:00:00.xxx,xxx] <inf> device_config: Peers:
                                           a3 01 42 b7 05 9c
   ```

To change a device's configuration, update `bleatbox.cfg` on its SD card and
reboot.

## Shell Commands

An interactive shell is available over USB serial (see
[Serial console](#7-serial-console)). These commands work regardless of whether
device provisioning succeeds.

| Command | Description |
|---------|-------------|
| `bleatbox play <index>` | Play sound file `<index>.mp3` from the SD card |
| `bleatbox volume <0-100>` | Set playback volume (runtime only, does not persist) |
| `bleatbox accel [count]` | Sample accelerometer at 100 Hz and print XYZ in milli-g (default 200 samples) |
