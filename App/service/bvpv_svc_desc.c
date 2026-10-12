/* 自描述服务：HELLO / TREE_PULL / ATTR_SNAPSHOT_PULL / ATTR_EXT_READ / ERRTEXT_PULL。
 * 拉取类服务共用游标分块（docs/PROTOCOL.md §7.2）。 */
#include <string.h>

#include "bvpv_errcode.h"
#include "bvpv_service.h"
#include "bvpv_svc_internal.h"
#include "bvpv_usb_frag.h"   /* BVPV_FRAG_PAYLOAD / BVPV_REASM_TIMEOUT_MS */

/* 解析游标分块请求（拉取类服务共用）：[0..1]start_index [2..3]max_bytes。
 * 长度不足返回 0x08（长度错误）。 */
static uint8_t parse_cursor_req(const bvpv_frame_t *req, uint16_t *start, uint16_t *maxb)
{
    if ((req->payload == 0) || (req->plen != 4u)) {
        return BVPV_ERR_LENGTH;
    }
    *start = (uint16_t)((uint16_t)req->payload[0] | ((uint16_t)req->payload[1] << 8));
    *maxb  = (uint16_t)((uint16_t)req->payload[2] | ((uint16_t)req->payload[3] << 8));
    return BVPV_ERR_OK;
}

/* HELLO（0x01）：能力协商。校验主机协议版本（不符回 0x0A），
 * 回设备协议版本/设备类型/最大消息/分片载荷/重组超时/设备名。 */
uint8_t svc_hello(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                  uint8_t *resp, uint16_t *resp_len, uint16_t cap)
{
    uint8_t host_ver = ((req->payload != 0) && (req->plen >= 1u)) ? req->payload[0] : 0u;
    const char *name;
    uint8_t nlen;
    uint16_t need;
    uint16_t o = 0u, capabilities;

    if (req->plen != 1u || req->payload == 0) return BVPV_ERR_LENGTH;
    if (host_ver != BVPV_PROTO_VERSION) {
        return BVPV_ERR_STATE;   /* 0x0A 版本不兼容 */
    }
    name = (ctx->device_name != 0) ? ctx->device_name : "";
    nlen = (uint8_t)strlen(name);
    need = (uint16_t)(14u + nlen);   /* ver1+type1+maxmsg2+frag1+timeout2+namelen1 = 8 */
    if (cap < need) {
        return BVPV_ERR_LENGTH;
    }

    resp[o++] = BVPV_PROTO_VERSION;
    resp[o++] = ctx->device_type;
    resp[o++] = (uint8_t)(BVPV_MAX_FRAME & 0xFFu);
    resp[o++] = (uint8_t)(BVPV_MAX_FRAME >> 8);
    resp[o++] = (uint8_t)BVPV_FRAG_PAYLOAD;
    resp[o++] = (uint8_t)(BVPV_REASM_TIMEOUT_MS & 0xFFu);
    resp[o++] = (uint8_t)(BVPV_REASM_TIMEOUT_MS >> 8);
    resp[o++] = nlen;
    memcpy(resp + o, name, nlen);
    o = (uint16_t)(o + nlen);
    capabilities = BVPV_CAP_DYNAMIC_ATTR |
        (bvpv_service_fw_supported() ? BVPV_CAP_FW_UPDATE : 0u) |
        (bvpv_service_catalog_supported() ? BVPV_CAP_CATALOG : 0u);
    resp[o++] = (uint8_t)capabilities;
    resp[o++] = (uint8_t)(capabilities >> 8);
    resp[o++] = (uint8_t)ctx->revision;
    resp[o++] = (uint8_t)(ctx->revision >> 8);
    resp[o++] = (uint8_t)(ctx->revision >> 16);
    resp[o++] = (uint8_t)(ctx->revision >> 24);

    *resp_len = o;
    return BVPV_ERR_OK;
}

/* TREE_PULL（0x02）：统一 v2 分组/参数记录，游标按记录计数。
 * max_bytes 仅限制记录区；单条记录不可跨页，零进展返回长度错误。 */
