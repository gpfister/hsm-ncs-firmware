#include "hsm/at_commands.h"
#include "hsm/keystore.h"
#include "hsm/crypto_cc310.h"
#include "hsm/sss.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_ficr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

LOG_MODULE_REGISTER(at_commands, CONFIG_LOG_DEFAULT_LEVEL);

#define FIRMWARE_VERSION "1.0.0"
#define HARDWARE_MODEL   "PCA10059"
#define BOOTLOADER_STAGE "B0+MCUboot"

#define AUTOLOCK_TIMEOUT_SEC 60

static struct k_timer s_autolock_timer;
static sss_share_t s_submitted_shares[SSS_THRESHOLD];
static uint8_t s_submitted_count = 0;
static int64_t s_first_share_time = 0;
static at_dfu_callbacks_t s_dfu_cbs = {0};

void at_commands_register_dfu_callbacks(const at_dfu_callbacks_t *cbs) {
    if (cbs) {
        s_dfu_cbs = *cbs;
    } else {
        memset(&s_dfu_cbs, 0, sizeof(s_dfu_cbs));
    }
}

static void autolock_timer_expiry(struct k_timer *timer) {
    ARG_UNUSED(timer);
    if (keystore_is_authenticated()) {
        LOG_INF("Auto-lock supervisor timer expired (60s inactivity); locking device");
        keystore_lock();
    }
}

static void restart_autolock_timer(void) {
    if (keystore_is_authenticated()) {
        k_timer_start(&s_autolock_timer, K_SECONDS(AUTOLOCK_TIMEOUT_SEC), K_NO_WAIT);
    }
}

void at_commands_reset_shares(void) {
    sss_zeroize(s_submitted_shares, sizeof(s_submitted_shares));
    s_submitted_count = 0;
    s_first_share_time = 0;
}

static int hex_char_to_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hsm_hex2bin(const char *hex, size_t hex_len, uint8_t *bin, size_t max_bin_len) {
    if ((hex_len % 2) != 0 || (hex_len / 2) > max_bin_len) {
        return -1;
    }
    for (size_t i = 0; i < hex_len / 2; i++) {
        int h = hex_char_to_val(hex[i * 2]);
        int l = hex_char_to_val(hex[i * 2 + 1]);
        if (h < 0 || l < 0) return -1;
        bin[i] = (uint8_t)((h << 4) | l);
    }
    return 0;
}

