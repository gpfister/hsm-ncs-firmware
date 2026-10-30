#ifndef HSM_SSS_H_
#define HSM_SSS_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SSS_SECRET_SIZE   32
#define SSS_THRESHOLD     3
#define SSS_TOTAL_SHARES  5

/**
 * Shamir Share Structure.
 * index: x-coordinate in GF(256), strictly in range [1, 5].
 * data:  32-byte secret fragment (y-values).
 */
typedef struct {
    uint8_t index;
    uint8_t data[SSS_SECRET_SIZE];
} sss_share_t;

/**
 * GF(256) field addition (constant time XOR) using irreducible polynomial 0x11D.
 */
uint8_t gf256_add(uint8_t a, uint8_t b);

/**
 * GF(256) field multiplication using ROM precomputed log/antilog tables.
 */
uint8_t gf256_mul(uint8_t a, uint8_t b);

/**
 * GF(256) field division using ROM precomputed log/antilog tables.
 */
uint8_t gf256_div(uint8_t a, uint8_t b);

/**
 * Constant-time zeroization of sensitive secret buffers with compiler barrier.
 */
void sss_zeroize(void *buf, size_t len);

/**
 * Split a secret into n shares with threshold k (3-of-5).
 * Generates polynomial coefficients using TRNG hardware or secure RNG.
 *
 * @param secret Pointer to secret buffer
 * @param secret_len Length of secret (typically 32 bytes)
 * @param shares_out Destination array of n shares
 * @param n Total shares to generate (5)
 * @param k Quorum threshold (3)
 * @return 0 on success, negative error code on invalid parameters
 */
int sss_split(const uint8_t *secret, size_t secret_len, sss_share_t *shares_out, uint8_t n, uint8_t k);

/**
 * Reconstruct a secret from k distinct shares using Lagrange interpolation.
 *
 * @param shares_in Array of k shares
 * @param k Number of shares provided (must equal SSS_THRESHOLD = 3)
 * @param secret_out Destination buffer for reconstructed secret
 * @param secret_len Length of secret (must be <= SSS_SECRET_SIZE)
 * @return 0 on success, negative error code on failure (e.g., duplicate index)
 */
int sss_recover(const sss_share_t *shares_in, uint8_t k, uint8_t *secret_out, size_t secret_len);

#ifdef __cplusplus
}
#endif

#endif /* HSM_SSS_H_ */
