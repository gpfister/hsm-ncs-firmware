#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hsm/hsm_core.h>
#include "led_status.h"
#include "usb_serial.h"
#include "dfu_service.h"

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

static void on_hsm_state_changed(hsm_state_t state) {
    LOG_INF("HSM State changed to: %d", state);
    led_status_set_state(state);
}

int main(void) {
    LOG_INF("==================================================");
    LOG_INF(" nRF52840 HSM Dongle Firmware (PCA10059)");
    LOG_INF(" YubiHSM 2 API Compatible | Dual-Stage Secure Boot");
    LOG_INF("==================================================");

    /* 1. Initialize Visual LED Indicator */
    led_status_init();

    /* 2. Initialize DFU Flash Service */
    dfu_service_init();

    /* 3. Initialize HSM Logic Engine */
    hsm_callbacks_t cbs = {
        .on_state_changed = on_hsm_state_changed,
        .on_dfu_write = dfu_service_write_chunk,
        .on_dfu_apply = dfu_service_apply,
    };
    hsm_init(&cbs);

    /* 4. Initialize Dual USB CDC ACM */
    usb_serial_init();

    LOG_INF("System ready. Entering packet polling loop...");

    /* Main polling loop */
    while (1) {
        usb_serial_poll();
        k_msleep(2);
    }

    return 0;
}
