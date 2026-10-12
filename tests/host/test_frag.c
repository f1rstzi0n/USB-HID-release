#include "test_harness.h"

#include "bvpv_usb_frag.h"
#include "bvpv_usb_frame.h"

/* 测试套件：L1 分片/重组——协议最易出错的部分，重点验证"四层防错"。
 * 分片头 byte0=[SOF:1][EOF:1][SEQ:6]，byte1=MSGID；64B 报表含 61B 载荷。
 * 判定方式：分片→重组往返字节一致；各类错误注入必须"宁可丢弃、绝不拼错"。 */

/* ---- 发送侧报表捕获 ---- */
typedef struct {
    uint8_t  reports[80][BVPV_REPORT_LEN];
    uint16_t count;
} capture_t;

static void emit_cb(const uint8_t *report64, void *ctx)
{
    capture_t *c = (capture_t *)ctx;
    memcpy(c->reports[c->count], report64, BVPV_REPORT_LEN);
    c->count++;
}

/* 构造一条合法 L2 帧（LEN/CRC 正确），长度 = 8+plen，供分片测试使用 */
static uint16_t build_frame(uint8_t *out, uint16_t plen)
{
    static uint8_t payload[BVPV_MAX_PAYLOAD];
    uint16_t i;
    for (i = 0u; i < plen; i++) {
        payload[i] = (uint8_t)(i * 37u + 11u);
    }
    return (uint16_t)bvpv_frame_encode(out, 0x05u, 0x00u, 0x0001u, payload, plen);
}

/* 用例：单报表消息（≤61B）。
 * 功能：验证短消息一片装下，零分片开销。
 * 预期：发送仅 1 张报表（SOF=EOF=1、SEQ=0）；重组一次即完成，内容与原帧一致。
 * 判据：cap.count==1、分片头 SOF|EOF 置位且 SEQ=0、feed 返回 true、字节一致。 */
static void test_single_fragment(void)
{
    uint8_t frame[64];
    uint16_t flen = build_frame(frame, 10u);        /* 18B，一片装下 */
    static capture_t cap;
    bvpv_frag_tx_t tx;
    bvpv_frag_rx_t rx;
    uint16_t mlen = 0u;

    cap.count = 0u;
    bvpv_frag_tx_init(&tx);
    TEST_ASSERT(bvpv_frag_tx_send(&tx, frame, flen, emit_cb, &cap));
    TEST_ASSERT_EQ_U(1u, cap.count);
    TEST_ASSERT_EQ_U(0xC0u, cap.reports[0][1] & 0xC0u);  /* SOF|EOF */
    TEST_ASSERT_EQ_U(0u,    cap.reports[0][1] & 0x3Fu);  /* SEQ=0 */

    bvpv_frag_rx_init(&rx);
    TEST_ASSERT(bvpv_frag_rx_feed(&rx, cap.reports[0], 0u, &mlen));
    TEST_ASSERT_EQ_U(flen, mlen);
    TEST_ASSERT_MEMEQ(frame, rx.buf, flen);
}

/* 用例：多分片消息。
 * 功能：验证 208B 消息切成 4 片、逐片重组。
 * 预期：发送 4 片；中间片不完成，末片才返回完成；内容一致。
 * 判据：cap.count==4；前 3 片 feed 返回 false、第 4 片 true；mlen 与字节一致。 */
static void test_multi_fragment(void)
{
    uint8_t frame[512];
    uint16_t flen = build_frame(frame, 200u);       /* 208B → 4 片 */
    static capture_t cap;
    bvpv_frag_tx_t tx;
    bvpv_frag_rx_t rx;
    uint16_t mlen = 0u;
    uint16_t i;
    bool done;

    cap.count = 0u;
    bvpv_frag_tx_init(&tx);
    TEST_ASSERT(bvpv_frag_tx_send(&tx, frame, flen, emit_cb, &cap));
    TEST_ASSERT_EQ_U(4u, cap.count);

    bvpv_frag_rx_init(&rx);
    done = false;
    for (i = 0u; i < cap.count; i++) {
        bool d = bvpv_frag_rx_feed(&rx, cap.reports[i], 0u, &mlen);
        if (i < (uint16_t)(cap.count - 1u)) {
            TEST_ASSERT(!d);
        } else {
            done = d;
        }
    }
    TEST_ASSERT(done);
    TEST_ASSERT_EQ_U(flen, mlen);
    TEST_ASSERT_MEMEQ(frame, rx.buf, flen);
}

