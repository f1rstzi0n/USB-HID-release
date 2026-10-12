/* 参数目录模型：自描述与值服务的数据基座（docs/PROTOCOL.md §6、§7）。
 * 测试可注入完整合成目录；固件注入 compact 运行契约与外部策略提供器。
 * 值以原始 LE 字节存储，服务层只做尺寸/范围校验，不解释业务含义。 */
#ifndef BVPV_CATALOG_H
#define BVPV_CATALOG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 数据类型（§6.2） ---- */
#define BVPV_TYPE_UNDEFINED   0x00u
#define BVPV_TYPE_UINT8       0x01u
#define BVPV_TYPE_UINT16      0x02u
#define BVPV_TYPE_UINT32      0x03u
#define BVPV_TYPE_UINT64      0x04u
#define BVPV_TYPE_INT32       0x05u
#define BVPV_TYPE_FLOAT32     0x06u
#define BVPV_TYPE_FIXED16_16  0x07u
#define BVPV_TYPE_UFIXED16_16 0x08u
#define BVPV_TYPE_UFRAC16     0x09u
#define BVPV_TYPE_TEXT8       0x0Au
#define BVPV_TYPE_TEXT16      0x0Bu
#define BVPV_TYPE_TEXT32      0x0Cu
#define BVPV_TYPE_COMPOSITE   0x0Du

/* ---- 权限（§6.3） ---- */
#define BVPV_ACC_OOO 0x00u
#define BVPV_ACC_RWA 0x01u
#define BVPV_ACC_ROO 0x02u
#define BVPV_ACC_RWO 0x03u
#define BVPV_ACC_OWO 0x04u

/* ---- flags 位（§7.4 核心属性标志字节） ---- */
#define BVPV_FLAG_DEFAULT_VISIBLE 0x01u   /* bit0=初始默认显示 */

/* ---- ext_flags 位（扩展属性存在性） ---- */
#define BVPV_EXT_HAS_MIN     0x01u
#define BVPV_EXT_HAS_MAX     0x02u
#define BVPV_EXT_HAS_DEFAULT 0x04u

/* ---- state 位（§7.9 运行时状态） ---- */
#define BVPV_STATE_VISIBLE 0x01u   /* bit0 运行时可见 */
#define BVPV_STATE_PENDING 0x02u   /* bit1 EEPROM-pending */

/* ---- 动态属性变化类型（CHANGE_QUERY） ---- */
#define BVPV_CHANGE_STATE 0x01u
#define BVPV_CHANGE_CORE  0x02u
#define BVPV_CHANGE_EXT   0x04u

typedef struct {
    uint32_t revision;
    uint32_t addr;
    uint8_t  mask;
} bvpv_change_entry_t;

/* 枚举项 */
typedef struct {
    uint32_t    value;
    const char *text;   /* UTF-8 */
} bvpv_enum_item_t;

/* 参数条目元数据（const，可下沉 Flash；值经 value 指针指向 RAM 缓冲）。
 * 运行时状态（可见性/pending）不在此结构，而在 ctx->states[]（RAM，按索引对应）。 */
typedef struct {
    uint32_t addr;          /* 24 位：类<<16 | 实例<<8 | 属性 */
    uint8_t  type;
    uint8_t  access;
    uint8_t  flags;         /* BVPV_FLAG_* */
    uint8_t  depth;         /* 树层级深度 */
    uint16_t elem_count;    /* 标量=1 */
    const char *name;       /* ASCII */
    uint8_t  ext_flags;     /* BVPV_EXT_HAS_* */
    uint8_t  min[8];        /* LE，按 type 解释（仅数值型 ≤8B；TEXT/COMPOSITE 不发射） */
    uint8_t  max[8];
    uint8_t  def[8];        /* 默认值（元素 ≤8B 时内联在此） */
    const uint8_t *def_ext; /* 默认值（元素 >8B 时指向完整 elem_size 字节）；否则 NULL */
    const char *unit;                       /* 可空 */
    const bvpv_enum_item_t *enums;          /* 可空 */
    uint8_t  enum_count;
    uint16_t elem_size;     /* 单元素字节数：仅 COMPOSITE 使用，其余按 type 推导 */
    uint8_t *value;         /* 运行时值缓冲（RAM）：elem_count × elem_size 字节，LE */
} bvpv_param_t;

/* 固件运行契约：内部 Flash 只保留执行所需字段，字符串和树由外置目录提供。 */
typedef struct {
    uint32_t addr;
    uint8_t *value;
    uint8_t min[8];
    uint8_t max[8];
    uint16_t elem_count;
    uint16_t elem_size;
    uint8_t type;
    uint8_t access;
    uint8_t flags;
    uint8_t ext_flags;      /* 只含 MIN/MAX，不发布初始化值 */
} bvpv_runtime_param_t;
typedef char bvpv_runtime_size_check[(sizeof(void *) != 4u ||
                                     sizeof(bvpv_runtime_param_t) == 32u) ? 1 : -1];

typedef struct {
    uint8_t access;
    uint8_t flags;
    uint8_t state;
    uint8_t ext_flags;
    const uint8_t *min;
    const uint8_t *max;
    const uint8_t *def;
    const char *unit;
    const bvpv_enum_item_t *enums;
    uint8_t enum_count;
    uint8_t min_bytes[8];   /* 提供器返回后仍有效的范围字节 */
    uint8_t max_bytes[8];
} bvpv_effective_attr_t;

