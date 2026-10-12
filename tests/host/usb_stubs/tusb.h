#ifndef TEST_TUSB_H
#define TEST_TUSB_H
#include <stdbool.h>
#include <stdint.h>
typedef enum { HID_REPORT_TYPE_INPUT=1, HID_REPORT_TYPE_OUTPUT=2 } hid_report_type_t;
bool tud_connect(void);
bool tud_disconnect(void);
void tud_task_ext(uint32_t timeout,bool in_isr);
bool tud_hid_ready(void);
bool tud_hid_report(uint8_t id,const void *report,uint16_t len);
#endif
