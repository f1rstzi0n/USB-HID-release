#include "bvpv_catalog.h"
#include "../service/bvpv_errcode.h"

#include <string.h>

/* 类型默认尺寸：定长类型返回字节数；COMPOSITE/UNDEFINED 返回 0（尺寸由目录 elem_size 决定）。 */
uint16_t bvpv_type_size(uint8_t type)
{
    switch (type) {
    case BVPV_TYPE_UINT8:        return 1u;
    case BVPV_TYPE_UINT16:
    case BVPV_TYPE_UFRAC16:      return 2u;
    case BVPV_TYPE_UINT32:
    case BVPV_TYPE_INT32:
    case BVPV_TYPE_FLOAT32:
    case BVPV_TYPE_FIXED16_16:
    case BVPV_TYPE_UFIXED16_16:  return 4u;
    case BVPV_TYPE_UINT64:       return 8u;
    case BVPV_TYPE_TEXT8:        return 8u;
    case BVPV_TYPE_TEXT16:       return 16u;
    case BVPV_TYPE_TEXT32:       return 32u;
    default:                     return 0u;   /* COMPOSITE / UNDEFINED */
    }
}

/* 单元素字节尺寸：COMPOSITE 取 elem_size（目录自定义），其余取类型默认尺寸。 */
uint16_t bvpv_param_elem_size(const bvpv_param_t *p)
{
    if (p == 0) {
        return 0u;
    }
    if (p->type == BVPV_TYPE_COMPOSITE) {
        return p->elem_size;
    }
    return bvpv_type_size(p->type);
}

/* 可读权限：RWA/ROO/RWO 可读，OWO/OOO 不可读。 */
bool bvpv_access_readable(uint8_t access)
{
    return (access == BVPV_ACC_RWA) || (access == BVPV_ACC_ROO) || (access == BVPV_ACC_RWO);
}

/* 可写权限：RWA/RWO/OWO 可写，ROO/OOO 不可写。 */
bool bvpv_access_writable(uint8_t access)
{
    return (access == BVPV_ACC_RWA) || (access == BVPV_ACC_RWO) || (access == BVPV_ACC_OWO);
}

/* EEPROM 持久权限：仅 RWA 进 EEPROM 持久集（沿 BVPV 语义）。 */
bool bvpv_access_eeprom_persistent(uint8_t access)
{
    return access == BVPV_ACC_RWA;
}

/* 地址合成：类/实例/属性三字节 → 24 位地址（类<<16 | 实例<<8 | 属性）。 */
uint32_t bvpv_addr(uint8_t cls, uint8_t inst, uint8_t attr)
{
    return ((uint32_t)cls << 16) | ((uint32_t)inst << 8) | (uint32_t)attr;
}

/* 按地址查找参数，返回树序元数据索引；生成目录用 Flash 索引二分。 */
int32_t bvpv_catalog_find(const bvpv_service_ctx_t *ctx, uint32_t addr)
{
    uint16_t i;
    if ((ctx == 0) || ((ctx->params == 0) && (ctx->runtime == 0))) {
        return -1;
    }
    if (ctx->runtime != 0) {
        uint16_t lo = 0u, hi = ctx->count;
        while (lo < hi) {
            uint16_t mid = (uint16_t)(lo + (hi - lo) / 2u);
            if (ctx->runtime[mid].addr < addr) lo = (uint16_t)(mid + 1u);
            else hi = mid;
        }
        return (lo < ctx->count && ctx->runtime[lo].addr == addr) ? (int32_t)lo : -1;
    }
    if (ctx->addr_order != 0) {
        uint16_t lo = 0u, hi = ctx->count;
        while (lo < hi) {
            uint16_t mid = (uint16_t)(lo + (hi - lo) / 2u);
            uint16_t index = ctx->addr_order[mid];
            uint32_t found = ctx->params[index].addr;
            if (found < addr) lo = (uint16_t)(mid + 1u);
            else hi = mid;
        }
        if (lo < ctx->count && ctx->params[ctx->addr_order[lo]].addr == addr)
            return (int32_t)ctx->addr_order[lo];
        return -1;
    }
    for (i = 0u; i < ctx->count; i++) {
        if (ctx->params[i].addr == addr) return (int32_t)i;
    }
    return -1;
}

bool bvpv_catalog_get(const bvpv_service_ctx_t *ctx, uint16_t index,
                      bvpv_param_t *out)
{
    const bvpv_runtime_param_t *p;
    if (ctx == 0 || out == 0 || index >= ctx->count) return false;
    if (ctx->runtime == 0) {
        if (ctx->params == 0) return false;
        *out = ctx->params[index];
        return true;
    }
    p = &ctx->runtime[index];
    memset(out, 0, sizeof(*out));
    out->addr = p->addr;
    out->type = p->type;
    out->access = p->access;
    out->flags = p->flags;
    out->elem_count = p->elem_count;
    out->elem_size = p->elem_size;
    out->ext_flags = (uint8_t)(p->ext_flags & (BVPV_EXT_HAS_MIN | BVPV_EXT_HAS_MAX));
    memcpy(out->min, p->min, sizeof(out->min));
    memcpy(out->max, p->max, sizeof(out->max));
    out->value = p->value;
    return true;
}

