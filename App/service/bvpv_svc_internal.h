/* 服务处理器内部接口：仅 bvpv_service.c 与各 svc_*.c 使用。
 * 约定：处理器把"状态码之后"的应答数据写入 resp（容量 cap），把数据字节数
 * 写入 *resp_len，返回状态码。状态字节由分发器统一放置在数据之前。 */
#ifndef BVPV_SVC_INTERNAL_H
#define BVPV_SVC_INTERNAL_H

#include <stdint.h>

#include "bvpv_catalog.h"
#include "bvpv_usb_frame.h"

/* 自描述服务（bvpv_svc_desc.c） */
uint8_t svc_hello(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                  uint8_t *resp, uint16_t *resp_len, uint16_t cap);
uint8_t svc_tree_pull(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                      uint8_t *resp, uint16_t *resp_len, uint16_t cap);
uint8_t svc_attr_ext_read(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                          uint8_t *resp, uint16_t *resp_len, uint16_t cap);
uint8_t svc_attr_core_read(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                           uint8_t *resp, uint16_t *resp_len, uint16_t cap);
uint8_t svc_attr_snapshot_pull(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                               uint8_t *resp, uint16_t *resp_len, uint16_t cap);
uint8_t svc_change_query(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                         uint8_t *resp, uint16_t *resp_len, uint16_t cap);
uint8_t svc_errtext_pull(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                         uint8_t *resp, uint16_t *resp_len, uint16_t cap);

uint8_t svc_runtime_snapshot_pull(const bvpv_service_ctx_t *, const bvpv_frame_t *,
                                   uint8_t *, uint16_t *, uint16_t);

/* 值服务（bvpv_svc_value.c） */
uint8_t svc_value_read(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                       uint8_t *resp, uint16_t *resp_len, uint16_t cap);
uint8_t svc_value_write(bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                        uint8_t *resp, uint16_t *resp_len, uint16_t cap);
uint8_t svc_value_read_batch(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                             uint8_t *resp, uint16_t *resp_len, uint16_t cap);
uint8_t svc_state_read(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                       uint8_t *resp, uint16_t *resp_len, uint16_t cap);
uint8_t svc_echo(const bvpv_service_ctx_t *ctx, const bvpv_frame_t *req,
                 uint8_t *resp, uint16_t *resp_len, uint16_t cap);

#endif /* BVPV_SVC_INTERNAL_H */
