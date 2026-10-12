#include "bvpv_usb_frame.h"

#include <string.h>

#include "bvpv_usb_crc16.h"

/* 组帧：填帧头（SVC/FLAGS/TID/LEN 小端）→ 拷载荷 → 对"SVC..载荷末"算 CRC16 追加尾部。
 * 返回帧长（8+plen）；参数非法（越界/有长度无数据）返回 0。 */
size_t bvpv_frame_encode(uint8_t *out, uint8_t svc, uint8_t flags, uint16_t tid,
                         const uint8_t *payload, uint16_t plen)
{
    uint16_t crc;

    if (out == 0) {
        return 0u;
    }
    if (plen > BVPV_MAX_PAYLOAD) {
        return 0u;
    }
    if ((plen > 0u) && (payload == 0)) {
        return 0u;
    }

    out[0] = svc;
    out[1] = flags;
    out[2] = (uint8_t)(tid & 0xFFu);
    out[3] = (uint8_t)(tid >> 8);
    out[4] = (uint8_t)(plen & 0xFFu);
    out[5] = (uint8_t)(plen >> 8);
    if (plen > 0u) {
        memcpy(out + BVPV_FRAME_HDR_LEN, payload, plen);
    }

    crc = bvpv_crc16(out, BVPV_FRAME_HDR_LEN + plen);
    out[BVPV_FRAME_HDR_LEN + plen]     = (uint8_t)(crc & 0xFFu);
    out[BVPV_FRAME_HDR_LEN + plen + 1u] = (uint8_t)(crc >> 8);

    return BVPV_FRAME_OVERHEAD + plen;
}

/* 解帧并校验：长度边界 → LEN 与实际帧长严格一致（挡篡改/截断）→ CRC16 校验。
 * 全部通过才填 out 并返回 true；任一不过返回 false（调用方不应答，主机走超时）。 */
bool bvpv_frame_decode(const uint8_t *frame, size_t len, bvpv_frame_t *out)
{
    uint16_t plen;
    uint16_t crc_stored;
    uint16_t crc_calc;

    if ((frame == 0) || (out == 0)) {
        return false;
    }
    if ((len < BVPV_FRAME_OVERHEAD) || (len > BVPV_MAX_FRAME)) {
        return false;
    }

    plen = (uint16_t)((uint16_t)frame[4] | ((uint16_t)frame[5] << 8));
    if (plen > BVPV_MAX_PAYLOAD) {
        return false;
    }
    /* LEN 字段必须与实际帧长严格一致（同时挡下篡改与截断） */
    if ((size_t)plen + BVPV_FRAME_OVERHEAD != len) {
        return false;
    }

    crc_stored = (uint16_t)((uint16_t)frame[BVPV_FRAME_HDR_LEN + plen] |
                            ((uint16_t)frame[BVPV_FRAME_HDR_LEN + plen + 1u] << 8));
    crc_calc = bvpv_crc16(frame, BVPV_FRAME_HDR_LEN + plen);
    if (crc_stored != crc_calc) {
        return false;
    }

    out->svc   = frame[0];
    out->flags = frame[1];
    out->tid   = (uint16_t)((uint16_t)frame[2] | ((uint16_t)frame[3] << 8));
    out->plen  = plen;
    out->payload = (plen > 0u) ? (frame + BVPV_FRAME_HDR_LEN) : (const uint8_t *)0;
    return true;
}
