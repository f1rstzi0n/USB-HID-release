/* 12MHz bypass / 180MHz 主时钟；USB 使用独立 PLLSAI 48MHz。 */
#include "board.h"
#include "main.h"
#include "usb_io.h"
#include "usb_port.h"
#include "tusb.h"
#include "device/dcd.h"
extern IWDG_HandleTypeDef hiwdg;
uint32_t usb_io_enter_critical(void)
{
    uint32_t saved = __get_PRIMASK();
    __disable_irq();
    return saved;
}
void usb_io_leave_critical(uint32_t saved) { if (!saved) __enable_irq(); }
uint32_t usb_port_millis(void) { return HAL_GetTick(); }
uint32_t tusb_time_millis_api(void) { return HAL_GetTick(); }
void usb_port_connect(bool connected) { (void)usb_io_connect_request(connected); }
void board_watchdog_refresh(void) { (void)HAL_IWDG_Refresh(&hiwdg); }
bool board_usb_hw_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    RCC_PeriphCLKInitTypeDef clock = {0};
    if (HSE_VALUE != 12000000u || __HAL_RCC_GET_FLAG(RCC_FLAG_HSERDY) == RESET) return false;
    clock.PeriphClockSelection = RCC_PERIPHCLK_CLK48;
    clock.PLLSAI.PLLSAIM = 6; clock.PLLSAI.PLLSAIN = 96;
    clock.PLLSAI.PLLSAIQ = 2; clock.PLLSAI.PLLSAIP = RCC_PLLSAIP_DIV4;
    clock.PLLSAIDivQ = 1; clock.Clk48ClockSelection = RCC_CLK48CLKSOURCE_PLLSAIP;
    if (HAL_RCCEx_PeriphCLKConfig(&clock) != HAL_OK) return false;
    __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOE_CLK_ENABLE();
    gpio.Pin = O_USB_DP_CTRL_Pin; gpio.Mode = GPIO_MODE_INPUT; gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(O_USB_DP_CTRL_GPIO_Port, &gpio);
    gpio.Pin = GPIO_PIN_9; HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = GPIO_PIN_11 | GPIO_PIN_12; gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH; gpio.Alternate = GPIO_AF10_OTG_FS;
    HAL_GPIO_Init(GPIOA, &gpio);
    HAL_NVIC_DisableIRQ(OTG_FS_IRQn); HAL_NVIC_ClearPendingIRQ(OTG_FS_IRQn);
    HAL_NVIC_SetPriority(OTG_FS_IRQn, 5, 0);
    __HAL_RCC_USB_OTG_FS_CLK_ENABLE();
    __HAL_RCC_USB_OTG_FS_FORCE_RESET(); __HAL_RCC_USB_OTG_FS_RELEASE_RESET();
    return __HAL_RCC_PLLSAI_GET_FLAG();
}
bool usb_io_stack_init(void)
{
    const tusb_rhport_init_t port = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    if (!board_usb_hw_init() || !tusb_init(0, &port)) return false;
    (void)tud_connect();
    return true;
}
void OTG_FS_IRQHandler(void) { tud_int_handler(0); }
void tud_event_hook_cb(uint8_t rhport, uint32_t event, bool in_isr)
{
    (void)rhport; (void)in_isr;
    if (event == DCD_EVENT_BUS_RESET_START) usb_io_link_reset();
}
void tud_mount_cb(void) { usb_io_link_changed(true); }
void tud_umount_cb(void) { usb_io_link_changed(false); }
const char *board_usb_serial(void)
{
    static const char hex[] = "0123456789ABCDEF";
    static char serial[25];
    uint32_t uid[3];
    unsigned word, nibble, shift;
    uid[0] = HAL_GetUIDw0(); uid[1] = HAL_GetUIDw1(); uid[2] = HAL_GetUIDw2();
    for (word = 0; word < 3; ++word) {
        for (nibble = 0; nibble < 8; ++nibble) {
            shift = 28u - nibble * 4u;
            serial[word * 8u + nibble] = hex[(uid[word] >> shift) & 15u];
        }
    }
    serial[24] = 0;
    return serial;
}
