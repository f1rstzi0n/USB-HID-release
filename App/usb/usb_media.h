#ifndef USB_MEDIA_H
#define USB_MEDIA_H
#include "content_store.h"
bool usb_media_init(void);
void usb_media_lock(void);
void usb_media_unlock(void);
bool usb_media_ready(void);
uint32_t usb_media_block_count(void);
bvpv_cd_read_result_t usb_media_read(uint32_t, uint8_t *, uint32_t);
uint8_t usb_media_event(bool *ready);
#endif