typedef void (*bvpv_attr_provider_t)(void *user, uint16_t index,
                                     const bvpv_param_t *param,
                                     bvpv_effective_attr_t *attr);

/* 外置目录在挂载时准备的 RAM 策略；正常读写不得在此访问 Flash。 */
typedef uint8_t (*bvpv_policy_provider_t)(void *user, uint16_t index,
                                         const bvpv_param_t *param,
                                         bvpv_effective_attr_t *attr);

/* OWO 命令由宿主执行；未绑定时服务拒绝执行。 */
typedef uint8_t (*bvpv_command_handler_t)(void *user, uint32_t addr,
                                           uint8_t element, const uint8_t *value,
                                           uint16_t length);

/* v2 参数树记录：根节点 0 隐含存在，父分组先于子记录出现。 */
#define BVPV_TREE_GROUP 0x01u
#define BVPV_TREE_PARAM 0x02u
typedef struct {
    uint8_t kind;
    uint16_t node_id;
    uint16_t parent_id;
    const char *key;       /* GROUP：可打印 ASCII key */
    uint32_t addr;        /* PARAM：24 位参数地址 */
} bvpv_tree_record_t;
/* 服务上下文：目录元数据 + 运行时状态 + 设备身份（HELLO 用） */
typedef struct {
    const bvpv_param_t *params;   /* 目录元数据表（const，可在 Flash），树通过地址关联 */
    const bvpv_runtime_param_t *runtime; /* 按地址升序；与 params 二选一 */
    const uint16_t *addr_order;    /* 按地址排序的原表索引；可空，测试小目录回退线性查找 */
    uint8_t      *states;         /* 运行时状态（RAM，count 项），与默认隐藏 flags 独立 */
    uint16_t      count;
    uint8_t       device_type;    /* 1=LOW, 2=MED, 3=HIGH（将来读 PD11/PB13/PB12） */
    const char   *device_name;    /* ASCII */
    bvpv_attr_provider_t attr_provider;
    void         *attr_provider_user;
    bvpv_policy_provider_t policy_provider;
    void         *policy_provider_user;
    bvpv_command_handler_t command_handler;
    void         *command_user;
    bvpv_change_entry_t *changes;
    uint16_t      change_capacity;
    uint16_t      change_head;
    uint16_t      change_count;
    uint32_t      revision;
    const bvpv_tree_record_t *tree;
    uint16_t      tree_count;
} bvpv_service_ctx_t;

/* 类型默认尺寸：定长类型返回字节数；COMPOSITE/UNDEFINED 返回 0（由 elem_size 决定） */
uint16_t bvpv_type_size(uint8_t type);

/* 单元素字节尺寸：COMPOSITE 取 elem_size，其余取 type 尺寸 */
uint16_t bvpv_param_elem_size(const bvpv_param_t *p);

/* 权限判定 */
bool bvpv_access_readable(uint8_t access);
bool bvpv_access_writable(uint8_t access);
bool bvpv_access_eeprom_persistent(uint8_t access);

/* 地址合成：类/实例/属性 → 24 位 */
uint32_t bvpv_addr(uint8_t cls, uint8_t inst, uint8_t attr);

/* 按地址查找，返回表内索引；未找到返回 -1 */
int32_t bvpv_catalog_find(const bvpv_service_ctx_t *ctx, uint32_t addr);

/* 将单项运行契约展开到调用方缓冲；不持有整张描述表。 */
bool bvpv_catalog_get(const bvpv_service_ctx_t *ctx, uint16_t index,
                      bvpv_param_t *out);

/* 初始化运行时状态：所有参数初始运行时可见；默认隐藏独立由 flags 决定。
 * 须在首次使用目录前调用一次（如 bvpv_service_init 之后）。 */
void bvpv_catalog_init_states(const bvpv_service_ctx_t *ctx);

uint8_t bvpv_catalog_effective_attr(const bvpv_service_ctx_t *ctx, uint16_t index,
                                    bvpv_effective_attr_t *attr);
uint32_t bvpv_catalog_set_state(bvpv_service_ctx_t *ctx, uint16_t index,
                                uint8_t state);
uint32_t bvpv_catalog_note_change(bvpv_service_ctx_t *ctx, uint16_t index,
                                  uint8_t mask);
const bvpv_change_entry_t *bvpv_catalog_change_at(const bvpv_service_ctx_t *ctx,
                                                  uint16_t ordinal);

/* 类型感知比较：a<b→负，a==b→0，a>b→正；不可比较类型（TEXT/COMPOSITE）返回 0 */
int bvpv_value_compare(uint8_t type, const uint8_t *a, const uint8_t *b);
bool bvpv_value_is_finite(uint8_t type, const uint8_t *value);

/* 默认值字节指针（共 bvpv_param_elem_size(p) 字节，LE）：
 * 元素 ≤8B → 指向内联 def[]；元素 >8B（TEXT16/32、COMPOSITE）→ 指向 def_ext[]。
 * 无默认值（ext_flags 无 BVPV_EXT_HAS_DEFAULT）时返回 NULL。
 * ⚠️ 取字节数一律用 bvpv_param_elem_size(p)，不可用 sizeof(p->def)——后者只有 8。
 * （历史缺陷：曾直接 memcpy(p->def, elem_size) 发 TLV，对 16/32 字节元素越界读相邻结构体字段。） */
const uint8_t *bvpv_param_default(const bvpv_param_t *p);

#ifdef __cplusplus
}
#endif

#endif /* BVPV_CATALOG_H */
