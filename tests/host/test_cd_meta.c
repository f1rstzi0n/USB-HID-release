#include "test_harness.h"

#include <string.h>

#include "bvpv_cd_meta.h"

/* 测试套件：MSC 光盘镜像的元数据与布局契约（docs/PROTOCOL.md §8）。
 * 覆盖镜像头编解码、有效性判定、LBA 换算与 CRC32 已知向量。
 * 判定方式：纯函数逐项断言，无硬件依赖。 */

/* 用例：CRC32 已知向量与增量一致性。
 * 功能：镜像整盘校验用的 CRC32（zlib/IEEE 802.3 约定）。
 * 预期：空串→0x00000000；"123456789"→0xCBF43926；标准长句→0x414FA339；
 *       分段计算与一次性计算结果相同；NULL/0 长度安全返回 0。
 * 判据：与公开向量逐位相符。 */
static void test_crc32_vectors(void)
{
    static const uint8_t fox[] = "The quick brown fox jumps over the lazy dog";

    TEST_ASSERT_EQ_U(0x00000000u, bvpv_cd_crc32((const uint8_t *)"", 0u));
    TEST_ASSERT_EQ_U(0xCBF43926u, bvpv_cd_crc32((const uint8_t *)"123456789", 9u));
    TEST_ASSERT_EQ_U(0x414FA339u, bvpv_cd_crc32(fox, sizeof(fox) - 1u));

    /* 增量：首段传 0，后续传上一段返回值——必须与一次性结果一致 */
    {
        uint32_t one = bvpv_cd_crc32(fox, sizeof(fox) - 1u);
        uint32_t c   = bvpv_cd_crc32_update(0u, fox, 10u);
        c = bvpv_cd_crc32_update(c, fox + 10u, (sizeof(fox) - 1u) - 10u);
        TEST_ASSERT_EQ_U(one, c);
    }
    /* 零长度段不改变累计值 */
    {
        uint32_t c = bvpv_cd_crc32_update(0u, (const uint8_t *)"123456789", 9u);
        TEST_ASSERT_EQ_U(0xCBF43926u, bvpv_cd_crc32_update(c, 0, 0u));
        TEST_ASSERT_EQ_U(c, bvpv_cd_crc32_update(c, 0, 0u));
    }
}

/* BVC4 保留原字段偏移与 CRC 范围，提交标志最后写入。 */
static void test_meta_roundtrip(void)
{
    bvpv_cd_record_t m = {0, 51200u, 0xDEADBEEFu, 0xFFFFFFFFu, 7u}, r;
    uint8_t buf[BVPV_CD_RECORD_LEN];
    bvpv_cd_record_encode(buf, &m);
    TEST_ASSERT_MEMEQ("BVC4", buf, 4u);
    TEST_ASSERT_EQ_U(4u, buf[4]);
    TEST_ASSERT_EQ_U(0u, buf[6]);
    TEST_ASSERT_EQ_U(0u, buf[7]);
    TEST_ASSERT_EQ_U(0xC8u, buf[9]);
    TEST_ASSERT_EQ_U(0xFFu, buf[28]);
    TEST_ASSERT_EQ_U(0xFFu, buf[31]);
    TEST_ASSERT(!bvpv_cd_record_decode(buf, &r));
    buf[28] = BVPV_CD_META_VALID;
    TEST_ASSERT(bvpv_cd_record_decode(buf, &r));
    TEST_ASSERT_EQ_U(m.kind, r.kind);
    TEST_ASSERT_EQ_U(m.bytes, r.bytes);
    TEST_ASSERT_EQ_U(m.crc32, r.crc32);
    TEST_ASSERT_EQ_U(m.sequence, r.sequence);
    TEST_ASSERT_EQ_U(m.write_count, r.write_count);
}

static void test_meta_decode_rejects(void)
{
    bvpv_cd_record_t m = {0, 2048, 0, 1, 1}, r;
    uint8_t buf[32];
    unsigned i;
    bvpv_cd_record_encode(buf, &m);
    buf[28] = BVPV_CD_META_VALID;
    for (i = 0; i < 29; i++)
    {
        buf[i] ^= 1;
        TEST_ASSERT(!bvpv_cd_record_decode(buf, &r));
        buf[i] ^= 1;
    }
    buf[6] = 1;
    {
        uint32_t crc = bvpv_cd_crc32(buf, 24);
        buf[24] = (uint8_t)crc; buf[25] = (uint8_t)(crc >> 8);
        buf[26] = (uint8_t)(crc >> 16); buf[27] = (uint8_t)(crc >> 24);
    }
    TEST_ASSERT(!bvpv_cd_record_decode(buf, &r)); /* CRC 正确时仍须拒绝非零保留字节。 */
    memcpy(buf, "BVC2", 4);
    TEST_ASSERT(!bvpv_cd_record_decode(buf, &r));
    memcpy(buf, "BVCD", 4);
    TEST_ASSERT(!bvpv_cd_record_decode(buf, &r));
    memset(buf, 255, sizeof(buf));
    TEST_ASSERT(!bvpv_cd_record_decode(buf, &r));
    TEST_ASSERT(!bvpv_cd_record_decode(NULL, &r));
    TEST_ASSERT(!bvpv_cd_record_decode(buf, NULL));
}

