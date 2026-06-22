SHELL := /bin/bash

VENV := .venv
ACTIVATE := . $(VENV)/bin/activate

BOARD ?= adafruit_feather_nrf52840/nrf52840/uf2

# Sysbuild builds two images: the main app (build/bleatbox) and the
# second-stage updater (build/updater). Provisioning needs BOTH — the UF2
# bootloader launches the updater at 0x26000, which chain-loads the app at
# 0x3e000. Flashing only the app would brick the box.
PROVISION_UF2 := build/bleatbox-provision.uf2
UPDATER_UF2 := build/updater/zephyr/zephyr.uf2
APP_UF2 := build/bleatbox/zephyr/zephyr.uf2
APP_BIN := build/bleatbox/zephyr/zephyr.bin
OTA_IMAGE := build/bleatbox-update.bin
OTA_SD_PATH := /SD:/bleatbox-update.bin

# UF2 bootloader mass-storage volume (Adafruit Feather nRF52840), where
# `make flash` drops the provisioning image. Override for a different mount.
UF2_VOLUME ?= /Volumes/FTHR840BOOT

.PHONY: build clean test flash provision-uf2 ota-image ota 1 2 3 4 5 6

build: test
	$(ACTIVATE) && west build --sysbuild -b $(BOARD)
	$(MAKE) provision-uf2 ota-image

# Single drag-drop artifact containing updater + app (initial provisioning).
provision-uf2:
	$(ACTIVATE) && python3 scripts/merge_uf2.py $(PROVISION_UF2) $(UPDATER_UF2) $(APP_UF2)

# OTA artifact: app image + header, for `smpmgr file upload` to the SD card.
ota-image:
	$(ACTIVATE) && python3 scripts/make_ota_image.py $(APP_BIN) $(OTA_IMAGE)

clean:
	rm -rf build build-tests

test:
	cmake -S tests -B build-tests
	cmake --build build-tests
	ctest --test-dir build-tests --output-on-failure

# Initial provisioning: double-press reset to enter UF2 mode, then flash
# the merged image (updater + app) by copying it to the bootloader volume.
# We can't use `west flash`: its uf2 runner has no --file option and can't
# flash two sysbuild images in one pass (the board resets after the first).
# OTA updates do not use this path.
flash: build
	@test -d "$(UF2_VOLUME)" || { \
		echo "$(UF2_VOLUME) not mounted — double-tap RESET to enter UF2 mode first."; \
		exit 1; \
	}
	cp -X $(PROVISION_UF2) "$(UF2_VOLUME)/"
	@echo "Copied $(PROVISION_UF2) to $(UF2_VOLUME) — the board will auto-reset."

# Over-the-air update of one box by device id, e.g. `make ota 4`.
# Builds the image, broadcasts an OTA-arm (opens a window on boxes in range),
# then uploads to and reboots only the named box. Needs `smpmgr` (run via uvx)
# and BLE permission for the terminal. macOS may show the box as "Zephyr" the
# first time — connecting by name still works once its name is cached.
ota: build
	@id="$(word 2,$(MAKECMDGOALS))"; \
	if ! echo "$$id" | grep -qE '^[1-6]$$'; then \
		echo "usage: make ota <device-id 1-6>   (e.g. make ota 4)"; exit 1; \
	fi; \
	name="bleatbox-dfu-$$id"; \
	echo "==> Arming, then updating $$name"; \
	uv run scripts/bleatctl.py --ota && \
	sleep 2 && \
	uvx smpmgr --timeout 30 --ble "$$name" file upload $(OTA_IMAGE) $(OTA_SD_PATH) && \
	uvx smpmgr --ble "$$name" os reset && \
	echo "==> $$name updated; it will reboot and apply the image"

# Swallow the numeric id so `make ota 4` doesn't error trying to build `4`.
1 2 3 4 5 6:
	@:
