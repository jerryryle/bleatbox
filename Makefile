SHELL := /bin/bash

VENV := .venv
ACTIVATE := . $(VENV)/bin/activate

BOARD ?= adafruit_feather_nrf52840/nrf52840/uf2

.PHONY: build clean test flash

build: test
	$(ACTIVATE) && west build -b $(BOARD)

clean:
	rm -rf build build-tests

test:
	cmake -S tests -B build-tests
	cmake --build build-tests
	ctest --test-dir build-tests --output-on-failure

flash:
	$(ACTIVATE) && west flash
