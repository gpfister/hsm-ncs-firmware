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

#ifdef __cplusplus
}
#endif

#endif /* HSM_KEYSTORE_H_ */
