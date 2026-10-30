#include "hsm/keystore.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(keystore, CONFIG_LOG_DEFAULT_LEVEL);

#define NVS_KEYSTORE_ID 0x1000

typedef struct {
    char pin[17];
    uint8_t retries;
    keystore_slot_t slots[KEYSTORE_MAX_SLOTS + 1];
} keystore_data_t;

static keystore_data_t s_data;
static bool s_authenticated = false;
static keystore_state_change_cb_t s_cb = NULL;

static struct nvs_fs s_nvs;
static bool s_nvs_ready = false;

static void notify_state(void) {
    if (s_cb) {
        s_cb(s_authenticated, s_data.retries, keystore_get_key_count());
    }
}

static void save_to_nvs(void) {
    if (!s_nvs_ready) {
        return;
    }
    int rc = nvs_write(&s_nvs, NVS_KEYSTORE_ID, &s_data, sizeof(s_data));
    if (rc < 0) {
        LOG_WRN("Failed to write keystore to NVS: %d", rc);
    }
}

static void init_defaults(void) {
    memset(&s_data, 0, sizeof(s_data));
    strncpy(s_data.pin, KEYSTORE_DEFAULT_PIN, sizeof(s_data.pin) - 1);
    s_data.retries = KEYSTORE_MAX_RETRIES;
}

static void init_nvs(void) {
#if FIXED_PARTITION_EXISTS(nvs_storage)
    s_nvs.flash_device = FIXED_PARTITION_DEVICE(nvs_storage);
    if (!s_nvs.flash_device || !device_is_ready(s_nvs.flash_device)) {
        LOG_WRN("NVS flash device not ready; falling back to transient keystore");
        return;
    }

    struct flash_pages_info info;
    s_nvs.offset = FIXED_PARTITION_OFFSET(nvs_storage);
    int rc = flash_get_page_info_by_offs(s_nvs.flash_device, s_nvs.offset, &info);
    if (rc != 0) {
        LOG_WRN("Failed to get flash page info: %d", rc);
        return;
    }

    s_nvs.sector_size = info.size;
    s_nvs.sector_count = 3U;

    rc = nvs_mount(&s_nvs);
    if (rc != 0) {
        LOG_WRN("nvs_mount failed: %d", rc);
        return;
    }

    s_nvs_ready = true;
    LOG_INF("NVS mounted successfully at offset 0x%lx", (unsigned long)s_nvs.offset);

    rc = nvs_read(&s_nvs, NVS_KEYSTORE_ID, &s_data, sizeof(s_data));
    if (rc > 0) {
        LOG_INF("Loaded keystore from NVS (retries: %u, keys: %u)",
                s_data.retries, keystore_get_key_count());
    } else {
        LOG_INF("No keystore record found in NVS; writing default store");
        init_defaults();
        save_to_nvs();
    }
#else
    LOG_INF("nvs_storage partition not configured, using RAM keystore");
#endif
}

int keystore_init(keystore_state_change_cb_t cb) {
    s_cb = cb;
    s_authenticated = false;
    init_defaults();
    init_nvs();
    notify_state();
    return 0;
}

bool keystore_is_authenticated(void) {
    return s_authenticated;
}

uint8_t keystore_get_retries(void) {
    return s_data.retries;
}

uint8_t keystore_get_key_count(void) {
    uint8_t count = 0;
    for (uint8_t i = 1; i <= KEYSTORE_MAX_SLOTS; i++) {
        if (s_data.slots[i].occupied) {
            count++;
        }
    }
    return count;
}

int keystore_authenticate(const char *pin) {
    if (!pin) {
        return -1;
    }

    if (s_data.retries == 0) {
        s_authenticated = false;
        notify_state();
        return -2; /* Blocked */
    }

    if (strncmp(pin, s_data.pin, sizeof(s_data.pin)) == 0) {
        s_data.retries = KEYSTORE_MAX_RETRIES;
        s_authenticated = true;
        save_to_nvs();
        notify_state();
        LOG_INF("PIN authentication succeeded");
        return 0;
    }

    if (s_data.retries > 0) {
        s_data.retries--;
    }
    s_authenticated = false;
    save_to_nvs();
    notify_state();
    LOG_WRN("PIN auth failed; remaining retries: %u", s_data.retries);

    return (s_data.retries == 0) ? -2 : -1;
}

void keystore_lock(void) {
    s_authenticated = false;
    notify_state();
    LOG_INF("Keystore locked");
}

void keystore_reset(void) {
    s_authenticated = false;
    notify_state();
    LOG_INF("Keystore session reset");
}

bool keystore_is_slot_occupied(uint8_t slot_id) {
    if (slot_id < 1 || slot_id > KEYSTORE_MAX_SLOTS) {
        return false;
    }
    return s_data.slots[slot_id].occupied;
}

const keystore_slot_t* keystore_get_slot(uint8_t slot_id) {
    if (slot_id < 1 || slot_id > KEYSTORE_MAX_SLOTS || !s_data.slots[slot_id].occupied) {
        return NULL;
    }
    return &s_data.slots[slot_id];
}

int keystore_set_slot(uint8_t slot_id, const char *algo, uint16_t size, const char *label, const uint8_t *pubkey_65b) {
    if (slot_id < 1 || slot_id > KEYSTORE_MAX_SLOTS || !algo) {
        return -1;
    }

    keystore_slot_t *slot = &s_data.slots[slot_id];
    slot->occupied = true;
    strncpy(slot->algo, algo, sizeof(slot->algo) - 1);
    slot->algo[sizeof(slot->algo) - 1] = '\0';
    slot->size = size;

    if (label) {
        strncpy(slot->label, label, sizeof(slot->label) - 1);
        slot->label[sizeof(slot->label) - 1] = '\0';
    } else {
        snprintf(slot->label, sizeof(slot->label), "Key%u", slot_id);
    }

    if (pubkey_65b) {
        memcpy(slot->pubkey, pubkey_65b, 65);
    }

    save_to_nvs();
    notify_state();
    return 0;
}

int keystore_delete_slot(uint8_t slot_id) {
    if (slot_id < 1 || slot_id > KEYSTORE_MAX_SLOTS) {
        return -1;
    }

    memset(&s_data.slots[slot_id], 0, sizeof(keystore_slot_t));
    save_to_nvs();
    notify_state();
    return 0;
}
