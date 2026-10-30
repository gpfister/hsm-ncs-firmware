# ==============================================================================
# nrf52840-hsm-firmware Makefile
# NCS workspace firmware lifecycle management
# ==============================================================================

SHELL := /bin/bash
.DEFAULT_GOAL := help

UV ?= uv
WEST ?= $(UV) run west
BOARD ?= nrf52840dongle/nrf52840/bare
APP_DIR := firmware/app

BUILD_DIR_PROD := build-prod
BUILD_DIR_DEBUG := build-debug

JLINK_SNR ?= 802007333
PORT ?= $(shell ls /dev/cu.usbmodem* 2>/dev/null | head -n 1)
BAUD ?= 115200

ROOT_DIR := $(abspath ..)
KEYS_DIR := $(ROOT_DIR)/keys
SIGN_KEY := $(KEYS_DIR)/root-ec-p256.pem

.PHONY: help init sync build build-prod build-debug flash flash-debug monitor clean distclean

help:
	@echo "======================================================================="
	@echo "                   nRF52840 HSM Firmware Build System                  "
	@echo "======================================================================="
	@echo "Usage: make <target> [VARIABLES]"
	@echo ""
	@echo "Workspace & Dependencies:"
	@echo "  make init                 Initialize UV virtualenv and ensure root keys exist"
	@echo "  make sync                 Synchronize Zephyr modules (west update -o depth=1)"
	@echo ""
	@echo "Building:"
	@echo "  make build                Build production firmware (alias for build-prod)"
	@echo "  make build-prod           Build production firmware (sysbuild, dual-stage B0 + MCUboot)"
	@echo "  make build-debug          Build debug firmware (DEBUG log level, asserts)"
	@echo ""
	@echo "Flashing & Monitoring:"
	@echo "  make flash                Flash production merged image via J-Link $(JLINK_SNR)"
	@echo "  make flash-debug          Flash debug merged image via J-Link $(JLINK_SNR)"
	@echo "  make monitor              Open serial console on $(PORT) at $(BAUD) baud"
	@echo ""
	@echo "Cleaning:"
	@echo "  make clean                Remove build directories only"
	@echo "  make distclean            Remove build directories, Zephyr modules, and .venv"
	@echo "======================================================================="

init:
	@echo "[INFO] Initializing Python virtual environment using uv..."
	@$(UV) sync
	@if [ ! -f "$(SIGN_KEY)" ]; then \
		echo "[INFO] Root key not found, generating via $(ROOT_DIR)/scripts/generate_keys.sh..."; \
		$(ROOT_DIR)/scripts/generate_keys.sh; \
	fi
	@echo "[SUCCESS] Initialization complete."

sync: init
	@echo "[INFO] Running 'west update -o depth=1' to synchronize Zephyr modules..."
	@if [ ! -d ".west" ]; then \
		$(WEST) init -l firmware; \
	fi
	@$(WEST) update -o=--depth=1
	@echo "[INFO] Exporting Zephyr CMake configuration..."
	@$(WEST) zephyr-export
	@echo "[SUCCESS] Workspace synchronized."

build: build-prod

build-prod:
	@echo "=================================================="
	@echo " Building Production Firmware (Board: $(BOARD))"
	@echo "=================================================="
	@if [ ! -f "$(SIGN_KEY)" ]; then $(ROOT_DIR)/scripts/generate_keys.sh; fi
	@mkdir -p bootloader && ln -sfn ../../keys bootloader/keys
	@mkdir -p $(BUILD_DIR_PROD) && ln -sfn ../../keys $(BUILD_DIR_PROD)/keys
	@$(WEST) build --sysbuild -b $(BOARD) -d $(BUILD_DIR_PROD) $(APP_DIR)
	@echo "[SUCCESS] Production build complete in $(BUILD_DIR_PROD)/"

build-debug:
	@echo "=================================================="
	@echo " Building Debug Firmware (Board: $(BOARD))"
	@echo "=================================================="
	@if [ ! -f "$(SIGN_KEY)" ]; then $(ROOT_DIR)/scripts/generate_keys.sh; fi
	@mkdir -p bootloader && ln -sfn ../../keys bootloader/keys
	@mkdir -p $(BUILD_DIR_DEBUG) && ln -sfn ../../keys $(BUILD_DIR_DEBUG)/keys
	@$(WEST) build --sysbuild -b $(BOARD) -d $(BUILD_DIR_DEBUG) $(APP_DIR) -- -DEXTRA_CONF_FILE=$(abspath $(APP_DIR)/debug.conf)
	@echo "[SUCCESS] Debug build complete in $(BUILD_DIR_DEBUG)/"

flash:
	@echo "=================================================="
	@echo " Flashing Production Image via J-Link $(JLINK_SNR)"
	@echo "=================================================="
	@if [ -f "$(BUILD_DIR_PROD)/merged.hex" ]; then \
		nrfutil device program --serial-number $(JLINK_SNR) --firmware "$(BUILD_DIR_PROD)/merged.hex" --options chip_erase_mode=ERASE_ALL; \
		nrfutil device reset --serial-number $(JLINK_SNR); \
	else \
		echo "[INFO] merged.hex not found, attempting west flash..."; \
		$(WEST) flash -d $(BUILD_DIR_PROD) --snr $(JLINK_SNR); \
	fi
	@echo "[SUCCESS] Flashing complete."

flash-debug:
	@echo "=================================================="
	@echo " Flashing Debug Image via J-Link $(JLINK_SNR)"
	@echo "=================================================="
	@if [ -f "$(BUILD_DIR_DEBUG)/merged.hex" ]; then \
		nrfutil device program --serial-number $(JLINK_SNR) --firmware "$(BUILD_DIR_DEBUG)/merged.hex" --options chip_erase_mode=ERASE_ALL; \
		nrfutil device reset --serial-number $(JLINK_SNR); \
	else \
		$(WEST) flash -d $(BUILD_DIR_DEBUG) --snr $(JLINK_SNR); \
	fi
	@echo "[SUCCESS] Flashing complete."

monitor:
	@if [ -z "$(PORT)" ]; then \
		echo "[ERROR] No serial port detected. Connect Dongle and specify PORT=/dev/..."; exit 1; \
	fi
	@echo "[INFO] Monitoring $(PORT) at $(BAUD) baud (Press Ctrl-] or Ctrl-C to exit)..."
	@python3 -m serial.tools.miniterm $(PORT) $(BAUD) || screen $(PORT) $(BAUD)

clean:
	@echo "[INFO] Cleaning build directories..."
	@rm -rf $(BUILD_DIR_PROD) $(BUILD_DIR_DEBUG)
	@echo "[SUCCESS] Clean complete."

distclean: clean
	@echo "[INFO] Removing Zephyr workspace modules and virtualenv..."
	@rm -rf .venv .west zephyr modules bootloader nrf nrfxlib tools test
	@echo "[SUCCESS] Distclean complete."
