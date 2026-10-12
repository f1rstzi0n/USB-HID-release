/* 媒体锁先于 SPI2 锁；TinyUSB 读取只尝试取锁，不阻塞 USB 栈。 */
#include "usb_media.h"
#include "usb_io.h"
#include "FreeRTOS.h"
#include "semphr.h"
static StaticSemaphore_t s_storage;
static SemaphoreHandle_t s_mutex;
static bool s_ready;
static uint32_t s_blocks;
bool usb_media_init(void) { s_mutex = xSemaphoreCreateMutexStatic(&s_storage); return s_mutex != 0; }
void usb_media_lock(void) { if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) configASSERT(0); }
void usb_media_unlock(void)
{
    uint32_t saved = usb_io_enter_critical();
    s_ready = content_store_ready(); s_blocks = content_store_block_count();
    usb_io_leave_critical(saved);
    (void)xSemaphoreGive(s_mutex);
}
bool usb_media_ready(void)
{
    uint32_t saved = usb_io_enter_critical();
    bool ready = s_ready;
    usb_io_leave_critical(saved);
    return ready;
}
uint32_t usb_media_block_count(void)
{
    uint32_t saved = usb_io_enter_critical();
    uint32_t blocks = s_ready ? s_blocks : 0u;
    usb_io_leave_critical(saved);
    return blocks;
}
bvpv_cd_read_result_t usb_media_read(uint32_t offset, uint8_t *out, uint32_t len)
{
    bvpv_cd_read_result_t result;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return BVPV_CD_READ_BUSY;
    result = content_store_read(offset, out, len);
    (void)xSemaphoreGive(s_mutex);
    return result;
}
uint8_t usb_media_event(bool *ready)
{
    uint8_t event;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        *ready = usb_media_ready();
        return 0;
    }
    event = content_store_media_event();
    *ready = content_store_ready();
    (void)xSemaphoreGive(s_mutex);
    return event;
}
