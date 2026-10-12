/* 报表交接 seam：协议任务与 TinyUSB 任务共享，所有存储静态分配。 */
#ifndef USB_IO_H
#define USB_IO_H
#include <stdbool.h>
#include <stdint.h>
#define USB_IO_REPORT_SIZE 64u
#define USB_IO_RX_SLOTS 16u
typedef struct { uint32_t epoch, ticket, received_ms; uint8_t report[64]; } usb_io_rx_t;
typedef struct { uint32_t epoch, token, next_ticket; bool delivered; } usb_io_done_t;
void usb_io_init(void);
bool usb_io_stack_init(void);
void usb_io_task(void);
uint32_t usb_io_epoch(void);
bool usb_io_mounted(void);
void usb_io_link_reset(void);
void usb_io_link_changed(bool mounted);
bool usb_io_rx_take(usb_io_rx_t *);
bool usb_io_done_take(usb_io_done_t *);
uint32_t usb_io_rx_open(void);
bool usb_io_rx_close(uint32_t epoch, uint32_t ticket);
bool usb_io_rx_overflowed(void);
bool usb_io_tx_submit(const uint8_t [64], uint32_t epoch, uint32_t token,
                      uint32_t deadline, bool last, bool reopen);
void usb_io_tx_cancel(uint32_t token);
uint32_t usb_io_connect_request(bool connected);
bool usb_io_connect_applied(uint32_t serial, uint32_t *actual_ms);
/* TinyUSB 回调只投递，测试穿过相同 interface。 */
void usb_io_report_received(const uint8_t *, uint16_t);
void usb_io_report_completed(bool delivered);
/* 平台只提供极短临界区；不得在其中操作 Flash 或等待。 */
uint32_t usb_io_enter_critical(void);
void usb_io_leave_critical(uint32_t saved);
extern volatile uint32_t g_usb_rx_overflows, g_usb_rx_high_water;
#endif
