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

By default, log output goes to the **hardware UART** on the Feather header pins
TX (P0.25) and RX (P0.24). Connect a 3.3V USB-to-UART adapter (e.g. FTDI
FT232R or CP2102) to these pins and GND:

```bash
# Find the device:
ls /dev/cu.usbmodem* /dev/cu.usbserial*

# Connect at 115200 baud:
screen /dev/cu.usbserial-0001 115200
```

**Optional: USB CDC ACM (log output over the Feather's USB connector)**

Add to `prj.conf`:
```
CONFIG_USB_DEVICE_STACK=y
CONFIG_UART_LINE_CTRL=y
```

This enables USB serial — logs appear on `/dev/cu.usbmodem*` when the Feather
is connected via USB. No external adapter needed, but the USB peripheral draws
~1 mA, which matters for battery-powered operation.

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
