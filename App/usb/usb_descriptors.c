/* USB 复合设备描述符：Interface0=HID（自定义 64B 报表，Usage Page 0xFF00/Usage 0x0001），
 * Interface1=MSC（08/06/50 BOT，只读光盘 LUN）。与 docs/PROTOCOL.md §2/§8 一致。
 * 说明：64B 报表无 HID Report ID 项，线级 64B 恰好一个 FS 中断包（PROTOCOL.md §2 的
 * "ReportID=0" 是我方自定义首字节，非 USB HID 报表号）。 */
#include "tusb.h"
#include "usb_identity.h"
#include "usb_descriptors.h"
#include <string.h>



/* ---------------- 设备描述符 ---------------- */
static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,                     /* 类别由各接口声明 */
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,                     /* 两个独立接口，无 IAD */
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID_APP,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

/* 设备描述符回调：返回设备级身份（VID/PID、复合设备类码、厂商/产品/序列号字符串索引）。 */
uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

/* ---------------- HID 报表描述符 ---------------- */
/* 厂商自定义 Usage Page 0xFF00 / Usage 0x01，输入/输出报表各 64B，无 Report ID。 */
static uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(64),
};

/* HID 报表描述符回调：返回我们的自定义报表格式（Usage Page 0xFF00/Usage 0x01，
 * 输入/输出各 64B、无 Report ID）。主机据此解析 HID 报表。 */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return desc_hid_report;
}

/* ---------------- 配置描述符 ---------------- */
enum {
    ITF_NUM_HID = 0,
    ITF_NUM_MSC,
    ITF_NUM_TOTAL,
};

#define EPNUM_HID_OUT  0x01
#define EPNUM_HID_IN   0x81
#define EPNUM_MSC_OUT  0x02
#define EPNUM_MSC_IN   0x82

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_MSC_DESC_LEN)

static uint8_t const desc_configuration[] = {
    /* 配置：2 接口，自供电；保留原有 VBUS 电流上限 100mA。 */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),

    /* Interface0：HID，非 boot 协议，报表 64B，中断 OUT 0x01 / IN 0x81，周期 1ms */
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_NONE,
                             sizeof(desc_hid_report), EPNUM_HID_OUT, EPNUM_HID_IN, 64, 1),

    /* Interface1：MSC（BOT），Bulk OUT 0x02 / IN 0x82，64B */
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

/* 配置描述符回调：返回"1 配置 = HID 接口 + MSC 接口"的完整描述（复合拓扑）。 */
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

/* ---------------- 字符串描述符 ---------------- */
static char const *const string_desc_arr[] = {
    (const char[]){0x09, 0x04},   /* 0: 支持语言 = English(0x0409) */
    USB_MANUFACTURER,             /* 1: 厂商 */
    USB_PRODUCT,                  /* 2: 产品 */
    NULL,                         /* 3: 序列号由 usb_serial_string 动态提供 */
};

static uint16_t desc_str[32];

static const char *s_serial = USB_SERIAL;
void usb_descriptors_set_serial(const char *serial)
{
    s_serial = serial ? serial : USB_SERIAL;
}

/* 字符串描述符回调：把 ASCII 字符串（厂商/产品/序列号）转成 USB 要求的 UTF-16LE。
 * index=0 返回语言 ID（English 0x0409），其余返回对应字符串。 */
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    uint8_t chr_count, i;
    (void)langid;

    if (index == 0u) {
        memcpy(&desc_str[1], string_desc_arr[0], 2);
        chr_count = 1u;
    } else {
        const char *str;
        if (index >= (sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
            return NULL;
        }
        str = (index == 3u) ? s_serial : string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31u) {
            chr_count = 31u;
        }
        for (i = 0u; i < chr_count; i++) {
            desc_str[1 + i] = (uint16_t)str[i];
        }
    }
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * chr_count + 2u));
    return desc_str;
}
