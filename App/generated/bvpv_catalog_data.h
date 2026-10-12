/* 真实参数目录入口（由 tools/catalog/catalog_gen 从 catalog/SW110048.catalog.csv 生成）。
 * 内部 Flash 仅运行契约，值/状态在 RAM；自描述从外部目录读取。 */
#ifndef BVPV_CATALOG_DATA_H
#define BVPV_CATALOG_DATA_H

#include "bvpv_catalog.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 返回地址升序运行契约（静态存储，覆盖整个运行期）。
 * 首次调用初始化状态；USB 重连和目录重载不恢复默认值。 */
bvpv_service_ctx_t *bvpv_catalog_data_ctx(void);
const char *bvpv_catalog_data_app_identity(void);

#ifdef __cplusplus
}
#endif

#endif /* BVPV_CATALOG_DATA_H */