uint8_t svc_tree_pull(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                      uint8_t *resp, uint16_t *resp_len, uint16_t cap)
{
    uint16_t start, maxb;
    uint8_t st = parse_cursor_req(req, &start, &maxb);
    uint16_t idx, used = 0u, next;
    if (st != BVPV_ERR_OK || req->plen != 4u) return BVPV_ERR_LENGTH;
    if (cap < 2u) return BVPV_ERR_LENGTH;
    if (start > ctx->tree_count) return BVPV_ERR_INDEX_OOB;
    if (ctx->tree_count != 0u && ctx->tree == 0) return BVPV_ERR_STATE;
    idx = start;
    while (idx < ctx->tree_count) {
        const bvpv_tree_record_t *r = &ctx->tree[idx];
        size_t key_len = 0u;
        uint16_t size;
        uint8_t *out;
        if (r->kind == BVPV_TREE_GROUP) {
            if (r->key == 0 || r->node_id == 0u) return BVPV_ERR_STATE;
            key_len = strlen(r->key);
            if (key_len == 0u || key_len > 255u) return BVPV_ERR_STATE;
            size = (uint16_t)(6u + key_len);
        } else if (r->kind == BVPV_TREE_PARAM) {
            size = 6u;
        } else return BVPV_ERR_STATE;
        if ((uint32_t)used + size > maxb || (uint32_t)2u + used + size > cap) {
            if (idx == start) return BVPV_ERR_LENGTH;
            break;
        }
        out = resp + 2u + used;
        out[0] = r->kind;
        if (r->kind == BVPV_TREE_GROUP) {
            out[1] = (uint8_t)r->node_id;
            out[2] = (uint8_t)(r->node_id >> 8);
            out[3] = (uint8_t)r->parent_id;
            out[4] = (uint8_t)(r->parent_id >> 8);
            out[5] = (uint8_t)key_len;
            memcpy(out + 6u, r->key, key_len);
        } else {
            out[1] = (uint8_t)r->parent_id;
            out[2] = (uint8_t)(r->parent_id >> 8);
            out[3] = (uint8_t)(r->addr >> 16);
            out[4] = (uint8_t)(r->addr >> 8);
            out[5] = (uint8_t)r->addr;
        }
        used = (uint16_t)(used + size);
        idx++;
    }
    next = idx >= ctx->tree_count ? 0xFFFFu : idx;
    resp[0] = (uint8_t)next;
    resp[1] = (uint8_t)(next >> 8);
    *resp_len = (uint16_t)(2u + used);
    return BVPV_ERR_OK;
}
/* 单参数和分页服务共用扩展属性编码，提供器由调用方读取一次。 */
static uint8_t encode_ext_attrs(const bvpv_param_t *p,
                                const bvpv_effective_attr_t *attr,
                                uint8_t *resp, uint16_t *resp_len, uint16_t cap)
{
    uint16_t esz = bvpv_param_elem_size(p);
    uint16_t o = 0u;
    /* 便捷宏：写一个数值型 TLV（tag + len + 原始字节）。长度显式给出——
     * 源缓冲不总是 elem_size 字节（Min/Max 是内联 8 字节数组），隐式用 esz 会越界读。 */
#define WRITE_TLV_NUM(tag, src, len)                                     \
    do {                                                                 \
        uint16_t n_ = (uint16_t)(len);                                   \
        if ((uint16_t)(o + 3u + n_) > cap) {                             \
            return BVPV_ERR_LENGTH;                                      \
        }                                                                \
        resp[o++] = (uint8_t)(tag);                                      \
        resp[o++] = (uint8_t)(n_ & 0xFFu);                               \
        resp[o++] = (uint8_t)(n_ >> 8);                                  \
        memcpy(resp + o, (src), n_);                                     \
        o = (uint16_t)(o + n_);                                          \
    } while (0)

    /* Min/Max 只有数值型有意义（bvpv_value_compare 对 TEXT/COMPOSITE 恒返回 0），
     * 因此长度以 8 字节内联数组为上限，元素更大时按 8 截断而非越界读。 */
    if ((attr->ext_flags & BVPV_EXT_HAS_MIN) != 0u) {
        WRITE_TLV_NUM(0x01u, attr->min, (esz < 8u) ? esz : 8u);
    }
    if ((attr->ext_flags & BVPV_EXT_HAS_MAX) != 0u) {
        WRITE_TLV_NUM(0x02u, attr->max, (esz < 8u) ? esz : 8u);
    }
    if ((attr->ext_flags & BVPV_EXT_HAS_DEFAULT) != 0u) {
        const uint8_t *def = attr->def;
        if (def != 0) {
            WRITE_TLV_NUM(0x03u, def, esz);
        }
    }
#undef WRITE_TLV_NUM

    /* EnumValues（tag 0x04）：重复 [4B 值 LE][1B 文本长][UTF-8] */
    if ((attr->enums != 0) && (attr->enum_count > 0u)) {
        uint16_t elen = 0u;
        uint8_t  k;
        for (k = 0u; k < attr->enum_count; k++) {
            elen = (uint16_t)(elen + 4u + 1u + (uint16_t)strlen(attr->enums[k].text));
        }
        if ((uint16_t)(o + 3u + elen) > cap) {
            return BVPV_ERR_LENGTH;
        }
        resp[o++] = 0x04u;
        resp[o++] = (uint8_t)(elen & 0xFFu);
        resp[o++] = (uint8_t)(elen >> 8);
        for (k = 0u; k < attr->enum_count; k++) {
            uint32_t v  = attr->enums[k].value;
            uint8_t  tl = (uint8_t)strlen(attr->enums[k].text);
            resp[o++] = (uint8_t)(v & 0xFFu);
            resp[o++] = (uint8_t)((v >> 8) & 0xFFu);
            resp[o++] = (uint8_t)((v >> 16) & 0xFFu);
            resp[o++] = (uint8_t)((v >> 24) & 0xFFu);
            resp[o++] = tl;
            memcpy(resp + o, attr->enums[k].text, tl);
            o = (uint16_t)(o + tl);
        }
    }

    /* Unit（tag 0x05）：UTF-8 单位串 */
    if (attr->unit != 0) {
        uint8_t ul = (uint8_t)strlen(attr->unit);
        if ((uint16_t)(o + 3u + ul) > cap) {
            return BVPV_ERR_LENGTH;
        }
        resp[o++] = 0x05u;
        resp[o++] = (uint8_t)(ul & 0xFFu);
        resp[o++] = (uint8_t)(ul >> 8);
        memcpy(resp + o, attr->unit, ul);
        o = (uint16_t)(o + ul);
    }

    *resp_len = o;
    return BVPV_ERR_OK;
}

