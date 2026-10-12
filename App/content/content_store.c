#include "content_store.h"
#include "bvpv_cd_store.h"
#include "usb_port.h"

volatile int32_t g_content_store_state;
volatile uint32_t g_content_image_bytes;
static bvpv_cd_store_t s_store;
static bool s_hold_media, s_last_ready;
static uint8_t s_event;

bool content_store_ready(void)
{
    return !s_hold_media && bvpv_cd_store_ready(&s_store);
}
static void sync_state(void)
{
    bool ready = content_store_ready();
    g_content_image_bytes = s_store.available ? s_store.active.bytes : 0;
    g_content_store_state = s_store.fault                       ? CONTENT_STORE_NO_FLASH
                            : (s_store.session || s_hold_media) ? CONTENT_STORE_UPDATING
                            : ready                             ? CONTENT_STORE_READY
                                                                : CONTENT_STORE_EMPTY;
    if (ready != s_last_ready)
    {
        s_event = ready ? 2u : 3u;
        s_last_ready = ready;
    }
}
bool content_store_init(const bvpv_cd_io_t *io, bool flash_available)
{
    bvpv_cd_store_init(&s_store, io);
    if (!flash_available)
    {
        s_store.fault = true;
        s_store.last_error = 255;
    }
    else
        (void)bvpv_cd_store_boot(&s_store);
    sync_state();
    return content_store_ready();
}
void content_store_task(void)
{
    bvpv_cd_store_task(&s_store, usb_port_millis());
    if (s_store.committed)
        s_hold_media = true;
    sync_state();
}
uint32_t content_store_block_count(void)
{
    return s_store.available ? bvpv_cd_block_count(s_store.active.bytes) : 0;
}
bvpv_cd_read_result_t content_store_read(uint32_t off, uint8_t *buf, uint32_t len)
{
    if (!content_store_ready() || !buf || !len || off >= s_store.active.bytes ||
        len > s_store.active.bytes - off)
        return BVPV_CD_READ_ERROR;
    return bvpv_cd_store_read(&s_store, off, buf, len);
}
bool content_store_request(uint8_t svc, const uint8_t *p, uint16_t len, uint8_t *out,
                           uint16_t *out_len)
{
    bvpv_cd_store_request(&s_store, svc, p, len, usb_port_millis());
    sync_state();
    return bvpv_cd_store_take_reply(&s_store, out, out_len);
}
bool content_store_poll_reply(uint8_t *out, uint16_t *len)
{
    return bvpv_cd_store_take_reply(&s_store, out, len);
}
void content_store_cancel(void)
{
    bvpv_cd_store_cancel(&s_store);
}
void content_store_mounted(void)
{
    s_hold_media = false;
    s_store.committed = false;
    sync_state();
}
uint8_t content_store_media_event(void)
{
    uint8_t event = s_event;
    s_event = 0;
    return event;
}
