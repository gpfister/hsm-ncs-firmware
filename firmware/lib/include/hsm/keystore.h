#ifndef HSM_KEYSTORE_H_
#define HSM_KEYSTORE_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KEYSTORE_MAX_SLOTS 16
#define KEYSTORE_DEFAULT_PIN "123456"
#define KEYSTORE_MAX_RETRIES 3
#define KEYSTORE_MAX_PIN_LEN 64
#define KEYSTORE_PIN_SALT_LEN 16
#define KEYSTORE_PIN_HASH_LEN 32

#define HSM_EPHEMERAL_PUBKEY_SIZE 65
#define HSM_GCM_TAG_SIZE 16
#define HSM_WRAPPED_KEY_SIZE 32

typedef enum {
    AUTH_METHOD_NONE = 0,
    AUTH_METHOD_PIN = 1,
    AUTH_METHOD_SHAMIR = 2,
} auth_method_t;

typedef struct {
    bool occupied;
    char algo[16];   /* "EC_P256" */
    uint16_t size;   /* 256 */
    char label[32];  /* "Key1" */
    uint8_t pubkey[65];
} keystore_slot_t;

typedef void (*keystore_state_change_cb_t)(bool authenticated, uint8_t retries, uint8_t key_count);

/**
 * Initialize persistent keystore and auth supervisor.
 */
int keystore_init(keystore_state_change_cb_t cb);

/**
 * Check if the device is configured with an authentication method.
 */
bool keystore_is_configured(void);

/**
 * Get active authentication method.
 */
auth_method_t keystore_get_auth_method(void);

/**
 * Initialize persistent keystore with PIN authentication.
 * Validates device not yet configured, stores salt/hash, sets auth_method = AUTH_METHOD_PIN,
 * marks configured, and commits to flash.
 *
 * @param pin Null-terminated PIN string
 * @param pin_len Length of PIN (between 6 and KEYSTORE_MAX_PIN_LEN)
 * @return 0 on success, negative error code on failure.
 */
int keystore_init_pin(const char *pin, size_t pin_len);

/**
 * Initialize persistent keystore with Shamir secret sharing authentication.
 * Validates device not yet configured, sets auth_method = AUTH_METHOD_SHAMIR,
 * marks configured, and commits to flash.
 *
 * @return 0 on success, negative error code on failure.
 */
int keystore_init_shamir(void);

/**
 * Check if session is currently authenticated.
 */
bool keystore_is_authenticated(void);

/**
 * Get remaining PIN retry count.
 */
uint8_t keystore_get_retries(void);

/**
 * Get count of configured/occupied key slots.
 */
uint8_t keystore_get_key_count(void);

/**
 * Authenticate with PIN.
 * @return 0 on success, -1 on invalid PIN, -2 on blocked.
 */
int keystore_authenticate(const char *pin);

/**
 * Lock active session (revert to locked state).
 */
void keystore_lock(void);

/**
 * Reset transient session state.
 */
void keystore_reset(void);

/**
 * Irreversible factory reset and cryptographic zeroization.
 * Destroys all persistent CC310 hardware keys, destroys volatile ephemeral migration keys,
 * securely wipes master Shamir secret, resets slot table and PIN retries to factory defaults,
 * commits cleared state to NVS flash, and transitions the HSM into unconfigured state.
 *
 * @return 0 on success, negative error code on failure.
 */
int keystore_factory_reset(void);

/**
 * Check if slot is occupied.
 */
bool keystore_is_slot_occupied(uint8_t slot_id);

/**
 * Retrieve slot metadata pointer.
 */
const keystore_slot_t* keystore_get_slot(uint8_t slot_id);

/**
 * Set slot metadata and mark occupied.
 */
int keystore_set_slot(uint8_t slot_id, const char *algo, uint16_t size, const char *label, const uint8_t *pubkey_65b);

/**
 * Delete key slot.
 */
int keystore_delete_slot(uint8_t slot_id);

/**
 * Store master Shamir secret in keystore.
 */
int keystore_set_master_secret(const uint8_t *secret_32b);

/**
 * Verify reconstructed Shamir secret against master secret.
 * If verified, marks keystore authenticated and returns 0.
 */
int keystore_verify_master_secret(const uint8_t *secret_32b);

/**
 * Check if keystore is initialized with Shamir secret.
 */
bool keystore_is_shamir_initialized(void);

/**
 * Generate ephemeral target EC P-256 keypair for blind migration.
 * Public key (uncompressed point 0x04 || X || Y) stored in pub_out_65.
 * Private key retained in volatile memory.
 */
int keystore_generate_ephemeral_keypair(uint8_t *pub_out_65);

/**
 * Perform ECDH hardware wrapping of private key at slot_id using target_pub_65.
 * Generates ephemeral source keypair, computes ECDH, derives AES-256-GCM key, wraps slot private key.
 * Immediately zeroizes secrets and invokes self-lock callback.
 */
int keystore_wrap_key_ecdh(uint8_t slot_id,
                           const uint8_t *target_pub_65,
                           uint8_t *source_pub_65,
                           uint8_t *ciphertext_out,
                           uint8_t *tag_16,
                           size_t *ct_len);

/**
 * Perform ECDH hardware unwrapping of private key into slot_id using stored target ephemeral private key.
 * Computes ECDH with source_pub_65, derives AES-256-GCM key, decrypts ciphertext, validates tag_16,
 * stores imported private key into slot_id.
 * Immediately zeroizes secrets, clears ephemeral target key, and invokes self-lock callback.
 */
int keystore_unwrap_key_ecdh(uint8_t slot_id,
                             const uint8_t *source_pub_65,
                             const uint8_t *ciphertext,
                             size_t ct_len,
                             const uint8_t *tag_16);

#ifdef __cplusplus
}
#endif

#endif /* HSM_KEYSTORE_H_ */
