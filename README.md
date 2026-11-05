# nrf52840-hsm-firmware

Firmware for the nRF52840-based Hardware Security Module (HSM) Dongle (`PCA10059`), built with the nRF Connect SDK v3.4.1 and Zephyr RTOS with sysbuild.

---

## Key Features

- **Dual-Stage Secure Boot**: Immutable B0 bootloader (using ARM CryptoCell CC310) $\rightarrow$ Upgradable MCUboot $\rightarrow$ Signed application image.
- **Cryptographic Hardware Isolation & Zeroization**: ARM CryptoCell CC310 hardware TRNG and ECDSA P-256 acceleration. Full hardware zeroization (`AT+RESET` / `AT+FACTORYRESET`) wiping all 32 CC310 persistent key slots, RAM Shamir master secret, restoring default PIN and retries, and writing unconfigured state to NVS.
- **Dynamic BLE Address USB Serial**: Reads the factory-burned 48-bit Bluetooth MAC address from `NRF_FICR->DEVICEADDR[1:0]` and formats it as a 12-digit hex serial (`%04X%08X`) dynamically registered via `usb_update_sn_string_descriptor()`.
- **Dual USB CDC-ACM Composite Interfaces**:
  - `cdc_acm_uart0` (Interface 0/1): Zephyr kernel console and logger (Protocol ID: `0x0001`, macOS node: `/dev/tty.usbmodem<BLE_ADDR>1`).
  - `cdc_acm_uart1` (Interface 2/3): HSM AT command & cryptographic operation channel (Protocol ID: `0x0001` per ADR-0013, macOS node: `/dev/tty.usbmodem<BLE_ADDR>3`).
- **Runtime Descriptor Patching**: Traversing `__usb_descriptor_start` dynamically sets interface protocol identifiers (`0x01` ITU-T V.250 for both CDC-ACM interfaces per ADR-0013 for zero-driver macOS `AppleUSBACMControl` dual enumeration) and user string descriptors (`"Zephyr CDC ACM Console"` and `"nRF52840 HSM AT Interface"`) without modifying upstream Zephyr trees.
- **Visual Status LED (PCA10059 RGB)**:
  - Pulsating Blue: Unconfigured / Genesis state.
  - Solid Red: Configured but locked.
  - Solid Green: Authenticated and unlocked for cryptographic operations.

---

## Firmware DFU Streaming (`AT+DFU_INFO?`, `AT+DFU_CHUNK`, `AT+DFU_APPLY`)

> [!NOTE]
> **DFU Streaming Architecture**:
> The firmware AT command engine (`AT`, `AT+INFO?`, `AT+STATUS?`, `AT+AUTH`, `AT+LOCK`, `AT+RESET`, `AT+FACTORYRESET`, `AT+KEYGEN`, `AT+SIGN`, `AT+PUBKEY`, `AT+RANDOM`, `AT+DFU_INFO?`, `AT+DFU_CHUNK`, `AT+DFU_APPLY`) runs on CDC-ACM UART1 (`1024`-byte line parser buffer). `AT+DFU_CHUNK=<offset>,<hex_data>` streams up to 256-byte chunks into MCUboot secondary slot (`image_1`), `AT+DFU_APPLY` schedules a test upgrade (`BOOT_UPGRADE_TEST`) and deferred cold reboot, and `dfu_service_init()` automatically confirms the new image (`boot_write_img_confirmed()`) upon successful boot.

---

## Build & Flash Commands

```bash
make init          # Initialize Python virtualenv and verify tools
make sync          # Synchronize Zephyr and external modules (depth=1)
make build-prod    # Build production signed firmware with sysbuild
make build-debug   # Build debug firmware with detailed console logging
make flash         # Flash production image via JLink (S/N: 802007333)
make monitor       # Connect to USB serial console (115200 8N1)
make clean         # Clean build files
make distclean     # Clean build files, Zephyr modules, and .venv
```
