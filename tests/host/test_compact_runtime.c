/* 真实固件运行契约及 USB 权限执行；不链接旧完整描述表。 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bvpv_catalog_data.h"
#include "bvpv_errcode.h"
#include "bvpv_svc_internal.h"

static uint32_t digest_byte(uint32_t hash, uint8_t byte)
{
    return (hash ^ byte) * 16777619u;
}

static void actual_defaults(void)
{
    bvpv_service_ctx_t *ctx = bvpv_catalog_data_ctx();
    uint32_t digest = 2166136261u, total = 0u, previous = 0u;
    uint16_t i;
    bvpv_param_t p;
    assert(ctx->count > 0u && ctx->count <= 2300u && ctx->params == NULL && ctx->runtime != NULL);
    assert(ctx->tree == NULL && ctx->tree_count >= ctx->count);
    assert(!bvpv_catalog_get(ctx, ctx->count, &p));
    assert(!bvpv_catalog_get(ctx, 0u, NULL));
    assert(!bvpv_catalog_get(NULL, 0u, &p));
    for (i = 0u; i < ctx->count; i++) {
        uint16_t j, size;
        assert(bvpv_catalog_get(ctx, i, &p));
        assert(i == 0u || p.addr > previous);
        previous = p.addr;
        assert(bvpv_catalog_find(ctx, p.addr) == (int32_t)i);
        assert(p.name == NULL && p.unit == NULL && p.enums == NULL);
        assert(p.def_ext == NULL && !(p.ext_flags & BVPV_EXT_HAS_DEFAULT));
        assert(ctx->states[i] == ((p.flags & BVPV_FLAG_DEFAULT_VISIBLE) ? BVPV_STATE_VISIBLE : 0u));
        size = bvpv_param_elem_size(&p);
        size = (uint16_t)(size * p.elem_count);
        if (size == 0u) size = 1u; /* 原 Undefined RAM 哨兵 */
        digest = digest_byte(digest, (uint8_t)(p.addr >> 16));
        digest = digest_byte(digest, (uint8_t)(p.addr >> 8));
        digest = digest_byte(digest, (uint8_t)p.addr);
        digest = digest_byte(digest, (uint8_t)size);
        digest = digest_byte(digest, (uint8_t)(size >> 8));
        for (j = 0u; j < size; j++) digest = digest_byte(digest, p.value[j]);
        total += size;
    }
    assert(total > 0u && strlen(bvpv_catalog_data_app_identity()) > 0u);
    printf("current source initialization: %lu bytes, FNV1a=%08lX\n", (unsigned long)total, (unsigned long)digest);
    ctx->states[0] |= BVPV_STATE_PENDING;
    assert(bvpv_catalog_data_ctx()->states[0] & BVPV_STATE_PENDING);
    ctx->states[0] &= (uint8_t)~BVPV_STATE_PENDING;
}

