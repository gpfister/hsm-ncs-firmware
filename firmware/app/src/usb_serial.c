#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/logging/log.h>
#include <hsm/hsm_core.h>
#include <hsm/yubihsm_proto.h>
#include "usb_serial.h"

LOG_MODULE_REGISTER(usb_serial, CONFIG_LOG_DEFAULT_LEVEL);

static const struct device *s_uart_hsm = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart1));

static uint8_t s_rx_buf[YMS_MAX_PAYLOAD_SIZE + YMS_HEADER_SIZE];
static size_t s_rx_len = 0;
static uint8_t s_tx_buf[YMS_MAX_PAYLOAD_SIZE + YMS_HEADER_SIZE];

static void uart_hsm_send_all(const uint8_t *data, size_t len) {
    if (!s_uart_hsm || !device_is_ready(s_uart_hsm)) return;
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(s_uart_hsm, data[i]);
    }
}

void usb_serial_poll(void) {
    if (!s_uart_hsm || !device_is_ready(s_uart_hsm)) {
        return;
    }

    uint8_t c;
    while (uart_poll_in(s_uart_hsm, &c) == 0) {
        if (s_rx_len < sizeof(s_rx_buf)) {
            s_rx_buf[s_rx_len++] = c;
        } else {
            /* Overflow, reset */
            s_rx_len = 0;
            continue;
        }

        /* Check if we have complete header */
        if (s_rx_len >= YMS_HEADER_SIZE) {
            yms_header_t *hdr = (yms_header_t *)s_rx_buf;
            uint16_t expected_payload_len = yms_get_len(hdr);
            size_t total_expected = (size_t)expected_payload_len + YMS_HEADER_SIZE;

            if (s_rx_len >= total_expected) {
                /* Packet complete -> process */
                size_t resp_len = 0;
                int rc = hsm_process_packet(s_rx_buf, total_expected,
                                            s_tx_buf, sizeof(s_tx_buf), &resp_len);
                if (rc == 0 && resp_len > 0) {
                    uart_hsm_send_all(s_tx_buf, resp_len);
                }

                /* Shift remaining if any (usually none) */
                if (s_rx_len > total_expected) {
                    size_t rem = s_rx_len - total_expected;
                    memmove(s_rx_buf, s_rx_buf + total_expected, rem);
                    s_rx_len = rem;
                } else {
                    s_rx_len = 0;
                }
            }
        }
    }
}

int usb_serial_init(void) {
    int ret = usb_enable(NULL);
    if (ret != 0) {
        LOG_ERR("Failed to enable USB: %d", ret);
        return ret;
    }

    LOG_INF("USB Device stack initialized successfully.");
    return 0;
}
