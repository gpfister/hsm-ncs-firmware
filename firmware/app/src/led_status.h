#ifndef LED_STATUS_H_
#define LED_STATUS_H_

#include <hsm/hsm_core.h>

#ifdef __cplusplus
extern "C" {
#endif

int led_status_init(void);
void led_status_set_state(hsm_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* LED_STATUS_H_ */
