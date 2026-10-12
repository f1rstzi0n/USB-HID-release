#include "test_harness.h"

#include "bvpv_usb_frame.h"

/* 测试套件：L2 帧编解码。
 * 帧布局：[0]SVC [1]FLAGS [2..3]TID-LE [4..5]LEN-LE [6..]载荷 [尾2]CRC16。
 * 判定方式：组帧/解帧往返字段一致；对篡改/截断/超限帧必须解码拒绝。 */

/* 用例：最小帧（无载荷）往返。
 * 功能：验证 N=0 的帧能正确组帧并被解码还原。
 * 预期：帧长 = 8（仅帧头+CRC）；解码后 svc/flags/tid/plen 与组帧输入一致，plen=0。
 * 判据：encode 返回 8，decode 返回 true 且各字段相等。 */
static void test_shared_csharp_vector(void)
{
    static const uint8_t payload[] = {0x10u, 0x20u, 0x30u};
    static const uint8_t expected[] = {
        0x7Fu, 0x04u, 0x34u, 0x12u, 0x03u, 0x00u,
        0x10u, 0x20u, 0x30u, 0x1Bu, 0xB0u
    };
    uint8_t buf[sizeof(expected)];
    size_t n = bvpv_frame_encode(buf, 0x7Fu, BVPV_PROTO_VERSION, 0x1234u,
                                 payload, (uint16_t)sizeof(payload));

    TEST_ASSERT_EQ_U(sizeof(expected), n);
    TEST_ASSERT_MEMEQ(expected, buf, sizeof(expected));
}
static void test_roundtrip_min(void)
{
    uint8_t buf[BVPV_FRAME_OVERHEAD];
    bvpv_frame_t f;
    size_t n = bvpv_frame_encode(buf, 0x05u, BVPV_PROTO_VERSION, 0x1234u, 0, 0u);

    TEST_ASSERT_EQ_U(BVPV_FRAME_OVERHEAD, n);
    TEST_ASSERT(bvpv_frame_decode(buf, n, &f));
    TEST_ASSERT_EQ_U(0x05u, f.svc);
    TEST_ASSERT_EQ_U(BVPV_PROTO_VERSION, f.flags);
    TEST_ASSERT_EQ_U(0x1234u, f.tid);
    TEST_ASSERT_EQ_U(0u, f.plen);
}

/* 用例：最大帧往返。
 * 功能：验证载荷上限（4088B，帧 4096B）下组帧/解帧不溢出、内容不丢。
 * 预期：帧长 = 4096；解码后载荷与原载荷逐字节一致。
 * 判据：decode 成功，plen=4088，payload 与输入 memcmp 相等。 */
static void test_roundtrip_max(void)
{
    static uint8_t payload[BVPV_MAX_PAYLOAD];
    static uint8_t buf[BVPV_MAX_FRAME];
    bvpv_frame_t f;
    uint32_t i;
    size_t n;

    for (i = 0u; i < BVPV_MAX_PAYLOAD; i++) {
        payload[i] = (uint8_t)(i * 31u + 7u);
    }
    n = bvpv_frame_encode(buf, 0x0Bu, 0x82u, 0xBEEFu, payload, BVPV_MAX_PAYLOAD);
    TEST_ASSERT_EQ_U(BVPV_MAX_FRAME, n);
    TEST_ASSERT(bvpv_frame_decode(buf, n, &f));
    TEST_ASSERT_EQ_U(0x0Bu, f.svc);
    TEST_ASSERT_EQ_U(0xBEEFu, f.tid);
    TEST_ASSERT_EQ_U(BVPV_MAX_PAYLOAD, f.plen);
    TEST_ASSERT_MEMEQ(payload, f.payload, BVPV_MAX_PAYLOAD);
}

/* 用例：任意单 bit 翻转都被拒（CRC 检错强度）。
 * 功能：对完整帧的每一个 bit 翻转后尝试解码。
 * 预期：所有 96 个 bit 的翻转版本全部被解码拒绝（CRC16 可检出所有单 bit 错误）。
 * 判据：96 次翻转解码全部返回 false。 */
static void test_crc_bitflip_rejected(void)
{
    uint8_t payload[4] = {1u, 2u, 3u, 4u};
    uint8_t buf[32];
    uint8_t tmp[32];
    bvpv_frame_t f;
    size_t n = bvpv_frame_encode(buf, 0x05u, 0x00u, 0x0001u, payload, 4u);
    int bit;

    TEST_ASSERT(n > 0u);
    for (bit = 0; bit < (int)(n * 8u); bit++) {
        memcpy(tmp, buf, n);
        tmp[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        TEST_ASSERT(!bvpv_frame_decode(tmp, n, &f));
    }
}

/* 用例：LEN 字段篡改。
 * 功能：翻转 LEN 低字节使"声明长度"与实际帧长不符。
 * 预期：解码拒绝（长度一致性检查 + CRC 双重拦截）。
 * 判据：decode 返回 false。 */
static void test_len_tamper_rejected(void)
{
    uint8_t payload[4] = {1u, 2u, 3u, 4u};
    uint8_t buf[32];
    bvpv_frame_t f;
    size_t n = bvpv_frame_encode(buf, 0x05u, 0x00u, 0x0001u, payload, 4u);

    TEST_ASSERT(n > 0u);
    buf[4] ^= 0x01u;   /* 篡改 LEN 低字节 */
    TEST_ASSERT(!bvpv_frame_decode(buf, n, &f));
}

/* 用例：截断帧。
 * 功能：传入比组帧结果短的缓冲（少 1 字节 / 不足帧头+CRC）。
 * 预期：解码拒绝（长度越界或 LEN 不匹配）。
 * 判据：两种截断都返回 false。 */
static void test_truncated_rejected(void)
{
    uint8_t payload[4] = {1u, 2u, 3u, 4u};
    uint8_t buf[32];
    bvpv_frame_t f;
    size_t n = bvpv_frame_encode(buf, 0x05u, 0x00u, 0x0001u, payload, 4u);

    TEST_ASSERT(n > 0u);
    TEST_ASSERT(!bvpv_frame_decode(buf, n - 1u, &f));              /* 少 1 字节 */
    TEST_ASSERT(!bvpv_frame_decode(buf, BVPV_FRAME_OVERHEAD - 1u, &f)); /* 不足头+CRC */
}

/* 用例：组帧侧拒绝非法输入。
 * 功能：载荷超限（>4088）、或"有长度无数据"（plen>0 但 payload=NULL）。
 * 预期：encode 返回 0（拒绝组帧）。
 * 判据：两种非法输入 encode 都返回 0。 */
static void test_overlength_rejected(void)
{
    uint8_t buf[8];
    TEST_ASSERT_EQ_U(0u, bvpv_frame_encode(buf, 0x05u, 0x00u, 0x0001u,
                                           0, (uint16_t)(BVPV_MAX_PAYLOAD + 1u)));
    /* plen>0 但 payload 为 NULL 同样拒绝 */
    TEST_ASSERT_EQ_U(0u, bvpv_frame_encode(buf, 0x05u, 0x00u, 0x0001u, 0, 4u));
}

int main(void)
{
    TEST_RUN(test_shared_csharp_vector);
    TEST_RUN(test_roundtrip_min);
    TEST_RUN(test_roundtrip_max);
    TEST_RUN(test_crc_bitflip_rejected);
    TEST_RUN(test_len_tamper_rejected);
    TEST_RUN(test_truncated_rejected);
    TEST_RUN(test_overlength_rejected);
    TEST_MAIN_END();
}
