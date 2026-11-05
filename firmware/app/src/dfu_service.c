#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <psa/crypto.h>
#include <string.h>
#include "dfu_service.h"

LOG_MODULE_REGISTER(dfu_service, CONFIG_LOG_DEFAULT_LEVEL);

static struct k_work_delayable s_reboot_work;

static void reboot_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    LOG_INF("Executing deferred cold reboot into MCUboot...");
    sys_reboot(SYS_REBOOT_COLD);
}

int dfu_service_init(void) {
    k_work_init_delayable(&s_reboot_work, reboot_work_handler);

    if (!boot_is_img_confirmed()) {
        int rc = boot_write_img_confirmed();
        if (rc != 0) {
            LOG_ERR("Failed to confirm running MCUboot image: %d", rc);
        } else {
            LOG_INF("Running MCUboot image confirmed as permanent.");
        }
    }

    LOG_INF("DFU Service initialized for MCUboot secondary slot.");
    return 0;
}

int dfu_service_get_info(uint8_t *major, uint8_t *minor, uint8_t *patch,
                         uint32_t *active_slot, uint32_t *image_size, uint8_t sha256[32]) {
    if (!major || !minor || !patch || !active_slot || !image_size || !sha256) {
        return -1;
    }

    *major = 1;
    *minor = 0;
    *patch = 0;
    *active_slot = 0;
    *image_size = 0;
    memset(sha256, 0, 32);

    const struct flash_area *fa = NULL;
    int rc = flash_area_open(FLASH_AREA_ID(image_0), &fa);
    if (rc != 0 || !fa) {
        LOG_ERR("Failed to open flash area image_0: %d", rc);
        return rc != 0 ? rc : -1;
    }

    uint32_t size_to_hash = (uint32_t)fa->fa_size;
    struct mcuboot_img_header hdr = {0};
    if (boot_read_bank_header(FLASH_AREA_ID(image_0), &hdr, sizeof(hdr)) == 0 &&
        hdr.mcuboot_version == 1 &&
        hdr.h.v1.image_size > 0 &&
        hdr.h.v1.image_size <= fa->fa_size) {
        size_to_hash = hdr.h.v1.image_size;
    }
    *image_size = size_to_hash;

    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    psa_status_t status = psa_hash_setup(&op, PSA_ALG_SHA_256);
    if (status == PSA_SUCCESS) {
        uint8_t buf[256];
        uint32_t offset = 0;
        while (offset < size_to_hash) {
            uint32_t chunk = size_to_hash - offset;
            if (chunk > sizeof(buf)) {
                chunk = sizeof(buf);
            }
            if (flash_area_read(fa, offset, buf, chunk) != 0) {
                psa_hash_abort(&op);
                break;
            }
            if (psa_hash_update(&op, buf, chunk) != PSA_SUCCESS) {
                psa_hash_abort(&op);
                break;
            }
            offset += chunk;
        }
        if (offset == size_to_hash) {
            size_t hash_len = 0;
            (void)psa_hash_finish(&op, sha256, 32, &hash_len);
        }
    }

    flash_area_close(fa);
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

    /* Align write length to 4-byte flash write word boundary (padding with 0xFF) */
    const uint8_t *write_buf = data;
    uint8_t padded_buf[256 + 4];
    size_t write_len = len;

    size_t rem = len % 4;
    if (rem != 0) {
        size_t pad = 4 - rem;
        write_len = len + pad;
        if (write_len <= sizeof(padded_buf)) {
            memcpy(padded_buf, data, len);
            memset(padded_buf + len, 0xff, pad);
            write_buf = padded_buf;
        }
    }

    rc = flash_area_write(fa, offset, write_buf, write_len);
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

    LOG_INF("Scheduling cold reboot into MCUboot in 200 ms...");
    k_work_schedule(&s_reboot_work, K_MSEC(200));
    return 0;
}
