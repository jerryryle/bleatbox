SHELL := /bin/bash

VENV := .venv
ACTIVATE := . $(VENV)/bin/activate

BOARD ?= adafruit_feather_nrf52840

.PHONY: build clean

build:
	$(ACTIVATE) && west build -b $(BOARD)

clean:
	rm -rf build
