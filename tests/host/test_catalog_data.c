#include "test_catalog_data.h"

#include <string.h>

/* 32 位常量 → 小端 4 字节（用于 FLOAT32 位模式的 Min/Max/Default 初始化） */
#define LE32(x) { (uint8_t)((x) & 0xFFu), (uint8_t)(((x) >> 8) & 0xFFu), \
                  (uint8_t)(((x) >> 16) & 0xFFu), (uint8_t)(((x) >> 24) & 0xFFu) }

/* IEEE754 位模式：0.0f=0x00000000  2.0f=0x40000000  100.0f=0x42C80000  10000.0f=0x461C4000 */

/* ---- 枚举表 ---- */static const bvpv_enum_item_t enum_CM[] = {
    { 0u, "Init" }, { 1u, "Homing" }, { 2u, "Position" }, { 3u, "Close" }, { 4u, "Open" },
};
static const bvpv_enum_item_t enum_SET9[] = {
    { 0u, "Local" }, { 1u, "Remote" }, { 2u, "RemoteLocked" },
};

/* ---- 元素 >8B 的默认值：内联 def[8] 装不下，走 def_ext 指向的独立数组 ----
 * 回归用：旧实现按 elem_size 从 def[8] 读 16 字节，会越界带出相邻结构体字段。 */
static const uint8_t defext_FirmwareVersion[16] = "SW110048.1.2.1";

/* ---- 运行时值缓冲（LE 字节） ---- */
static uint8_t val_ProductCode[8];
static uint8_t val_FirmwareVersion[16];
static uint8_t val_MACAddress[1];
static uint8_t val_CM[1];
static uint8_t val_ValveCommand[1];
static uint8_t val_SET0[4];
static uint8_t val_Display6[4];
static uint8_t val_SetValveDrive[4];
static uint8_t val_ReadValveVoltage[4];
static uint8_t val_AndromedaCMD[16];
static uint8_t val_SET9[4];
static uint8_t val_LearnBank1[16];   /* 4 × FLOAT32 */
static uint8_t val_FactoryReset[1];
static uint8_t val_CalibSecret[4];

/* ---- 参数表（按树前序，const 与固件目录模型一致） ---- */
const bvpv_param_t test_params[] = {
    { .addr = 0x010103u, .type = BVPV_TYPE_TEXT8, .access = BVPV_ACC_ROO,
      .flags = BVPV_FLAG_DEFAULT_VISIBLE, .depth = 0u, .elem_count = 1u,
      .name = "ProductCode", .value = val_ProductCode },

    { .addr = 0x640105u, .type = BVPV_TYPE_TEXT16, .access = BVPV_ACC_ROO,
      .flags = BVPV_FLAG_DEFAULT_VISIBLE, .depth = 0u, .elem_count = 1u,
      .name = "FirmwareVersion", .ext_flags = BVPV_EXT_HAS_DEFAULT,
      .def_ext = defext_FirmwareVersion,
      .value = val_FirmwareVersion },

    { .addr = 0x030101u, .type = BVPV_TYPE_UINT8, .access = BVPV_ACC_RWO,
      .flags = BVPV_FLAG_DEFAULT_VISIBLE, .depth = 0u, .elem_count = 1u,
      .name = "MACAddress", .ext_flags = BVPV_EXT_HAS_MIN | BVPV_EXT_HAS_MAX,
      .min = { 1u }, .max = { 95u },
      .value = val_MACAddress },

    { .addr = 0x690103u, .type = BVPV_TYPE_UINT8, .access = BVPV_ACC_RWA,
      .flags = BVPV_FLAG_DEFAULT_VISIBLE, .depth = 1u, .elem_count = 1u,
      .name = "CM", .enums = enum_CM, .enum_count = 5u,
      .value = val_CM },

    { .addr = 0x690104u, .type = BVPV_TYPE_UINT8, .access = BVPV_ACC_RWO,
      .flags = 0u, .depth = 1u, .elem_count = 1u,
      .name = "ValveCommand", .value = val_ValveCommand },

    { .addr = 0x7001C8u, .type = BVPV_TYPE_FLOAT32, .access = BVPV_ACC_RWA,
      .flags = BVPV_FLAG_DEFAULT_VISIBLE, .depth = 1u, .elem_count = 1u,
      .name = "SET0", .ext_flags = BVPV_EXT_HAS_MIN | BVPV_EXT_HAS_MAX | BVPV_EXT_HAS_DEFAULT,
      .min = LE32(0x00000000u), .max = LE32(0x42C80000u), .def = LE32(0x00000000u),
      .unit = "%", .value = val_SET0 },

    { .addr = 0x710106u, .type = BVPV_TYPE_FLOAT32, .access = BVPV_ACC_ROO,
      .flags = BVPV_FLAG_DEFAULT_VISIBLE, .depth = 1u, .elem_count = 1u,
      .name = "Display6", .unit = "mTorr", .value = val_Display6 },

    { .addr = 0x6A0190u, .type = BVPV_TYPE_FLOAT32, .access = BVPV_ACC_RWO,
      .flags = BVPV_FLAG_DEFAULT_VISIBLE, .depth = 1u, .elem_count = 1u,
      .name = "SetValveDrive", .ext_flags = BVPV_EXT_HAS_MIN | BVPV_EXT_HAS_MAX,
      .min = LE32(0x00000000u), .max = LE32(0x461C4000u),
      .value = val_SetValveDrive },

    { .addr = 0x6A0191u, .type = BVPV_TYPE_FLOAT32, .access = BVPV_ACC_ROO,
      .flags = BVPV_FLAG_DEFAULT_VISIBLE, .depth = 1u, .elem_count = 1u,
      .name = "ReadValveVoltage", .value = val_ReadValveVoltage },

    { .addr = 0x6801FEu, .type = BVPV_TYPE_COMPOSITE, .access = BVPV_ACC_ROO,
      .flags = BVPV_FLAG_DEFAULT_VISIBLE, .depth = 1u, .elem_count = 1u,
      .name = "AndromedaCMD", .elem_size = 16u,
      .value = val_AndromedaCMD },

    { .addr = 0x7001D1u, .type = BVPV_TYPE_FLOAT32, .access = BVPV_ACC_RWA,
      .flags = BVPV_FLAG_DEFAULT_VISIBLE, .depth = 2u, .elem_count = 1u,
      .name = "SET9", .ext_flags = BVPV_EXT_HAS_MIN | BVPV_EXT_HAS_MAX,
      .min = LE32(0x00000000u), .max = LE32(0x40000000u),
      .enums = enum_SET9, .enum_count = 3u,
      .value = val_SET9 },

    { .addr = 0x750100u, .type = BVPV_TYPE_FLOAT32, .access = BVPV_ACC_RWA,
      .flags = 0u, .depth = 2u, .elem_count = 4u,
      .name = "LearnBank1", .value = val_LearnBank1 },

    { .addr = 0x0B0101u, .type = BVPV_TYPE_UINT8, .access = BVPV_ACC_OWO,
      .flags = 0u, .depth = 2u, .elem_count = 1u,
      .name = "FactoryReset", .ext_flags = BVPV_EXT_HAS_MIN | BVPV_EXT_HAS_MAX,
      .min = {1u}, .max = {2u}, .value = val_FactoryReset },

    { .addr = 0x660102u, .type = BVPV_TYPE_FLOAT32, .access = BVPV_ACC_RWA,
      .flags = 0u, .depth = 2u, .elem_count = 1u,
      .name = "CalibSecret", .value = val_CalibSecret },
};

