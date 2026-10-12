#include "bvpv_usb_crc16.h"

#define BVPV_CRC16_POLY 0x1021u
#define BVPV_CRC16_INIT 0xFFFFu

/* 增量计算：把一段数据并入进行中的 CRC。crc 传上一次的返回值（首段传 0xFFFF）。
 * 按位计算：每字节先高 8 位异或进 crc，再做 8 次"左移+（溢出则异或多项式）"。 */
uint16_t bvpv_crc16_update(uint16_t crc, const uint8_t *data, size_t len)
{
    size_t i;
    int bit;

    if (data == 0) {
        return crc;
    }
    for (i = 0; i < len; i++) {
        crc = (uint16_t)(crc ^ ((uint16_t)data[i] << 8));
        for (bit = 0; bit < 8; bit++) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((crc << 1) ^ BVPV_CRC16_POLY);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* 一次性计算：从初值 0xFFFF 开始对整个缓冲求 CRC。 */
uint16_t bvpv_crc16(const uint8_t *data, size_t len)
{
    return bvpv_crc16_update(BVPV_CRC16_INIT, data, len);
}