/* ATTR_EXT_READ（0x04）：按地址读取扩展属性。 */
uint8_t svc_attr_ext_read(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                          uint8_t *resp, uint16_t *resp_len, uint16_t cap)
{
    uint32_t addr;
    int32_t i;
    bvpv_effective_attr_t attr;
    bvpv_param_t param;
    uint8_t status;
    if (req->payload == 0 || req->plen < 4u) return BVPV_ERR_LENGTH;
    addr = ((uint32_t)req->payload[0] << 16) | ((uint32_t)req->payload[1] << 8) |
           (uint32_t)req->payload[2];
    i = bvpv_catalog_find(ctx, addr);
    if (i < 0) return BVPV_ERR_NOT_FOUND;
    if (!bvpv_catalog_get(ctx, (uint16_t)i, &param)) return BVPV_ERR_STATE;
    status = bvpv_catalog_effective_attr(ctx, (uint16_t)i, &attr);
    if (status != BVPV_ERR_OK) return status;
    return encode_ext_attrs(&param, &attr, resp, resp_len, cap);
}
static uint32_t desc_rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void desc_wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ATTR_CORE_READ（0x10）：按地址读取当前生效的核心属性。
 * 应答数据：[type][access][flags][state][elem_count LE]。 */
uint8_t svc_attr_core_read(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                           uint8_t *resp, uint16_t *resp_len, uint16_t cap)
{
    uint32_t addr;
    int32_t index;
    const bvpv_param_t *p;
    bvpv_effective_attr_t attr;
    bvpv_param_t param;
    uint8_t status;
    if ((req->payload == 0) || (req->plen < 4u)) return BVPV_ERR_LENGTH;
    if (cap < 6u) return BVPV_ERR_LENGTH;
    addr = ((uint32_t)req->payload[0] << 16) |
           ((uint32_t)req->payload[1] << 8) | req->payload[2];
    index = bvpv_catalog_find(ctx, addr);
    if (index < 0) return BVPV_ERR_NOT_FOUND;
    if (!bvpv_catalog_get(ctx, (uint16_t)index, &param)) return BVPV_ERR_STATE;
    p = &param;
    status = bvpv_catalog_effective_attr(ctx, (uint16_t)index, &attr);
    if (status != BVPV_ERR_OK) return status;
    resp[0] = p->type;
    resp[1] = attr.access;
    resp[2] = attr.flags;
    resp[3] = attr.state;
    resp[4] = (uint8_t)p->elem_count;
    resp[5] = (uint8_t)(p->elem_count >> 8);
    *resp_len = 6u;
    return BVPV_ERR_OK;
}

