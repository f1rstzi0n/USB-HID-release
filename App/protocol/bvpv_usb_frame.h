/* L2 帧编解码。帧布局（docs/PROTOCOL.md §4）：
 *   [0]SVC [1]FLAGS [2..3]TID-LE [4..5]LEN-LE [6..]载荷 [尾2]CRC16-LE
 * 帧总长 = 8 + N，最大 4096B。全部多字节字段小端。 */
#ifndef BVPV_USB_FRAME_H
#define BVPV_USB_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BVPV_FRAME_HDR_LEN  6u                                    /* SVC+FLAGS+TID+LEN */
#define BVPV_FRAME_CRC_LEN  2u                                    /* 尾部 CRC16 */
#define BVPV_FRAME_OVERHEAD (BVPV_FRAME_HDR_LEN + BVPV_FRAME_CRC_LEN) /* 8 */
#define BVPV_MAX_PAYLOAD    4088u
#define BVPV_MAX_FRAME      (BVPV_FRAME_OVERHEAD + BVPV_MAX_PAYLOAD)  /* 4096 */

/* FLAGS 位定义 */
#define BVPV_FLAGS_DIR_RESPONSE 0x80u   /* bit7：0=请求，1=应答 */
#define BVPV_FLAGS_VERSION_MASK 0x07u   /* bit2..0：协议版本，当前=4 */
#define BVPV_PROTO_VERSION      0x04u

/* 解码后的帧视图（payload 指向原帧内部，不拷贝） */
typedef struct {
    uint8_t  svc;
    uint8_t  flags;
    uint16_t tid;
    const uint8_t *payload;   /* plen=0 时为 NULL */
    uint16_t plen;
} bvpv_frame_t;

/* 组帧：向 out 写入完整帧，返回帧长（8+plen）；plen 超限或参数非法返回 0。
 * out 至少需 8+plen 字节。plen>0 时 payload 不可为 NULL。 */
size_t bvpv_frame_encode(uint8_t *out, uint8_t svc, uint8_t flags, uint16_t tid,
                         const uint8_t *payload, uint16_t plen);

/* 解帧并校验：长度边界、LEN 与实际帧长一致性、CRC16。全部通过返回 true 并填 out。 */
bool bvpv_frame_decode(const uint8_t *frame, size_t len, bvpv_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BVPV_USB_FRAME_H */