const uint16_t test_params_count =
    (uint16_t)(sizeof(test_params) / sizeof(test_params[0]));

/* 运行时状态（RAM，按参数索引对应 test_params） */
uint8_t test_states[sizeof(test_params) / sizeof(test_params[0])];
static bvpv_change_entry_t test_changes[4];
static bvpv_tree_record_t test_tree[2u + sizeof(test_params) / sizeof(test_params[0])];

void test_catalog_reset(void)
{
    uint16_t i;
    /* 依 flags bit0 复位运行时可见性（state 已移出参数表，存于 test_states） */
    for (i = 0u; i < test_params_count; i++) {
        test_states[i] =
            ((test_params[i].flags & BVPV_FLAG_DEFAULT_VISIBLE) != 0u) ? BVPV_STATE_VISIBLE : 0u;
    }
    /* 清零全部数值/复合缓冲 */
    memset(val_MACAddress, 0, sizeof(val_MACAddress));
    memset(val_CM, 0, sizeof(val_CM));
    memset(val_ValveCommand, 0, sizeof(val_ValveCommand));
    memset(val_SET0, 0, sizeof(val_SET0));
    memset(val_Display6, 0, sizeof(val_Display6));
    memset(val_SetValveDrive, 0, sizeof(val_SetValveDrive));
    memset(val_ReadValveVoltage, 0, sizeof(val_ReadValveVoltage));
    memset(val_AndromedaCMD, 0, sizeof(val_AndromedaCMD));
    memset(val_SET9, 0, sizeof(val_SET9));
    memset(val_LearnBank1, 0, sizeof(val_LearnBank1));
    memset(val_FactoryReset, 0, sizeof(val_FactoryReset));
    memset(val_CalibSecret, 0, sizeof(val_CalibSecret));
    /* 恢复文本参数 */
    memset(val_ProductCode, 0, sizeof(val_ProductCode));
    memcpy(val_ProductCode, "BV500", 5u);
    memset(val_FirmwareVersion, 0, sizeof(val_FirmwareVersion));
    memcpy(val_FirmwareVersion, "SW110048.1.2.1", 14u);
}

void test_catalog_make_ctx(bvpv_service_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    test_tree[0] = (bvpv_tree_record_t){BVPV_TREE_GROUP, 1u, 0u, "general", 0u};
    test_tree[1] = (bvpv_tree_record_t){BVPV_TREE_GROUP, 2u, 1u, "advanced", 0u};
    for (uint16_t i = 0; i < test_params_count; i++)
        test_tree[i + 2u] = (bvpv_tree_record_t){BVPV_TREE_PARAM, 0u,
            i < 10u ? 1u : 2u, 0, test_params[i].addr};
    ctx->tree = test_tree;
    ctx->tree_count = test_params_count + 2u;
    ctx->params      = test_params;
    ctx->states      = test_states;
    ctx->count       = test_params_count;
    ctx->device_type = 2u;   /* MED */
    ctx->device_name = "BV500-USB";
    ctx->changes = test_changes;
    ctx->change_capacity = (uint16_t)(sizeof(test_changes) / sizeof(test_changes[0]));
    bvpv_catalog_init_states(ctx);
}