/* 用例：最大消息 + SEQ 回绕。
 * 功能：验证 4096B 上限消息切 68 片，SEQ 模 64 回绕（第 64 片后回到 0）仍正确重组；
 *       末片仅 9 个有效字节（验证"长度感知重组"不把填充字节计入）。
 * 预期：68 片；片64/65 的 SEQ 回绕为 0/1；末片置 EOF；重组得完整 4096B。
 * 判据：cap.count==68、SEQ 回绕正确、末片 EOF、最终 mlen==4096 且字节一致。 */
static void test_max_message_seq_wrap(void)
{
    static uint8_t frame[BVPV_MAX_FRAME];
    uint16_t flen = build_frame(frame, BVPV_MAX_PAYLOAD);  /* 4096B */
    static capture_t cap;
    bvpv_frag_tx_t tx;
    bvpv_frag_rx_t rx;
    uint16_t mlen = 0u;
    uint16_t i;
    bool done = false;

    cap.count = 0u;
    bvpv_frag_tx_init(&tx);
    TEST_ASSERT(bvpv_frag_tx_send(&tx, frame, flen, emit_cb, &cap));
    TEST_ASSERT_EQ_U(68u, cap.count);                       /* ceil(4096/61) */
    TEST_ASSERT_EQ_U(0u, cap.reports[64][1] & 0x3Fu);       /* 第 65 片 SEQ 回绕到 0 */
    TEST_ASSERT_EQ_U(1u, cap.reports[65][1] & 0x3Fu);
    TEST_ASSERT((cap.reports[67][1] & BVPV_FRAG_EOF) != 0u); /* 末片置 EOF */

    bvpv_frag_rx_init(&rx);
    for (i = 0u; i < cap.count; i++) {
        done = bvpv_frag_rx_feed(&rx, cap.reports[i], 0u, &mlen);
    }
    TEST_ASSERT(done);
    TEST_ASSERT_EQ_U(BVPV_MAX_FRAME, mlen);
    TEST_ASSERT_MEMEQ(frame, rx.buf, BVPV_MAX_FRAME);
}

/* 用例：丢中间片（防错规则1）。
 * 功能：模拟丢了 SEQ=1 的中间片，只发片 0/2/3。
 * 预期：SEQ 跳号（期望 1 收到 2）→ 丢弃半成品，绝不把片1+片3 错拼成消息。
 * 判据：整个过程 feed 不返回 true；且接收端能自愈——重发完整消息可正常重组。 */
static void test_lost_middle_fragment(void)
{
    uint8_t frame[512];
    uint16_t flen = build_frame(frame, 200u);       /* 4 片：SEQ 0,1,2,3 */
    static capture_t cap;
    bvpv_frag_tx_t tx;
    bvpv_frag_rx_t rx;
    uint16_t mlen = 0u;
    uint16_t i;
    bool done;

    cap.count = 0u;
    bvpv_frag_tx_init(&tx);
    bvpv_frag_tx_send(&tx, frame, flen, emit_cb, &cap);
    TEST_ASSERT_EQ_U(4u, cap.count);

    bvpv_frag_rx_init(&rx);
    TEST_ASSERT(!bvpv_frag_rx_feed(&rx, cap.reports[0], 0u, &mlen)); /* SOF，半成品 */
    /* 跳过 reports[1]（SEQ=1） */
    TEST_ASSERT(!bvpv_frag_rx_feed(&rx, cap.reports[2], 0u, &mlen)); /* SEQ 跳号→丢弃 */
    TEST_ASSERT(!bvpv_frag_rx_feed(&rx, cap.reports[3], 0u, &mlen)); /* 已非重组态 */

    /* 接收端须已自愈：重发同一条完整消息可正常重组 */
    bvpv_frag_rx_init(&rx);
    done = false;
    for (i = 0u; i < cap.count; i++) {
        done = bvpv_frag_rx_feed(&rx, cap.reports[i], 0u, &mlen);
    }
    TEST_ASSERT(done);
    TEST_ASSERT_EQ_U(flen, mlen);
}

/* 用例：SOF 再同步（防错规则2）。
 * 功能：模拟主机进程崩溃重启——消息 A 收到一半，突然来了新消息 B 的 SOF。
 * 预期：丢弃 A 的半成品，以 B 的 SOF 重新开始并成功重组 B。
 * 判据：B（单片消息）feed 返回 true，重组内容等于 B。 */