/* ATTR_SNAPSHOT_PULL（0x12）：分页返回同一修订的名称、核心与扩展属性。 */
uint8_t svc_attr_snapshot_pull(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                               uint8_t *resp, uint16_t *resp_len, uint16_t cap)
{
    uint16_t start, maxb, index, used = 0u, next;
    uint32_t expected, revision;
    if (req->payload == 0 || req->plen != 8u || cap < 6u) return BVPV_ERR_LENGTH;
    start = (uint16_t)((uint16_t)req->payload[0] | ((uint16_t)req->payload[1] << 8));
    maxb = (uint16_t)((uint16_t)req->payload[2] | ((uint16_t)req->payload[3] << 8));
    expected = desc_rd32le(req->payload + 4u);
    if (start > ctx->count) return BVPV_ERR_INDEX_OOB;
    revision = ctx->revision;
    if (revision == 0u || (expected != 0u && expected != revision))
        return BVPV_ERR_STATE;
    index = start;
    while (index < ctx->count) {
        bvpv_param_t param;
        const bvpv_param_t *p = &param;
        bvpv_effective_attr_t attr;
        size_t name_len;
        uint16_t ext_len = 0u, remaining, record_size, base;
        uint8_t *record;
        uint8_t status;
        if (!bvpv_catalog_get(ctx, index, &param)) return BVPV_ERR_STATE;
        name_len = p->name != 0 ? strlen(p->name) : 0u;
        if (name_len == 0u || name_len > 255u) return BVPV_ERR_STATE;
        base = (uint16_t)(12u + name_len);
        if ((uint32_t)used + base > maxb || (uint32_t)used + base + 6u > cap) {
            if (index == start) return BVPV_ERR_LENGTH;
            break;
        }
        status = bvpv_catalog_effective_attr(ctx, index, &attr);
        if (status != BVPV_ERR_OK) return status;
        record = resp + 6u + used;
        remaining = (uint16_t)(maxb - used - base);
        if (remaining > (uint16_t)(cap - used - base - 6u))
            remaining = (uint16_t)(cap - used - base - 6u);
        status = encode_ext_attrs(p, &attr, record + base, &ext_len, remaining);
        if (status == BVPV_ERR_LENGTH && index != start) break;
        if (status != BVPV_ERR_OK) return status;
        record_size = (uint16_t)(base + ext_len);
        record[0] = (uint8_t)(p->addr >> 16);
        record[1] = (uint8_t)(p->addr >> 8);
        record[2] = (uint8_t)p->addr;
        record[3] = p->type;
        record[4] = attr.access;
        record[5] = attr.flags;
        record[6] = attr.state;
        record[7] = (uint8_t)p->elem_count;
        record[8] = (uint8_t)(p->elem_count >> 8);
        record[9] = (uint8_t)name_len;
        memcpy(record + 10u, p->name, name_len);
        record[10u + name_len] = (uint8_t)ext_len;
        record[11u + name_len] = (uint8_t)(ext_len >> 8);
        used = (uint16_t)(used + record_size);
        index++;
    }
    if (ctx->revision != revision) return BVPV_ERR_STATE;
    next = index == ctx->count ? 0xFFFFu : index;
    desc_wr32le(resp, revision);
    resp[4] = (uint8_t)next;
    resp[5] = (uint8_t)(next >> 8);
    *resp_len = (uint16_t)(6u + used);
    return BVPV_ERR_OK;
}
/* CHANGE_QUERY（0x11）：固定修订快照上的分页变化读取。
 * 请求：[since u32][snapshot u32，首包0][cursor u16][max_bytes u16]
 * 应答：[snapshot u32][current u32][next u16][flags][addr3+mask]...。
 * flags.bit0 表示日志已覆盖或快照非法，主机必须全量重同步。 */
