#include "hsm/at_commands.h"
#include "hsm/keystore.h"
#include "hsm/crypto_cc310.h"
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
        uint8_t auth = keystore_is_authenticated() ? 1 : 0;
        uint8_t retries = keystore_get_retries();
        uint8_t keycount = keystore_get_key_count();
        snprintf(resp_buf, max_resp_len,
                 "+STATUS: AUTH=%u,RETRIES=%u,KEYCOUNT=%u\r\nOK\r\n",
                 auth, retries, keycount);
        return 0;
    }

    /* 4. AT+AUTH=<pin> */
    if (strncmp(p, "+AUTH=", 6) == 0 || strncmp(p, "+auth=", 6) == 0) {
        const char *pin = p + 6;
        size_t pin_len = strlen(pin);
        if (pin_len < 1 || pin_len > 16) {
            at_commands_format_error(resp_buf, max_resp_len, CME_ERR_INVALID_PARAMETER);
            return 0;
        }

        int auth_rc = keystore_authenticate(pin);
        if (auth_rc == 0) {
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
        keystore_lock();
        snprintf(resp_buf, max_resp_len, "OK\r\n");
        return 0;
    }

    /* 6. AT+RESET */
    if (strcmp(p, "+RESET") == 0 || strcmp(p, "+reset") == 0) {
        keystore_reset();
        snprintf(resp_buf, max_resp_len, "OK\r\n");
        return 0;
    }

    /* 7. AT+SLOTS? */
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

    /* 8. AT+KEYGEN=<id>,<algo> */
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
        return 0;
    }

    /* 9. AT+PUBKEY=<id> */
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
            /* Fall back to pubkey cached in slot */
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

    /* 10. AT+SIGN=<id>,<hash> */
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

    /* 11. AT+RANDOM=<len> */
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

    /* Unrecognized command */
    at_commands_format_error(resp_buf, max_resp_len, CME_ERR_COMMAND_UNSUPPORTED);
    return 0;
}
