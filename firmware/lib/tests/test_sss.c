#include "hsm/sss.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#define TEST_PASS 0
#define TEST_FAIL 1

static void test_gf256_arithmetic(void) {
    printf("[TEST] GF(256) arithmetic properties... ");

    /* Addition (XOR) */
    assert(gf256_add(0x57, 0x83) == (0x57 ^ 0x83));
    assert(gf256_add(0xAA, 0xAA) == 0x00);
    assert(gf256_add(0x00, 0x42) == 0x42);

    /* Multiplication Identity and Zero */
    assert(gf256_mul(0x00, 0x55) == 0x00);
    assert(gf256_mul(0x55, 0x00) == 0x00);
    assert(gf256_mul(0x01, 0x7E) == 0x7E);
    assert(gf256_mul(0x7E, 0x01) == 0x7E);

    /* Division Identity and Self */
    assert(gf256_div(0x00, 0x44) == 0x00);
    assert(gf256_div(0x44, 0x01) == 0x44);
    assert(gf256_div(0x44, 0x44) == 0x01);

    /* Multiplication & Division Inverse for all non-zero elements */
    for (int a = 1; a < 256; a++) {
        for (int b = 1; b < 256; b++) {
            uint8_t q = gf256_div((uint8_t)a, (uint8_t)b);
            uint8_t prod = gf256_mul(q, (uint8_t)b);
            assert(prod == (uint8_t)a);
        }
    }

    printf("PASSED\n");
}

static void test_shamir_all_combinations(void) {
    printf("[TEST] Shamir 3-of-5 all 10 combinations... ");

    static const uint8_t original_secret[SSS_SECRET_SIZE] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
        0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10
    };

    sss_share_t shares[SSS_TOTAL_SHARES];
    int rc = sss_split(original_secret, SSS_SECRET_SIZE, shares, SSS_TOTAL_SHARES, SSS_THRESHOLD);
    assert(rc == 0);

    /* Verify indices 1..5 */
    for (int i = 0; i < 5; i++) {
        assert(shares[i].index == (uint8_t)(i + 1));
    }

    /* All 10 combinations of 3 shares from 5: (n choose k = 5 choose 3 = 10) */
    static const int combos[10][3] = {
        {0, 1, 2}, /* 1, 2, 3 */
        {0, 1, 3}, /* 1, 2, 4 */
        {0, 1, 4}, /* 1, 2, 5 */
        {0, 2, 3}, /* 1, 3, 4 */
        {0, 2, 4}, /* 1, 3, 5 */
        {0, 3, 4}, /* 1, 4, 5 */
        {1, 2, 3}, /* 2, 3, 4 */
        {1, 2, 4}, /* 2, 3, 5 */
        {1, 3, 4}, /* 2, 4, 5 */
        {2, 3, 4}  /* 3, 4, 5 */
    };

    for (int c = 0; c < 10; c++) {
        sss_share_t subset[3];
        subset[0] = shares[combos[c][0]];
        subset[1] = shares[combos[c][1]];
        subset[2] = shares[combos[c][2]];

        uint8_t recovered[SSS_SECRET_SIZE];
        memset(recovered, 0, sizeof(recovered));

        rc = sss_recover(subset, 3, recovered, SSS_SECRET_SIZE);
        assert(rc == 0);
        assert(memcmp(recovered, original_secret, SSS_SECRET_SIZE) == 0);
    }

    printf("PASSED (10/10 combinations matched)\n");
}

static void test_duplicate_share_rejection(void) {
    printf("[TEST] Duplicate share index rejection... ");

    uint8_t dummy_secret[SSS_SECRET_SIZE] = {0x42};
    sss_share_t shares[SSS_TOTAL_SHARES];
    assert(sss_split(dummy_secret, SSS_SECRET_SIZE, shares, 5, 3) == 0);

    /* Create subset with duplicate index */
    sss_share_t invalid_subset[3];
    invalid_subset[0] = shares[0]; /* index 1 */
    invalid_subset[1] = shares[0]; /* index 1 (duplicate!) */
    invalid_subset[2] = shares[2]; /* index 3 */

    uint8_t recovered[SSS_SECRET_SIZE];
    int rc = sss_recover(invalid_subset, 3, recovered, SSS_SECRET_SIZE);
    assert(rc == -3); /* Must reject duplicate with -3 */

    printf("PASSED (rc=%d)\n", rc);
}

static void test_zeroization(void) {
    printf("[TEST] Sensitive buffer zeroization... ");

    uint8_t secret[SSS_SECRET_SIZE];
    memset(secret, 0xA5, SSS_SECRET_SIZE);

    sss_zeroize(secret, SSS_SECRET_SIZE);
    for (size_t i = 0; i < SSS_SECRET_SIZE; i++) {
        assert(secret[i] == 0x00);
    }

    printf("PASSED\n");
}

static void test_lagrange_timing_benchmark(void) {
    printf("[TEST] Lagrange reconstruction timing benchmark... ");

    uint8_t secret[SSS_SECRET_SIZE];
    for (int i = 0; i < SSS_SECRET_SIZE; i++) secret[i] = (uint8_t)i;

    sss_share_t shares[SSS_TOTAL_SHARES];
    assert(sss_split(secret, SSS_SECRET_SIZE, shares, 5, 3) == 0);

    sss_share_t subset[3] = {shares[0], shares[2], shares[4]};
    uint8_t recovered[SSS_SECRET_SIZE];

    clock_t start = clock();
    const int iterations = 1000;
    for (int i = 0; i < iterations; i++) {
        int rc = sss_recover(subset, 3, recovered, SSS_SECRET_SIZE);
        assert(rc == 0);
    }
    clock_t end = clock();

    double total_ms = ((double)(end - start) / CLOCKS_PER_SEC) * 1000.0;
    double avg_ms = total_ms / iterations;

    printf("PASSED (Avg %.4f ms per reconstruction; limit < 5.0 ms)\n", avg_ms);
    assert(avg_ms < 5.0);
}

int main(void) {
    printf("==================================================\n");
    printf(" Shamir Secret Sharing Engine (GF256) Unit Tests \n");
    printf("==================================================\n");

    test_gf256_arithmetic();
    test_shamir_all_combinations();
    test_duplicate_share_rejection();
    test_zeroization();
    test_lagrange_timing_benchmark();

    printf("==================================================\n");
    printf(" All SSS unit tests passed successfully!         \n");
    printf("==================================================\n");
    return 0;
}
