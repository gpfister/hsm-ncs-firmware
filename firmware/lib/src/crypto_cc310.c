#include "hsm/crypto_cc310.h"
#include <psa/crypto.h>
#include <psa/crypto_extra.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(crypto_cc310, CONFIG_LOG_DEFAULT_LEVEL);

static bool s_initialized = false;

int crypto_cc310_init(void) {
    if (s_initialized) {
        return 0;
    }

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        LOG_ERR("psa_crypto_init failed: %d", status);
        return -1;
    }

    s_initialized = true;
    LOG_INF("CC310 / PSA Crypto initialized successfully.");
    return 0;
}

int crypto_cc310_get_random(uint8_t *dest, size_t len) {
    if (!s_initialized || !dest || len == 0) {
        return -1;
    }

    psa_status_t status = psa_generate_random(dest, len);
    if (status != PSA_SUCCESS) {
        LOG_ERR("psa_generate_random failed: %d", status);
        return -1;
    }
    return 0;
}

int crypto_cc310_generate_p256_keypair(uint8_t slot_id, uint8_t *pubkey_65b, size_t *pubkey_len) {
    if (!s_initialized || !pubkey_65b || !pubkey_len) {
        return -1;
    }

    psa_key_id_t key_id = (psa_key_id_t)(PSA_KEY_ID_USER_MIN + slot_id);

    /* Purge / destroy existing key in this slot if present */
    (void)psa_destroy_key(key_id);

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_PERSISTENT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_id(&attr, key_id);

    psa_status_t status = psa_generate_key(&attr, &key_id);
    psa_reset_key_attributes(&attr);

    if (status != PSA_SUCCESS) {
        LOG_ERR("psa_generate_key failed for slot %u: %d", slot_id, status);
        return -1;
    }

    size_t out_len = 0;
    status = psa_export_public_key(key_id, pubkey_65b, CC310_P256_PUBKEY_SIZE, &out_len);
    if (status != PSA_SUCCESS) {
        LOG_ERR("psa_export_public_key failed: %d", status);
        (void)psa_destroy_key(key_id);
        return -1;
    }

    *pubkey_len = out_len;
    LOG_INF("Generated P-256 keypair in slot %u (pubkey len %u)", slot_id, (unsigned int)out_len);
    return 0;
}

int crypto_cc310_export_p256_pubkey(uint8_t slot_id, uint8_t *pubkey_65b, size_t *pubkey_len) {
    if (!s_initialized || !pubkey_65b || !pubkey_len) {
        return -1;
    }

    psa_key_id_t key_id = (psa_key_id_t)(PSA_KEY_ID_USER_MIN + slot_id);
    size_t out_len = 0;
    psa_status_t status = psa_export_public_key(key_id, pubkey_65b, CC310_P256_PUBKEY_SIZE, &out_len);
    if (status != PSA_SUCCESS) {
        LOG_ERR("psa_export_public_key failed for slot %u: %d", slot_id, status);
        return -1;
    }

    *pubkey_len = out_len;
    return 0;
}

int crypto_cc310_sign_hash(uint8_t slot_id, const uint8_t *hash_32b, uint8_t *sig_64b, size_t *sig_len) {
    if (!s_initialized || !hash_32b || !sig_64b || !sig_len) {
        return -1;
    }

    psa_key_id_t key_id = (psa_key_id_t)(PSA_KEY_ID_USER_MIN + slot_id);
    size_t out_len = 0;

    psa_status_t status = psa_sign_hash(key_id,
                                         PSA_ALG_ECDSA(PSA_ALG_SHA_256),
                                         hash_32b,
                                         CC310_SHA256_DIGEST_SIZE,
                                         sig_64b,
                                         CC310_P256_SIGNATURE_SIZE,
                                         &out_len);
    if (status != PSA_SUCCESS) {
        LOG_ERR("psa_sign_hash failed for slot %u: %d", slot_id, status);
        return -1;
    }

    *sig_len = out_len;
    LOG_INF("Signed hash with slot %u (sig len %u)", slot_id, (unsigned int)out_len);
    return 0;
}

int crypto_cc310_destroy_key(uint8_t slot_id) {
    if (!s_initialized) {
        return -1;
    }

    psa_key_id_t key_id = (psa_key_id_t)(PSA_KEY_ID_USER_MIN + slot_id);
    psa_status_t status = psa_destroy_key(key_id);
    if (status != PSA_SUCCESS && status != PSA_ERROR_DOES_NOT_EXIST) {
        LOG_WRN("psa_destroy_key status for slot %u: %d", slot_id, status);
        return -1;
    }
    return 0;
}
