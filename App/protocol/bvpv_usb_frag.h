/* L1 分片层：把 L2 帧切片进 64B HID 报表（发送），并从报表重组出完整帧（接收）。
 * 分片头（docs/PROTOCOL.md §3）：byte0=[SOF:1][EOF:1][SEQ:6]，byte1=MSGID。
 * 报表布局：[0]=ReportID(0) [1]=byte0 [2]=byte1 [3..63]=载荷(61B)。
 *
 * 接收为**长度感知**：从帧头 LEN 字段（帧偏移 4..5）获知消息真实长度，
 * 末片的零填充字节不计入。这是同一协议栈内 L1 对 L2 布局的合理依赖，
 * 使最大 4096B 帧（需 68 片、末片仅 9 有效字节）能被正确重组。 */
#ifndef BVPV_USB_FRAG_H
#define BVPV_USB_FRAG_H

#include <stdbool.h>
#include <stdint.h>

#include "bvpv_usb_frame.h"   /* 复用 BVPV_MAX_FRAME / BVPV_FRAME_OVERHEAD / BVPV_MAX_PAYLOAD */

#ifdef __cplusplus
extern "C" {
#endif

#define BVPV_REPORT_LEN        64u
#define BVPV_FRAG_HDR          2u
#define BVPV_FRAG_PAYLOAD      (BVPV_REPORT_LEN - 1u - BVPV_FRAG_HDR)  /* 61 */
#define BVPV_SEQ_MOD           64u
#define BVPV_REASM_TIMEOUT_MS  200u

/* 分片头位掩码（byte0） */
#define BVPV_FRAG_SOF  0x80u
#define BVPV_FRAG_EOF  0x40u
#define BVPV_FRAG_SEQ_MASK 0x3Fu

/* ---------------- 发送（分片） ---------------- */

typedef struct {
    uint8_t msgid;   /* 本发送方向下一条消息的 MSGID */
} bvpv_frag_tx_t;

void bvpv_frag_tx_init(bvpv_frag_tx_t *tx);

/* 把 msg[0..len) 切成 64B 报表，逐片经 emit 回调发出；全部发出返回 true。
 * len 须在 [BVPV_FRAME_OVERHEAD, BVPV_MAX_FRAME]（一条完整 L2 帧）。
 * 发完一条消息后 tx->msgid 自增（模 256）。 */
bool bvpv_frag_tx_send(bvpv_frag_tx_t *tx, const uint8_t *msg, uint16_t len,
                       void (*emit)(const uint8_t *report64, void *ctx), void *ctx);

/* ---------------- 接收（重组） ---------------- */

typedef struct {
    uint8_t  buf[BVPV_MAX_FRAME]; /* 重组缓冲；组齐后消息在 buf[0..len) */
    uint16_t len;                 /* 已累积字节数（完成时=消息全长） */
    uint16_t expected_total;      /* 从帧头 LEN 推算的消息全长；0=尚未获知 */
    uint8_t  expected_seq;        /* 期望的下一片 SEQ（模 64） */
    uint8_t  msgid;               /* 正在重组的消息 MSGID */
    uint8_t  active;              /* 正在重组中 */
    uint32_t start_ms;            /* 首片时间戳（超时基准） */
} bvpv_frag_rx_t;

void bvpv_frag_rx_init(bvpv_frag_rx_t *rx);

/* 喂入一张 64B 报表。组齐一条完整消息时返回 true 并把长度写入 *msg_len_out，
 * 消息内容在 rx->buf（调用方须在下次 feed 前取走）。否则返回 false。
 * 内置四层防错（SEQ 连续 / SOF 再同步 / MSGID 比对 / 超时再入）。 */
bool bvpv_frag_rx_feed(bvpv_frag_rx_t *rx, const uint8_t *report64, uint32_t now_ms,
                       uint16_t *msg_len_out);

/* 周期性调用（建议每 1ms tick）：首片后超过 200ms 未收齐则丢弃半成品。 */
void bvpv_frag_rx_check_timeout(bvpv_frag_rx_t *rx, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* BVPV_USB_FRAG_H */