typedef struct {
    uint8_t status, access;
    uint8_t min[8], max[8];
} policy_t;
static uint8_t policy(void *user, uint16_t index, const bvpv_param_t *param,
                       bvpv_effective_attr_t *attr)
{
    policy_t *p = (policy_t *)user;
    (void)index; (void)param;
    if (p->status) return p->status;
    attr->access = p->access;
    attr->min = p->min; attr->max = p->max;
    attr->ext_flags = BVPV_EXT_HAS_MIN | BVPV_EXT_HAS_MAX;
    attr->state = 0u; /* 目录隐藏不得清除 pending */
    return BVPV_ERR_OK;
}
static void put_float(uint8_t *bytes, float value)
{
    memcpy(bytes, &value, sizeof(value));
}
static uint8_t write_float(bvpv_service_ctx_t *ctx, uint32_t bits)
{
    uint8_t request[10] = {1u,1u,1u,0u,4u,0u};
    bvpv_frame_t frame;
    uint16_t length = 0u;
    request[6] = (uint8_t)bits; request[7] = (uint8_t)(bits >> 8);
    request[8] = (uint8_t)(bits >> 16); request[9] = (uint8_t)(bits >> 24);
    memset(&frame, 0, sizeof(frame)); frame.payload = request; frame.plen = sizeof(request);
    return svc_value_write(ctx, &frame, NULL, &length, 0u);
}
static void usb_policy(void)
{
    uint8_t value[4] = {0}, states[1] = {BVPV_STATE_VISIBLE};
    bvpv_runtime_param_t runtime = {
        .addr=0x010101u, .type=BVPV_TYPE_FLOAT32, .access=BVPV_ACC_RWA,
        .flags=BVPV_FLAG_DEFAULT_VISIBLE, .elem_count=1u, .elem_size=4u,
        .ext_flags=BVPV_EXT_HAS_MIN | BVPV_EXT_HAS_MAX, .value=value
    };
    policy_t restrictions = {0};
    bvpv_service_ctx_t ctx = {.runtime=&runtime,.count=1u,.states=states,
        .policy_provider=policy,.policy_provider_user=&restrictions};
    bvpv_effective_attr_t attr;
    bvpv_frame_t frame;
    uint8_t request[5] = {1u,1u,1u,1u,0u}, reply[8];
    uint16_t length = 0u;
    put_float(runtime.min, 0.0f); put_float(runtime.max, 100.0f);
    restrictions.access = BVPV_ACC_RWA;
    put_float(restrictions.min, 10.0f); put_float(restrictions.max, 90.0f);
    assert(bvpv_catalog_effective_attr(&ctx, 0u, &attr) == BVPV_ERR_OK);
    assert(attr.min == attr.min_bytes && attr.max == attr.max_bytes);
    assert(write_float(&ctx, 0x40A00000u) == BVPV_ERR_OUT_OF_RANGE); /* 5 */
    assert(write_float(&ctx, 0x42480000u) == BVPV_ERR_OK); /* 50 */
    assert(states[0] & BVPV_STATE_PENDING);
    assert(bvpv_catalog_effective_attr(&ctx, 0u, &attr) == BVPV_ERR_OK);
    assert(attr.state == BVPV_STATE_PENDING);
    assert(write_float(&ctx, 0x7FC00000u) == BVPV_ERR_OUT_OF_RANGE); /* NaN */
    assert(write_float(&ctx, 0x7F800000u) == BVPV_ERR_OUT_OF_RANGE); /* +Inf */
    assert(write_float(&ctx, 0xFF800000u) == BVPV_ERR_OUT_OF_RANGE); /* -Inf */
    { uint32_t observed; memcpy(&observed, value, sizeof(observed)); assert(observed == 0x42480000u); }
    restrictions.access = BVPV_ACC_ROO;
    assert(write_float(&ctx, 0x42480000u) == BVPV_ERR_ACCESS_DENIED);
    restrictions.access = BVPV_ACC_OWO;
    assert(write_float(&ctx, 0x42480000u) == BVPV_ERR_STATE);
    restrictions.access = BVPV_ACC_OOO;
    memset(&frame, 0, sizeof(frame)); frame.payload=request+1; frame.plen=4u;
    assert(svc_value_read(&ctx, &frame, reply, &length, sizeof(reply)) == BVPV_ERR_ACCESS_DENIED);
    restrictions.access = BVPV_ACC_RWA;
    put_float(restrictions.min, -1.0f);
    assert(bvpv_catalog_effective_attr(&ctx, 0u, &attr) == BVPV_ERR_STATE);
    put_float(restrictions.min, 91.0f);
    assert(bvpv_catalog_effective_attr(&ctx, 0u, &attr) == BVPV_ERR_STATE);
    restrictions.min[0]=0u; restrictions.min[1]=0u; restrictions.min[2]=0xC0u; restrictions.min[3]=0x7Fu;
    assert(bvpv_catalog_effective_attr(&ctx, 0u, &attr) == BVPV_ERR_STATE);
    put_float(restrictions.min, 10.0f);
    restrictions.status = BVPV_ERR_BUSY;
    assert(svc_value_read(&ctx, &frame, reply, &length, sizeof(reply)) == BVPV_ERR_BUSY);
    frame.payload=request; frame.plen=sizeof(request);
    assert(svc_value_read_batch(&ctx, &frame, reply, &length, sizeof(reply)) == BVPV_ERR_OK);
    assert(length==3u && reply[0]==BVPV_ERR_BUSY && reply[1]==0u && reply[2]==0u);
}
int main(void)
{
    actual_defaults(); usb_policy();
    puts("PASS compact runtime: all frozen defaults, sorted contract and USB policy");
    return 0;
}