static void test_sof_resync(void)
{
    uint8_t frameA[512];
    uint8_t frameB[64];
    uint16_t lenA = build_frame(frameA, 200u);      /* 4 片 */
    uint16_t lenB = build_frame(frameB, 10u);       /* 1 片 */
    static capture_t capA, capB;
    bvpv_frag_tx_t tx;
    bvpv_frag_rx_t rx;
    uint16_t mlen = 0u;

    capA.count = 0u; capB.count = 0u;
    bvpv_frag_tx_init(&tx);
    bvpv_frag_tx_send(&tx, frameA, lenA, emit_cb, &capA);   /* msgid 0 */
    bvpv_frag_tx_send(&tx, frameB, lenB, emit_cb, &capB);   /* msgid 1 */

    bvpv_frag_rx_init(&rx);
    TEST_ASSERT(!bvpv_frag_rx_feed(&rx, capA.reports[0], 0u, &mlen)); /* A 半成品 */
    TEST_ASSERT(!bvpv_frag_rx_feed(&rx, capA.reports[1], 0u, &mlen));
    /* 主机"重启"：B 的 SOF 中途到达 → 丢弃 A，重组 B（单片即完成） */
    TEST_ASSERT(bvpv_frag_rx_feed(&rx, capB.reports[0], 0u, &mlen));
    TEST_ASSERT_EQ_U(lenB, mlen);
    TEST_ASSERT_MEMEQ(frameB, rx.buf, lenB);
}

/* 用例：MSGID 不匹配（防错规则3）。
 * 功能：消息 A 重组中，混入另一条消息 B 的片（MSGID 不同，但 SEQ 恰好与 A 期望连续）。
 * 预期：MSGID 不同即丢弃，防止跨消息拼接。
 * 判据：feed 不返回 true；之后重发完整 A 可正常重组。 */
static void test_msgid_mismatch(void)
{
    uint8_t frameA[512];
    uint8_t frameB[512];
    uint16_t lenA = build_frame(frameA, 200u);
    uint16_t lenB = build_frame(frameB, 200u);
    static capture_t capA, capB;
    bvpv_frag_tx_t tx;
    bvpv_frag_rx_t rx;
    uint16_t mlen = 0u;
    uint16_t i;
    bool done;

    capA.count = 0u; capB.count = 0u;
    bvpv_frag_tx_init(&tx);
    bvpv_frag_tx_send(&tx, frameA, lenA, emit_cb, &capA);   /* msgid 0 */
    bvpv_frag_tx_send(&tx, frameB, lenB, emit_cb, &capB);   /* msgid 1 */

    bvpv_frag_rx_init(&rx);
    TEST_ASSERT(!bvpv_frag_rx_feed(&rx, capA.reports[0], 0u, &mlen)); /* A 片0, msgid0 */
    /* B 的片1：非 SOF、SEQ=1（与 A 期望连续）但 MSGID=1 → 规则3 丢弃 */
    TEST_ASSERT(!bvpv_frag_rx_feed(&rx, capB.reports[1], 0u, &mlen));

    bvpv_frag_rx_init(&rx);
    done = false;
    for (i = 0u; i < capA.count; i++) {
        done = bvpv_frag_rx_feed(&rx, capA.reports[i], 0u, &mlen);
    }
    TEST_ASSERT(done);
}

/* 用例：重组超时（防错规则4）。
 * 功能：消息 A 收到首片后超过 200ms 未收齐。
 * 预期：半成品被丢弃；后续新消息 B 可干净重组。两条路径都验证：
 *       (a) 周期调用 check_timeout；(b) feed 内置的迟到片超时拦截。
 * 判据：超时后 B 重组成功（feed 返回 true、内容正确）；迟到的 A 残片不被续接。 */
