#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hsm/hsm_core.h>
#include <hsm/crypto_cc310.h>
#include <hsm/keystore.h>
#include <hsm/at_commands.h>
#include "led_status.h"
#include "usb_serial.h"
#include "dfu_service.h"

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

static void on_keystore_state_changed(bool authenticated, uint8_t retries, uint8_t key_count) {
    LOG_INF("Keystore state changed: auth=%d, retries=%u, keys=%u",
            authenticated ? 1 : 0, retries, key_count);

    if (retries == 0) {
        /* Blocked / Lockout -> Solid RED */
        led_status_set_state(HSM_STATE_LOCKED);
    } else if (authenticated) {
        /* Authenticated / Unlocked -> Solid GREEN */
        led_status_set_state(HSM_STATE_UNLOCKED);
    } else if (key_count == 0 && !keystore_is_shamir_initialized()) {
        /* Unconfigured / Empty -> Breathing WHITE */
        led_status_set_state(HSM_STATE_UNCONFIGURED);
    } else {
        /* Configured but locked -> Solid RED */
        led_status_set_state(HSM_STATE_LOCKED);
    }
}

int main(void) {
    LOG_INF("==================================================");
    LOG_INF(" nRF52840 HSM Dongle Firmware (PCA10059)");
    LOG_INF(" AT Protocol Engine | Dual-Stage Secure Boot");
    LOG_INF(" Arm CryptoCell CC310 Hardware Acceleration");
    LOG_INF("==================================================");

    /* 1. Initialize Visual LED Indicator */
    led_status_init();

    /* 2. Initialize CryptoCell CC310 / PSA Crypto */
    crypto_cc310_init();

    /* 3. Initialize Persistent Keystore & Auth Supervisor */
    keystore_init(on_keystore_state_changed);

    /* 4. Initialize AT Command Processor */
    at_commands_init();

    /* 5. Initialize DFU Flash Service & Register AT Callbacks */
    dfu_service_init();
    static const at_dfu_callbacks_t dfu_cbs = {
        .write_chunk = dfu_service_write_chunk,
        .apply = dfu_service_apply,
        .get_info = dfu_service_get_info,
    };
    at_commands_register_dfu_callbacks(&dfu_cbs);

    /* 6. Initialize Dual USB CDC ACM (Console + AT Protocol) */
    usb_serial_init();
    LOG_INF("Dongle USB Serial (BLE Address): %s", usb_serial_get_ble_address());

    LOG_INF("HSM AT Protocol Engine ready. Entering polling loop...");

    /* Main polling loop */
    while (1) {
        usb_serial_poll();
        k_msleep(2);
    }

    return 0;
}
