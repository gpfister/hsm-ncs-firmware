#include "hsm/keystore.h"
#include "hsm/crypto_cc310.h"
#include "hsm/sss.h"
#include <psa/crypto.h>
#include <psa/crypto_extra.h>
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
    bool shamir_initialized;
    uint8_t master_secret[32];
    keystore_slot_t slots[KEYSTORE_MAX_SLOTS + 1];
} keystore_data_t;

static keystore_data_t s_data;
static bool s_authenticated = false;
static keystore_state_change_cb_t s_cb = NULL;

static struct nvs_fs s_nvs;
static bool s_nvs_ready = false;

/* Volatile ephemeral target key ID for blind migration */
static psa_key_id_t s_target_ephemeral_key_id = 0;

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
    s_data.shamir_initialized = false;
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
        LOG_INF("Loaded keystore from NVS (retries: %u, keys: %u, shamir: %d)",
                s_data.retries, keystore_get_key_count(), s_data.shamir_initialized ? 1 : 0);
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
    if (s_target_ephemeral_key_id != 0) {
        (void)psa_destroy_key(s_target_ephemeral_key_id);
        s_target_ephemeral_key_id = 0;
    }
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

int keystore_set_master_secret(const uint8_t *secret_32b) {
    if (!secret_32b) {
        return -1;
    }
    memcpy(s_data.master_secret, secret_32b, 32);
    s_data.shamir_initialized = true;
    save_to_nvs();
    keystore_lock();
    return 0;
}

int keystore_verify_master_secret(const uint8_t *secret_32b) {
    if (!secret_32b) {
        return -1;
    }
    if (!s_data.shamir_initialized) {
        /* If factory uninitialized, accepting first quorum initializes it */
        keystore_set_master_secret(secret_32b);
        s_authenticated = true;
        notify_state();
        return 0;
    }

    if (memcmp(s_data.master_secret, secret_32b, 32) == 0) {
        s_authenticated = true;
        notify_state();
        LOG_INF("Master Shamir secret verified; keystore authenticated");
        return 0;
    }

    s_authenticated = false;
    notify_state();
    LOG_WRN("Master Shamir secret verification failed");
    return -1;
}

bool keystore_is_shamir_initialized(void) {
    return s_data.shamir_initialized;
}

int keystore_generate_ephemeral_keypair(uint8_t *pub_out_65) {
    if (!pub_out_65) {
        return -1;
    }

    if (s_target_ephemeral_key_id != 0) {
        (void)psa_destroy_key(s_target_ephemeral_key_id);
        s_target_ephemeral_key_id = 0;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
    psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);

    psa_status_t status = psa_generate_key(&attr, &s_target_ephemeral_key_id);
    psa_reset_key_attributes(&attr);
    if (status != PSA_SUCCESS) {
        LOG_ERR("psa_generate_key target ephemeral failed: %d", status);
        return -1;
    }

    size_t out_len = 0;
    status = psa_export_public_key(s_target_ephemeral_key_id, pub_out_65, CC310_P256_PUBKEY_SIZE, &out_len);
    if (status != PSA_SUCCESS || out_len != CC310_P256_PUBKEY_SIZE) {
        LOG_ERR("psa_export_public_key target ephemeral failed: %d", status);
        (void)psa_destroy_key(s_target_ephemeral_key_id);
        s_target_ephemeral_key_id = 0;
        return -1;
    }

    LOG_INF("Generated ephemeral target keypair (id=%u)", (unsigned int)s_target_ephemeral_key_id);
    return 0;
}

/* Internal HMAC-SHA256 using PSA Hash */
static int hsm_sha256(const uint8_t *data, size_t data_len, uint8_t *out_digest) {
    size_t out_len = 0;
    psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, data, data_len, out_digest, 32, &out_len);
    return (status == PSA_SUCCESS && out_len == 32) ? 0 : -1;
}

