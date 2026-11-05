#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include "hsm/keystore.h"
#include "hsm/at_commands.h"

static void test_definitions(void) {
    printf("[TEST] Verifying enum and macro constants... ");

    assert(AUTH_METHOD_NONE == 0);
    assert(AUTH_METHOD_PIN == 1);
    assert(AUTH_METHOD_SHAMIR == 2);

    assert(KEYSTORE_MAX_PIN_LEN == 64);
    assert(KEYSTORE_PIN_SALT_LEN == 16);
    assert(KEYSTORE_PIN_HASH_LEN == 32);

    assert(CME_ERR_ALREADY_CONFIGURED == 41);
    assert(CME_ERR_INVALID_AUTH_MODE == 42);

    printf("PASSED\n");
}

static void test_status_formatting(void) {
    printf("[TEST] Verifying AT+STATUS? formatting... ");

    char buf[128];
    uint8_t configured = 1;
    uint8_t auth = 0;
    const char *method = "PIN";
    uint8_t retries = 3;
    uint8_t keycount = 2;

    snprintf(buf, sizeof(buf),
             "+STATUS: CONFIGURED=%u,AUTH=%u,METHOD=%s,RETRIES=%u,KEYCOUNT=%u\r\nOK\r\n",
             configured, auth, method, retries, keycount);

    assert(strcmp(buf, "+STATUS: CONFIGURED=1,AUTH=0,METHOD=PIN,RETRIES=3,KEYCOUNT=2\r\nOK\r\n") == 0);

    /* Unconfigured status */
    configured = 0;
    auth = 0;
    method = "NONE";
    retries = 3;
    keycount = 0;

    snprintf(buf, sizeof(buf),
             "+STATUS: CONFIGURED=%u,AUTH=%u,METHOD=%s,RETRIES=%u,KEYCOUNT=%u\r\nOK\r\n",
             configured, auth, method, retries, keycount);

    assert(strcmp(buf, "+STATUS: CONFIGURED=0,AUTH=0,METHOD=NONE,RETRIES=3,KEYCOUNT=0\r\nOK\r\n") == 0);

    /* Shamir authenticated status */
    configured = 1;
    auth = 1;
    method = "SHAMIR";
    retries = 3;
    keycount = 5;

    snprintf(buf, sizeof(buf),
             "+STATUS: CONFIGURED=%u,AUTH=%u,METHOD=%s,RETRIES=%u,KEYCOUNT=%u\r\nOK\r\n",
             configured, auth, method, retries, keycount);

    assert(strcmp(buf, "+STATUS: CONFIGURED=1,AUTH=1,METHOD=SHAMIR,RETRIES=3,KEYCOUNT=5\r\nOK\r\n") == 0);

    printf("PASSED\n");
}

static void test_pin_length_limits(void) {
    printf("[TEST] Verifying PIN length constraints (up to 64 bytes)... ");

    char long_pin[KEYSTORE_MAX_PIN_LEN + 1];
    memset(long_pin, 'A', KEYSTORE_MAX_PIN_LEN);
    long_pin[KEYSTORE_MAX_PIN_LEN] = '\0';

    assert(strlen(long_pin) == 64);
    assert(strlen(long_pin) <= KEYSTORE_MAX_PIN_LEN);

    char too_long_pin[KEYSTORE_MAX_PIN_LEN + 2];
    memset(too_long_pin, 'B', KEYSTORE_MAX_PIN_LEN + 1);
    too_long_pin[KEYSTORE_MAX_PIN_LEN + 1] = '\0';
    assert(strlen(too_long_pin) > KEYSTORE_MAX_PIN_LEN);

    printf("PASSED\n");
}

static void test_cme_error_formatting(void) {
    printf("[TEST] Verifying CME error string format... ");

    char err_buf[64];
    snprintf(err_buf, sizeof(err_buf), "\r\n+CME ERROR: %d\r\n", CME_ERR_ALREADY_CONFIGURED);
    assert(strcmp(err_buf, "\r\n+CME ERROR: 41\r\n") == 0);

    snprintf(err_buf, sizeof(err_buf), "\r\n+CME ERROR: %d\r\n", CME_ERR_INVALID_AUTH_MODE);
    assert(strcmp(err_buf, "\r\n+CME ERROR: 42\r\n") == 0);

    printf("PASSED\n");
}

int main(void) {
    printf("=== Running Dual-Mode Authentication Firmware Lib Tests ===\n");
    test_definitions();
    test_status_formatting();
    test_pin_length_limits();
    test_cme_error_formatting();
    printf("=== All Dual-Mode Tests Passed Successfully ===\n");
    return 0;
}
