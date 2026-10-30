#ifndef HSM_AT_COMMANDS_H_
#define HSM_AT_COMMANDS_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CME Error Codes */
#define CME_ERR_DEVICE_LOCKED       1
#define CME_ERR_INVALID_PIN         2
#define CME_ERR_PIN_BLOCKED         3
#define CME_ERR_KEY_NOT_FOUND       4
#define CME_ERR_INVALID_PARAMETER   5
#define CME_ERR_CRYPTO_FAILURE      6
#define CME_ERR_BUFFER_OVERFLOW     7
#define CME_ERR_COMMAND_UNSUPPORTED 8
#define CME_ERR_DUPLICATE_SHARE     103

/**
 * Initialize AT command processor, auto-lock supervisor, and related subsystems.
 */
int at_commands_init(void);

/**
 * Execute an incoming AT command line.
 *
 * @param cmd_line Null-terminated incoming command line string
 * @param resp_buf Output buffer for the response string
 * @param max_resp_len Maximum capacity of response buffer
 * @return 0 on success (response generated)
 */
int at_commands_execute(const char *cmd_line, char *resp_buf, size_t max_resp_len);

/**
 * Generate a standard CME error response string.
 */
void at_commands_format_error(char *resp_buf, size_t max_resp_len, int error_code);

/**
 * Reset volatile share ceremony buffer.
 */
void at_commands_reset_shares(void);

#ifdef __cplusplus
}
#endif

#endif /* HSM_AT_COMMANDS_H_ */