static int hsm_hmac_sha256(const uint8_t *key, size_t key_len,
                           const uint8_t *msg, size_t msg_len,
                           uint8_t *out_mac) {
    uint8_t k_block[64] = {0};
    if (key_len > 64) {
        if (hsm_sha256(key, key_len, k_block) != 0) {
            return -1;
        }
    } else {
        memcpy(k_block, key, key_len);
    }

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k_block[i] ^ 0x36;
        opad[i] = k_block[i] ^ 0x5c;
    }

    /* Inner hash = H(ipad || msg) */
    uint8_t inner_buf[64 + 128];
    if (64 + msg_len > sizeof(inner_buf)) {
        sss_zeroize(k_block, sizeof(k_block));
        return -1;
    }
    memcpy(inner_buf, ipad, 64);
    memcpy(inner_buf + 64, msg, msg_len);
    uint8_t inner_hash[32];
    if (hsm_sha256(inner_buf, 64 + msg_len, inner_hash) != 0) {
        sss_zeroize(k_block, sizeof(k_block));
        sss_zeroize(inner_buf, sizeof(inner_buf));
        return -1;
    }

    /* Outer hash = H(opad || inner_hash) */
    uint8_t outer_buf[64 + 32];
    memcpy(outer_buf, opad, 64);
    memcpy(outer_buf + 64, inner_hash, 32);
    int rc = hsm_sha256(outer_buf, 64 + 32, out_mac);

    sss_zeroize(k_block, sizeof(k_block));
    sss_zeroize(ipad, sizeof(ipad));
    sss_zeroize(opad, sizeof(opad));
    sss_zeroize(inner_buf, sizeof(inner_buf));
    sss_zeroize(outer_buf, sizeof(outer_buf));
    sss_zeroize(inner_hash, sizeof(inner_hash));
    return rc;
}

static int hsm_hkdf_sha256(const uint8_t *salt, size_t salt_len,
                           const uint8_t *ikm, size_t ikm_len,
                           const uint8_t *info, size_t info_len,
                           uint8_t *okm, size_t okm_len) {
    uint8_t prk[32];
    if (hsm_hmac_sha256(salt, salt_len, ikm, ikm_len, prk) != 0) {
        return -1;
    }

    /* Expand */
    uint8_t t[32];
    size_t t_len = 0;
    size_t generated = 0;
    uint8_t counter = 1;

    while (generated < okm_len) {
        uint8_t msg[32 + 64 + 1];
        size_t msg_len = 0;
        if (t_len > 0) {
            memcpy(msg, t, t_len);
            msg_len += t_len;
        }
        if (info_len > 0) {
            memcpy(msg + msg_len, info, info_len);
            msg_len += info_len;
        }
        msg[msg_len++] = counter++;

        if (hsm_hmac_sha256(prk, sizeof(prk), msg, msg_len, t) != 0) {
            sss_zeroize(prk, sizeof(prk));
            sss_zeroize(t, sizeof(t));
            return -1;
        }
        t_len = 32;
        size_t to_copy = (okm_len - generated < 32) ? (okm_len - generated) : 32;
        memcpy(okm + generated, t, to_copy);
        generated += to_copy;
    }

    sss_zeroize(prk, sizeof(prk));
    sss_zeroize(t, sizeof(t));
    return 0;
}

