/* MSC 镜像布局与元数据编解码，纯 C、无硬件依赖。
 * 单镜像槽；BVC4 交替提交无介质记录和有效镜像记录。
 * 字节格式、CRC 范围与提交顺序唯一真源：docs/PROTOCOL.md §8。 */
#ifndef BVPV_CD_META_H
#define BVPV_CD_META_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- W25Q64 分区：8 MiB，全部按 4 KiB 擦除边界对齐 ---- */
#define BVPV_FLASH_SIZE_BYTES   0x800000u
#define BVPV_CD_IMAGE_BASE      0x000000u
#define BVPV_CD_IMAGE_AREA_BYTES 0x400000u
#define BVPV_FLASH_PARAM_BASE   0x400000u
#define BVPV_FLASH_PARAM_BYTES  0x100000u
#define BVPV_FLASH_MCU_BASE     0x500000u
#define BVPV_FLASH_MCU_BYTES    0x2F0000u
#define BVPV_CD_META_BASE       0x7F0000u
#define BVPV_CD_META_AREA_BYTES 0x010000u

#define BVPV_CD_BLOCK_SIZE      2048u
#define BVPV_CD_MAX_BLOCKS      (BVPV_CD_IMAGE_AREA_BYTES / BVPV_CD_BLOCK_SIZE)
#define BVPV_CD_META_VALID      0xA5u
#define BVPV_CD_LAYOUT_VERSION  4u
#define BVPV_CD_SLOT_BYTES      BVPV_CD_IMAGE_AREA_BYTES
#define BVPV_CD_SLOT_BLOCKS     BVPV_CD_MAX_BLOCKS
#define BVPV_CD_RECORD_BASE(n)  (BVPV_CD_META_BASE + (uint32_t)(n) * 4096u)
#define BVPV_CD_RECORD_LEN      32u
#define BVPV_CD_COMMIT_OFFSET   28u
#define BVPV_CD_RECORD_IMAGE    0u
#define BVPV_CD_RECORD_NO_MEDIA 1u

typedef struct {
    uint8_t kind;
    uint32_t bytes, crc32, sequence, write_count;
} bvpv_cd_record_t;

/* CRC 覆盖 [0..23]；[28] 提交字节必须独立编程、最后写入。 */
void bvpv_cd_record_encode(uint8_t out[BVPV_CD_RECORD_LEN], const bvpv_cd_record_t *record);
bool bvpv_cd_record_decode(const uint8_t in[BVPV_CD_RECORD_LEN], bvpv_cd_record_t *record);
bool bvpv_cd_sequence_newer(uint32_t a, uint32_t b);

/* LBA 换算失败（块内偏移 ≥ BVPV_CD_BLOCK_SIZE）时的返回值。 */
#define BVPV_CD_OFFSET_INVALID   0xFFFFFFFFu

/* LBA + 块内偏移 → 镜像内字节偏移（lba*2048 + offset）。
 * offset ≥ BVPV_CD_BLOCK_SIZE 返回 BVPV_CD_OFFSET_INVALID。 */
uint32_t bvpv_cd_lba_to_image_offset(uint32_t lba, uint32_t offset);

/* 镜像字节数 → 2048B 块数（向上取整）。 */
uint32_t bvpv_cd_block_count(uint32_t image_bytes);

/* CRC32（IEEE 802.3 / zlib 约定）：反射多项式 0xEDB88320、初值 0xFFFFFFFF、末取反。
 * 增量式，语义与 zlib 的 crc32() 完全一致：首段传 0，后续传上一次的返回值。
 * 16 项半字节表 + 两查表/字节：零可变状态、完全可重入（与 bvpv_crc16 的取舍一致）。 */
uint32_t bvpv_cd_crc32(const uint8_t *data, size_t len);
uint32_t bvpv_cd_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* BVPV_CD_META_H */
