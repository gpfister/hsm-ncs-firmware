#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <hsm/at_parser.h>
#include <hsm/at_commands.h>
#include "usb_serial.h"

LOG_MODULE_REGISTER(usb_serial, CONFIG_LOG_DEFAULT_LEVEL);

static const struct device *s_uart_hsm = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart1));

static at_parser_t s_parser;
static char s_line_buf[AT_PARSER_MAX_LINE_LEN];
static char s_resp_buf[1024];

void usb_serial_write_at_response(const char *resp) {
    if (!s_uart_hsm || !device_is_ready(s_uart_hsm) || !resp) {
        return;
    }
    size_t len = strlen(resp);
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(s_uart_hsm, (unsigned char)resp[i]);
    }
}

void usb_serial_poll(void) {
    if (!s_uart_hsm || !device_is_ready(s_uart_hsm)) {
        return;
    }

    uint8_t c;
    while (uart_poll_in(s_uart_hsm, &c) == 0) {
        bool line_ready = false;
        int rc = at_parser_feed(&s_parser, c, s_line_buf, sizeof(s_line_buf), &line_ready);

        if (rc < 0) {
            /* Overflow detected */
            at_commands_format_error(s_resp_buf, sizeof(s_resp_buf), CME_ERR_BUFFER_OVERFLOW);
            usb_serial_write_at_response(s_resp_buf);
            continue;
        }

        if (line_ready) {
            LOG_DBG("AT CMD RX: %s", s_line_buf);
            s_resp_buf[0] = '\0';
            int exec_rc = at_commands_execute(s_line_buf, s_resp_buf, sizeof(s_resp_buf));
            if (exec_rc == 0 && s_resp_buf[0] != '\0') {
                LOG_DBG("AT RESP TX: %s", s_resp_buf);
                usb_serial_write_at_response(s_resp_buf);
            }
        }
    }
}

int usb_serial_init(void) {
    at_parser_init(&s_parser);

    int ret = usb_enable(NULL);
    if (ret != 0) {
        LOG_ERR("Failed to enable USB: %d", ret);
        return ret;
    }

    LOG_INF("Dual USB CDC ACM stack initialized successfully.");
    return 0;
}
