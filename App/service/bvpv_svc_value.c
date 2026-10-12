/* 值服务：VALUE_READ / VALUE_WRITE / VALUE_READ_BATCH / STATE_READ / ECHO。
 * 语义沿 BVPV：写后读回验证、写不重发、RWA 进 EEPROM-pending（docs/PROTOCOL.md §7）。 */
#include <string.h>

#include "bvpv_errcode.h"
#include "bvpv_svc_internal.h"

/* 从请求载荷解析 3 字节地址（[类][实例][属性]，MSB 在前）。 */
static uint32_t parse_addr3(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

/* VALUE_READ（0x05）：读单参数值。[0..2]地址 [3]元素索引（标量填 0）。
 * 校验链：存在性(0x01) → 可读权限(0x02) → 元素索引(0x05) → 回 [2B 长度][值]。 */
uint8_t svc_value_read(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                       uint8_t *resp, uint16_t *resp_len, uint16_t cap)
{
    uint32_t addr;
    int32_t  i;
    bvpv_param_t param;
    const bvpv_param_t *p = &param;
    bvpv_effective_attr_t attr;
    uint8_t  ei, status;
    uint16_t esz;

    if ((req->payload == 0) || (req->plen < 4u)) {
        return BVPV_ERR_LENGTH;
    }
    addr = parse_addr3(req->payload);
    i    = bvpv_catalog_find(ctx, addr);
    if (i < 0) {
        return BVPV_ERR_NOT_FOUND;
    }
    if (!bvpv_catalog_get(ctx, (uint16_t)i, &param)) return BVPV_ERR_STATE;
    status = bvpv_catalog_effective_attr(ctx, (uint16_t)i, &attr);
    if (status != BVPV_ERR_OK) return status;
    if (!bvpv_access_readable(attr.access)) {
        return BVPV_ERR_ACCESS_DENIED;
    }
    ei = req->payload[3];
    if (ei >= p->elem_count) {
        return BVPV_ERR_INDEX_OOB;
    }
    esz = bvpv_param_elem_size(p);
    if (cap < (uint16_t)(2u + esz)) {
        return BVPV_ERR_LENGTH;
    }
    resp[0] = (uint8_t)(esz & 0xFFu);
    resp[1] = (uint8_t)(esz >> 8);
    memcpy(resp + 2, p->value + ((uint32_t)ei * esz), esz);
    *resp_len = (uint16_t)(2u + esz);
    return BVPV_ERR_OK;
}

/* VALUE_WRITE（0x06）：写值。[0..2]地址 [3]元素索引 [4..5]值长 [6..]值。仅回状态码。
 * 校验链：存在性(0x01) → 可写权限(0x02) → 索引(0x05) → 值长=类型尺寸(0x08)
 *        → 数值范围(越界 0x03) → 写值缓冲；RWA 置 EEPROM-pending（state bit1）。
 * 语义沿 BVPV：主机须独立 VALUE_READ 读回比对才算写入完成；写不重发。 */
uint8_t svc_value_write(bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                        uint8_t *resp, uint16_t *resp_len, uint16_t cap)
{
    uint32_t addr;
    int32_t  i;
    bvpv_param_t param;
    const bvpv_param_t *p = &param;
    bvpv_effective_attr_t attr;
    uint8_t  ei, status;
    uint16_t vlen, esz;
    const uint8_t *val;

    (void)resp; (void)resp_len; (void)cap;
    if ((req->payload == 0) || (req->plen < 6u)) {
        return BVPV_ERR_LENGTH;
    }
    addr = parse_addr3(req->payload);
    i    = bvpv_catalog_find(ctx, addr);
    if (i < 0) {
        return BVPV_ERR_NOT_FOUND;
    }
    if (!bvpv_catalog_get(ctx, (uint16_t)i, &param)) return BVPV_ERR_STATE;
    status = bvpv_catalog_effective_attr(ctx, (uint16_t)i, &attr);
    if (status != BVPV_ERR_OK) return status;
    if (!bvpv_access_writable(attr.access)) {
        return BVPV_ERR_ACCESS_DENIED;
    }
    ei = req->payload[3];
    if (ei >= p->elem_count) {
        return BVPV_ERR_INDEX_OOB;
    }
    vlen = (uint16_t)((uint16_t)req->payload[4] | ((uint16_t)req->payload[5] << 8));
    esz  = bvpv_param_elem_size(p);
    if (vlen != esz) {
        return BVPV_ERR_LENGTH;   /* 0x08 长度错误 */
    }
    if (req->plen != (uint16_t)(6u + esz)) {
        return BVPV_ERR_LENGTH;
    }
    val = req->payload + 6;
    if (!bvpv_value_is_finite(p->type, val)) return BVPV_ERR_OUT_OF_RANGE;

    /* 数值范围校验（value_compare 对非数值类型返回 0，自然通过） */
    if (((attr.ext_flags & BVPV_EXT_HAS_MIN) != 0u) &&
        (bvpv_value_compare(p->type, val, attr.min) < 0)) {
        return BVPV_ERR_OUT_OF_RANGE;
    }
    if (((attr.ext_flags & BVPV_EXT_HAS_MAX) != 0u) &&
        (bvpv_value_compare(p->type, val, attr.max) > 0)) {
        return BVPV_ERR_OUT_OF_RANGE;
    }

    if (attr.access == BVPV_ACC_OWO) {
        return ctx->command_handler
            ? ctx->command_handler(ctx->command_user, addr, ei, val, vlen)
            : BVPV_ERR_NOT_SUPPORTED;
    }
    memcpy(p->value + ((uint32_t)ei * esz), val, esz);
    if (bvpv_access_eeprom_persistent(attr.access)) {
        bvpv_catalog_set_state(ctx, (uint16_t)i,
                               (uint8_t)(attr.state | BVPV_STATE_PENDING));
    }
    return BVPV_ERR_OK;
}

/* VALUE_READ_BATCH（0x07）：批量读（实时轮询通道）。
 * 请求 [0]=count(1..64) + count×[3B 地址][1B 索引]。
 * 应答帧级 status=0，逐项 [item_status][2B len][值]，单点失败不影响他项（失败项 len=0）。 */
uint8_t svc_value_read_batch(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                             uint8_t *resp, uint16_t *resp_len, uint16_t cap)
{
    uint8_t  count;
    uint16_t o = 0u;
    uint8_t  k;

    if ((req->payload == 0) || (req->plen < 1u)) {
        return BVPV_ERR_LENGTH;
    }
    count = req->payload[0];
    if ((count < 1u) || (count > 64u)) {
        return BVPV_ERR_LENGTH;
    }
    if (req->plen < (uint16_t)(1u + (uint16_t)count * 4u)) {
        return BVPV_ERR_LENGTH;
    }

    for (k = 0u; k < count; k++) {
        const uint8_t *e = req->payload + 1u + ((uint16_t)k * 4u);
        uint32_t addr = parse_addr3(e);
        uint8_t  ei   = e[3];
        int32_t  i    = bvpv_catalog_find(ctx, addr);
        uint8_t  ist;
        uint16_t esz  = 0u;

        if ((uint16_t)(o + 3u) > cap) {
            return BVPV_ERR_LENGTH;
        }
        if (i < 0) {
            ist = BVPV_ERR_NOT_FOUND;
        } else {
            bvpv_param_t param;
            const bvpv_param_t *p = &param;
            bvpv_effective_attr_t item_attr;
            if (!bvpv_catalog_get(ctx, (uint16_t)i, &param)) {
                ist = BVPV_ERR_STATE;
            } else if ((ist = bvpv_catalog_effective_attr(ctx, (uint16_t)i, &item_attr)) != BVPV_ERR_OK) {
                /* 有效策略失败只影响本项，不读值。 */
            } else if (!bvpv_access_readable(item_attr.access)) {
                ist = BVPV_ERR_ACCESS_DENIED;
            } else if (ei >= p->elem_count) {
                ist = BVPV_ERR_INDEX_OOB;
            } else {
                esz = bvpv_param_elem_size(p);
                if ((uint16_t)(o + 3u + esz) > cap) {
                    return BVPV_ERR_LENGTH;
                }
                memcpy(resp + o + 3u, p->value + ((uint32_t)ei * esz), esz);
                ist = BVPV_ERR_OK;
            }
        }
        resp[o]      = ist;
        resp[o + 1u] = (uint8_t)(esz & 0xFFu);
        resp[o + 2u] = (uint8_t)(esz >> 8);
        o = (uint16_t)(o + 3u + ((ist == BVPV_ERR_OK) ? esz : 0u));
    }
    *resp_len = o;
    return BVPV_ERR_OK;   /* 帧级 status=0：已处理（单项错误在 item_status） */
}

/* STATE_READ（0x08）：读参数运行时状态。[0..2]地址 [3]保留 → 回 1 字节 state
 * （bit0=运行时可见，bit1=值已修改未持久化/pending）。地址不存在回 0x01。 */
uint8_t svc_state_read(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                       uint8_t *resp, uint16_t *resp_len, uint16_t cap)
{
    uint32_t addr;
    int32_t  i;

    if ((req->payload == 0) || (req->plen < 3u)) {
        return BVPV_ERR_LENGTH;
    }
    addr = parse_addr3(req->payload);
    i    = bvpv_catalog_find(ctx, addr);
    if (i < 0) {
        return BVPV_ERR_NOT_FOUND;
    }
    if (cap < 1u) {
        return BVPV_ERR_LENGTH;
    }
    {
        bvpv_effective_attr_t attr;
        uint8_t status = bvpv_catalog_effective_attr(ctx, (uint16_t)i, &attr);
        if (status != BVPV_ERR_OK) return status;
        resp[0] = attr.state;
    }
    *resp_len = 1u;
    return BVPV_ERR_OK;
}

/* ECHO（0x7F）：把请求载荷原样回显（链路诊断/产测用）。 */
uint8_t svc_echo(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                 uint8_t *resp, uint16_t *resp_len, uint16_t cap)
{
    (void)ctx;
    if ((req->plen > 0u) && (req->payload == 0)) {
        return BVPV_ERR_LENGTH;
    }
    if (cap < req->plen) {
        return BVPV_ERR_LENGTH;
    }
    if (req->plen > 0u) {
        memcpy(resp, req->payload, req->plen);
    }
    *resp_len = req->plen;
    return BVPV_ERR_OK;
}