/* 默认隐藏和运行时可见独立；初始均可见，后续由有效属性提供器限制。 */
void bvpv_catalog_init_states(const bvpv_service_ctx_t *ctx)
{
    uint16_t i;
    if ((ctx == 0) || (ctx->states == 0) ||
        ((ctx->params == 0) && (ctx->runtime == 0))) {
        return;
    }
    for (i = 0u; i < ctx->count; i++) {
        ctx->states[i] = BVPV_STATE_VISIBLE;
    }
    ((bvpv_service_ctx_t *)ctx)->change_head = 0u;
    ((bvpv_service_ctx_t *)ctx)->change_count = 0u;
    ((bvpv_service_ctx_t *)ctx)->revision = 1u;
}

uint8_t bvpv_catalog_effective_attr(const bvpv_service_ctx_t *ctx, uint16_t index,
                                     bvpv_effective_attr_t *attr)
{
    bvpv_param_t param;
    const bvpv_param_t *p;
    uint8_t status;
    if (attr == 0 || !bvpv_catalog_get(ctx, index, &param)) return BVPV_ERR_NOT_FOUND;
    p = ctx->runtime != 0 ? &param : &ctx->params[index];
    attr->access = p->access;
    attr->flags = p->flags;
    attr->state = (ctx->states != 0) ? ctx->states[index] : 0u;
    attr->ext_flags = p->ext_flags;
    attr->min = p->min;
    attr->max = p->max;
    attr->def = bvpv_param_default(p);
    attr->unit = p->unit;
    attr->enums = p->enums;
    attr->enum_count = p->enum_count;
    if (ctx->attr_provider != 0) {
        ctx->attr_provider(ctx->attr_provider_user, index, p, attr);
    }
    if (ctx->policy_provider != 0) {
        status = ctx->policy_provider(ctx->policy_provider_user, index, p, attr);
        if (status != BVPV_ERR_OK) return status;
    }
    if (attr->access > BVPV_ACC_OWO ||
        (bvpv_access_readable(attr->access) && !bvpv_access_readable(p->access)) ||
        (bvpv_access_writable(attr->access) && !bvpv_access_writable(p->access)) ||
        (bvpv_access_eeprom_persistent(attr->access) && !bvpv_access_eeprom_persistent(p->access)) ||
        (attr->access != BVPV_ACC_OOO &&
         ((attr->access == BVPV_ACC_OWO) != (p->access == BVPV_ACC_OWO))))
        return BVPV_ERR_STATE;
    attr->flags = (uint8_t)(attr->flags & p->flags);
    if (ctx->states != 0)
        attr->state = (uint8_t)((attr->state & BVPV_STATE_VISIBLE) |
                               (ctx->states[index] & BVPV_STATE_PENDING));
    if ((attr->ext_flags & (BVPV_EXT_HAS_MIN | BVPV_EXT_HAS_MAX)) != 0u &&
        (p->type == BVPV_TYPE_UNDEFINED || p->type > BVPV_TYPE_UFRAC16))
        return BVPV_ERR_STATE;
    if ((attr->ext_flags & BVPV_EXT_HAS_MIN) != 0u) {
        if (!bvpv_value_is_finite(p->type, attr->min)) return BVPV_ERR_STATE;
        memcpy(attr->min_bytes, attr->min, sizeof(attr->min_bytes));
        attr->min = attr->min_bytes;
    } else attr->min = 0;
    if ((attr->ext_flags & BVPV_EXT_HAS_MAX) != 0u) {
        if (!bvpv_value_is_finite(p->type, attr->max)) return BVPV_ERR_STATE;
        memcpy(attr->max_bytes, attr->max, sizeof(attr->max_bytes));
        attr->max = attr->max_bytes;
    } else attr->max = 0;
    if (((p->ext_flags & BVPV_EXT_HAS_MIN) != 0u &&
         ((attr->ext_flags & BVPV_EXT_HAS_MIN) == 0u ||
          bvpv_value_compare(p->type, attr->min, p->min) < 0)) ||
        ((p->ext_flags & BVPV_EXT_HAS_MAX) != 0u &&
         ((attr->ext_flags & BVPV_EXT_HAS_MAX) == 0u ||
          bvpv_value_compare(p->type, attr->max, p->max) > 0)) ||
        ((attr->ext_flags & (BVPV_EXT_HAS_MIN | BVPV_EXT_HAS_MAX)) ==
         (BVPV_EXT_HAS_MIN | BVPV_EXT_HAS_MAX) &&
         bvpv_value_compare(p->type, attr->min, attr->max) > 0))
        return BVPV_ERR_STATE;
    return BVPV_ERR_OK;
}