int keystore_wrap_key_ecdh(uint8_t slot_id,
                           const uint8_t *target_pub_65,
                           uint8_t *source_pub_65,
                           uint8_t *ciphertext_out,
                           uint8_t *tag_16,
                           size_t *ct_len) {
    if (!s_authenticated) {
        return -1;
    }
    if (!keystore_is_slot_occupied(slot_id)) {
        return -2;
    }
    if (!target_pub_65 || !source_pub_65 || !ciphertext_out || !tag_16 || !ct_len) {
        return -3;
    }

    /* 1. Generate ephemeral source EC P-256 keypair */
    psa_key_id_t source_eph_key = 0;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
    psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);

    psa_status_t status = psa_generate_key(&attr, &source_eph_key);
    psa_reset_key_attributes(&attr);
    if (status != PSA_SUCCESS) {
        LOG_ERR("psa_generate_key source eph failed: %d", status);
        return -4;
    }

    size_t pub_len = 0;
    status = psa_export_public_key(source_eph_key, source_pub_65, CC310_P256_PUBKEY_SIZE, &pub_len);
    if (status != PSA_SUCCESS || pub_len != CC310_P256_PUBKEY_SIZE) {
        (void)psa_destroy_key(source_eph_key);
        return -4;
    }

    /* 2. Compute ECDH shared secret Z = d_S * Q_T */
    uint8_t shared_secret[32];
    size_t secret_len = 0;
    status = psa_raw_key_agreement(PSA_ALG_ECDH, source_eph_key, target_pub_65, CC310_P256_PUBKEY_SIZE,
                                   shared_secret, sizeof(shared_secret), &secret_len);
    (void)psa_destroy_key(source_eph_key);
    if (status != PSA_SUCCESS || secret_len != 32) {
        LOG_ERR("psa_raw_key_agreement wrap failed: %d", status);
        sss_zeroize(shared_secret, sizeof(shared_secret));
        return -5;
    }

    /* 3. Derive K_enc (32 bytes) and IV (12 bytes) via HKDF-SHA256 */
    uint8_t key_and_iv[44];
    static const uint8_t salt[] = "HSM_MIGRATE_SALT_V1";
    static const uint8_t info[] = "HSM_MIGRATE_KEY_V1";
    int hkdf_rc = hsm_hkdf_sha256(salt, sizeof(salt) - 1,
                                  shared_secret, sizeof(shared_secret),
                                  info, sizeof(info) - 1,
                                  key_and_iv, sizeof(key_and_iv));
    sss_zeroize(shared_secret, sizeof(shared_secret));
    if (hkdf_rc != 0) {
        sss_zeroize(key_and_iv, sizeof(key_and_iv));
        return -6;
    }

    uint8_t *k_enc = key_and_iv;
    uint8_t *iv = key_and_iv + 32;

    /* 4. Export private scalar of slot_id */
    uint8_t priv_key[32];
    size_t priv_len = 0;
    int exp_rc = crypto_cc310_export_p256_privkey(slot_id, priv_key, &priv_len);
    if (exp_rc != 0 || priv_len != 32) {
        sss_zeroize(key_and_iv, sizeof(key_and_iv));
        sss_zeroize(priv_key, sizeof(priv_key));
        return -7;
    }

    /* 5. Wrap private key with AES-256-GCM */
    psa_key_id_t aes_key = 0;
    psa_key_attributes_t aes_attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&aes_attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&aes_attr, 256);
    psa_set_key_usage_flags(&aes_attr, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&aes_attr, PSA_ALG_GCM);

    status = psa_import_key(&aes_attr, k_enc, 32, &aes_key);
    psa_reset_key_attributes(&aes_attr);
    if (status != PSA_SUCCESS) {
        sss_zeroize(key_and_iv, sizeof(key_and_iv));
        sss_zeroize(priv_key, sizeof(priv_key));
        return -8;
    }

    uint8_t ct_and_tag[32 + 16];
    size_t out_len = 0;
    status = psa_aead_encrypt(aes_key, PSA_ALG_GCM, iv, 12, NULL, 0,
                              priv_key, 32, ct_and_tag, sizeof(ct_and_tag), &out_len);
    (void)psa_destroy_key(aes_key);
    sss_zeroize(key_and_iv, sizeof(key_and_iv));
    sss_zeroize(priv_key, sizeof(priv_key));

    if (status != PSA_SUCCESS || out_len != (32 + 16)) {
        LOG_ERR("psa_aead_encrypt failed: %d", status);
        return -9;
    }

    memcpy(ciphertext_out, ct_and_tag, 32);
    memcpy(tag_16, ct_and_tag + 32, 16);
    *ct_len = 32;

    LOG_INF("Successfully wrapped slot %u key via ECDH/AES-GCM", slot_id);

    /* Immediate self-lock transition */
    keystore_lock();
    return 0;
}

