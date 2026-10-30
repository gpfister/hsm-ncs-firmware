#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_ficr.h>
#include <stdio.h>
#include <string.h>
#include <hsm/at_parser.h>
#include <hsm/at_commands.h>
#include "usb_serial.h"

LOG_MODULE_REGISTER(usb_serial, CONFIG_LOG_DEFAULT_LEVEL);

#ifndef USB_BSTRING_LENGTH
#define USB_BSTRING_LENGTH(s) (sizeof(s) * 2 - 2)
#endif
#ifndef USB_STRING_DESCRIPTOR_LENGTH
#define USB_STRING_DESCRIPTOR_LENGTH(s) (sizeof(s) * 2)
#endif

#define CONSOLE_IF_STR "Zephyr CDC ACM Console"
#define HSM_IF_STR     "nRF52840 HSM AT Interface"

struct usb_console_if_string_desc {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bString[USB_BSTRING_LENGTH(CONSOLE_IF_STR)];
} __packed;

struct usb_hsm_if_string_desc {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bString[USB_BSTRING_LENGTH(HSM_IF_STR)];
} __packed;

USBD_STRING_DESCR_USER_DEFINE(primary) struct usb_console_if_string_desc s_console_str_desc = {
	.bLength = USB_STRING_DESCRIPTOR_LENGTH(CONSOLE_IF_STR),
	.bDescriptorType = USB_DESC_STRING,
	.bString = CONSOLE_IF_STR,
};

USBD_STRING_DESCR_USER_DEFINE(primary) struct usb_hsm_if_string_desc s_hsm_str_desc = {
	.bLength = USB_STRING_DESCRIPTOR_LENGTH(HSM_IF_STR),
	.bDescriptorType = USB_DESC_STRING,
	.bString = HSM_IF_STR,
};

static const struct device *s_uart_hsm = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart1));

static at_parser_t s_parser;
static char s_line_buf[AT_PARSER_MAX_LINE_LEN];
static char s_resp_buf[1024];

static char s_ble_sn[13];
static bool s_ble_sn_ready;

static void compute_ble_sn(void)
{
	if (s_ble_sn_ready) {
		return;
	}

	uint32_t addr1 = 0;
	uint32_t addr0 = 0;

#if defined(NRF_FICR)
	addr1 = NRF_FICR->DEVICEADDR[1] & 0x0000FFFFUL;
	addr0 = NRF_FICR->DEVICEADDR[0];

	/* If Random Static Address type per BLE spec, set upper two bits */
	if ((NRF_FICR->DEVICEADDRTYPE & 0x01UL) != 0) {
		addr1 |= 0x0000C000UL;
	}

	/* Fallback to DEVICEID if DEVICEADDR is blank/unprogrammed */
	if ((addr0 == 0xFFFFFFFFUL && (addr1 & 0xFFFFUL) == 0xFFFFUL) ||
	    (addr0 == 0 && (addr1 & 0xFFFFUL) == 0)) {
		addr0 = NRF_FICR->DEVICEID[0];
		addr1 = NRF_FICR->DEVICEID[1] & 0x0000FFFFUL;
	}
#endif

	/* Fallback to hwinfo driver if FICR still blank */
	if ((addr0 == 0xFFFFFFFFUL && (addr1 & 0xFFFFUL) == 0xFFFFUL) ||
	    (addr0 == 0 && (addr1 & 0xFFFFUL) == 0)) {
		uint8_t hwid[6] = {0};
		ssize_t hwlen = hwinfo_get_device_id(hwid, sizeof(hwid));
		if (hwlen > 0) {
			addr1 = ((uint32_t)hwid[0] << 8) | hwid[1];
			addr0 = ((uint32_t)hwid[2] << 24) | ((uint32_t)hwid[3] << 16) |
				((uint32_t)hwid[4] << 8) | hwid[5];
		} else {
			addr1 = 0x0000C0DEUL;
			addr0 = 0xCAFE0001UL;
		}
	}

	/* Format into a 12-character uppercase hex string (MSB-to-LSB) */
	snprintf(s_ble_sn, sizeof(s_ble_sn), "%04X%08X",
		 (unsigned int)(addr1 & 0xFFFFUL), (unsigned int)addr0);
	s_ble_sn_ready = true;
}

/* Override Zephyr's weak hook to return the dynamic 12-char BLE serial number */
uint8_t *usb_update_sn_string_descriptor(void)
{
	compute_ble_sn();
	return (uint8_t *)s_ble_sn;
}

const char *usb_serial_get_ble_address(void)
{
	compute_ble_sn();
	return s_ble_sn;
}

extern uint8_t *usb_get_device_descriptor(void);
extern int usb_get_str_descriptor_idx(void *ptr);
extern struct usb_desc_header __usb_descriptor_start[];

static int usb_serial_patch_descriptors(void)
{
	/* Force descriptor table generation & fixup first */
	uint8_t *dev_desc = usb_get_device_descriptor();
	if (!dev_desc) {
		LOG_ERR("Failed to get device descriptor");
		return -EINVAL;
	}

	int console_idx = usb_get_str_descriptor_idx(&s_console_str_desc);
	int hsm_idx = usb_get_str_descriptor_idx(&s_hsm_str_desc);

	LOG_DBG("String descriptor indices: console=%d, hsm=%d", console_idx, hsm_idx);

	struct usb_desc_header *head = __usb_descriptor_start;
	while (head->bLength != 0U) {
		if (head->bDescriptorType == USB_DESC_INTERFACE) {
			struct usb_if_descriptor *if_desc = (struct usb_if_descriptor *)head;
			if (if_desc->bInterfaceNumber == 0) {
				if_desc->bInterfaceProtocol = 0x01;
				if (console_idx > 0) {
					if_desc->iInterface = (uint8_t)console_idx;
				}
				LOG_INF("Patched Interface 0 (Console): proto=0x0001, iInterface=%u",
					if_desc->iInterface);
			} else if (if_desc->bInterfaceNumber == 2) {
				if_desc->bInterfaceProtocol = 0x02;
				if (hsm_idx > 0) {
					if_desc->iInterface = (uint8_t)hsm_idx;
				}
				LOG_INF("Patched Interface 2 (HSM AT): proto=0x0002, iInterface=%u",
					if_desc->bInterfaceProtocol, if_desc->iInterface);
			}
		} else if (head->bDescriptorType == USB_DESC_INTERFACE_ASSOC) {
			struct usb_association_descriptor *iad = (struct usb_association_descriptor *)head;
			if (iad->bFirstInterface == 0) {
				iad->bFunctionProtocol = 0x01;
				if (console_idx > 0) {
					iad->iFunction = (uint8_t)console_idx;
				}
			} else if (iad->bFirstInterface == 2) {
				iad->bFunctionProtocol = 0x02;
				if (hsm_idx > 0) {
					iad->iFunction = (uint8_t)hsm_idx;
				}
			}
		}

		head = (struct usb_desc_header *)((uint8_t *)head + head->bLength);
	}

	return 0;
}

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

	/* Pre-compute dynamic serial number */
	compute_ble_sn();
	LOG_INF("USB Serial Number (BLE Address): %s", s_ble_sn);

	/* Patch descriptors before enabling USB stack */
	int ret = usb_serial_patch_descriptors();
	if (ret != 0) {
		LOG_WRN("Descriptor patching returned %d", ret);
	}

	ret = usb_enable(NULL);
	if (ret != 0) {
		LOG_ERR("Failed to enable USB: %d", ret);
		return ret;
	}

	LOG_INF("Dual USB CDC ACM stack initialized successfully.");
	return 0;
}
