/* 内容仓 USB 适配：Flash I/O 由宿主注入。 */
#ifndef CONTENT_STORE_H
#define CONTENT_STORE_H
#include <stdbool.h>
#include <stdint.h>
#include "bvpv_cd_store.h"
typedef enum
{
    CONTENT_STORE_NO_FLASH = 0,
    CONTENT_STORE_EMPTY,
    CONTENT_STORE_READY,
    CONTENT_STORE_UPDATING
} content_store_state_t;
extern volatile int32_t g_content_store_state;
extern volatile uint32_t g_content_image_bytes;
bool content_store_init(const bvpv_cd_io_t *io, bool flash_available);
void content_store_task(void);
bool content_store_ready(void);
uint32_t content_store_block_count(void);
bvpv_cd_read_result_t content_store_read(uint32_t offset, uint8_t *buf, uint32_t len);
bool content_store_request(uint8_t svc, const uint8_t *p, uint16_t len, uint8_t *out,
                           uint16_t *out_len);
bool content_store_poll_reply(uint8_t *out, uint16_t *len);
void content_store_cancel(void);
void content_store_mounted(void);
/* MMC 事件：0=无变化，2=插入，3=移除。 */
uint8_t content_store_media_event(void);
#endif
