/* 实际描述符和 MSC 回调；只替换内容仓。 */
#include "test_harness.h"
#include "tusb.h"
#include "usb_identity.h"
#include "content_store.h"
#include "bvpv_cd_meta.h"

static bool ready;
static uint32_t blocks;
static unsigned reads;
static uint8_t media_event;
static bvpv_cd_read_result_t read_result = BVPV_CD_READ_OK;
uint8_t usb_media_event(bool *snapshot) { uint8_t e=media_event; media_event=0; *snapshot=ready; return e; }
bool usb_media_ready(void) { return ready; }
uint32_t usb_media_block_count(void) { return ready ? blocks : 0u; }
bvpv_cd_read_result_t usb_media_read(uint32_t off, uint8_t *buf, uint32_t len)
{
    reads++;
    TEST_ASSERT(ready && off + len <= blocks * BVPV_CD_BLOCK_SIZE);
    if (read_result != BVPV_CD_READ_OK) return read_result;
    memset(buf, 0xA5, len);
    return BVPV_CD_READ_OK;
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void test_descriptors(void)
{
    const tusb_desc_device_t *dev = (const tusb_desc_device_t *)tud_descriptor_device_cb();
    const uint8_t *cfg = tud_descriptor_configuration_cb(0);
    const uint8_t eps[] = {0x01, 0x81, 0x02, 0x82};
    unsigned itfs = 0, endpoints = 0;
    uint16_t report_len = 0, total = le16(cfg + 2);
    TEST_ASSERT_EQ_U(18, dev->bLength);
    TEST_ASSERT_EQ_U(USB_VID, dev->idVendor);
    TEST_ASSERT_EQ_U(USB_PID_APP, dev->idProduct);
    TEST_ASSERT_EQ_U(0, dev->bDeviceClass);
    TEST_ASSERT_EQ_U(0, dev->bDeviceSubClass);
    TEST_ASSERT_EQ_U(0, dev->bDeviceProtocol);
    TEST_ASSERT_EQ_U(64, dev->bMaxPacketSize0);
    TEST_ASSERT_EQ_U(1, dev->bNumConfigurations);
    TEST_ASSERT_EQ_U(2, cfg[4]);
    TEST_ASSERT_EQ_U(0xC0, cfg[7]);
    TEST_ASSERT_EQ_U(1, CFG_TUD_VBUS_DETECT_HW);
    TEST_ASSERT_EQ_U(64, total);
    for (uint16_t off = 0; off < total;) {
        const uint8_t *d = cfg + off;
        TEST_ASSERT(d[0] >= 2 && off + d[0] <= total);
        if (d[0] < 2 || off + d[0] > total) { break; }
        if (d[1] == TUSB_DESC_INTERFACE) {
            TEST_ASSERT_EQ_U(itfs, d[2]);
            TEST_ASSERT_EQ_U(2, d[4]);
            TEST_ASSERT_EQ_U(itfs == 0 ? 3 : 8, d[5]);
            TEST_ASSERT_EQ_U(itfs == 0 ? 0 : 6, d[6]);
            TEST_ASSERT_EQ_U(itfs == 0 ? 0 : 0x50, d[7]);
            itfs++;
        } else if (d[1] == HID_DESC_TYPE_HID) {
            report_len = le16(d + 7);
        } else if (d[1] == TUSB_DESC_ENDPOINT) {
            TEST_ASSERT(endpoints < sizeof(eps));
            if (endpoints < sizeof(eps)) { TEST_ASSERT_EQ_U(eps[endpoints], d[2]); }
            TEST_ASSERT_EQ_U(itfs == 1 ? 3 : 2, d[3]);
            TEST_ASSERT_EQ_U(64, le16(d + 4));
            if (itfs == 1) { TEST_ASSERT_EQ_U(1, d[6]); }
            endpoints++;
        }
        off = (uint16_t)(off + d[0]);
    }
    TEST_ASSERT_EQ_U(2, itfs);
    TEST_ASSERT_EQ_U(4, endpoints);
    /* 解析 HID 短项，验证输入/输出均为 64B，且无 Report ID。 */
    {
        const uint8_t *r = tud_hid_descriptor_report_cb(0);
        uint32_t size = 0, count = 0, in_bits = 0, out_bits = 0;
        TEST_ASSERT(report_len > 0);
        for (uint16_t off = 0; off < report_len;) {
            uint8_t key = r[off++];
            unsigned n = key & 3u;
            uint32_t val = 0;
            if (n == 3) { n = 4; }
            TEST_ASSERT(off + n <= report_len);
            if (off + n > report_len) { break; }
            for (unsigned i = 0; i < n; i++) { val |= (uint32_t)r[off++] << (8u * i); }
            switch (key & 0xFCu) {
            case 0x74: size = val; break;
            case 0x94: count = val; break;
            case 0x80: in_bits += size * count; break;
            case 0x90: out_bits += size * count; break;
            case 0x84: TEST_ASSERT(false); break;
            default: break;
            }
        }
        TEST_ASSERT_EQ_U(512, in_bits);
        TEST_ASSERT_EQ_U(512, out_bits);
    }
}
static void test_cd_capacity(void)
{
    const uint32_t sizes[] = {23, 100, BVPV_CD_MAX_BLOCKS};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        uint32_t advertised;
        uint16_t size;
        uint8_t cdb[16] = {0x43}, toc[20] = {0}, data[64];
        ready = true; blocks = sizes[i]; reads = 0;
        tud_msc_capacity_cb(0, &advertised, &size);
        TEST_ASSERT_EQ_U(blocks, advertised);
        TEST_ASSERT_EQ_U(2048, size);
        cdb[8] = sizeof(toc);
        TEST_ASSERT_EQ_U(sizeof(toc), tud_msc_scsi_cb(0, cdb, toc, sizeof(toc)));
        TEST_ASSERT_EQ_U(advertised, be32(toc + 16));
        TEST_ASSERT(tud_msc_test_unit_ready_cb(0));
        TEST_ASSERT(!tud_msc_is_writable_cb(0));
        TEST_ASSERT_EQ_U(64, tud_msc_read10_cb(0, blocks - 1, 1984, data, sizeof(data)));
        TEST_ASSERT_EQ_U(0xA5, data[63]);
        read_result = BVPV_CD_READ_BUSY;
        TEST_ASSERT_EQ_U(TUD_MSC_RET_BUSY, tud_msc_read10_cb(0, 0, 0, data, sizeof(data)));
        read_result = BVPV_CD_READ_ERROR;
        TEST_ASSERT_EQ_U(TUD_MSC_RET_ERROR, tud_msc_read10_cb(0, 0, 0, data, sizeof(data)));
        read_result = BVPV_CD_READ_OK;
        TEST_ASSERT(tud_msc_read10_cb(0, blocks, 0, data, sizeof(data)) < 0);
        TEST_ASSERT_EQ_U(3, reads);
        TEST_ASSERT(tud_msc_write10_cb(0, 0, 0, data, sizeof(data)) < 0);
    }
}
static void test_empty_cd(void)
{
    uint32_t advertised;
    uint16_t size;
    uint8_t data[64];
    ready = false; blocks = 23; reads = 0;
    tud_msc_capacity_cb(0, &advertised, &size);
    TEST_ASSERT(advertised > 0);
    TEST_ASSERT_EQ_U(2048, size);
    TEST_ASSERT(!tud_msc_test_unit_ready_cb(0));
    TEST_ASSERT(!tud_msc_is_writable_cb(0));
    TEST_ASSERT(tud_msc_read10_cb(0, 0, 0, data, sizeof(data)) < 0);
    TEST_ASSERT_EQ_U(0, reads);
}
static void test_media_events(void)
{
    uint8_t cdb[16]={0x4A}, resp[8];
    cdb[4]=0x10; cdb[8]=8;
    ready=false;media_event=3;
    TEST_ASSERT_EQ_U(8,tud_msc_scsi_cb(0,cdb,resp,8));
    TEST_ASSERT_EQ_U(4,resp[2]);TEST_ASSERT_EQ_U(3,resp[4]);TEST_ASSERT_EQ_U(0,resp[5]);
    ready=true;media_event=2;
    TEST_ASSERT_EQ_U(8,tud_msc_scsi_cb(0,cdb,resp,8));
    TEST_ASSERT_EQ_U(2,resp[4]);TEST_ASSERT_EQ_U(2,resp[5]);
    TEST_ASSERT_EQ_U(8,tud_msc_scsi_cb(0,cdb,resp,8));
    TEST_ASSERT_EQ_U(0x84,resp[2]);TEST_ASSERT_EQ_U(0,resp[4]);
}
int main(void)
{
    TEST_RUN(test_descriptors);
    TEST_RUN(test_cd_capacity);
    TEST_RUN(test_empty_cd);
    TEST_RUN(test_media_events);
    TEST_MAIN_END();
}