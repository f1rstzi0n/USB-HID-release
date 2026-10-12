/* 错误码与文本表（docs/PROTOCOL.md §9）。ERRTEXT_PULL 的数据源。
 * 文本用 ASCII 英文：与"设备只传 ASCII"一致，中文本地化留上位机。 */
#ifndef BVPV_ERRCODE_H
#define BVPV_ERRCODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BVPV_ERR_OK            0x00u   /* 成功 */
#define BVPV_ERR_NOT_FOUND     0x01u   /* 参数不存在 */
#define BVPV_ERR_ACCESS_DENIED 0x02u   /* 权限拒绝 */
#define BVPV_ERR_OUT_OF_RANGE  0x03u   /* 值越界 */
#define BVPV_ERR_TYPE_MISMATCH 0x04u   /* 类型不符 */
#define BVPV_ERR_INDEX_OOB     0x05u   /* 元素索引越界 */
#define BVPV_ERR_BUSY          0x06u   /* 设备忙碌 */
#define BVPV_ERR_CRC           0x07u   /* 校验失败 */
#define BVPV_ERR_LENGTH        0x08u   /* 长度错误 */
#define BVPV_ERR_NOT_SUPPORTED 0x09u   /* 服务不支持 */
#define BVPV_ERR_STATE         0x0Au   /* 运行状态不允许（含版本不兼容） */
#define BVPV_ERR_MSC_STATE     0x0Bu   /* MSC 更新状态错误 */
#define BVPV_ERR_SIGNATURE     0x0Cu   /* 发布签名失败 */
#define BVPV_ERR_TARGET        0x0Du   /* 固件目标不匹配 */
#define BVPV_ERR_FLASH         0x0Eu   /* 内部 Flash 失败 */
#define BVPV_ERR_IMAGE_DIGEST  0x0Fu   /* 应用摘要失败 */
#define BVPV_ERR_UNKNOWN       0xFFu   /* 未知错误 */

typedef struct {
    uint8_t     code;
    const char *text;   /* ASCII */
} bvpv_errtext_t;

extern const bvpv_errtext_t bvpv_errtext_table[];
extern const uint16_t       bvpv_errtext_count;

#ifdef __cplusplus
}
#endif

#endif /* BVPV_ERRCODE_H */
