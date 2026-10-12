/* CRC16-CCITT-FALSE：多项式 0x1021、初值 0xFFFF、不反射、异或出 0。
 * 依据 docs/PROTOCOL.md §4。采用按位计算：零静态表、零可变状态、完全可重入，
 * 对参数通信这种帧长（≤4096B）性能足够。 */
#ifndef BVPV_USB_CRC16_H
#define BVPV_USB_CRC16_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一次性计算。len=0 时返回初值 0xFFFF（此时 data 可为 NULL）。 */
uint16_t bvpv_crc16(const uint8_t *data, size_t len);

/* 增量计算：crc 传上一段的返回值，首段传 0xFFFF。 */
uint16_t bvpv_crc16_update(uint16_t crc, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* BVPV_USB_CRC16_H */
