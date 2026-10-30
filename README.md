# nrf52840-hsm-firmware

Firmware for the nRF52840-based Hardware Security Module (HSM) Dongle.

## Key Features
- **Dual-Stage Secure Boot**: Immutable B0 bootloader (using ARM CryptoCell CC310) -> Upgradable MCUboot -> Signed firmware image.
- **Root Keys**: Signed with root-level ECDSA P-256 keys (`keys/root-ec-p256.pem`).
- **Dual USB CDC ACM**: Virtual COM Port 0 for Zephyr kernel logging and console, Port 1 for YubiHSM 2 binary command/response interactions.
- **Visual Status LED**: Pulsating blue when unconfigured, solid red when locked, solid green when authenticated/unlocked.
- **YubiHSM 2 API Support**: Implemented in pure logic library (`firmware/lib`).

## Quick Commands
```bash
make init          # Initialize Python virtualenv and generate root keys
make sync          # Synchronize Zephyr and external modules (depth=1)
make build-prod    # Build production signed firmware with sysbuild
make build-debug   # Build debug firmware with DEBUG logs
make flash         # Flash production image via JLink 802007333
make monitor       # Connect to USB console
make clean         # Clean build files
make distclean     # Clean build files, Zephyr modules, and .venv
```
