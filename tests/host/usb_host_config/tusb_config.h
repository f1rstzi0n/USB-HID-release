/* TinyUSB 设备栈配置：STM32F446、无 OS、单 rhport0 设备模式（Full-Speed）。
 * 复合设备：1×HID（自定义 64B 报表）+ 1×MSC（只读光盘）。 */
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_MCU             OPT_MCU_STM32F4
#define CFG_TUSB_OS              OPT_OS_NONE
#define CFG_TUSB_RHPORT0_MODE    (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#ifndef pdTICKS_TO_MS
#define pdTICKS_TO_MS(ticks) ((uint32_t)(((uint64_t)(ticks) * 1000u) / configTICK_RATE_HZ))
#endif
#define CFG_TUSB_DEBUG           0

/* 主板外部供电：PA9 检测 VBUS，拔线时由 DWC2 报告会话结束。 */
#define CFG_TUD_VBUS_DETECT_HW   1

#define CFG_TUD_ENDPOINT0_SIZE   64

/* 保持 TinyUSB 默认事件队列深度；HID 应答由完成回调串行提交。 */
#define CFG_TUD_TASK_QUEUE_SZ    16

/* 设备类开关 */
#define CFG_TUD_CDC              0
#define CFG_TUD_MSC              1
#define CFG_TUD_HID              1
#define CFG_TUD_MIDI             0
#define CFG_TUD_VENDOR           0

/* HID：64B 报表缓冲（与 L0 报表尺寸一致） */
#define CFG_TUD_HID_EP_BUFSIZE   64

/* MSC：64B 端点缓冲（Full-Speed Bulk） */
#define CFG_TUD_MSC_EP_BUFSIZE   64

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H */