int keystore_unwrap_key_ecdh(uint8_t slot_id,
                             const uint8_t *source_pub_65,
                             const uint8_t *ciphertext,
                             size_t ct_len,
                             const uint8_t *tag_16) {
    if (s_target_ephemeral_key_id == 0) {
        LOG_ERR("No ephemeral target key generated for unwrap");
        return -1;
    }
    if (slot_id < 1 || slot_id > KEYSTORE_MAX_SLOTS || !source_pub_65 || !ciphertext || ct_len != 32 || !tag_16) {
        return -2;
    }

    /* 1. Compute ECDH shared secret Z = d_T * Q_S */
    uint8_t shared_secret[32];
    size_t secret_len = 0;
    psa_status_t status = psa_raw_key_agreement(PSA_ALG_ECDH, s_target_ephemeral_key_id, source_pub_65, CC310_P256_PUBKEY_SIZE,
                                                shared_secret, sizeof(shared_secret), &secret_len);
    (void)psa_destroy_key(s_target_ephemeral_key_id);
    s_target_ephemeral_key_id = 0;

    if (status != PSA_SUCCESS || secret_len != 32) {
        LOG_ERR("psa_raw_key_agreement for unwrap failed: %d", status);
        sss_zeroize(shared_secret, sizeof(shared_secret));
        return -3;
    }

    /* 2. Derive K_enc (32 bytes) and IV (12 bytes) via HKDF-SHA256 */
    uint8_t key_and_iv[44];
    static const uint8_t salt[] = "HSM_MIGRATE_SALT_V1";
    static const uint8_t info[] = "HSM_MIGRATE_KEY_V1";
    int hkdf_rc = hsm_hkdf_sha256(salt, sizeof(salt) - 1,
                                  shared_secret, sizeof(shared_secret),
                                  info, sizeof(info) - 1,
                                  key_and_iv, sizeof(key_and_iv));
    sss_zeroize(shared_secret, sizeof(shared_secret));
    if (hkdf_rc != 0) {
        sss_zeroize(key_and_iv, sizeof(key_and_iv));
        return -4;
    }

    uint8_t *k_enc = key_and_iv;
    uint8_t *iv = key_and_iv + 32;

    /* 3. Decrypt and authenticate using AES-256-GCM */
    psa_key_id_t aes_key = 0;
    psa_key_attributes_t aes_attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&aes_attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&aes_attr, 256);
    psa_set_key_usage_flags(&aes_attr, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&aes_attr, PSA_ALG_GCM);

    status = psa_import_key(&aes_attr, k_enc, 32, &aes_key);
    psa_reset_key_attributes(&aes_attr);
    if (status != PSA_SUCCESS) {
        sss_zeroize(key_and_iv, sizeof(key_and_iv));
        return -5;
    }

    uint8_t ct_and_tag[32 + 16];
    memcpy(ct_and_tag, ciphertext, 32);
    memcpy(ct_and_tag + 32, tag_16, 16);

    uint8_t decrypted_priv[32];
    size_t pt_len = 0;
    status = psa_aead_decrypt(aes_key, PSA_ALG_GCM, iv, 12, NULL, 0,
                              ct_and_tag, sizeof(ct_and_tag), decrypted_priv, sizeof(decrypted_priv), &pt_len);
    (void)psa_destroy_key(aes_key);
    sss_zeroize(key_and_iv, sizeof(key_and_iv));

    if (status != PSA_SUCCESS || pt_len != 32) {
        LOG_ERR("psa_aead_decrypt unwrap auth failure: %d", status);
        sss_zeroize(decrypted_priv, sizeof(decrypted_priv));
        return -6;
    }

    /* 4. Import decrypted private key into slot */
    uint8_t pubkey_65b[CC310_P256_PUBKEY_SIZE];
    size_t pub_len = 0;
    int imp_rc = crypto_cc310_import_p256_keypair(slot_id, decrypted_priv, pt_len, pubkey_65b, &pub_len);
    sss_zeroize(decrypted_priv, sizeof(decrypted_priv));

    if (imp_rc != 0 || pub_len != CC310_P256_PUBKEY_SIZE) {
        LOG_ERR("crypto_cc310_import_p256_keypair failed: %d", imp_rc);
        return -7;
    }

    char label[32];
    snprintf(label, sizeof(label), "MigratedKey%u", slot_id);
    keystore_set_slot(slot_id, "EC_P256", 256, label, pubkey_65b);

    LOG_INF("Successfully unwrapped and imported key to slot %u", slot_id);

    /* Immediate self-lock transition */
    keystore_lock();
    return 0;
}
