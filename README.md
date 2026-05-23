# Tissue Box — Networked Sound Trigger Firmware

Zephyr RTOS firmware for a network of Adafruit Feather nRF52840 Express boards
that trigger synchronized sound playback over BLE when any device detects
vibration.

## Hardware

- **MCU:** Adafruit Feather nRF52840 Express
- **Audio:** Adafruit Music Maker FeatherWing w/ Amp (VS1053B codec)
- **Trigger:** SW-18010P vibration switch wired to pin A0 (P0.04) and GND

### Wiring the vibration switch

Connect one leg of the SW-18010P to Feather pin **A0** and the other to **GND**.
The firmware enables the nRF52840's internal pull-up resistor, so no external
resistor is required.

### SD card

Format a microSD card as **FAT32**. Place MP3 files in the root directory named
`0.mp3`, `1.mp3`, ..., `9.mp3` (matching `NUM_SOUNDS`). Insert into the Music
Maker FeatherWing's card slot.

## macOS Development Setup

### 1. Install Homebrew dependencies

```bash
brew install cmake ninja gperf python3 ccache qemu dtc wget
pip3 install west
```

### 2. Install Zephyr SDK

```bash
cd ~
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.0/zephyr-sdk-0.17.0_macos-x86_64.tar.xz
# or for Apple Silicon:
# wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.0/zephyr-sdk-0.17.0_macos-aarch64.tar.xz
tar xf zephyr-sdk-0.17.0_macos-*.tar.xz
cd zephyr-sdk-0.17.0
./setup.sh
```

### 3. Initialize Zephyr workspace

```bash
west init ~/zephyrproject
cd ~/zephyrproject
west update
west zephyr-export
pip3 install -r ~/zephyrproject/zephyr/scripts/requirements.txt
```

### 4. Set environment

```bash
# Add to your shell profile (~/.zshrc):
export ZEPHYR_BASE=~/zephyrproject/zephyr
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.0
```

### 5. Build

```bash
cd /path/to/tissue-box
west build -b adafruit_feather_nrf52840 .
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

The nRF52840 provides a USB CDC ACM serial console for log output:

```bash
# Find the device:
ls /dev/cu.usbmodem*

# Connect (baud rate is irrelevant for USB CDC):
screen /dev/cu.usbmodem14101 115200
```

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
2. Set **Nrf Connect: Toolchain Path** to your Zephyr SDK:
   ```
   ~/zephyr-sdk-0.17.0
   ```
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
3. Log output appears in the integrated terminal — no need to leave VS Code.

## Identifying Device IDs (FICR)

Each nRF52840 has a factory-programmed unique device ID. This firmware uses
the lower byte of `FICR->DEVICEID[0]` as the 1-byte device ID.

### Method 1: Read from firmware log

Flash the firmware with placeholder `KNOWN_DEVICE_IDS[]` values. On boot, the
firmware logs:

```
[00:00:00.000,000] <inf> tissue_box: FICR Device ID: 0xAB
[00:00:00.000,000] <err> tissue_box: Device ID 0xAB NOT in KNOWN_DEVICE_IDS — halting
```

Record the `0xAB` value for each board.

### Method 2: Read via nrfjprog

If you have a J-Link probe:

```bash
nrfjprog --memrd 0x10000060 --n 4
# Output: 0x10000060: XXXXXXAB
#                             ^^ lower byte = device ID
```

### Method 3: Read via Python/adafruit-nrfutil

```bash
pip3 install adafruit-nrfutil
# Then read via the DFU serial interface (board-specific)
```

### Populating the device array

After collecting all device IDs, update `KNOWN_DEVICE_IDS[]` in `src/main.c`:

```c
static const uint8_t KNOWN_DEVICE_IDS[] = {
    0xAB, 0x3F, 0x72, /* ... one entry per physical board */
};
```

Rebuild and flash all devices with the same firmware.
