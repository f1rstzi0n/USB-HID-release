/* APP 仅验证身份与请求软件复位，不含内部 Flash 擦写。 */
#include "fw_platform.h"
#include "fw_image.h"
#include "fw_public_key.h"
#include "main.h"
#include "board.h"
#include "usb_io.h"
#include "usb_port.h"
#include "content_store.h"
#include "bvpv_service.h"
#include "bvpv_errcode.h"
#include <string.h>
#define FW_BOOT_REQUEST 0xB007F446u
__attribute__((section(".boot_request"), used)) static volatile uint32_t s_request[2];
static fw_image_t s_image;
static fw_maintenance_check_t s_maintenance;
static bool s_enter_pending, s_enter_ready, s_reset_waiting;
static uint32_t s_disconnect_serial;
static bool flash_read(void *ctx, uint32_t address, uint8_t *out, uint32_t len)
{
    (void)ctx;
    if (!out || address < FW_HEADER_BASE || address > FW_FLASH_END || len > FW_FLASH_END - address) return false;
    memcpy(out, (const void *)(uintptr_t)address, len);
    return true;
}
static void app_request(uint8_t svc, const uint8_t *data, uint16_t len, uint8_t *out, uint16_t *out_len)
{
    (void)data;
    *out_len = 1;
    if (len) { out[0] = BVPV_ERR_LENGTH; return; }
    if (svc == FW_SVC_INFO) { fw_image_info(&s_image, FW_MODE_APP, out); *out_len = FW_INFO_SIZE; }
    else if (svc == BVPV_SVC_ENTER_BOOTLOADER) {
        if (!s_maintenance || !s_maintenance() || g_content_store_state == CONTENT_STORE_UPDATING)
            out[0] = BVPV_ERR_BUSY;
        else { s_enter_pending = true; out[0] = BVPV_ERR_OK; }
    } else out[0] = BVPV_ERR_NOT_SUPPORTED;
}
static void app_sent(uint8_t svc, bool delivered)
{
    if (svc == BVPV_SVC_ENTER_BOOTLOADER && s_enter_pending) {
        s_enter_ready = delivered; s_enter_pending = false;
    }
}
static void app_cancel(void) { s_enter_pending = s_enter_ready = false; }
static const bvpv_fw_backend_t backend = { app_request, app_sent, app_cancel };
void fw_platform_app_init(fw_maintenance_check_t check)
{
    uint32_t words[3];
    uint8_t uid[12];
    const fw_flash_io_t io = { .read = flash_read };
    words[0] = HAL_GetUIDw0(); words[1] = HAL_GetUIDw1(); words[2] = HAL_GetUIDw2();
    memcpy(uid, words, sizeof(uid)); s_maintenance = check;
    fw_image_init(&s_image, &io, fw_public_key, uid);
    __HAL_RCC_CLEAR_RESET_FLAGS();
    bvpv_service_set_fw_backend(&backend);
}
void fw_platform_task(void)
{
    uint32_t disconnected_ms;
    if (s_enter_ready && !s_reset_waiting) {
        s_enter_ready = false; s_reset_waiting = true;
        s_disconnect_serial = usb_io_connect_request(false);
    }
    if (s_reset_waiting && usb_io_connect_applied(s_disconnect_serial, &disconnected_ms) &&
        (uint32_t)(usb_port_millis() - disconnected_ms) >= 250u) {
        s_request[0] = FW_BOOT_REQUEST; s_request[1] = ~FW_BOOT_REQUEST;
        __DSB(); NVIC_SystemReset();
    }
}

void fw_validation_progress(void) { board_watchdog_refresh(); }
