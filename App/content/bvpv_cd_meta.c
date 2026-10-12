#include "bvpv_cd_meta.h"

#include <string.h>

/* CRC32 半字节表（反射多项式 0xEDB88320）。
 * 表项 n = 单个半字节 n 经 4 轮反射移位异或的结果。写在源码里而不是运行时生成：
 * 免初始化、免可变状态，可重入且不占 RAM。 */
static const uint32_t S_CRC32_NIBBLE[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

/* ---- 小端编解码 ---- */
static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

uint32_t bvpv_cd_lba_to_image_offset(uint32_t lba, uint32_t offset)
{
    if (offset >= BVPV_CD_BLOCK_SIZE) {
        return BVPV_CD_OFFSET_INVALID;
    }
    return (lba * BVPV_CD_BLOCK_SIZE) + offset;
}

uint32_t bvpv_cd_block_count(uint32_t image_bytes)
{
    return (image_bytes + BVPV_CD_BLOCK_SIZE - 1u) / BVPV_CD_BLOCK_SIZE;
}

uint32_t bvpv_cd_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    uint32_t c = ~crc;   /* zlib 约定：入参是"已取反"的中间值 */
    size_t   i;

    if (data == 0) {
        return crc;   /* len 非 0 而指针为空视为无数据；len=0 亦然 */
    }
    for (i = 0u; i < len; i++) {
        c ^= (uint32_t)data[i];
        c = (c >> 4) ^ S_CRC32_NIBBLE[c & 0x0Fu];
        c = (c >> 4) ^ S_CRC32_NIBBLE[c & 0x0Fu];
    }
    return ~c;
}

uint32_t bvpv_cd_crc32(const uint8_t *data, size_t len)
{
    return bvpv_cd_crc32_update(0u, data, len);
}

void bvpv_cd_record_encode(uint8_t out[BVPV_CD_RECORD_LEN], const bvpv_cd_record_t *r)
{
    memset(out, 0xFF, BVPV_CD_RECORD_LEN);
    memcpy(out, "BVC4", 4u);
    out[4] = BVPV_CD_LAYOUT_VERSION;
    out[5] = r->kind;
    out[6] = out[7] = 0;
    wr32le(out + 8, r->bytes);
    wr32le(out + 12, r->crc32);
    wr32le(out + 16, r->sequence);
    wr32le(out + 20, r->write_count);
    wr32le(out + 24, bvpv_cd_crc32(out, 24u));
}

bool bvpv_cd_record_decode(const uint8_t in[BVPV_CD_RECORD_LEN], bvpv_cd_record_t *r)
{
    uint32_t bytes;
    if (!in || !r || memcmp(in, "BVC4", 4u) || in[4] != BVPV_CD_LAYOUT_VERSION || in[5] > 1u || in[6] || in[7] ||
        in[BVPV_CD_COMMIT_OFFSET] != BVPV_CD_META_VALID ||
        rd32le(in + 24) != bvpv_cd_crc32(in, 24u)) return false;
    bytes = rd32le(in + 8);
    if (in[29] != 0xFFu || in[30] != 0xFFu || in[31] != 0xFFu) return false;
    if (in[5] == BVPV_CD_RECORD_NO_MEDIA) {
        if (bytes || rd32le(in + 12)) return false;
    } else if (!bytes || bytes > BVPV_CD_SLOT_BYTES || bytes % BVPV_CD_BLOCK_SIZE) return false;
    r->kind = in[5];
    r->bytes = bytes;
    r->crc32 = rd32le(in + 12);
    r->sequence = rd32le(in + 16);
    r->write_count = rd32le(in + 20);
    return true;
}

bool bvpv_cd_sequence_newer(uint32_t a, uint32_t b)
{
    return a != b && (uint32_t)(a - b) < 0x80000000u;
}
