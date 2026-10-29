#ifndef HSM_CORE_H_
#define HSM_CORE_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "yubihsm_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HSM_STATE_UNCONFIGURED = 0,
    HSM_STATE_LOCKED       = 1,
    HSM_STATE_UNLOCKED     = 2,
} hsm_state_t;

typedef void (*hsm_state_cb_t)(hsm_state_t state);
typedef int (*hsm_dfu_write_cb_t)(uint32_t offset, const uint8_t *data, size_t len);
typedef int (*hsm_dfu_apply_cb_t)(void);

typedef struct {
    hsm_state_cb_t on_state_changed;
    hsm_dfu_write_cb_t on_dfu_write;
    hsm_dfu_apply_cb_t on_dfu_apply;
} hsm_callbacks_t;

void hsm_init(const hsm_callbacks_t *cbs);
hsm_state_t hsm_get_state(void);
void hsm_set_state(hsm_state_t new_state);

/**
 * Process a YubiHSM binary frame received from USB serial
 */
int hsm_process_packet(const uint8_t *req, size_t req_len,
                       uint8_t *resp, size_t max_resp_len, size_t *resp_len);

#ifdef __cplusplus
}
#endif

#endif /* HSM_CORE_H_ */
