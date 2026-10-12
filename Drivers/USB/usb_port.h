#ifndef USB_PORT_H
#define USB_PORT_H
#include <stdint.h>
#include <stdbool.h>
uint32_t usb_port_millis(void);
void usb_port_connect(bool);
#endif