static void test_meta_usable(void)
{
    const uint32_t sizes[] = {0, 2049, BVPV_CD_SLOT_BYTES + 2048, BVPV_CD_SLOT_BYTES};
    bvpv_cd_record_t m = {0, 0, 0, 1, 1}, r;
    uint8_t buf[32];
    unsigned i;
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
        m.bytes = sizes[i];
        bvpv_cd_record_encode(buf, &m);
        buf[28] = BVPV_CD_META_VALID;
        TEST_ASSERT(bvpv_cd_record_decode(buf, &r) == (i == 3));
    }
    TEST_ASSERT(bvpv_cd_sequence_newer(0, 0xFFFFFFFFu));
    TEST_ASSERT(!bvpv_cd_sequence_newer(1, 1));
    TEST_ASSERT(!bvpv_cd_sequence_newer(0xFFFFFFFFu, 0));
}

/* 用例：LBA 与块数换算。
 * 功能：SCSI READ_10 的 (lba, offset) → 镜像区内字节偏移；以及容量上报的块数。
 * 预期：lba/offset 换算 = lba*2048+offset；末字节边界正确；offset ≥ 2048 → INVALID；
 *       block_count 向上取整；镜像区容量恰好 2048 块。
 * 判据：各边界值断言成立。 */
static void test_lba_layout(void)
{
    TEST_ASSERT_EQ_U(0x400000u, BVPV_CD_IMAGE_BASE + BVPV_CD_IMAGE_AREA_BYTES);
    TEST_ASSERT_EQ_U(0x400000u, BVPV_FLASH_PARAM_BASE);
    TEST_ASSERT_EQ_U(BVPV_FLASH_MCU_BASE, BVPV_FLASH_PARAM_BASE + BVPV_FLASH_PARAM_BYTES);
    TEST_ASSERT_EQ_U(BVPV_CD_META_BASE, BVPV_FLASH_MCU_BASE + BVPV_FLASH_MCU_BYTES);
    TEST_ASSERT_EQ_U(0x800000u, BVPV_CD_META_BASE + BVPV_CD_META_AREA_BYTES);
    TEST_ASSERT_EQ_U(0x7F0000u, BVPV_CD_RECORD_BASE(0));
    TEST_ASSERT_EQ_U(0x7F1000u, BVPV_CD_RECORD_BASE(1));
    TEST_ASSERT_EQ_U(0u, bvpv_cd_lba_to_image_offset(0u, 0u));
    TEST_ASSERT_EQ_U(2047u, bvpv_cd_lba_to_image_offset(0u, 2047u));
    TEST_ASSERT_EQ_U(2048u, bvpv_cd_lba_to_image_offset(1u, 0u));
    TEST_ASSERT_EQ_U(2048u + 5u, bvpv_cd_lba_to_image_offset(1u, 5u));
    TEST_ASSERT_EQ_U(BVPV_CD_IMAGE_AREA_BYTES - 1u,
                     bvpv_cd_lba_to_image_offset(BVPV_CD_MAX_BLOCKS - 1u, 2047u));
    TEST_ASSERT_EQ_U(BVPV_CD_OFFSET_INVALID, bvpv_cd_lba_to_image_offset(0u, 2048u));
    TEST_ASSERT_EQ_U(BVPV_CD_OFFSET_INVALID, bvpv_cd_lba_to_image_offset(3u, 0xFFFFu));

    TEST_ASSERT_EQ_U(0u, bvpv_cd_block_count(0u));
    TEST_ASSERT_EQ_U(1u, bvpv_cd_block_count(1u));
    TEST_ASSERT_EQ_U(1u, bvpv_cd_block_count(2048u));
    TEST_ASSERT_EQ_U(2u, bvpv_cd_block_count(2049u));
    TEST_ASSERT_EQ_U(25u, bvpv_cd_block_count(51200u));   /* 工厂镜像量级 */
    TEST_ASSERT_EQ_U(2048u, bvpv_cd_block_count(BVPV_CD_IMAGE_AREA_BYTES));
    TEST_ASSERT_EQ_U(2048u, BVPV_CD_MAX_BLOCKS);
}

static void test_no_media_record(void)
{
    bvpv_cd_record_t m = {BVPV_CD_RECORD_NO_MEDIA, 0, 0, 9, 7}, r;
    uint8_t buf[32];
    bvpv_cd_record_encode(buf, &m);
    buf[28] = BVPV_CD_META_VALID;
    TEST_ASSERT(bvpv_cd_record_decode(buf, &r));
    TEST_ASSERT_EQ_U(BVPV_CD_RECORD_NO_MEDIA, r.kind);
    TEST_ASSERT_EQ_U(9u, r.sequence);
    m.bytes = 2048;
    bvpv_cd_record_encode(buf, &m); buf[28] = BVPV_CD_META_VALID;
    TEST_ASSERT(!bvpv_cd_record_decode(buf, &r));
    m.bytes = 0; m.crc32 = 1;
    bvpv_cd_record_encode(buf, &m); buf[28] = BVPV_CD_META_VALID;
    TEST_ASSERT(!bvpv_cd_record_decode(buf, &r));
    m.crc32 = 0; m.kind = 2;
    bvpv_cd_record_encode(buf, &m); buf[28] = BVPV_CD_META_VALID;
    TEST_ASSERT(!bvpv_cd_record_decode(buf, &r));
}
int main(void)
{
    TEST_RUN(test_crc32_vectors);
    TEST_RUN(test_meta_roundtrip);
    TEST_RUN(test_meta_decode_rejects);
    TEST_RUN(test_meta_usable);
    TEST_RUN(test_lba_layout);
    TEST_RUN(test_no_media_record);
    TEST_MAIN_END();
}
