/* USB 身份集中定义（CLAUDE.md 约定：编译期宏，出货前一次性替换）。
 * 当前为过渡期临时值（pid.codes 测试段 VID=0x1209），正式 VID/PID 走 ST 分配计划。
 * 可用 -DUSB_VID=... / -DUSB_PID_APP=... 在构建时覆盖。 */
#ifndef USB_IDENTITY_H
#define USB_IDENTITY_H

#ifndef USB_VID
#define USB_VID        0x1209u   /* 临时：pid.codes。出货前替换为 ST 分配的正式 VID */
#endif
#ifndef USB_PID_APP
#define USB_PID_APP    0x0001u   /* 应用态 PID（临时） */
#endif
#ifndef USB_PID_BOOT
#define USB_PID_BOOT   0x0002u   /* 独立 HID Bootloader 态 PID */
#endif

#ifndef USB_MANUFACTURER
#define USB_MANUFACTURER "AURASKY"
#endif
#ifndef USB_PRODUCT
#define USB_PRODUCT      "BV500-USB"
#endif
#ifndef USB_SERIAL
#define USB_SERIAL       "000001"
#endif

#endif /* USB_IDENTITY_H */
