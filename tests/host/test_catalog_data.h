/* 合成测试目录：为服务层测试提供覆盖各类型/权限/结构的代表性参数集。
 * 仅用于 host 测试；真实目录将来由 DAT 生成器产出后注入。 */
#ifndef TEST_CATALOG_DATA_H
#define TEST_CATALOG_DATA_H

#include "bvpv_catalog.h"

extern const bvpv_param_t test_params[];
extern uint8_t           test_states[];   /* 运行时状态（RAM，按参数索引对应） */
extern const uint16_t    test_params_count;

/* 复位 value/state 到初始（测试隔离用） */
void test_catalog_reset(void);

/* 填充服务上下文（目录 + 设备身份） */
void test_catalog_make_ctx(bvpv_service_ctx_t *ctx);

#endif /* TEST_CATALOG_DATA_H */
