/* APP 与 Boot 的共用协议 module；所有调用仅由处理任务执行。 */
#ifndef USB_PROTO_H
#define USB_PROTO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bvpv_usb_frame.h"
#define USB_REPLY_REENUMERATE 1u
#define USB_REPLY_KEEP_RX_CLOSED 4u
typedef struct {
    size_t (*dispatch)(const bvpv_frame_t *, uint8_t *, uint16_t);
    size_t (*poll)(uint8_t *, uint16_t);
    bool (*pending)(void);
    void (*task)(void);
    void (*cancel)(void);
    void (*reply_complete)(uint8_t svc, bool delivered);
    uint8_t (*reply_policy)(const uint8_t *frame, size_t len);
} usb_proto_backend_t;
void usb_proto_init_backend(const usb_proto_backend_t *);
void usb_proto_task(void);
void usb_proto_disconnected(void);
#endif
