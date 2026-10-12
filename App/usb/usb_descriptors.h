#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H
/* serial 必须在 tusb_init 前设置，且指向覆盖 USB 运行期的 NUL 结尾字符串。 */
void usb_descriptors_set_serial(const char *serial);
#endif