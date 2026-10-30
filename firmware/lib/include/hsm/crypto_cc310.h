#ifndef HSM_CRYPTO_CC310_H_
#define HSM_CRYPTO_CC310_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CC310_P256_PUBKEY_SIZE 65
#define CC310_P256_SIGNATURE_SIZE 64
#define CC310_SHA256_DIGEST_SIZE 32
#define CC310_P256_PRIVKEY_SIZE 32

/**
 * Initialize hardware CryptoCell CC310 / PSA Crypto subsystem.
 */
int crypto_cc310_init(void);

/**
 * Generate cryptographically secure random bytes using hardware TRNG.
 */
int crypto_cc310_get_random(uint8_t *dest, size_t len);

/**
 * Generate a persistent NIST P-256 (secp256r1) keypair in the designated slot.
 */
int crypto_cc310_generate_p256_keypair(uint8_t slot_id, uint8_t *pubkey_65b, size_t *pubkey_len);

/**
 * Export uncompressed public key (0x04 || X || Y) for specified key slot.
 */
int crypto_cc310_export_p256_pubkey(uint8_t slot_id, uint8_t *pubkey_65b, size_t *pubkey_len);

/**
 * Sign 32-byte SHA-256 hash using the specified key slot.
 * Returns raw 64-byte signature (R || S).
 */
int crypto_cc310_sign_hash(uint8_t slot_id, const uint8_t *hash_32b, uint8_t *sig_64b, size_t *sig_len);

/**
 * Destroy/delete persistent key for the designated slot.
 */
int crypto_cc310_destroy_key(uint8_t slot_id);

/**
 * Export raw private scalar (32 bytes) for wrapping during migration.
 */
int crypto_cc310_export_p256_privkey(uint8_t slot_id, uint8_t *privkey_32b, size_t *privkey_len);

/**
 * Import raw private scalar (32 bytes) after unwrapping during migration.
 */
int crypto_cc310_import_p256_keypair(uint8_t slot_id, const uint8_t *privkey_32b, size_t privkey_len, uint8_t *pubkey_65b, size_t *pubkey_len);

#ifdef __cplusplus
}
#endif

#endif /* HSM_CRYPTO_CC310_H_ */
