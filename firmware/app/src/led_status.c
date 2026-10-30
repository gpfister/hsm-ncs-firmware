#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include "led_status.h"

LOG_MODULE_REGISTER(led_status, CONFIG_LOG_DEFAULT_LEVEL);

/* RGB LED GPIO nodes on PCA10059 */
static const struct gpio_dt_spec s_led_red   = GPIO_DT_SPEC_GET(DT_NODELABEL(led1_red), gpios);
static const struct gpio_dt_spec s_led_green = GPIO_DT_SPEC_GET(DT_NODELABEL(led1_green), gpios);
static const struct gpio_dt_spec s_led_blue  = GPIO_DT_SPEC_GET(DT_NODELABEL(led1_blue), gpios);

/* RGB LED PWM nodes on PCA10059 */
#if DT_NODE_EXISTS(DT_NODELABEL(red_pwm_led))
static const struct pwm_dt_spec s_pwm_red = PWM_DT_SPEC_GET(DT_NODELABEL(red_pwm_led));
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(green_pwm_led))
static const struct pwm_dt_spec s_pwm_green = PWM_DT_SPEC_GET(DT_NODELABEL(green_pwm_led));
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(blue_pwm_led))
static const struct pwm_dt_spec s_pwm_blue = PWM_DT_SPEC_GET(DT_NODELABEL(blue_pwm_led));
#endif

static volatile hsm_state_t s_current_state = HSM_STATE_UNCONFIGURED;
static struct k_thread s_led_thread;
static K_THREAD_STACK_DEFINE(s_led_stack, 1024);
static struct k_sem s_state_sem;

static void led_thread_fn(void *arg1, void *arg2, void *arg3) {
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    bool pwm_red_ready = false;
    bool pwm_green_ready = false;
    bool pwm_blue_ready = false;

#if DT_NODE_EXISTS(DT_NODELABEL(red_pwm_led))
    pwm_red_ready = device_is_ready(s_pwm_red.dev);
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(green_pwm_led))
    pwm_green_ready = device_is_ready(s_pwm_green.dev);
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(blue_pwm_led))
    pwm_blue_ready = device_is_ready(s_pwm_blue.dev);
#endif

    int breath_counter = 0;
    int breath_dir = 1;

    while (1) {
        hsm_state_t st = s_current_state;

        switch (st) {
        case HSM_STATE_UNCONFIGURED: {
            /*
             * Synchronous white breathing glow across Red, Green, and Blue channels.
             * 3.0s total period: 50 steps up + 50 steps down at 30ms/step = 3000ms.
             */
            uint32_t duty = (uint32_t)(breath_counter * 2); /* 0% to 100% */

            if (pwm_red_ready) {
                uint32_t pulse_r = (uint32_t)((uint64_t)s_pwm_red.period * duty / 100);
                pwm_set_pulse_dt(&s_pwm_red, pulse_r);
            }
            if (pwm_green_ready) {
                uint32_t pulse_g = (uint32_t)((uint64_t)s_pwm_green.period * duty / 100);
                pwm_set_pulse_dt(&s_pwm_green, pulse_g);
            }
            if (pwm_blue_ready) {
                uint32_t pulse_b = (uint32_t)((uint64_t)s_pwm_blue.period * duty / 100);
                pwm_set_pulse_dt(&s_pwm_blue, pulse_b);
            }

            if (!pwm_red_ready || !pwm_green_ready || !pwm_blue_ready) {
                int on = (duty > 50) ? 1 : 0;
                gpio_pin_set_dt(&s_led_red, on);
                gpio_pin_set_dt(&s_led_green, on);
                gpio_pin_set_dt(&s_led_blue, on);
            }

            breath_counter += breath_dir;
            if (breath_counter >= 50) {
                breath_counter = 50;
                breath_dir = -1;
            } else if (breath_counter <= 0) {
                breath_counter = 0;
                breath_dir = 1;
            }

            /* Wait 30ms or wake immediately upon state transition */
            (void)k_sem_take(&s_state_sem, K_MSEC(30));
            break;
        }

        case HSM_STATE_LOCKED: {
            /*
             * Configured & Locked State:
             * Blue and Green strictly 0% (OFF), Red 100% solid ON.
             */
            if (pwm_blue_ready) {
                pwm_set_pulse_dt(&s_pwm_blue, 0);
            }
            if (pwm_green_ready) {
                pwm_set_pulse_dt(&s_pwm_green, 0);
            }
            if (pwm_red_ready) {
                pwm_set_pulse_dt(&s_pwm_red, s_pwm_red.period);
            }

            gpio_pin_set_dt(&s_led_blue, 0);
            gpio_pin_set_dt(&s_led_green, 0);
            gpio_pin_set_dt(&s_led_red, 1);

            /* Sleep until next state transition signal */
            (void)k_sem_take(&s_state_sem, K_FOREVER);
            break;
        }

        case HSM_STATE_UNLOCKED: {
            /*
             * Authenticated State:
             * Red and Blue strictly OFF (0%), Green 100% solid ON.
             */
            if (pwm_red_ready) {
                pwm_set_pulse_dt(&s_pwm_red, 0);
            }
            if (pwm_blue_ready) {
                pwm_set_pulse_dt(&s_pwm_blue, 0);
            }
            if (pwm_green_ready) {
                pwm_set_pulse_dt(&s_pwm_green, s_pwm_green.period);
            }

            gpio_pin_set_dt(&s_led_red, 0);
            gpio_pin_set_dt(&s_led_blue, 0);
            gpio_pin_set_dt(&s_led_green, 1);

            /* Sleep until next state transition signal */
            (void)k_sem_take(&s_state_sem, K_FOREVER);
            break;
        }
        }
    }
}

int led_status_init(void) {
    k_sem_init(&s_state_sem, 0, 1);

    if (!gpio_is_ready_dt(&s_led_red) || !gpio_is_ready_dt(&s_led_green) || !gpio_is_ready_dt(&s_led_blue)) {
        LOG_WRN("GPIO LED devices not all ready");
    }

    gpio_pin_configure_dt(&s_led_red, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&s_led_green, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&s_led_blue, GPIO_OUTPUT_INACTIVE);

    k_thread_create(&s_led_thread, s_led_stack,
                    K_THREAD_STACK_SIZEOF(s_led_stack),
                    led_thread_fn,
                    NULL, NULL, NULL,
                    K_PRIO_COOP(7), 0, K_NO_WAIT);
    k_thread_name_set(&s_led_thread, "led_status");

    return 0;
}

void led_status_set_state(hsm_state_t state) {
    s_current_state = state;
    /* Wake thread immediately (< 50ms) */
    k_sem_give(&s_state_sem);
}
