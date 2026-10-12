#include "bvpv_usb_frag.h"

#include <string.h>

/* ===================== 发送（分片） ===================== */

/* 初始化发送侧：MSGID 从 0 开始（每条消息发完自增）。 */
void bvpv_frag_tx_init(bvpv_frag_tx_t *tx)
{
    if (tx != 0) {
        tx->msgid = 0u;
    }
}

/* 把一条完整 L2 帧切成 64B 报表逐片经 emit 发出。
 * 每片：[ReportID=0][byte0=SOF|EOF|SEQ][byte1=MSGID][61B 载荷]，末片不足零填充。
 * 发完全部片后 MSGID 自增。len 须在 [8, 4096]（一条完整 L2 帧），否则返回 false。 */
bool bvpv_frag_tx_send(bvpv_frag_tx_t *tx, const uint8_t *msg, uint16_t len,
                       void (*emit)(const uint8_t *report64, void *ctx), void *ctx)
{
    uint16_t nfrag;
    uint16_t i;
    uint8_t  report[BVPV_REPORT_LEN];

    if ((tx == 0) || (emit == 0) || (msg == 0)) {
        return false;
    }
    if ((len < BVPV_FRAME_OVERHEAD) || (len > BVPV_MAX_FRAME)) {
        return false;
    }

    nfrag = (uint16_t)((len + BVPV_FRAG_PAYLOAD - 1u) / BVPV_FRAG_PAYLOAD);

    for (i = 0; i < nfrag; i++) {
        uint16_t offset = (uint16_t)(i * BVPV_FRAG_PAYLOAD);
        uint16_t chunk  = (uint16_t)(len - offset);
        uint8_t  b0     = (uint8_t)(i % BVPV_SEQ_MOD);

        if (chunk > BVPV_FRAG_PAYLOAD) {
            chunk = BVPV_FRAG_PAYLOAD;
        }
        if (i == 0u) {
            b0 |= BVPV_FRAG_SOF;
        }
        if (i == (uint16_t)(nfrag - 1u)) {
            b0 |= BVPV_FRAG_EOF;
        }

        report[0] = 0x00u;      /* Report ID，恒 0 */
        report[1] = b0;
        report[2] = tx->msgid;
        if (chunk > 0u) {
            memcpy(report + 3, msg + offset, chunk);
        }
        /* 末片有效载荷不足 61B 时零填充，保证报表内容确定（接收端按 LEN 忽略填充） */
        if (chunk < BVPV_FRAG_PAYLOAD) {
            memset(report + 3 + chunk, 0, BVPV_FRAG_PAYLOAD - chunk);
        }
        emit(report, ctx);
    }

    tx->msgid = (uint8_t)(tx->msgid + 1u);
    return true;
}

/* ===================== 接收（重组） ===================== */

/* 初始化接收侧：清空重组状态（无消息在进行）。 */
void bvpv_frag_rx_init(bvpv_frag_rx_t *rx)
{
    if (rx != 0) {
        memset(rx, 0, sizeof(*rx));
    }
}

/* 复位重组状态（丢弃半成品）。buf 内容不清：无敏感信息，且完成后调用方可能仍在读。 */
static void bvpv_rx_reset(bvpv_frag_rx_t *rx)
{
    rx->len            = 0u;
    rx->expected_total = 0u;
    rx->expected_seq   = 0u;
    rx->active         = 0u;
    /* buf 内容不清零：无敏感信息，且完成后调用方可能仍在读取 */
}

/* 周期超时检查（建议每 1ms tick 调用一次）：首片后超 200ms 未收齐则丢弃半成品（规则4）。
 * 用无符号减法比较，天然容忍 32 位毫秒计数回绕。 */
void bvpv_frag_rx_check_timeout(bvpv_frag_rx_t *rx, uint32_t now_ms)
{
    if (rx == 0) {
        return;
    }
    /* 规则4：首片后超时未收齐，丢弃半成品。无符号减法天然处理 32 位回绕。 */
    if ((rx->active != 0u) &&
        ((uint32_t)(now_ms - rx->start_ms) > BVPV_REASM_TIMEOUT_MS)) {
        bvpv_rx_reset(rx);
    }
}