uint8_t svc_change_query(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                         uint8_t *resp, uint16_t *resp_len, uint16_t cap)
{
    uint32_t since, snapshot, oldest_revision;
    uint16_t cursor, max_bytes, used = 0u, next;
    uint8_t flags = 0u;
    const bvpv_change_entry_t *oldest;
    if ((req->payload == 0) || (req->plen < 12u)) return BVPV_ERR_LENGTH;
    if (cap < 11u) return BVPV_ERR_LENGTH;
    since = desc_rd32le(req->payload);
    snapshot = desc_rd32le(req->payload + 4u);
    cursor = (uint16_t)((uint16_t)req->payload[8] |
                        ((uint16_t)req->payload[9] << 8));
    max_bytes = (uint16_t)((uint16_t)req->payload[10] |
                           ((uint16_t)req->payload[11] << 8));
    if (snapshot == 0u) snapshot = ctx->revision;
    if (cursor > ctx->change_count) return BVPV_ERR_INDEX_OOB;

    oldest = bvpv_catalog_change_at(ctx, 0u);
    oldest_revision = (oldest != 0) ? oldest->revision : ctx->revision;
    if ((snapshot > ctx->revision) || (since > snapshot) ||
        ((oldest != 0) && ((since + 1u) < oldest_revision))) {
        flags = 0x01u;
        cursor = ctx->change_count;
    }

    next = cursor;
    while ((flags == 0u) && (next < ctx->change_count)) {
        const bvpv_change_entry_t *entry = bvpv_catalog_change_at(ctx, next);
        if (entry == 0) {
            flags = 0x01u;
            break;
        }
        if ((entry->revision > since) && (entry->revision <= snapshot)) {
            if (((uint16_t)(used + 4u) > max_bytes) ||
                ((uint16_t)(11u + used + 4u) > cap)) {
                break;
            }
            resp[11u + used] = (uint8_t)(entry->addr >> 16);
            resp[12u + used] = (uint8_t)(entry->addr >> 8);
            resp[13u + used] = (uint8_t)entry->addr;
            resp[14u + used] = entry->mask;
            used = (uint16_t)(used + 4u);
        }
        next++;
    }
    if ((flags != 0u) || (next >= ctx->change_count)) next = 0xFFFFu;
    desc_wr32le(resp, snapshot);
    desc_wr32le(resp + 4u, ctx->revision);
    resp[8] = (uint8_t)next;
    resp[9] = (uint8_t)(next >> 8);
    resp[10] = flags;
    *resp_len = (uint16_t)(11u + used);
    return BVPV_ERR_OK;
}

/* ERRTEXT_PULL（0x09）：错误码文本表，游标分块。条目 [1B 错误码][1B 文本长][ASCII]。
 * 游标语义同 TREE_PULL；数据源为 bvpv_errcode 的错误码表。 */
