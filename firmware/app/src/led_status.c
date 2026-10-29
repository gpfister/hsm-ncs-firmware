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

#if DT_NODE_EXISTS(DT_NODELABEL(blue_pwm_led))
static const struct pwm_dt_spec s_pwm_blue = PWM_DT_SPEC_GET(DT_NODELABEL(blue_pwm_led));
#endif

static volatile hsm_state_t s_current_state = HSM_STATE_UNCONFIGURED;
static struct k_thread s_led_thread;
static K_THREAD_STACK_DEFINE(s_led_stack, 1024);

static void led_thread_fn(void *arg1, void *arg2, void *arg3) {
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    int breath_counter = 0;
    int breath_dir = 1;

    while (1) {
        hsm_state_t st = s_current_state;

        switch (st) {
        case HSM_STATE_UNCONFIGURED: {
            /* Slowly pulsating blue */
            gpio_pin_set_dt(&s_led_red, 0);
            gpio_pin_set_dt(&s_led_green, 0);

#if DT_NODE_EXISTS(DT_NODELABEL(blue_pwm_led))
            if (device_is_ready(s_pwm_blue.dev)) {
                uint32_t pulse = (uint32_t)((uint64_t)s_pwm_blue.period * breath_counter / 100);
                pwm_set_pulse_dt(&s_pwm_blue, pulse);
            } else {
                gpio_pin_set_dt(&s_led_blue, (breath_counter > 50) ? 1 : 0);
            }
#else
            gpio_pin_set_dt(&s_led_blue, (breath_counter > 50) ? 1 : 0);
#endif

            breath_counter += breath_dir * 2;
            if (breath_counter >= 100) {
                breath_counter = 100;
                breath_dir = -1;
            } else if (breath_counter <= 0) {
                breath_counter = 0;
                breath_dir = 1;
            }
            k_msleep(30);
            break;
        }

        case HSM_STATE_LOCKED: {
            /* Solid RED */
#if DT_NODE_EXISTS(DT_NODELABEL(blue_pwm_led))
            if (device_is_ready(s_pwm_blue.dev)) {
                pwm_set_pulse_dt(&s_pwm_blue, 0);
            }
#endif
            gpio_pin_set_dt(&s_led_blue, 0);
            gpio_pin_set_dt(&s_led_green, 0);
            gpio_pin_set_dt(&s_led_red, 1);
            k_msleep(100);
            break;
        }

        case HSM_STATE_UNLOCKED: {
            /* Solid GREEN */
#if DT_NODE_EXISTS(DT_NODELABEL(blue_pwm_led))
            if (device_is_ready(s_pwm_blue.dev)) {
                pwm_set_pulse_dt(&s_pwm_blue, 0);
            }
#endif
            gpio_pin_set_dt(&s_led_blue, 0);
            gpio_pin_set_dt(&s_led_red, 0);
            gpio_pin_set_dt(&s_led_green, 1);
            k_msleep(100);
            break;
        }
        }
    }
}

int led_status_init(void) {
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
}