/* 喂入一张 64B 报表，组齐一条完整消息时返回 true 并把长度写入 *msg_len_out
 * （消息在 rx->buf，调用方须在下次 feed 前取走）。否则返回 false。
 * 核心流程：内置超时再入 → SOF 重启/SEQ 连续/MSGID 比对（四层防错）→ 长度感知追加
 * （从帧头 LEN 推算真实全长，末片填充不计入）→ 长度凑满且为 EOF 才判完成。 */
bool bvpv_frag_rx_feed(bvpv_frag_rx_t *rx, const uint8_t *report64, uint32_t now_ms,
                       uint16_t *msg_len_out)
{
    uint8_t b0;
    uint8_t msgid;
    bool    sof;
    bool    eof;
    uint8_t seq;
    const uint8_t *payload;
    uint16_t want;

    if ((rx == 0) || (report64 == 0)) {
        return false;
    }

    /* 内置超时再入：即便调用方忘了 check_timeout，过期半成品也不会被续接 */
    bvpv_frag_rx_check_timeout(rx, now_ms);

    b0      = report64[1];
    msgid   = report64[2];
    sof     = (b0 & BVPV_FRAG_SOF) != 0u;
    eof     = (b0 & BVPV_FRAG_EOF) != 0u;
    seq     = (uint8_t)(b0 & BVPV_FRAG_SEQ_MASK);
    payload = report64 + 3;

    if (sof) {
        /* 规则2：新 SOF 总是重启重组（丢弃任何半成品） */
        bvpv_rx_reset(rx);
        if (seq != 0u) {          /* SOF 必须 SEQ=0 */
            return false;
        }
        rx->active  = 1u;
        rx->msgid   = msgid;
        rx->start_ms = now_ms;
    } else {
        if (rx->active == 0u) {   /* 无消息在进行 */
            return false;
        }
        if (msgid != rx->msgid) { /* 规则3：MSGID 不匹配 */
            bvpv_rx_reset(rx);
            return false;
        }
        if (seq != rx->expected_seq) { /* 规则1：SEQ 连续性（防 1+3 错拼） */
            bvpv_rx_reset(rx);
            return false;
        }
    }

    /* 追加有效载荷：长度已知时只取到 expected_total，绝不越出 buf */
    want = BVPV_FRAG_PAYLOAD;
    if (rx->expected_total != 0u) {
        uint16_t room = (uint16_t)(rx->expected_total - rx->len);
        if (want > room) {
            want = room;
        }
    }
    {
        uint16_t cap = (uint16_t)(BVPV_MAX_FRAME - rx->len);
        if (want > cap) {
            want = cap;
        }
    }
    if (want > 0u) {
        memcpy(rx->buf + rx->len, payload, want);
        rx->len = (uint16_t)(rx->len + want);
    }
    rx->expected_seq = (uint8_t)((rx->expected_seq + 1u) % BVPV_SEQ_MOD);

    /* 一旦凑齐帧头（≥6B），从 LEN 字段（偏移 4..5）推算消息真实全长 */
    if ((rx->expected_total == 0u) && (rx->len >= BVPV_FRAME_HDR_LEN)) {
        uint16_t plen  = (uint16_t)((uint16_t)rx->buf[4] | ((uint16_t)rx->buf[5] << 8));
        uint16_t total = (uint16_t)(BVPV_FRAME_OVERHEAD + plen);
        if ((plen > BVPV_MAX_PAYLOAD) || (total > BVPV_MAX_FRAME)) {
            bvpv_rx_reset(rx);    /* 非法长度字段 */
            return false;
        }
        rx->expected_total = total;
        if (rx->len > rx->expected_total) {
            /* 短消息装在单片里：丢弃多取的填充字节 */
            rx->len = rx->expected_total;
        }
    }

    /* 完成判定：长度凑满且本片须为 EOF */
    if ((rx->expected_total != 0u) && (rx->len == rx->expected_total)) {
        if (!eof) {               /* 长度已满却非末片：发送方违规 */
            bvpv_rx_reset(rx);
            return false;
        }
        if (msg_len_out != 0) {
            *msg_len_out = rx->len;
        }
        rx->active = 0u;          /* 保留 buf/len 供调用方读取，下次 SOF 再复位 */
        return true;
    }

    if (eof) {                    /* 长度未满却收到末片：消息被截断 */
        bvpv_rx_reset(rx);
        return false;
    }
    return false;
}
