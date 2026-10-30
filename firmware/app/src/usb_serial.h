#ifndef USB_SERIAL_H_
#define USB_SERIAL_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int usb_serial_init(void);
void usb_serial_poll(void);
void usb_serial_write_at_response(const char *resp);

#ifdef __cplusplus
}
#endif

#endif /* USB_SERIAL_H_ */