static void hsm_bin2hex(const uint8_t *bin, size_t bin_len, char *hex, size_t max_hex_len) {
    static const char digits[] = "0123456789ABCDEF";
    if (max_hex_len < (bin_len * 2 + 1)) return;
    for (size_t i = 0; i < bin_len; i++) {
        hex[i * 2]     = digits[(bin[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = digits[bin[i] & 0x0F];
    }
    hex[bin_len * 2] = '\0';
}

void at_commands_format_error(char *resp_buf, size_t max_resp_len, int error_code) {
    if (resp_buf && max_resp_len > 0) {
        snprintf(resp_buf, max_resp_len, "\r\n+CME ERROR: %d\r\n", error_code);
    }
}

int at_commands_init(void) {
    at_commands_reset_shares();
    k_timer_init(&s_autolock_timer, autolock_timer_expiry, NULL);
    return 0;
}

static void get_device_uid_string(char *uid_buf, size_t buf_len) {
    uint32_t uid_high = NRF_FICR->DEVICEADDR[1] | 0xC000;
    uint32_t uid_low = NRF_FICR->DEVICEADDR[0];
    snprintf(uid_buf, buf_len, "%08X%08X", (unsigned int)uid_high, (unsigned int)uid_low);
}

int at_commands_execute(const char *cmd_line, char *resp_buf, size_t max_resp_len) {
    if (!cmd_line || !resp_buf || max_resp_len < 16) {
        return -1;
    }

    /* Check share collection timeout */
    if (s_submitted_count > 0 && s_first_share_time > 0) {
        if ((k_uptime_get() - s_first_share_time) > (AUTOLOCK_TIMEOUT_SEC * 1000)) {
            LOG_WRN("Volatile share buffer expired (60s); resetting");
            at_commands_reset_shares();
        }
    }

    /* Trim leading whitespace */
    while (*cmd_line && isspace((unsigned char)*cmd_line)) {
        cmd_line++;
    }

    if (*cmd_line == '\0') {
        resp_buf[0] = '\0';
        return 0;
    }

    /* Must start with AT */
    if (strncmp(cmd_line, "AT", 2) != 0 && strncmp(cmd_line, "at", 2) != 0) {
        at_commands_format_error(resp_buf, max_resp_len, CME_ERR_COMMAND_UNSUPPORTED);
        return 0;
    }

    const char *p = cmd_line + 2;

    /* Feed activity watchdog on any recognized AT command while authenticated */
    restart_autolock_timer();

    /* 1. AT -> OK */
    if (*p == '\0') {
        snprintf(resp_buf, max_resp_len, "OK\r\n");
        return 0;
    }

    /* 2. AT+INFO? */
    if (strcmp(p, "+INFO?") == 0 || strcmp(p, "+info?") == 0) {
        char uid[32];
        get_device_uid_string(uid, sizeof(uid));
        snprintf(resp_buf, max_resp_len,
                 "+INFO: FW=\"%s\",HW=\"%s\",UID=\"%s\",BOOT=\"%s\"\r\nOK\r\n",
                 FIRMWARE_VERSION, HARDWARE_MODEL, uid, BOOTLOADER_STAGE);
        return 0;
    }

    /* 3. AT+STATUS? */
    if (strcmp(p, "+STATUS?") == 0 || strcmp(p, "+status?") == 0) {
        uint8_t configured = keystore_is_configured() ? 1 : 0;
        uint8_t auth = keystore_is_authenticated() ? 1 : 0;
        const char *method_str = "NONE";
        switch (keystore_get_auth_method()) {
        case AUTH_METHOD_PIN:
            method_str = "PIN";
            break;
        case AUTH_METHOD_SHAMIR:
            method_str = "SHAMIR";
            break;
        case AUTH_METHOD_NONE:
        default:
            method_str = "NONE";
            break;
        }
        uint8_t retries = keystore_get_retries();
        uint8_t keycount = keystore_get_key_count();
        snprintf(resp_buf, max_resp_len,
                 "+STATUS: CONFIGURED=%u,AUTH=%u,METHOD=%s,RETRIES=%u,KEYCOUNT=%u\r\nOK\r\n",
                 configured, auth, method_str, retries, keycount);
        return 0;
    }

    /* 4. AT+AUTH=<pin> */
    if (strncmp(p, "+AUTH=", 6) == 0 || strncmp(p, "+auth=", 6) == 0) {
        if (keystore_get_auth_method() == AUTH_METHOD_SHAMIR || !keystore_is_configured()) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_AUTH_MODE);
            return 0;
        }

        const char *pin = p + 6;
        size_t pin_len = strlen(pin);
        if (pin_len < 1 || pin_len > KEYSTORE_MAX_PIN_LEN) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        int auth_rc = keystore_authenticate(pin);
        if (auth_rc == 0) {
            restart_autolock_timer();
            snprintf(resp_buf, max_resp_len, "OK\r\n");
        } else if (auth_rc == -2) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_PIN_BLOCKED);
        } else {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PIN);
        }
        return 0;
    }

    /* 5. AT+LOCK */
    if (strcmp(p, "+LOCK") == 0 || strcmp(p, "+lock") == 0) {
        k_timer_stop(&s_autolock_timer);
        at_commands_reset_shares();
        keystore_lock();
        snprintf(resp_buf, max_resp_len, "OK\r\n");
        return 0;
    }

    /* 6. AT+RESET / AT+FACTORYRESET */
    if (strcmp(p, "+RESET") == 0 || strcmp(p, "+reset") == 0 ||
        strcmp(p, "+FACTORYRESET") == 0 || strcmp(p, "+factoryreset") == 0) {
        k_timer_stop(&s_autolock_timer);
        at_commands_reset_shares();
        keystore_factory_reset();
        snprintf(resp_buf, max_resp_len, "OK\r\n");
        return 0;
    }

    /* 7. AT+INIT=PIN,<pin> and AT+INIT=SHAMIR */
    if (strncmp(p, "+INIT=", 6) == 0 || strncmp(p, "+init=", 6) == 0) {
        if (keystore_is_configured()) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_ALREADY_CONFIGURED);
            return 0;
        }

        const char *arg = p + 6;
        if (strncmp(arg, "PIN,", 4) == 0 || strncmp(arg, "pin,", 4) == 0) {
            const char *pin = arg + 4;
            size_t pin_len = strlen(pin);
            if (pin_len < 6 || pin_len > KEYSTORE_MAX_PIN_LEN) {
                at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
                return 0;
            }

            int rc = keystore_init_pin(pin, pin_len);
            if (rc == 0) {
                snprintf(resp_buf, max_resp_len, "OK\r\n");
            } else if (rc == -2) {
                at_commands_format_error(resp_buf, max_resp_len, CME_ERR_ALREADY_CONFIGURED);
            } else {
                at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            }
            return 0;
        } else if (strcmp(arg, "SHAMIR") == 0 || strcmp(arg, "shamir") == 0 || strcmp(arg, "1") == 0) {
            uint8_t master_secret[SSS_SECRET_SIZE];
            if (crypto_cc310_get_random(master_secret, sizeof(master_secret)) != 0) {
                at_commands_format_error(resp_buf, max_resp_len, CME_ERR_CRYPTO_FAILURE);
                return 0;
            }

            sss_share_t shares[SSS_TOTAL_SHARES];
            if (sss_split(master_secret, SSS_SECRET_SIZE, shares, SSS_TOTAL_SHARES, SSS_THRESHOLD) != 0) {
                sss_zeroize(master_secret, sizeof(master_secret));
                at_commands_format_error(resp_buf, max_resp_len, CME_ERR_CRYPTO_FAILURE);
                return 0;
            }

            keystore_set_master_secret(master_secret);
            sss_zeroize(master_secret, sizeof(master_secret));
            keystore_init_shamir();
            at_commands_reset_shares();

            size_t offset = 0;
            for (int i = 0; i < SSS_TOTAL_SHARES; i++) {
                char hex_share[SSS_SECRET_SIZE * 2 + 1];
                hsm_bin2hex(shares[i].data, SSS_SECRET_SIZE, hex_share, sizeof(hex_share));
                int written = snprintf(resp_buf + offset, max_resp_len - offset,
                                       "+INIT: %u,%s\r\n", shares[i].index, hex_share);
                if (written > 0 && (size_t)written < (max_resp_len - offset)) {
                    offset += (size_t)written;
                }
            }
            snprintf(resp_buf + offset, max_resp_len - offset, "OK\r\n");

            /* Device is initialized in locked state (solid RED) */
            keystore_lock();
            return 0;
        } else {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }
    }

    /* 8. AT+SHARE=<idx>,<share_hex> */
    if (strncmp(p, "+SHARE=", 7) == 0 || strncmp(p, "+share=", 7) == 0) {
        if (keystore_get_auth_method() == AUTH_METHOD_PIN || !keystore_is_configured()) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_AUTH_MODE);
            return 0;
        }
        const char *args = p + 7;
        char *comma = strchr(args, ',');
        if (!comma) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        char idx_str[8];
        size_t idx_len = (size_t)(comma - args);
        if (idx_len >= sizeof(idx_str)) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }
        memcpy(idx_str, args, idx_len);
        idx_str[idx_len] = '\0';

        int idx = atoi(idx_str);
        if (idx < 1 || idx > SSS_TOTAL_SHARES) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        const char *share_hex = comma + 1;
        size_t share_hex_len = strlen(share_hex);
        if (share_hex_len != (SSS_SECRET_SIZE * 2)) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        /* Check for duplicate share index submission in current ceremony */
        for (uint8_t i = 0; i < s_submitted_count; i++) {
            if (s_submitted_shares[i].index == (uint8_t)idx) {
                LOG_WRN("Duplicate share index %u rejected", idx);
                at_commands_format_error(resp_buf, max_resp_len, CME_ERR_DUPLICATE_SHARE);
                return 0;
            }
        }

        if (s_submitted_count >= SSS_THRESHOLD) {
            /* Already reached quorum */
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        uint8_t share_bytes[SSS_SECRET_SIZE];
        if (hsm_hex2bin(share_hex, share_hex_len, share_bytes, sizeof(share_bytes)) != 0) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        if (s_submitted_count == 0) {
            s_first_share_time = k_uptime_get();
        }

        s_submitted_shares[s_submitted_count].index = (uint8_t)idx;
        memcpy(s_submitted_shares[s_submitted_count].data, share_bytes, SSS_SECRET_SIZE);
        sss_zeroize(share_bytes, sizeof(share_bytes));

        /* Derive ephemeral commitment word index (11 bits: 0..2047) */
        uint16_t word_idx = 0;
        crypto_cc310_get_random((uint8_t *)&word_idx, sizeof(word_idx));
        word_idx %= 2048;

        s_submitted_count++;

        if (s_submitted_count < SSS_THRESHOLD) {
            snprintf(resp_buf, max_resp_len, "+SHARE: %u/3 ACCEPTED,WORD=%u\r\nOK\r\n",
                     s_submitted_count, word_idx);
            return 0;
        }

        /* Quorum reached (3 shares) -> Reconstruct secret and verify */
        uint8_t recovered[SSS_SECRET_SIZE];
        int rec_rc = sss_recover(s_submitted_shares, SSS_THRESHOLD, recovered, SSS_SECRET_SIZE);
        if (rec_rc != 0) {
            at_commands_reset_shares();
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_CRYPTO_FAILURE);
            return 0;
        }

        int ver_rc = keystore_verify_master_secret(recovered);
        sss_zeroize(recovered, sizeof(recovered));
        at_commands_reset_shares();

        if (ver_rc == 0) {
            restart_autolock_timer();
            snprintf(resp_buf, max_resp_len, "+SHARE: 3/3 ACCEPTED,QUORUM_MET,WORD=%u\r\nOK\r\n", word_idx);
        } else {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_CRYPTO_FAILURE);
        }
        return 0;
    }

    /* 9. AT+MIGRATE_TARGET_INIT */
    if (strcmp(p, "+MIGRATE_TARGET_INIT") == 0 || strcmp(p, "+migrate_target_init") == 0) {
        uint8_t pub_65[CC310_P256_PUBKEY_SIZE];
        if (keystore_generate_ephemeral_keypair(pub_65) != 0) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_CRYPTO_FAILURE);
            return 0;
        }

        char pub_hex[CC310_P256_PUBKEY_SIZE * 2 + 1];
        hsm_bin2hex(pub_65, CC310_P256_PUBKEY_SIZE, pub_hex, sizeof(pub_hex));
        snprintf(resp_buf, max_resp_len, "+MIGRATE_TARGET_INIT: PUB=\"%s\"\r\nOK\r\n", pub_hex);
        return 0;
    }

    /* 10. AT+MIGRATE_WRAP=<slot_id>,<target_pub_hex> */
    if (strncmp(p, "+MIGRATE_WRAP=", 14) == 0 || strncmp(p, "+migrate_wrap=", 14) == 0) {
        if (!keystore_is_authenticated()) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_DEVICE_LOCKED);
            return 0;
        }

        const char *args = p + 14;
        char *comma = strchr(args, ',');
        if (!comma) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        char slot_str[8];
        size_t slot_len = (size_t)(comma - args);
        if (slot_len >= sizeof(slot_str)) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }
        memcpy(slot_str, args, slot_len);
        slot_str[slot_len] = '\0';

        int slot_id = atoi(slot_str);
        if (slot_id < 1 || slot_id > KEYSTORE_MAX_SLOTS) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        if (!keystore_is_slot_occupied((uint8_t)slot_id)) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_KEY_NOT_FOUND);
            return 0;
        }

        const char *target_pub_hex = comma + 1;
        size_t target_hex_len = strlen(target_pub_hex);
        if (target_hex_len != (CC310_P256_PUBKEY_SIZE * 2)) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        uint8_t target_pub[CC310_P256_PUBKEY_SIZE];
        if (hsm_hex2bin(target_pub_hex, target_hex_len, target_pub, sizeof(target_pub)) != 0) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        uint8_t source_pub[CC310_P256_PUBKEY_SIZE];
        uint8_t ciphertext[32];
        uint8_t tag[16];
        size_t ct_len = 0;

        int wrap_rc = keystore_wrap_key_ecdh((uint8_t)slot_id, target_pub, source_pub, ciphertext, tag, &ct_len);
        if (wrap_rc != 0) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_CRYPTO_FAILURE);
            return 0;
        }

        char src_hex[CC310_P256_PUBKEY_SIZE * 2 + 1];
        char ct_hex[32 * 2 + 1];
        char tag_hex[16 * 2 + 1];
        hsm_bin2hex(source_pub, CC310_P256_PUBKEY_SIZE, src_hex, sizeof(src_hex));
        hsm_bin2hex(ciphertext, 32, ct_hex, sizeof(ct_hex));
        hsm_bin2hex(tag, 16, tag_hex, sizeof(tag_hex));

        snprintf(resp_buf, max_resp_len,
                 "+MIGRATE_WRAP: PUB=\"%s\",CT=\"%s\",TAG=\"%s\"\r\nOK\r\n",
                 src_hex, ct_hex, tag_hex);
        return 0;
    }

    /* 11. AT+MIGRATE_UNWRAP=<slot_id>,<source_pub_hex>,<ciphertext_hex>,<tag_hex> */
    if (strncmp(p, "+MIGRATE_UNWRAP=", 16) == 0 || strncmp(p, "+migrate_unwrap=", 16) == 0) {
        const char *args = p + 16;
        char *comma1 = strchr(args, ',');
        if (!comma1) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }
        char *comma2 = strchr(comma1 + 1, ',');
        if (!comma2) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }
        char *comma3 = strchr(comma2 + 1, ',');
        if (!comma3) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        char slot_str[8];
        size_t slot_len = (size_t)(comma1 - args);
        if (slot_len >= sizeof(slot_str)) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }
        memcpy(slot_str, args, slot_len);
        slot_str[slot_len] = '\0';

        int slot_id = atoi(slot_str);
        if (slot_id < 1 || slot_id > KEYSTORE_MAX_SLOTS) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        size_t src_len = (size_t)(comma2 - (comma1 + 1));
        if (src_len != (CC310_P256_PUBKEY_SIZE * 2)) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }
        uint8_t source_pub[CC310_P256_PUBKEY_SIZE];
        if (hsm_hex2bin(comma1 + 1, src_len, source_pub, sizeof(source_pub)) != 0) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        size_t ct_len = (size_t)(comma3 - (comma2 + 1));
        if (ct_len != (32 * 2)) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }
        uint8_t ciphertext[32];
        if (hsm_hex2bin(comma2 + 1, ct_len, ciphertext, sizeof(ciphertext)) != 0) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        const char *tag_hex = comma3 + 1;
        size_t tag_len = strlen(tag_hex);
        if (tag_len != (16 * 2)) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }
        uint8_t tag[16];
        if (hsm_hex2bin(tag_hex, tag_len, tag, sizeof(tag)) != 0) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        int unwrap_rc = keystore_unwrap_key_ecdh((uint8_t)slot_id, source_pub, ciphertext, 32, tag);
        if (unwrap_rc != 0) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_CRYPTO_FAILURE);
            return 0;
        }

        snprintf(resp_buf, max_resp_len, "OK\r\n");
        return 0;
    }

    /* 12. AT+SLOTS? */
    if (strcmp(p, "+SLOTS?") == 0 || strcmp(p, "+slots?") == 0) {
        size_t offset = 0;
        for (uint8_t id = 1; id <= KEYSTORE_MAX_SLOTS; id++) {
            if (keystore_is_slot_occupied(id)) {
                const keystore_slot_t *slot = keystore_get_slot(id);
                int written = snprintf(resp_buf + offset, max_resp_len - offset,
                                       "+SLOTS: %u,%s,%u,%s\r\n",
                                       id, slot->algo, slot->size, slot->label);
                if (written > 0 && (size_t)written < max_resp_len - offset) {
                    offset += (size_t)written;
                }
            }
        }
        snprintf(resp_buf + offset, max_resp_len - offset, "OK\r\n");
        return 0;
    }

    /* 13. AT+KEYGEN=<id>,<algo> */
    if (strncmp(p, "+KEYGEN=", 8) == 0 || strncmp(p, "+keygen=", 8) == 0) {
        if (!keystore_is_authenticated()) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_DEVICE_LOCKED);
            return 0;
        }

        const char *args = p + 8;
        char *comma = strchr(args, ',');
        if (!comma) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        char id_str[8];
        size_t id_len = (size_t)(comma - args);
        if (id_len >= sizeof(id_str)) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }
        memcpy(id_str, args, id_len);
        id_str[id_len] = '\0';

        int id = atoi(id_str);
        if (id < 1 || id > KEYSTORE_MAX_SLOTS) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        const char *algo = comma + 1;
        if (strcmp(algo, "EC_P256") != 0 && strcmp(algo, "ec_p256") != 0) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        uint8_t pubkey_65b[CC310_P256_PUBKEY_SIZE];
        size_t pubkey_len = 0;
        int rc = crypto_cc310_generate_p256_keypair((uint8_t)id, pubkey_65b, &pubkey_len);
        if (rc != 0 || pubkey_len != CC310_P256_PUBKEY_SIZE) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_CRYPTO_FAILURE);
            return 0;
        }

        char label[16];
        snprintf(label, sizeof(label), "Key%d", id);
        keystore_set_slot((uint8_t)id, "EC_P256", 256, label, pubkey_65b);

        char pub_hex[CC310_P256_PUBKEY_SIZE * 2 + 1];
        hsm_bin2hex(pubkey_65b, pubkey_len, pub_hex, sizeof(pub_hex));

        snprintf(resp_buf, max_resp_len, "+KEYGEN: %d,PUB=\"%s\"\r\nOK\r\n", id, pub_hex);

        /* Auto self-lock immediately upon keygen completion */
        keystore_lock();
        return 0;
    }

    /* 14. AT+PUBKEY=<id> */
    if (strncmp(p, "+PUBKEY=", 8) == 0 || strncmp(p, "+pubkey=", 8) == 0) {
        const char *id_str = p + 8;
        int id = atoi(id_str);
        if (id < 1 || id > KEYSTORE_MAX_SLOTS) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        if (!keystore_is_slot_occupied((uint8_t)id)) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_KEY_NOT_FOUND);
            return 0;
        }

        uint8_t pubkey_65b[CC310_P256_PUBKEY_SIZE];
        size_t pubkey_len = 0;
        int rc = crypto_cc310_export_p256_pubkey((uint8_t)id, pubkey_65b, &pubkey_len);
        if (rc != 0 || pubkey_len != CC310_P256_PUBKEY_SIZE) {
            const keystore_slot_t *slot = keystore_get_slot((uint8_t)id);
            if (slot && slot->pubkey[0] == 0x04) {
                memcpy(pubkey_65b, slot->pubkey, CC310_P256_PUBKEY_SIZE);
                pubkey_len = CC310_P256_PUBKEY_SIZE;
            } else {
                at_commands_format_error(resp_buf, max_resp_len, CME_ERR_CRYPTO_FAILURE);
                return 0;
            }
        }

        char pub_hex[CC310_P256_PUBKEY_SIZE * 2 + 1];
        hsm_bin2hex(pubkey_65b, pubkey_len, pub_hex, sizeof(pub_hex));

        snprintf(resp_buf, max_resp_len, "+PUBKEY: %d,PUB=\"%s\"\r\nOK\r\n", id, pub_hex);
        return 0;
    }

    /* 15. AT+SIGN=<id>,<hash> */
    if (strncmp(p, "+SIGN=", 6) == 0 || strncmp(p, "+sign=", 6) == 0) {
        if (!keystore_is_authenticated()) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_DEVICE_LOCKED);
            return 0;
        }

        const char *args = p + 6;
        char *comma = strchr(args, ',');
        if (!comma) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        char id_str[8];
        size_t id_len = (size_t)(comma - args);
        if (id_len >= sizeof(id_str)) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }
        memcpy(id_str, args, id_len);
        id_str[id_len] = '\0';

        int id = atoi(id_str);
        if (id < 1 || id > KEYSTORE_MAX_SLOTS) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        if (!keystore_is_slot_occupied((uint8_t)id)) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_KEY_NOT_FOUND);
            return 0;
        }

        const char *hash_hex = comma + 1;
        size_t hash_hex_len = strlen(hash_hex);
        if (hash_hex_len != CC310_SHA256_DIGEST_SIZE * 2) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        uint8_t hash_bytes[CC310_SHA256_DIGEST_SIZE];
        if (hsm_hex2bin(hash_hex, hash_hex_len, hash_bytes, sizeof(hash_bytes)) != 0) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        uint8_t sig[CC310_P256_SIGNATURE_SIZE];
        size_t sig_len = 0;
        int rc = crypto_cc310_sign_hash((uint8_t)id, hash_bytes, sig, &sig_len);
        if (rc != 0 || sig_len != CC310_P256_SIGNATURE_SIZE) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_CRYPTO_FAILURE);
            return 0;
        }

        char sig_hex[CC310_P256_SIGNATURE_SIZE * 2 + 1];
        hsm_bin2hex(sig, sig_len, sig_hex, sizeof(sig_hex));

        snprintf(resp_buf, max_resp_len, "+SIGN: %s\r\nOK\r\n", sig_hex);
        return 0;
    }

    /* 16. AT+RANDOM=<len> */
    if (strncmp(p, "+RANDOM=", 8) == 0 || strncmp(p, "+random=", 8) == 0) {
        const char *len_str = p + 8;
        int req_len = atoi(len_str);
        if (req_len < 1 || req_len > 128) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        uint8_t rnd_buf[128];
        int rc = crypto_cc310_get_random(rnd_buf, (size_t)req_len);
        if (rc != 0) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_CRYPTO_FAILURE);
            return 0;
        }

        char rnd_hex[128 * 2 + 1];
        hsm_bin2hex(rnd_buf, (size_t)req_len, rnd_hex, sizeof(rnd_hex));

        snprintf(resp_buf, max_resp_len, "+RANDOM: %s\r\nOK\r\n", rnd_hex);
        return 0;
    }

    /* 17. AT+DFU_INFO? */
    if (strcmp(p, "+DFU_INFO?") == 0 || strcmp(p, "+dfu_info?") == 0) {
        uint8_t major = 1;
        uint8_t minor = 0;
        uint8_t patch = 0;
        uint32_t active_slot = 0;
        uint32_t image_size = 0;
        uint8_t sha256[32] = {0};

        if (s_dfu_cbs.get_info != NULL) {
            if (s_dfu_cbs.get_info(&major, &minor, &patch, &active_slot, &image_size, sha256) != 0) {
                at_commands_format_error(resp_buf, max_resp_len, CME_ERR_CRYPTO_FAILURE);
                return 0;
            }
        }

        char sha256_hex[32 * 2 + 1];
        hsm_bin2hex(sha256, sizeof(sha256), sha256_hex, sizeof(sha256_hex));

        snprintf(resp_buf, max_resp_len,
                 "+DFU_INFO: VER=\"%u.%u.%u\",SLOT=%u,SIZE=%u,SHA256=\"%s\"\r\nOK\r\n",
                 major, minor, patch, (unsigned int)active_slot, (unsigned int)image_size, sha256_hex);
        return 0;
    }

    /* 18. AT+DFU_CHUNK=<offset>,<hex_data> */
    if (strncmp(p, "+DFU_CHUNK=", 11) == 0 || strncmp(p, "+dfu_chunk=", 11) == 0) {
        if (!keystore_is_authenticated()) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_DEVICE_LOCKED);
            return 0;
        }

        const char *args = p + 11;
        char *comma = strchr(args, ',');
        if (!comma) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        char offset_str[16];
        size_t offset_len = (size_t)(comma - args);
        if (offset_len == 0 || offset_len >= sizeof(offset_str)) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }
        for (size_t i = 0; i < offset_len; i++) {
            if (!isdigit((unsigned char)args[i])) {
                at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
                return 0;
            }
        }
        memcpy(offset_str, args, offset_len);
        offset_str[offset_len] = '\0';

        char *endptr = NULL;
        unsigned long parsed_offset = strtoul(offset_str, &endptr, 10);
        if (endptr == NULL || *endptr != '\0' || parsed_offset > UINT32_MAX) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        const char *hex_data = comma + 1;
        size_t hex_len = strlen(hex_data);
        if (hex_len < 2 || (hex_len % 2) != 0 || (hex_len / 2) > 256) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        uint8_t chunk_buf[256];
        size_t bin_len = hex_len / 2;
        if (hsm_hex2bin(hex_data, hex_len, chunk_buf, sizeof(chunk_buf)) != 0) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        if (s_dfu_cbs.write_chunk != NULL) {
            if (s_dfu_cbs.write_chunk((uint32_t)parsed_offset, chunk_buf, bin_len) != 0) {
                at_commands_format_error(resp_buf, max_resp_len, CME_ERR_CRYPTO_FAILURE);
                return 0;
            }
        }

        snprintf(resp_buf, max_resp_len, "OK\r\n");
        return 0;
    }

    /* 19. AT+DFU_APPLY */
    if (strcmp(p, "+DFU_APPLY") == 0 || strcmp(p, "+dfu_apply") == 0) {
        if (!keystore_is_authenticated()) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_DEVICE_LOCKED);
            return 0;
        }

        if (s_dfu_cbs.apply != NULL) {
            if (s_dfu_cbs.apply() != 0) {
                at_commands_format_error(resp_buf, max_resp_len, CME_ERR_CRYPTO_FAILURE);
                return 0;
            }
        }

        snprintf(resp_buf, max_resp_len, "OK\r\n");
        return 0;
    }

    /* Unrecognized command */
    at_commands_format_error(resp_buf, max_resp_len, CME_ERR_COMMAND_UNSUPPORTED);
    return 0;
}
