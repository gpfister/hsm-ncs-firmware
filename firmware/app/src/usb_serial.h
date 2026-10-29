#ifndef USB_SERIAL_H_
#define USB_SERIAL_H_

#ifdef __cplusplus
extern "C" {
#endif

int usb_serial_init(void);
void usb_serial_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_SERIAL_H_ */
