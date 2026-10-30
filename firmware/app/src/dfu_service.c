#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include "dfu_service.h"

LOG_MODULE_REGISTER(dfu_service, CONFIG_LOG_DEFAULT_LEVEL);

int dfu_service_init(void) {
    LOG_INF("DFU Service initialized for MCUboot secondary slot.");
    return 0;
}

int dfu_service_write_chunk(uint32_t offset, const uint8_t *data, size_t len) {
    const struct flash_area *fa = NULL;
    int rc = flash_area_open(FLASH_AREA_ID(image_1), &fa);
    if (rc != 0) {
        LOG_ERR("Failed to open flash area image_1: %d", rc);
        return rc;
    }

    if (offset == 0) {
        LOG_INF("Erasing secondary slot before write...");
        rc = flash_area_erase(fa, 0, fa->fa_size);
        if (rc != 0) {
            LOG_ERR("Failed to erase slot 1: %d", rc);
            flash_area_close(fa);
            return rc;
        }
    }

    rc = flash_area_write(fa, offset, data, len);
    flash_area_close(fa);

    if (rc != 0) {
        LOG_ERR("Failed to write chunk at offset 0x%x (len %zu): %d", offset, len, rc);
        return rc;
    }

    return 0;
}

int dfu_service_apply(void) {
    LOG_INF("Marking MCUboot upgrade image for test...");
    int rc = boot_request_upgrade(BOOT_UPGRADE_TEST);
    if (rc != 0) {
        LOG_ERR("boot_request_upgrade failed: %d", rc);
        return rc;
    }

    LOG_INF("Rebooting into MCUboot to swap images...");
    k_msleep(100);
    sys_reboot(SYS_REBOOT_COLD);
    return 0;
}