static void test_reassembly_timeout(void)
{
    uint8_t frameA[512];
    uint8_t frameB[64];
    uint16_t lenA = build_frame(frameA, 200u);
    uint16_t lenB = build_frame(frameB, 10u);
    static capture_t capA, capB;
    bvpv_frag_tx_t tx;
    bvpv_frag_rx_t rx;
    uint16_t mlen = 0u;

    capA.count = 0u; capB.count = 0u;
    bvpv_frag_tx_init(&tx);
    bvpv_frag_tx_send(&tx, frameA, lenA, emit_cb, &capA);
    bvpv_frag_tx_send(&tx, frameB, lenB, emit_cb, &capB);

    /* 显式 check_timeout 路径 */
    bvpv_frag_rx_init(&rx);
    TEST_ASSERT(!bvpv_frag_rx_feed(&rx, capA.reports[0], 0u, &mlen));  /* A 半成品 @t=0 */
    bvpv_frag_rx_check_timeout(&rx, 250u);                              /* 250ms 后超时 */
    TEST_ASSERT(bvpv_frag_rx_feed(&rx, capB.reports[0], 250u, &mlen)); /* B 干净重组 */
    TEST_ASSERT_EQ_U(lenB, mlen);

    /* 内置超时路径：未显式 check_timeout，迟到的非 SOF 片也不得续接过期消息 */
    bvpv_frag_rx_init(&rx);
    TEST_ASSERT(!bvpv_frag_rx_feed(&rx, capA.reports[0], 0u, &mlen));  /* A 半成品 @t=0 */
    TEST_ASSERT(!bvpv_frag_rx_feed(&rx, capA.reports[1], 300u, &mlen));/* 300ms 后片1被丢弃 */
    TEST_ASSERT(bvpv_frag_rx_feed(&rx, capB.reports[0], 300u, &mlen)); /* B 仍可重组 */
}

/* 用例：双向 MSGID 独立。
 * 功能：主机→设备、设备→主机两个方向各自从 0 独立计数 MSGID。
 * 预期：两个方向的接收实例互不干扰，各自正确重组。
 * 判据：两个独立 rx 实例都重组成功、内容各自正确。 */
static void test_independent_directions(void)
{
    uint8_t frameA[512];
    uint8_t frameB[512];
    uint16_t lenA = build_frame(frameA, 200u);
    uint16_t lenB = build_frame(frameB, 150u);
    static capture_t capH, capD;
    bvpv_frag_tx_t txHost, txDev;
    bvpv_frag_rx_t rxDev, rxHost;
    uint16_t mlA = 0u, mlB = 0u;
    uint16_t i;
    bool doneA = false, doneB = false;

    capH.count = 0u; capD.count = 0u;
    bvpv_frag_tx_init(&txHost);
    bvpv_frag_tx_init(&txDev);
    bvpv_frag_tx_send(&txHost, frameA, lenA, emit_cb, &capH);  /* 主机 msgid 0 */
    bvpv_frag_tx_send(&txDev,  frameB, lenB, emit_cb, &capD);  /* 设备 msgid 0（独立） */

    bvpv_frag_rx_init(&rxDev);
    bvpv_frag_rx_init(&rxHost);
    for (i = 0u; i < capH.count; i++) {
        doneA = bvpv_frag_rx_feed(&rxDev, capH.reports[i], 0u, &mlA);
    }
    for (i = 0u; i < capD.count; i++) {
        doneB = bvpv_frag_rx_feed(&rxHost, capD.reports[i], 0u, &mlB);
    }
    TEST_ASSERT(doneA);
    TEST_ASSERT(doneB);
    TEST_ASSERT_EQ_U(lenA, mlA);
    TEST_ASSERT_EQ_U(lenB, mlB);
    TEST_ASSERT_MEMEQ(frameA, rxDev.buf, lenA);
    TEST_ASSERT_MEMEQ(frameB, rxHost.buf, lenB);
}

/* 用例：发送侧 MSGID 自增。
 * 功能：连续发两条消息。
 * 预期：第一条 MSGID=0、第二条 MSGID=1（模 256 自增）。
 * 判据：两张报表的 MSGID 字段分别为 0 和 1。 */
static void test_msgid_increments(void)
{
    uint8_t frame[64];
    uint16_t flen = build_frame(frame, 10u);
    static capture_t cap;
    bvpv_frag_tx_t tx;

    cap.count = 0u;
    bvpv_frag_tx_init(&tx);
    bvpv_frag_tx_send(&tx, frame, flen, emit_cb, &cap);   /* msgid 0 */
    bvpv_frag_tx_send(&tx, frame, flen, emit_cb, &cap);   /* msgid 1 */
    TEST_ASSERT_EQ_U(2u, cap.count);
    TEST_ASSERT_EQ_U(0u, cap.reports[0][2]);
    TEST_ASSERT_EQ_U(1u, cap.reports[1][2]);
}

int main(void)
{
    TEST_RUN(test_single_fragment);
    TEST_RUN(test_multi_fragment);
    TEST_RUN(test_max_message_seq_wrap);
    TEST_RUN(test_lost_middle_fragment);
    TEST_RUN(test_sof_resync);
    TEST_RUN(test_msgid_mismatch);
    TEST_RUN(test_reassembly_timeout);
    TEST_RUN(test_independent_directions);
    TEST_RUN(test_msgid_increments);
    TEST_MAIN_END();
}