uint32_t bvpv_catalog_note_change(bvpv_service_ctx_t *ctx, uint16_t index,
                                  uint8_t mask)
{
    bvpv_change_entry_t *e;
    if ((ctx == 0) || (index >= ctx->count) || (mask == 0u)) {
        return (ctx != 0) ? ctx->revision : 0u;
    }
    ctx->revision++;
    if (ctx->revision == 0u) {
        ctx->revision = 1u;
    }
    if ((ctx->changes == 0) || (ctx->change_capacity == 0u)) {
        return ctx->revision;
    }
    e = &ctx->changes[ctx->change_head];
    e->revision = ctx->revision;
    e->addr = ctx->runtime != 0 ? ctx->runtime[index].addr : ctx->params[index].addr;
    e->mask = mask;
    ctx->change_head = (uint16_t)((ctx->change_head + 1u) % ctx->change_capacity);
    if (ctx->change_count < ctx->change_capacity) {
        ctx->change_count++;
    }
    return ctx->revision;
}

uint32_t bvpv_catalog_set_state(bvpv_service_ctx_t *ctx, uint16_t index,
                                uint8_t state)
{
    if ((ctx == 0) || (ctx->states == 0) || (index >= ctx->count)) {
        return (ctx != 0) ? ctx->revision : 0u;
    }
    if (ctx->states[index] == state) {
        return ctx->revision;
    }
    ctx->states[index] = state;
    return bvpv_catalog_note_change(ctx, index, BVPV_CHANGE_STATE);
}

const bvpv_change_entry_t *bvpv_catalog_change_at(const bvpv_service_ctx_t *ctx,
                                                  uint16_t ordinal)
{
    uint16_t oldest, pos;
    if ((ctx == 0) || (ctx->changes == 0) || (ordinal >= ctx->change_count) ||
        (ctx->change_capacity == 0u)) {
        return 0;
    }
    oldest = (uint16_t)((ctx->change_head + ctx->change_capacity -
                         ctx->change_count) % ctx->change_capacity);
    pos = (uint16_t)((oldest + ordinal) % ctx->change_capacity);
    return &ctx->changes[pos];
}

/* ---- 小端读取 ---- */
static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64le(const uint8_t *p)
{
    uint64_t v = 0u;
    int i;
    for (i = 7; i >= 0; i--) {
        v = (v << 8) | (uint64_t)p[i];
    }
    return v;
}

#define BVPV_CMP(x, y) (((x) > (y)) ? 1 : (((x) < (y)) ? -1 : 0))

bool bvpv_value_is_finite(uint8_t type, const uint8_t *value)
{
    if (value == 0) return false;
    return type != BVPV_TYPE_FLOAT32 || (rd32le(value) & 0x7F800000u) != 0x7F800000u;
}

/* 类型感知值比较：a<b→负、a==b→0、a>b→正。供写范围校验（Min/Max）。
 * 各数值类型按 LE 字节解码后比较（整型按位宽、INT32/FIXED 按有符号、FLOAT32 按 float）；
 * TEXT/COMPOSITE 等不可比较类型返回 0（使范围校验自然通过）。 */
int bvpv_value_compare(uint8_t type, const uint8_t *a, const uint8_t *b)
{
    if ((a == 0) || (b == 0)) {
        return 0;
    }
    switch (type) {
    case BVPV_TYPE_UINT8:
        return BVPV_CMP(a[0], b[0]);
    case BVPV_TYPE_UINT16:
    case BVPV_TYPE_UFRAC16:
        return BVPV_CMP(rd16le(a), rd16le(b));
    case BVPV_TYPE_UINT32:
    case BVPV_TYPE_UFIXED16_16:
        return BVPV_CMP(rd32le(a), rd32le(b));
    case BVPV_TYPE_UINT64:
        return BVPV_CMP(rd64le(a), rd64le(b));
    case BVPV_TYPE_INT32:
    case BVPV_TYPE_FIXED16_16:
        return BVPV_CMP((int32_t)rd32le(a), (int32_t)rd32le(b));
    case BVPV_TYPE_FLOAT32: {
        uint32_t ua = rd32le(a);
        uint32_t ub = rd32le(b);
        float fa, fb;
        memcpy(&fa, &ua, sizeof(fa));
        memcpy(&fb, &ub, sizeof(fb));
        return BVPV_CMP(fa, fb);
    }
    default:
        return 0;   /* TEXT / COMPOSITE / UNDEFINED：不可比较 */
    }
}

const uint8_t *bvpv_param_default(const bvpv_param_t *p)
{
    if ((p->ext_flags & BVPV_EXT_HAS_DEFAULT) == 0u) {
        return 0;
    }
    /* 元素 >8B 的默认值放不进内联 def[8]，由生成器放到 def_ext[] 指向的数组 */
    return (p->def_ext != 0) ? p->def_ext : p->def;
}
