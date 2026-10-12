/* STM32F446 Fake 示例：硬件与生成数据在此接入可复用 USB 子系统。 */
#include "usb_app.h"
#include "board.h"
#include "fw_platform.h"
#include "content_store.h"
#include "bvpv_catalog_data.h"
#include "bvpv_errcode.h"
#include "bvpv_service.h"
#include "usb_descriptors.h"
#include "usb_proto.h"
#include "usb_port.h"
#include "usb_io.h"
#include "spi2_bus.h"
#include "w25q64.h"
#include "bvpv_catalog_store.h"
#include "fw_public_key.h"

static bvpv_catalog_store_t s_catalog_store;
static bvpv_service_ctx_t s_usb_catalog;
static uint8_t s_cd_owner, s_catalog_owner;
static const void *s_content_owner;

static bvpv_cd_read_result_t flash_read(void *ctx, uint32_t addr, uint8_t *out, uint32_t len)
{
    w25q64_status_t result;
    (void)ctx;
    result = w25q64_read(addr, out, len);
    return result == W25Q64_OK ? BVPV_CD_READ_OK :
           result == W25Q64_BUSY ? BVPV_CD_READ_BUSY : BVPV_CD_READ_ERROR;
}
static bool flash_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    return w25q64_erase_start(addr) == W25Q64_OK;
}
static bool flash_program(void *ctx, uint32_t addr, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    return w25q64_program_start(addr, data, len) == W25Q64_OK;
}
static int flash_poll(void *ctx)
{
    (void)ctx;
    return w25q64_poll();
}
static bool flash_reserve(void *ctx, bool reserve)
{
    if (reserve) {
        if (s_content_owner && s_content_owner != ctx) return false;
        if (!spi2_bus_reserve_flash(true)) return false;
        s_content_owner = ctx;
        return true;
    }
    if (s_content_owner == ctx) {
        (void)spi2_bus_reserve_flash(false);
        s_content_owner = 0;
    }
    return true;
}
static uint8_t fake_command(void *user, uint32_t addr, uint8_t element,
                            const uint8_t *value, uint16_t length)
{
    (void)user; (void)addr; (void)element; (void)value; (void)length;
    return BVPV_ERR_OK; /* Fake 只校验并应答，不执行电机/学习/保存/重启。 */
}
static bool fake_maintenance_ready(void) { return true; } /* Fake 无执行机构动作。 */
static bool catalog_request(uint8_t svc, const uint8_t *data, uint16_t length,
                            uint8_t *out, uint16_t *out_len, uint16_t cap)
{
    if (svc == BVPV_SVC_CATALOG_UPDATE_BEGIN && !fake_maintenance_ready()) {
        if (!cap) return false;
        out[0] = BVPV_ERR_STATE; *out_len = 1;
        return true;
    }
    return bvpv_catalog_store_request(&s_catalog_store, svc, data, length,
                                      out, out_len, cap, usb_port_millis());
}
static bool catalog_poll(uint8_t *out, uint16_t *out_len, uint16_t cap)
{
    return bvpv_catalog_store_take_reply(&s_catalog_store, out, out_len, cap);
}
static void catalog_cancel(void) { bvpv_catalog_store_cancel(&s_catalog_store); }
static void catalog_task(void) { bvpv_catalog_store_task(&s_catalog_store, usb_port_millis()); }
static bool catalog_ready(void) { return bvpv_catalog_store_ready(&s_catalog_store); }
static bool content_maintenance(void) { return s_content_owner != 0; }
static bool catalog_committed(void) { return s_catalog_store.committed; }
static void catalog_reply_complete(uint8_t svc, bool delivered)
{
    (void)delivered;
    if (svc == BVPV_SVC_CATALOG_UPDATE_FINISH) s_catalog_store.committed = false;
}
static void cd_task(void)
{
    /* 目录启动校验优先，光盘随后准备；HID 前台由协议任务先处理。 */
    if (!s_catalog_store.booting && !s_catalog_store.session) content_store_task();
}

static uint8_t app_reply_policy(const uint8_t *frame, size_t len)
{
    uint8_t svc;
    if (len < BVPV_FRAME_OVERHEAD + 1u || frame[BVPV_FRAME_HDR_LEN] != 0) return 0;
    svc = frame[0];
    if (svc == BVPV_SVC_ENTER_BOOTLOADER) return USB_REPLY_KEEP_RX_CLOSED;
    if (svc == BVPV_SVC_MSC_UPDATE_FINISH && len == BVPV_FRAME_OVERHEAD + 2u &&
        frame[BVPV_FRAME_HDR_LEN + 1u] == 1)
        return USB_REPLY_REENUMERATE;
    if (svc == BVPV_SVC_CATALOG_UPDATE_FINISH && bvpv_service_catalog_committed())
        return USB_REPLY_REENUMERATE;
    return 0;
}
static const usb_proto_backend_t app_backend = {
    bvpv_service_dispatch, bvpv_service_poll, bvpv_service_pending,
    bvpv_service_task, bvpv_service_cancel, bvpv_service_reply_complete, app_reply_policy
};

void usb_app_init(void)
{
    static const bvpv_cd_io_t io = {&s_cd_owner, flash_read, flash_erase, flash_program,
                                    flash_poll, flash_reserve};
    static const bvpv_cd_io_t catalog_io = {&s_catalog_owner, flash_read, flash_erase,
                                           flash_program, flash_poll, flash_reserve};
    static const bvpv_msc_backend_t msc = {content_store_request,
        content_store_poll_reply, content_store_cancel, cd_task};
    static const bvpv_catalog_backend_t catalog_backend = {
        catalog_request, catalog_poll, catalog_cancel, catalog_task,
        catalog_ready, content_maintenance, catalog_reply_complete, catalog_committed};
    bvpv_service_ctx_t *catalog;
    bool flash_available = w25q64_init();
    (void)content_store_init(&io, flash_available);
    s_usb_catalog = *bvpv_catalog_data_ctx();
    catalog = &s_usb_catalog;
    bvpv_catalog_store_init(&s_catalog_store, &catalog_io, catalog,
                             fw_public_key, bvpv_catalog_data_app_identity());
    if (flash_available) bvpv_catalog_store_boot(&s_catalog_store);
    else { s_catalog_store.fault = true; s_catalog_store.last_error = BVPV_ERR_UNKNOWN; }
    catalog->command_handler = fake_command;
    catalog->command_user = 0;
    usb_descriptors_set_serial(board_usb_serial());
    bvpv_service_init(catalog);
    bvpv_service_set_msc_backend(&msc);
    usb_proto_init_backend(&app_backend);
    bvpv_service_set_catalog_backend(&catalog_backend);
    fw_platform_app_init(fake_maintenance_ready);
}
void usb_app_task(void)
{
    static uint32_t mounted_epoch;
    uint32_t epoch;
    usb_proto_task();
    epoch = usb_io_epoch();
    if (usb_io_mounted() && mounted_epoch != epoch) {
        content_store_mounted(); mounted_epoch = epoch;
    }
    fw_platform_task();
}