uint8_t svc_errtext_pull(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                         uint8_t *resp, uint16_t *resp_len, uint16_t cap)
{
    uint16_t start, maxb;
    uint8_t  st = parse_cursor_req(req, &start, &maxb);
    uint16_t idx, used = 0u, next;

    (void)ctx;
    if (st != BVPV_ERR_OK) {
        return st;
    }
    if (cap < 2u) return BVPV_ERR_LENGTH;
    if (start > bvpv_errtext_count) {
        return BVPV_ERR_INDEX_OOB;
    }
    idx = start;
    while (idx < bvpv_errtext_count) {
        const bvpv_errtext_t *e = &bvpv_errtext_table[idx];
        uint8_t  tl  = (uint8_t)strlen(e->text);
        uint16_t esz = (uint16_t)(2u + tl);

        if ((uint32_t)used + esz > maxb || (uint32_t)2u + used + esz > cap) {
            if (idx == start) return BVPV_ERR_LENGTH;
            break;
        }
        resp[2u + used + 0u] = e->code;
        resp[2u + used + 1u] = tl;
        memcpy(resp + 2u + used + 2u, e->text, tl);
        used = (uint16_t)(used + esz);
        idx++;
    }
    next = (idx >= bvpv_errtext_count) ? 0xFFFFu : idx;
    resp[0] = (uint8_t)(next & 0xFFu);
    resp[1] = (uint8_t)(next >> 8);
    *resp_len = (uint16_t)(2u + used);
    return BVPV_ERR_OK;
}

/* 动态快照不携带名称、默认值、枚举及静态显示标志。 */
uint8_t svc_runtime_snapshot_pull(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                                   uint8_t *resp, uint16_t *resp_len, uint16_t cap)
{
    uint16_t cursor, limit, index, used = 0, next;
    uint32_t expected, revision;
    if (!req->payload || req->plen != 8u || cap < 6u) return BVPV_ERR_LENGTH;
    cursor = (uint16_t)(req->payload[0] | ((uint16_t)req->payload[1] << 8));
    limit = (uint16_t)(req->payload[2] | ((uint16_t)req->payload[3] << 8));
    expected = desc_rd32le(req->payload + 4);
    revision = ctx->revision;
    if (cursor > ctx->count) return BVPV_ERR_INDEX_OOB;
    if (!revision || (expected && expected != revision)) return BVPV_ERR_STATE;
    for (index = cursor; index < ctx->count; index++) {
        bvpv_param_t p;
        bvpv_effective_attr_t attr;
        uint16_t esz, size;
        uint8_t min_len, max_len, status;
        uint8_t *record;
        if (!bvpv_catalog_get(ctx, index, &p)) return BVPV_ERR_STATE;
        status = bvpv_catalog_effective_attr(ctx, index, &attr);
        if (status != BVPV_ERR_OK) return status;
        esz = bvpv_param_elem_size(&p);
        if (esz > 8u) esz = 0u;
        min_len = (attr.ext_flags & BVPV_EXT_HAS_MIN) ? (uint8_t)esz : 0u;
        max_len = (attr.ext_flags & BVPV_EXT_HAS_MAX) ? (uint8_t)esz : 0u;
        if ((min_len && !attr.min) || (max_len && !attr.max)) return BVPV_ERR_STATE;
        size = (uint16_t)(7u + min_len + max_len);
        if ((uint32_t)used + size > limit || (uint32_t)used + size + 6u > cap) {
            if (index == cursor) return BVPV_ERR_LENGTH;
            break;
        }
        record = resp + 6u + used;
        record[0] = (uint8_t)(p.addr >> 16);
        record[1] = (uint8_t)(p.addr >> 8);
        record[2] = (uint8_t)p.addr;
        record[3] = attr.access;
        record[4] = attr.state;
        record[5] = min_len;
        record[6] = max_len;
        if (min_len) memcpy(record + 7, attr.min, min_len);
        if (max_len) memcpy(record + 7 + min_len, attr.max, max_len);
        used = (uint16_t)(used + size);
    }
    if (ctx->revision != revision) return BVPV_ERR_STATE;
    next = index == ctx->count ? 0xFFFFu : index;
    desc_wr32le(resp, revision);
    resp[4] = (uint8_t)next; resp[5] = (uint8_t)(next >> 8);
    *resp_len = (uint16_t)(6u + used);
    return BVPV_ERR_OK;
}
