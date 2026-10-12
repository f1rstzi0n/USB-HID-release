/* 服务分发器：把 L2 请求帧路由到对应服务处理器，产出 L2 应答帧。
 * 集中处理：SVC 回显、FLAGS 方向位置位、TID 回显、载荷首字节状态码、帧编码。
 * 服务码见 docs/PROTOCOL.md §5。 */
#ifndef BVPV_SERVICE_H
#define BVPV_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "bvpv_catalog.h"
#include "bvpv_usb_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 能力位（HELLO 设备名之后追加） ---- */
#define BVPV_CAP_DYNAMIC_ATTR 0x0001u
#define BVPV_CAP_FW_UPDATE    0x0004u
#define BVPV_CAP_CATALOG      0x0008u

/* ---- 服务码（§5） ---- */
#define BVPV_SVC_HELLO             0x01u
#define BVPV_SVC_TREE_PULL         0x02u
/* 0x03 保留：v3 使用带名称的 ATTR_SNAPSHOT_PULL。 */
#define BVPV_SVC_ATTR_EXT_READ     0x04u
#define BVPV_SVC_VALUE_READ        0x05u
#define BVPV_SVC_VALUE_WRITE       0x06u
#define BVPV_SVC_VALUE_READ_BATCH  0x07u
#define BVPV_SVC_STATE_READ        0x08u
#define BVPV_SVC_ERRTEXT_PULL      0x09u
#define BVPV_SVC_MSC_UPDATE_BEGIN  0x0Au
#define BVPV_SVC_MSC_UPDATE_BLOCK  0x0Bu
#define BVPV_SVC_MSC_UPDATE_FINISH 0x0Cu
#define BVPV_SVC_MSC_UPDATE_ABORT  0x0Du
#define BVPV_SVC_ENTER_BOOTLOADER  0x0Eu
#define BVPV_SVC_MSC_INFO          0x0Fu
#define BVPV_SVC_ATTR_CORE_READ    0x10u
#define BVPV_SVC_CHANGE_QUERY      0x11u
#define BVPV_SVC_ATTR_SNAPSHOT_PULL 0x12u
#define BVPV_SVC_FW_INFO           0x13u
#define BVPV_SVC_CATALOG_INFO   0x30u
#define BVPV_SVC_RUNTIME_SNAPSHOT_PULL 0x31u
#define BVPV_SVC_MAIN_UI_PULL   0x32u
#define BVPV_SVC_CATALOG_UPDATE_BEGIN 0x33u
#define BVPV_SVC_CATALOG_UPDATE_BLOCK 0x34u
#define BVPV_SVC_CATALOG_UPDATE_FINISH 0x35u
#define BVPV_SVC_CATALOG_UPDATE_ABORT 0x36u
#define BVPV_SVC_ECHO              0x7Fu

/* MCU 更新由宿主注入；未绑定时能力位为零，ENTER_BOOTLOADER 仍拒绝。 */
typedef struct {
    void (*request)(uint8_t, const uint8_t *, uint16_t, uint8_t *, uint16_t *);
    void (*reply_complete)(uint8_t, bool);
    void (*cancel)(void);
} bvpv_fw_backend_t;
void bvpv_service_set_fw_backend(const bvpv_fw_backend_t *);
bool bvpv_service_fw_supported(void);
void bvpv_service_reply_complete(uint8_t svc, bool delivered);
/* 可选内容 backend；响应含首字节状态码，最多 27 字节。false 表示延迟完成。 */
typedef struct {
    bool (*request)(uint8_t svc, const uint8_t *, uint16_t, uint8_t *, uint16_t *);
    bool (*poll)(uint8_t *, uint16_t *);
    void (*cancel)(void); /* USB 断线/总线复位时撤销未提交更新 */
    void (*task)(void);   /* 在协议任务中推进异步内容更新 */
} bvpv_msc_backend_t;
void bvpv_service_set_msc_backend(const bvpv_msc_backend_t *);
size_t bvpv_service_poll(uint8_t *out, uint16_t cap);
bool bvpv_service_pending(void);
void bvpv_service_cancel(void);

/* 外部目录响应含状态字节，所有缓冲区显式传入容量。 */
typedef struct {
    bool (*request)(uint8_t, const uint8_t *, uint16_t, uint8_t *, uint16_t *, uint16_t);
    bool (*poll)(uint8_t *, uint16_t *, uint16_t);
    void (*cancel)(void);
    void (*task)(void);
    bool (*ready)(void);
    bool (*maintenance)(void);
    void (*reply_complete)(uint8_t, bool);
    bool (*committed)(void);
} bvpv_catalog_backend_t;
void bvpv_service_set_catalog_backend(const bvpv_catalog_backend_t *);
bool bvpv_service_catalog_supported(void);
bool bvpv_service_catalog_committed(void);
void bvpv_service_task(void);

/* 注册服务上下文（目录 + 设备身份）。须在 dispatch 前调用。 */
void bvpv_service_init(bvpv_service_ctx_t *ctx);

/* 处理一个已解码的请求帧，向 out 写入完整应答帧，返回应答帧长。
 * 无法产生应答（参数非法/容量不足）返回 0。out 至少需 BVPV_MAX_FRAME 字节。 */
size_t bvpv_service_dispatch(const bvpv_frame_t *req, uint8_t *out, uint16_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* BVPV_SERVICE_H */
