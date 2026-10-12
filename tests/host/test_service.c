#include "test_harness.h"

#include <string.h>

#include "bvpv_errcode.h"
#include "bvpv_service.h"
#include "bvpv_usb_frag.h"
#include "test_catalog_data.h"

/* 测试套件：服务层端到端（帧进帧出全流程）。
 * 覆盖 docs/PROTOCOL.md §5 服务码表与 §7 载荷：建连/自描述/读值/写值/批量读/状态/错误码。
 * 判定方式：每个请求经 do_req 走完整 编码→解码→分发→编码→解码 链路，
 *           并隐含校验应答通用契约（SVC 回显、TID 回显、方向位置位、载荷首字节为状态码）。 */

/* ---- 端到端助手：构造请求帧 → 分发 → 解码应答帧 ---- */
static uint8_t  g_reqbuf[BVPV_MAX_FRAME];
static uint8_t  g_respbuf[BVPV_MAX_FRAME];
static bvpv_frame_t g_req;
static bvpv_frame_t g_resp;
static uint16_t g_tid = 0u;

/* 发起一次事务，返回应答状态码；应答帧视图存于 g_resp。 */
static uint8_t do_req(uint8_t svc, const uint8_t *payload, uint16_t plen)
{
    size_t rn, an;
    g_tid++;
    rn = bvpv_frame_encode(g_reqbuf, svc, BVPV_PROTO_VERSION, g_tid, payload, plen);
    TEST_ASSERT(rn > 0u);
    TEST_ASSERT(bvpv_frame_decode(g_reqbuf, rn, &g_req));
    an = bvpv_service_dispatch(&g_req, g_respbuf, (uint16_t)sizeof(g_respbuf));
    TEST_ASSERT(an > 0u);
    TEST_ASSERT(bvpv_frame_decode(g_respbuf, an, &g_resp));
    /* 应答通用契约：SVC 回显、TID 回显、FLAGS 方向位置位 */
    TEST_ASSERT_EQ_U(svc, g_resp.svc);
    TEST_ASSERT_EQ_U(g_tid, g_resp.tid);
    TEST_ASSERT((g_resp.flags & BVPV_FLAGS_DIR_RESPONSE) != 0u);
    TEST_ASSERT(g_resp.plen >= 1u);
    return g_resp.payload[0];
}

/* 游标拉取：循环直至 next_index=0xFFFF，返回总条目字节数，条目拼到 out。 */
static uint16_t pull_all(uint8_t svc, uint16_t max_bytes, uint8_t *out, uint16_t out_cap)
{
    uint16_t start = 0u, total = 0u;
    for (;;) {
        uint8_t req[4];
        uint8_t st;
        uint16_t next, dlen;
        req[0] = (uint8_t)(start & 0xFFu);
        req[1] = (uint8_t)(start >> 8);
        req[2] = (uint8_t)(max_bytes & 0xFFu);
        req[3] = (uint8_t)(max_bytes >> 8);
        st = do_req(svc, req, 4u);
        TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
        next = (uint16_t)((uint16_t)g_resp.payload[1] | ((uint16_t)g_resp.payload[2] << 8));
        dlen = (uint16_t)(g_resp.plen - 1u - 2u);   /* 载荷 = status1 + next2 + 条目 */
        if ((out != 0) && (dlen > 0u)) {
            TEST_ASSERT((uint32_t)total + dlen <= out_cap);
            memcpy(out + total, g_resp.payload + 3, dlen);
            total = (uint16_t)(total + dlen);
        }
        if (next == 0xFFFFu) {
            break;
        }
        TEST_ASSERT(next > start);   /* 游标必须前进，防死循环 */
        start = next;
    }
    return total;
}

static bvpv_service_ctx_t g_ctx;   /* 静态：s_ctx 指向它，须覆盖整个程序生命周期 */

static uint8_t fake_command(void *user, uint32_t addr, uint8_t index,
                            const uint8_t *value, uint16_t length)
{
    (void)user; (void)addr; (void)index; (void)value; (void)length;
    return BVPV_ERR_OK;
}
static void setup(void)
{
    test_catalog_reset();
    test_catalog_make_ctx(&g_ctx);
    g_ctx.command_handler = fake_command;
    bvpv_service_init(&g_ctx);
}

/* 用例：HELLO 能力协商。
 * 功能：版本兼容时返回设备能力（协议版本/设备类型/最大消息/分片参数/超时/设备名）；
 *       版本不兼容时拒绝。
 * 预期：ver=3 → status=OK；ver=1 → status=0x0A，不协商旧格式。
 * 判据：status 与各能力字段断言成立。 */
static void test_hello(void)
{
    uint8_t st;
    uint8_t nlen;
    setup();
    st = do_req(BVPV_SVC_HELLO, (const uint8_t *)"\x04", 1u);
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
    /* 载荷：status0 ver1 type1 maxmsg2 frag1 timeout2 namelen1 name */
    TEST_ASSERT_EQ_U(BVPV_PROTO_VERSION, g_resp.payload[1]);
    TEST_ASSERT_EQ_U(2u, g_resp.payload[2]);                       /* MED */
    TEST_ASSERT_EQ_U(BVPV_MAX_FRAME,
                     (uint16_t)((uint16_t)g_resp.payload[3] | ((uint16_t)g_resp.payload[4] << 8)));
    TEST_ASSERT_EQ_U(BVPV_FRAG_PAYLOAD, g_resp.payload[5]);
    nlen = g_resp.payload[8];
    TEST_ASSERT_EQ_U(9u, nlen);                                    /* "BV500-USB" */
    TEST_ASSERT_MEMEQ("BV500-USB", g_resp.payload + 9, 9u);

    TEST_ASSERT_EQ_U(15u + nlen, g_resp.plen);
    TEST_ASSERT_EQ_U(1u, g_resp.payload[9u + nlen]);
    TEST_ASSERT_EQ_U(0u, g_resp.payload[10u + nlen]);
    TEST_ASSERT_EQ_U(BVPV_ERR_LENGTH, do_req(BVPV_SVC_HELLO, 0, 0u));
    TEST_ASSERT_EQ_U(BVPV_ERR_LENGTH, do_req(BVPV_SVC_HELLO, (const uint8_t *)"\x03\x00", 2u));
    /* 版本不兼容 */
    st = do_req(BVPV_SVC_HELLO, (const uint8_t *)"\x01", 1u);
    TEST_ASSERT_EQ_U(BVPV_ERR_STATE, st);
}

/* 用例：ECHO 回环。
 * 功能：链路诊断服务，把请求载荷原样回显。
 * 预期：status=OK，应答数据与请求数据逐字节一致。
 * 判据：status==OK 且 memcmp 相等。 */
static void test_echo(void)
{
    uint8_t msg[8] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };
    uint8_t st;
    setup();
    st = do_req(BVPV_SVC_ECHO, msg, 8u);
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
    TEST_ASSERT_MEMEQ(msg, g_resp.payload + 1, 8u);
}

/* 命名分组与参数记录单/多页等价、空间不足拒绝以及 C/C# 共用线级向量。 */
static void test_tree_pull(void)
{
    static uint8_t buf[1024], pages[1024];
    uint16_t total;
    uint8_t req[4] = {0u, 0u, 6u, 0u};
    static const bvpv_tree_record_t vector[] = {
        {BVPV_TREE_GROUP, 1u, 0u, "g", 0u},
        {BVPV_TREE_PARAM, 0u, 1u, 0, 0x010107u}
    };
    static const uint8_t expected[] = {
        0u, 0xFFu, 0xFFu, 1u, 1u, 0u, 0u, 0u, 1u, 'g',
        2u, 1u, 0u, 1u, 1u, 7u
    };
    setup();
    total = pull_all(BVPV_SVC_TREE_PULL, 0xFFFFu, buf, sizeof(buf));
    TEST_ASSERT_EQ_U(total, pull_all(BVPV_SVC_TREE_PULL, 16u, pages, sizeof(pages)));
    TEST_ASSERT_MEMEQ(buf, pages, total);
    TEST_ASSERT_EQ_U(1u, buf[0]);
    TEST_ASSERT_EQ_U(1u, buf[1]);
    TEST_ASSERT_EQ_U(7u, buf[5]);
    TEST_ASSERT_MEMEQ("general", buf + 6u, 7u);
    TEST_ASSERT_EQ_U(BVPV_ERR_LENGTH, do_req(BVPV_SVC_TREE_PULL, req, sizeof(req)));
    req[0] = (uint8_t)(g_ctx.tree_count + 1u); req[2] = 255u;
    TEST_ASSERT_EQ_U(BVPV_ERR_INDEX_OOB, do_req(BVPV_SVC_TREE_PULL, req, sizeof(req)));
    g_ctx.tree = vector; g_ctx.tree_count = 2u;
    req[0] = 0u;
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, do_req(BVPV_SVC_TREE_PULL, req, sizeof(req)));
    TEST_ASSERT_EQ_U(sizeof(expected), g_resp.plen);
    TEST_ASSERT_MEMEQ(expected, g_resp.payload, sizeof(expected));
    req[2] = 7u;
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, do_req(BVPV_SVC_TREE_PULL, req, sizeof(req)));
    TEST_ASSERT_EQ_U(1u, g_resp.payload[1]);
    TEST_ASSERT_EQ_U(10u, g_resp.plen);
    req[0] = 1u; req[2] = 5u;
    TEST_ASSERT_EQ_U(BVPV_ERR_LENGTH, do_req(BVPV_SVC_TREE_PULL, req, sizeof(req)));
    req[2] = 6u;
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, do_req(BVPV_SVC_TREE_PULL, req, sizeof(req)));
    TEST_ASSERT_EQ_U(9u, g_resp.plen);
    TEST_ASSERT_EQ_U(0xFFu, g_resp.payload[1]);
}
/* 用例：ATTR_EXT_READ 读扩展属性（按需单查，TLV 编码）。
 * 功能：读某参数的 Min/Max/Default/枚举串/单位等扩展属性。
 * 预期：SET0 含 Min/Max/Default/Unit 四类 TLV 且 Max=100.0f、单位="%"，
 *       CM 含枚举串（首项值0/"Init"）且无 Min；不存在的地址 → status=0x01。
 * 判据：TLV 集合扫描断言成立；坏地址返回 0x01。 */
static void test_attr_ext_read(void)
{
    uint8_t req[4] = { 0x70u, 0x01u, 0xC8u, 0x00u };  /* SET0 */
    uint8_t st;
    setup();

    st = do_req(BVPV_SVC_ATTR_EXT_READ, req, 4u);
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
    /* 扫描 TLV 集合，应含 0x01(Min) 0x02(Max) 0x03(Default) 0x05(Unit) */
    {
        uint16_t off = 1u;   /* 跳过状态字节 */
        uint16_t end = g_resp.plen;
        int has_min = 0, has_max = 0, has_def = 0, has_unit = 0;
        while (off < end) {
            uint8_t tag = g_resp.payload[off];
            uint16_t tl = (uint16_t)((uint16_t)g_resp.payload[off + 1u] | ((uint16_t)g_resp.payload[off + 2u] << 8));
            if (tag == 0x01u) { has_min = 1; TEST_ASSERT_EQ_U(4u, tl); }
            if (tag == 0x02u) {
                has_max = 1;
                TEST_ASSERT_EQ_U(4u, tl);
                TEST_ASSERT_MEMEQ((const uint8_t *)"\x00\x00\xC8\x42", g_resp.payload + off + 3u, 4u); /* 100.0f */
            }
            if (tag == 0x03u) { has_def = 1; TEST_ASSERT_EQ_U(4u, tl); }
            if (tag == 0x05u) { has_unit = 1; TEST_ASSERT_MEMEQ("%", g_resp.payload + off + 3u, 1u); }
            off = (uint16_t)(off + 3u + tl);
        }
        TEST_ASSERT(has_min);
        TEST_ASSERT(has_max);
        TEST_ASSERT(has_def);
        TEST_ASSERT(has_unit);
    }

    /* 枚举参数 CM：应含 EnumValues(0x04)，且无 Min/Max */
    {
        uint8_t reqcm[4] = { 0x69u, 0x01u, 0x03u, 0x00u };
        uint16_t off, end;
        int has_enum = 0, has_min = 0;
        st = do_req(BVPV_SVC_ATTR_EXT_READ, reqcm, 4u);
        TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
        off = 1u;
        end = g_resp.plen;
        while (off < end) {
            uint8_t tag = g_resp.payload[off];
            uint16_t tl = (uint16_t)((uint16_t)g_resp.payload[off + 1u] | ((uint16_t)g_resp.payload[off + 2u] << 8));
            if (tag == 0x04u) {
                has_enum = 1;
                /* 首枚举项：值 0 + 文本 "Init" */
                TEST_ASSERT_EQ_U(0u, g_resp.payload[off + 3u]);
                TEST_ASSERT_MEMEQ("Init", g_resp.payload + off + 3u + 5u, 4u);
            }
            if (tag == 0x01u) { has_min = 1; }
            off = (uint16_t)(off + 3u + tl);
        }
        TEST_ASSERT(has_enum);
        TEST_ASSERT(!has_min);
    }

    /* 元素 >8B 的默认值（FirmwareVersion TEXT16 = 0x640105）：Default TLV 必须
     * 完整 16 字节。回归用例——旧实现从内联 def[8] 读 16 字节，越界带出相邻字段。 */
    {
        static const uint8_t exp[16] = "SW110048.1.2.1";
        uint8_t reqfv[4] = { 0x64u, 0x01u, 0x05u, 0x00u };
        uint16_t off, end;
        int has_def = 0;
        st = do_req(BVPV_SVC_ATTR_EXT_READ, reqfv, 4u);
        TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
        off = 1u;
        end = g_resp.plen;
        while (off < end) {
            uint8_t tag = g_resp.payload[off];
            uint16_t tl = (uint16_t)((uint16_t)g_resp.payload[off + 1u] | ((uint16_t)g_resp.payload[off + 2u] << 8));
            if (tag == 0x03u) {
                has_def = 1;
                TEST_ASSERT_EQ_U(16u, tl);                       /* 长度按 elem_size，不是 8 */
                TEST_ASSERT_MEMEQ(exp, g_resp.payload + off + 3u, 16u);
            }
            off = (uint16_t)(off + 3u + tl);
        }
        TEST_ASSERT(has_def);
    }

    /* 不存在的地址 → 0x01 */
    {
        uint8_t reqbad[4] = { 0xFFu, 0xFFu, 0xFFu, 0x00u };
        st = do_req(BVPV_SVC_ATTR_EXT_READ, reqbad, 4u);
        TEST_ASSERT_EQ_U(BVPV_ERR_NOT_FOUND, st);
    }
}

/* 用例：VALUE_READ 读单参数值。
 * 功能：读标量/数组元素，并覆盖全部读错误路径。
 * 预期：读 ProductCode 返回 8 字节 "BV500"；地址不存在→0x01；OWO 读→0x02；
 *       数组索引越界→0x05；数组合法元素(index2)→OK 且返回 4 字节。
 * 判据：status 与返回长度/内容断言成立。 */
static void test_value_read(void)
{
    uint8_t st;
    setup();

    /* ProductCode TEXT8 = "BV500"（8 字节字段，NUL 填充） */
    {
        uint8_t req[4] = { 0x01u, 0x01u, 0x03u, 0x00u };
        st = do_req(BVPV_SVC_VALUE_READ, req, 4u);
        TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
        TEST_ASSERT_EQ_U(8u, (uint16_t)((uint16_t)g_resp.payload[1] | ((uint16_t)g_resp.payload[2] << 8)));
        TEST_ASSERT_MEMEQ("BV500", g_resp.payload + 3, 5u);
    }
    /* 不存在 → 0x01 */
    {
        uint8_t req[4] = { 0xFFu, 0xFFu, 0xFFu, 0x00u };
        st = do_req(BVPV_SVC_VALUE_READ, req, 4u);
        TEST_ASSERT_EQ_U(BVPV_ERR_NOT_FOUND, st);
    }
    /* OWO（FactoryReset）读 → 0x02 */
    {
        uint8_t req[4] = { 0x0Bu, 0x01u, 0x01u, 0x00u };
        st = do_req(BVPV_SVC_VALUE_READ, req, 4u);
        TEST_ASSERT_EQ_U(BVPV_ERR_ACCESS_DENIED, st);
    }
    /* 数组索引越界（LearnBank1 elem_count=4，读 index 4）→ 0x05 */
    {
        uint8_t req[4] = { 0x75u, 0x01u, 0x00u, 0x04u };
        st = do_req(BVPV_SVC_VALUE_READ, req, 4u);
        TEST_ASSERT_EQ_U(BVPV_ERR_INDEX_OOB, st);
    }
    /* 数组合法元素（index 2）可读 */
    {
        uint8_t req[4] = { 0x75u, 0x01u, 0x00u, 0x02u };
        st = do_req(BVPV_SVC_VALUE_READ, req, 4u);
        TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
        TEST_ASSERT_EQ_U(4u, (uint16_t)((uint16_t)g_resp.payload[1] | ((uint16_t)g_resp.payload[2] << 8)));
    }
}

/* 用例：VALUE_WRITE 写参数值。
 * 功能：写值并验证"写后读回一致 + RWA 进 EEPROM-pending"，覆盖全部写错误路径。
 * 预期：写 SET0=50.0f → OK，读回=50.0f 且 pending 置位；ROO 写→0x02；越界(150>max100)→0x03；
 *       长度错(≠类型尺寸)→0x08；索引越界→0x05。
 * 判据：各 status 断言 + 读回比对 + 目录 state 白盒断言。 */
static void test_value_write(void)
{
    uint8_t st;
    setup();

    /* 写 SET0 = 50.0f（0x42480000，LE: 00 00 48 42） */
    {
        uint8_t req[10] = { 0x70u, 0x01u, 0xC8u, 0x00u, 0x04u, 0x00u, 0x00u, 0x00u, 0x48u, 0x42u };
        st = do_req(BVPV_SVC_VALUE_WRITE, req, 10u);
        TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
    }
    /* 读回一致 */
    {
        uint8_t req[4] = { 0x70u, 0x01u, 0xC8u, 0x00u };
        st = do_req(BVPV_SVC_VALUE_READ, req, 4u);
        TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
        TEST_ASSERT_MEMEQ((const uint8_t *)"\x00\x00\x48\x42", g_resp.payload + 3, 4u);
    }
    /* RWA 写后 pending 置位（白盒查运行时状态数组） */
    {
        int32_t i;
        for (i = 0; i < (int32_t)test_params_count; i++) {
            if (test_params[i].addr == 0x7001C8u) {
                break;
            }
        }
        TEST_ASSERT(i < (int32_t)test_params_count);
        TEST_ASSERT((test_states[i] & BVPV_STATE_PENDING) != 0u);
    }
    /* OWO 校验通过但不执行、不改变命令槽及其它参数；旧帧也不可产生写入。 */
    {
        uint8_t command[7] = {0x0Bu, 1u, 1u, 0u, 1u, 0u, 1u};
        int32_t i = bvpv_catalog_find(&g_ctx, 0x0B0101u);
        uint32_t revision = g_ctx.revision;
        TEST_ASSERT(i >= 0);
        TEST_ASSERT_EQ_U(BVPV_ERR_OK, do_req(BVPV_SVC_VALUE_WRITE, command, sizeof(command)));
        g_ctx.command_handler = 0;
        TEST_ASSERT_EQ_U(BVPV_ERR_NOT_SUPPORTED, do_req(BVPV_SVC_VALUE_WRITE, command, sizeof(command)));
        g_ctx.command_handler = fake_command;
        TEST_ASSERT_EQ_U(0u, test_params[i].value[0]);
        TEST_ASSERT_EQ_U(revision, g_ctx.revision);
        command[6] = 0u;
        TEST_ASSERT_EQ_U(BVPV_ERR_OUT_OF_RANGE, do_req(BVPV_SVC_VALUE_WRITE, command, sizeof(command)));
        command[6] = 1u; command[3] = 1u;
        TEST_ASSERT_EQ_U(BVPV_ERR_INDEX_OOB, do_req(BVPV_SVC_VALUE_WRITE, command, sizeof(command)));
        command[3] = 0u;
        TEST_ASSERT_EQ_U(BVPV_ERR_LENGTH, do_req(BVPV_SVC_VALUE_WRITE, command, sizeof(command) - 1u));
        g_req.flags = 1u;
        TEST_ASSERT(bvpv_service_dispatch(&g_req, g_respbuf, sizeof(g_respbuf)) > 0u);
        TEST_ASSERT_EQ_U(BVPV_ERR_STATE, g_respbuf[BVPV_FRAME_HDR_LEN]);
        TEST_ASSERT_EQ_U(BVPV_FLAGS_DIR_RESPONSE | BVPV_PROTO_VERSION, g_respbuf[1]);
        TEST_ASSERT_EQ_U(0u, test_params[i].value[0]);
    }
    /* ROO（Display6）写 → 0x02 */
    {
        uint8_t req[10] = { 0x71u, 0x01u, 0x06u, 0x00u, 0x04u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u };
        st = do_req(BVPV_SVC_VALUE_WRITE, req, 10u);
        TEST_ASSERT_EQ_U(BVPV_ERR_ACCESS_DENIED, st);
    }
    /* 越界写（SET0 = 150.0f 超过 max 100）→ 0x03 */
    {
        uint8_t req[10] = { 0x70u, 0x01u, 0xC8u, 0x00u, 0x04u, 0x00u, 0x00u, 0x00u, 0x30u, 0x43u }; /* 150.0f=0x43300000 */
        st = do_req(BVPV_SVC_VALUE_WRITE, req, 10u);
        TEST_ASSERT_EQ_U(BVPV_ERR_OUT_OF_RANGE, st);
    }
    /* 长度错（value_len != 类型尺寸）→ 0x08 */
    {
        uint8_t req[9] = { 0x70u, 0x01u, 0xC8u, 0x00u, 0x03u, 0x00u, 0x00u, 0x00u, 0x00u };
        st = do_req(BVPV_SVC_VALUE_WRITE, req, 9u);
        TEST_ASSERT_EQ_U(BVPV_ERR_LENGTH, st);
    }
    /* 索引越界（LearnBank1 index 4）→ 0x05 */
    {
        uint8_t req[10] = { 0x75u, 0x01u, 0x00u, 0x04u, 0x04u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u };
        st = do_req(BVPV_SVC_VALUE_WRITE, req, 10u);
        TEST_ASSERT_EQ_U(BVPV_ERR_INDEX_OOB, st);
    }
}

/* 用例：VALUE_READ_BATCH 批量读（实时轮询通道）。
 * 功能：一次读多个参数，逐项返回独立状态（单点失败不影响他项）。
 * 预期：帧级 status=OK；项1 ProductCode ok(8B)、项2 不存在(0x01,len=0)、项3 SET0 ok(4B)。
 * 判据：逐项 item_status 与长度/内容断言成立。 */
static void test_value_read_batch(void)
{
    uint8_t st;
    setup();

    {
        /* 3 项：ProductCode(ok)、0xFFFFFF(notfound)、SET0(ok) */
        uint8_t req[13] = {
            0x03u,                                  /* count */
            0x01u, 0x01u, 0x03u, 0x00u,             /* ProductCode */
            0xFFu, 0xFFu, 0xFFu, 0x00u,             /* notfound */
            0x70u, 0x01u, 0xC8u, 0x00u,             /* SET0 */
        };
        st = do_req(BVPV_SVC_VALUE_READ_BATCH, req, 13u);
        TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);   /* 帧级 status=0 */
        /* 项1：ok，len=8，值 "BV500" */
        TEST_ASSERT_EQ_U(BVPV_ERR_OK, g_resp.payload[1]);
        TEST_ASSERT_EQ_U(8u, (uint16_t)((uint16_t)g_resp.payload[2] | ((uint16_t)g_resp.payload[3] << 8)));
        TEST_ASSERT_MEMEQ("BV500", g_resp.payload + 4, 5u);
        /* 项2：notfound，len=0（偏移 4+8=12） */
        TEST_ASSERT_EQ_U(BVPV_ERR_NOT_FOUND, g_resp.payload[12]);
        TEST_ASSERT_EQ_U(0u, (uint16_t)((uint16_t)g_resp.payload[13] | ((uint16_t)g_resp.payload[14] << 8)));
        /* 项3：ok，len=4（偏移 12+3=15） */
        TEST_ASSERT_EQ_U(BVPV_ERR_OK, g_resp.payload[15]);
        TEST_ASSERT_EQ_U(4u, (uint16_t)((uint16_t)g_resp.payload[16] | ((uint16_t)g_resp.payload[17] << 8)));
    }
}

/* 用例：STATE_READ 读参数运行时状态。
 * 功能：读参数的可见性与"已修改未持久化"标志。
 * 预期：初始 SET0 可见且无 pending；写一次后再读 pending 置位。
 * 判据：state 字节的 bit0(可见)/bit1(pending) 断言成立。 */
static void test_state_read(void)
{
    uint8_t st;
    uint8_t req[4] = { 0x70u, 0x01u, 0xC8u, 0x00u };  /* SET0 */
    setup();

    /* 初始：可见、无 pending */
    st = do_req(BVPV_SVC_STATE_READ, req, 4u);
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
    TEST_ASSERT((g_resp.payload[1] & BVPV_STATE_VISIBLE) != 0u);
    TEST_ASSERT((g_resp.payload[1] & BVPV_STATE_PENDING) == 0u);

    /* 写一次后再读：pending 置位 */
    {
        uint8_t wreq[10] = { 0x70u, 0x01u, 0xC8u, 0x00u, 0x04u, 0x00u, 0x00u, 0x00u, 0x48u, 0x42u };
        st = do_req(BVPV_SVC_VALUE_WRITE, wreq, 10u);
        TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
    }
    st = do_req(BVPV_SVC_STATE_READ, req, 4u);
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
    TEST_ASSERT((g_resp.payload[1] & BVPV_STATE_PENDING) != 0u);
}

/* 用例：ERRTEXT_PULL 拉错误码文本表（自描述延伸）。
 * 功能：游标分块拉取设备侧错误码→文本映射。
 * 预期：拉全 13 条；首条目 code=0/"OK"；含 0xFF/"Unknown error"；遍历恰好消费完。
 * 判据：首条目字段、含 0xFF、遍历偏移=总字节数。 */
static void test_errtext_pull(void)
{
    static uint8_t buf[1024];
    uint16_t total;
    setup();

    total = pull_all(BVPV_SVC_ERRTEXT_PULL, 0xFFFFu, buf, (uint16_t)sizeof(buf));
    TEST_ASSERT(total > 0u);
    /* 首条目：code 0x00 "OK"；应包含 0xFF "Unknown error" */
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, buf[0]);
    TEST_ASSERT_EQ_U(2u, buf[1]);          /* "OK" 长度 2 */
    TEST_ASSERT_MEMEQ("OK", buf + 2, 2u);
    /* 遍历确认含 0xFF */
    {
        uint16_t off = 0u;
        int has_ff = 0;
        while (off < total) {
            uint8_t code = buf[off];
            uint8_t tl = buf[off + 1u];
            if (code == 0xFFu) { has_ff = 1; }
            off = (uint16_t)(off + 2u + tl);
        }
        TEST_ASSERT(has_ff);
        TEST_ASSERT_EQ_U(total, off);
    }
}

/* 用例：完整连接序列（模拟上位机建连）。
 * 功能：HELLO → 拉参数树(全) → 拉完整属性快照 → 之后普通读写可用。
 * 预期：三步全部 status=OK，且建连后能正常 VALUE_READ。
 * 判据：各步 status==OK，读 ProductCode 成功。 */
static void test_connect_sequence(void)
{
    static uint8_t buf[2048];
    uint8_t st;
    setup();

    st = do_req(BVPV_SVC_HELLO, (const uint8_t *)"\x04", 1u);
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
    TEST_ASSERT(pull_all(BVPV_SVC_TREE_PULL, 256u, buf, (uint16_t)sizeof(buf)) > 0u);
    {
        uint8_t snapshot[8] = {0u, 0u, 0u, 8u, 0u, 0u, 0u, 0u};
        TEST_ASSERT_EQ_U(BVPV_ERR_OK,
            do_req(BVPV_SVC_ATTR_SNAPSHOT_PULL, snapshot, sizeof(snapshot)));
        TEST_ASSERT_EQ_U(0xFFu, g_resp.payload[5]);
        TEST_ASSERT_EQ_U(0xFFu, g_resp.payload[6]);
    }

    /* 建连后普通读写可用 */
    {
        uint8_t req[4] = { 0x01u, 0x01u, 0x03u, 0x00u };
        st = do_req(BVPV_SVC_VALUE_READ, req, 4u);
        TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
    }
}

static uint32_t test_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 分页快照必须与旧接口逐项一致，并在修订变化时拒绝继续分页。 */
static void count_attr_provider(void *user, uint16_t index,
                                const bvpv_param_t *param, bvpv_effective_attr_t *attr)
{
    (void)index; (void)param; (void)attr;
    (*(unsigned *)user)++;
}

static void test_attr_snapshot_pull(void)
{
    uint8_t req[8] = {0u, 0u, 128u, 0u, 0u, 0u, 0u, 0u};
    uint8_t page[256];
    uint16_t cursor = 0u;
    uint32_t revision = 0u;
    unsigned visited = 0u, calls = 0u;
    setup();
    g_ctx.attr_provider = count_attr_provider;
    g_ctx.attr_provider_user = &calls;
    TEST_ASSERT_EQ_U(BVPV_ERR_LENGTH, do_req(BVPV_SVC_ATTR_SNAPSHOT_PULL, req, 7u));
    for (;;) {
        uint16_t next, off, len, count = 0u;
        uint8_t st;
        req[0] = (uint8_t)cursor;
        req[1] = (uint8_t)(cursor >> 8);
        req[4] = (uint8_t)revision;
        req[5] = (uint8_t)(revision >> 8);
        req[6] = (uint8_t)(revision >> 16);
        req[7] = (uint8_t)(revision >> 24);
        calls = 0u;
        st = do_req(BVPV_SVC_ATTR_SNAPSHOT_PULL, req, sizeof(req));
        TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
        if (st != BVPV_ERR_OK || g_resp.plen < 7u || g_resp.plen > sizeof(page)) return;
        len = g_resp.plen;
        memcpy(page, g_resp.payload, len);
        if (revision == 0u) revision = test_rd32(page + 1u);
        TEST_ASSERT_EQ_U(revision, test_rd32(page + 1u));
        next = (uint16_t)((uint16_t)page[5] | ((uint16_t)page[6] << 8));
        off = 7u;
        while (off < len) {
            uint8_t addr[4], core[6];
            uint16_t ext;
            if ((unsigned)(len - off) < 12u) { TEST_ASSERT(0); return; }
            {
                uint8_t name_len = page[off + 9u];
                uint16_t ext_at = (uint16_t)(off + 10u + name_len);
                if (name_len == 0u || (unsigned)(len - off) < (unsigned)(12u + name_len)) {
                    TEST_ASSERT(0); return;
                }
                ext = (uint16_t)((uint16_t)page[ext_at] | ((uint16_t)page[ext_at + 1u] << 8));
                if (len - ext_at - 2u < ext) { TEST_ASSERT(0); return; }
                TEST_ASSERT(visited < test_params_count);
                if (visited >= test_params_count) return;
                TEST_ASSERT_EQ_U(strlen(test_params[visited].name), name_len);
                TEST_ASSERT_MEMEQ(test_params[visited].name, page + off + 10u, name_len);
                TEST_ASSERT_EQ_U(test_params[visited].addr,
                    ((uint32_t)page[off] << 16) | ((uint32_t)page[off + 1u] << 8) | page[off + 2u]);
                memcpy(core, page + off + 3u, sizeof(core));
                memcpy(addr, page + off, 3u);
                addr[3] = 0u;
                TEST_ASSERT_EQ_U(BVPV_ERR_OK, do_req(BVPV_SVC_ATTR_CORE_READ, addr, 4u));
                TEST_ASSERT_EQ_U(7u, g_resp.plen);
                TEST_ASSERT_MEMEQ(core, g_resp.payload + 1u, 6u);
                TEST_ASSERT_EQ_U(BVPV_ERR_OK, do_req(BVPV_SVC_ATTR_EXT_READ, addr, 4u));
                TEST_ASSERT_EQ_U((unsigned)ext + 1u, g_resp.plen);
                TEST_ASSERT_MEMEQ(page + ext_at + 2u, g_resp.payload + 1u, ext);
                off = (uint16_t)(ext_at + 2u + ext);
            }
            visited++;
            count++;
        }
        TEST_ASSERT_EQ_U(len, off);
        TEST_ASSERT(calls >= 3u * count && calls <= 3u * count + 1u);
        TEST_ASSERT(count > 0u);
        if (next == 0xFFFFu) break;
        TEST_ASSERT_EQ_U((unsigned)cursor + count, next);
        if (cursor == 0u) {
            req[0] = (uint8_t)next;
            req[1] = (uint8_t)(next >> 8);
            req[4] = (uint8_t)revision;
            bvpv_catalog_note_change(&g_ctx, 0u, BVPV_CHANGE_CORE);
            TEST_ASSERT_EQ_U(BVPV_ERR_STATE,
                do_req(BVPV_SVC_ATTR_SNAPSHOT_PULL, req, sizeof(req)));
            revision = g_ctx.revision;
        }
        cursor = next;
    }
    TEST_ASSERT_EQ_U(test_params_count, visited);
    {
        static const uint8_t vector[] = {
            0u, 1u, 0u, 0u, 0u, 0xFFu, 0xFFu,
            0x01u, 0x01u, 0x03u, BVPV_TYPE_TEXT8, BVPV_ACC_ROO,
            1u, 1u, 1u, 0u, 11u, 'P', 'r', 'o', 'd', 'u', 'c', 't',
            'C', 'o', 'd', 'e', 0u, 0u
        };
        setup();
        g_ctx.count = 1u;
        memset(req, 0, sizeof(req));
        req[2] = 128u;
        TEST_ASSERT_EQ_U(BVPV_ERR_OK, do_req(BVPV_SVC_ATTR_SNAPSHOT_PULL, req, sizeof(req)));
        TEST_ASSERT_EQ_U(sizeof(vector), g_resp.plen);
        TEST_ASSERT_MEMEQ(vector, g_resp.payload, sizeof(vector));
    }
    {
        char name[256];
        bvpv_param_t long_param = test_params[0];
        memset(name, 'A', 255u);
        name[255] = '\0';
        long_param.name = name;
        setup();
        g_ctx.params = &long_param;
        g_ctx.count = 1u;
        memset(req, 0, sizeof(req));
        req[2] = 267u & 0xFFu;
        req[3] = 267u >> 8;
        TEST_ASSERT_EQ_U(BVPV_ERR_OK, do_req(BVPV_SVC_ATTR_SNAPSHOT_PULL, req, sizeof(req)));
        TEST_ASSERT_EQ_U(274u, g_resp.plen);
        TEST_ASSERT_EQ_U(255u, g_resp.payload[16u]);
        TEST_ASSERT_MEMEQ(name, g_resp.payload + 17u, 255u);
        req[2] = 266u & 0xFFu;
        TEST_ASSERT_EQ_U(BVPV_ERR_LENGTH, do_req(BVPV_SVC_ATTR_SNAPSHOT_PULL, req, sizeof(req)));
    }
    req[2] = 1u;
    req[3] = 0u;
    req[0] = req[1] = 0u;
    TEST_ASSERT_EQ_U(BVPV_ERR_LENGTH, do_req(BVPV_SVC_ATTR_SNAPSHOT_PULL, req, sizeof(req)));
}
static void test_dynamic_attributes(void)
{
    uint8_t st;
    uint8_t addr[4] = { 0x70u, 0x01u, 0xC8u, 0u };
    uint8_t query[12] = { 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
                          0u, 0u, 4u, 0u };
    int32_t index;
    setup();

    st = do_req(BVPV_SVC_ATTR_CORE_READ, addr, sizeof(addr));
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
    TEST_ASSERT_EQ_U(BVPV_TYPE_FLOAT32, g_resp.payload[1]);
    TEST_ASSERT_EQ_U(BVPV_ACC_RWA, g_resp.payload[2]);
    TEST_ASSERT((g_resp.payload[4] & BVPV_STATE_VISIBLE) != 0u);

    index = bvpv_catalog_find(&g_ctx, 0x7001C8u);
    TEST_ASSERT(index >= 0);
    bvpv_catalog_set_state(&g_ctx, (uint16_t)index,
                           (uint8_t)(test_states[index] | BVPV_STATE_PENDING));
    st = do_req(BVPV_SVC_CHANGE_QUERY, query, sizeof(query));
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
    TEST_ASSERT_EQ_U(g_ctx.revision, test_rd32(g_resp.payload + 1u));
    TEST_ASSERT_EQ_U(g_ctx.revision, test_rd32(g_resp.payload + 5u));
    TEST_ASSERT_EQ_U(0xFFFFu, (uint16_t)((uint16_t)g_resp.payload[9] |
                                         ((uint16_t)g_resp.payload[10] << 8)));
    TEST_ASSERT_EQ_U(0u, g_resp.payload[11]);
    TEST_ASSERT_MEMEQ(addr, g_resp.payload + 12u, 3u);
    TEST_ASSERT_EQ_U(BVPV_CHANGE_STATE, g_resp.payload[15]);

    /* 四项日志容量被第五项覆盖后，从 revision=1 查询必须要求全量同步。 */
    for (uint8_t k = 0u; k < 5u; k++) {
        bvpv_catalog_set_state(&g_ctx, (uint16_t)index,
                               (k & 1u) ? BVPV_STATE_VISIBLE :
                                          (uint8_t)(BVPV_STATE_VISIBLE | BVPV_STATE_PENDING));
    }
    memset(query + 4u, 0, 4u);
    query[8] = query[9] = 0u;
    st = do_req(BVPV_SVC_CHANGE_QUERY, query, sizeof(query));
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, st);
    TEST_ASSERT((g_resp.payload[11] & 0x01u) != 0u);
}

/* 用例：未知服务码。
 * 功能：分发器对未定义/未实现的服务码统一拒绝。
 * 预期：SVC=0x55（未定义）→ status=0x09（服务不支持）。
 * 判据：status==0x09。 */
static void test_unknown_svc(void)
{
    uint8_t st;
    setup();
    st = do_req(0x55u, 0, 0u);
    TEST_ASSERT_EQ_U(BVPV_ERR_NOT_SUPPORTED, st);
}

static unsigned firmware_requests, firmware_completions, firmware_cancels;
static bool firmware_delivered;
static void firmware_request(uint8_t svc, const uint8_t *p, uint16_t len,
                             uint8_t *out, uint16_t *out_len)
{
    (void)p;
    firmware_requests++;
    *out_len = svc == BVPV_SVC_FW_INFO && !len ? 149u : 1u;
    memset(out, 0, *out_len);
    out[0] = len ? BVPV_ERR_LENGTH : BVPV_ERR_OK;
    if (*out_len == 149u) out[1] = 1u;
}
static void firmware_sent(uint8_t svc, bool delivered)
{
    TEST_ASSERT_EQ_U(BVPV_SVC_ENTER_BOOTLOADER, svc);
    firmware_completions++;
    firmware_delivered = delivered;
}
static void firmware_cancel(void) { firmware_cancels++; }
static void test_firmware_app_dispatch(void)
{
    const bvpv_fw_backend_t backend = {firmware_request, firmware_sent, firmware_cancel};
    uint8_t hello = BVPV_PROTO_VERSION;
    setup();
    TEST_ASSERT_EQ_U(BVPV_ERR_NOT_SUPPORTED, do_req(BVPV_SVC_FW_INFO, 0, 0));
    TEST_ASSERT_EQ_U(BVPV_ERR_NOT_SUPPORTED, do_req(BVPV_SVC_ENTER_BOOTLOADER, 0, 0));
    bvpv_service_set_fw_backend(&backend);
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, do_req(BVPV_SVC_HELLO, &hello, 1));
    TEST_ASSERT_EQ_U(BVPV_CAP_DYNAMIC_ATTR | BVPV_CAP_FW_UPDATE,
        g_resp.payload[9u + g_resp.payload[8u]]);
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, do_req(BVPV_SVC_FW_INFO, 0, 0));
    TEST_ASSERT_EQ_U(149u, g_resp.plen);
    TEST_ASSERT_EQ_U(1u, g_resp.payload[1]);
    TEST_ASSERT_EQ_U(BVPV_ERR_LENGTH, do_req(BVPV_SVC_FW_INFO, &hello, 1));
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, do_req(BVPV_SVC_ENTER_BOOTLOADER, 0, 0));
    TEST_ASSERT_EQ_U(0u, firmware_completions);
    bvpv_service_reply_complete(BVPV_SVC_ENTER_BOOTLOADER, true);
    TEST_ASSERT_EQ_U(1u, firmware_completions);
    TEST_ASSERT(firmware_delivered);
    bvpv_service_reply_complete(BVPV_SVC_ENTER_BOOTLOADER, false);
    TEST_ASSERT_EQ_U(2u, firmware_completions);
    TEST_ASSERT(!firmware_delivered);
    TEST_ASSERT_EQ_U(BVPV_ERR_NOT_SUPPORTED, do_req(0x20u, 0, 0));
    TEST_ASSERT_EQ_U(3u, firmware_requests);
    bvpv_service_cancel();
    TEST_ASSERT_EQ_U(1u, firmware_cancels);
    setup();
    TEST_ASSERT(!bvpv_service_fw_supported());
}

static bool external_ready, external_maintenance, external_reply;
static unsigned external_tasks;
static bool external_request(uint8_t svc, const uint8_t *p, uint16_t n,
                             uint8_t *out, uint16_t *len, uint16_t cap)
{
    (void)p; (void)n;
    TEST_ASSERT(cap > 2u);
    if (svc == BVPV_SVC_CATALOG_UPDATE_BEGIN) return false;
    out[0] = BVPV_ERR_OK; out[1] = 42u; *len = 2u;
    return true;
}
static bool external_poll(uint8_t *out, uint16_t *len, uint16_t cap)
{
    TEST_ASSERT(cap >= 2u);
    if (!external_reply) return false;
    out[0] = BVPV_ERR_OK; out[1] = 7u; *len = 2u;
    return true;
}
static bool external_is_ready(void) { return external_ready; }
static bool external_is_maintenance(void) { return external_maintenance; }
static void external_task(void) { external_tasks++; }

static void test_external_catalog_dispatch(void)
{
    const bvpv_catalog_backend_t backend = {
        external_request, external_poll, 0, external_task,
        external_is_ready, external_is_maintenance, 0, 0
    };
    uint8_t hello = BVPV_PROTO_VERSION, addr[] = {1, 1, 3, 0};
    uint8_t response[BVPV_MAX_FRAME];
    bvpv_frame_t req = {0}, view;
    size_t len;
    setup(); external_ready = false; external_maintenance = false; external_reply = false;
    bvpv_service_set_catalog_backend(&backend);
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, do_req(BVPV_SVC_HELLO, &hello, 1));
    TEST_ASSERT_EQ_U(BVPV_CAP_DYNAMIC_ATTR | BVPV_CAP_CATALOG,
                    g_resp.payload[9u + g_resp.payload[8u]]);
    TEST_ASSERT_EQ_U(BVPV_ERR_STATE, do_req(BVPV_SVC_VALUE_READ, addr, sizeof(addr)));
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, do_req(BVPV_SVC_CATALOG_INFO, 0, 0));
    TEST_ASSERT_EQ_U(42u, g_resp.payload[1]);
    {
        const uint8_t diagnostics[] = {0, 0, 128, 0};
        TEST_ASSERT_EQ_U(BVPV_ERR_OK,
            do_req(BVPV_SVC_ERRTEXT_PULL, diagnostics, sizeof(diagnostics)));
        TEST_ASSERT(g_resp.plen > 3u); /* 缺目录仍有固件恢复诊断。 */
    }
    external_ready = true; external_maintenance = true;
    TEST_ASSERT_EQ_U(BVPV_ERR_BUSY, do_req(BVPV_SVC_VALUE_READ, addr, sizeof(addr)));
    TEST_ASSERT_EQ_U(BVPV_ERR_BUSY, do_req(BVPV_SVC_ENTER_BOOTLOADER, 0, 0));
    TEST_ASSERT_EQ_U(BVPV_ERR_BUSY, do_req(BVPV_SVC_MSC_UPDATE_BEGIN, 0, 0));
    external_maintenance = false;
    TEST_ASSERT_EQ_U(BVPV_ERR_OK, do_req(BVPV_SVC_VALUE_READ, addr, sizeof(addr)));
    external_tasks = 0; bvpv_service_task(); TEST_ASSERT_EQ_U(1u, external_tasks);
    req.svc = BVPV_SVC_CATALOG_UPDATE_BEGIN; req.flags = BVPV_PROTO_VERSION; req.tid = 123u;
    TEST_ASSERT_EQ_U(0u, bvpv_service_dispatch(&req, response, sizeof(response)));
    TEST_ASSERT(bvpv_service_pending());
    TEST_ASSERT_EQ_U(0u, bvpv_service_poll(response, sizeof(response)));
    external_reply = true;
    len = bvpv_service_poll(response, sizeof(response));
    TEST_ASSERT(bvpv_frame_decode(response, len, &view));
    TEST_ASSERT_EQ_U(req.svc, view.svc); TEST_ASSERT_EQ_U(req.tid, view.tid);
    TEST_ASSERT_EQ_U(2u, view.plen); TEST_ASSERT_EQ_U(7u, view.payload[1]);
    TEST_ASSERT(!bvpv_service_pending());
    setup(); TEST_ASSERT(!bvpv_service_catalog_supported());
}

static void test_runtime_snapshot(void)
{
    uint8_t req[8] = {0, 0, 48, 0, 0, 0, 0, 0};
    uint16_t cursor = 0;
    uint32_t revision = 0;
    unsigned visited = 0;
    setup();
    do {
        uint16_t off = 7u, next;
        req[0] = (uint8_t)cursor; req[1] = (uint8_t)(cursor >> 8);
        for (unsigned i = 0; i < 4; i++) req[4+i] = (uint8_t)(revision >> (8*i));
        TEST_ASSERT_EQ_U(BVPV_ERR_OK, do_req(BVPV_SVC_RUNTIME_SNAPSHOT_PULL, req, sizeof(req)));
        revision = test_rd32(g_resp.payload + 1);
        TEST_ASSERT_EQ_U(g_ctx.revision, revision);
        next = (uint16_t)(g_resp.payload[5] | ((uint16_t)g_resp.payload[6] << 8));
        while (off < g_resp.plen) {
            const bvpv_param_t *p = &test_params[visited++];
            uint8_t min = g_resp.payload[off+5], max = g_resp.payload[off+6];
            TEST_ASSERT_EQ_U(p->addr >> 16, g_resp.payload[off]);
            TEST_ASSERT_EQ_U((p->addr >> 8) & 255u, g_resp.payload[off+1]);
            TEST_ASSERT_EQ_U(p->addr & 255u, g_resp.payload[off+2]);
            TEST_ASSERT_EQ_U(p->access, g_resp.payload[off+3]);
            TEST_ASSERT_EQ_U(test_states[visited-1], g_resp.payload[off+4]);
            TEST_ASSERT(min <= 8u && max <= 8u);
            off = (uint16_t)(off + 7u + min + max);
            TEST_ASSERT(off <= g_resp.plen);
        }
        TEST_ASSERT(next == 0xFFFFu || next > cursor);
        cursor = next;
    } while (cursor != 0xFFFFu);
    TEST_ASSERT_EQ_U(test_params_count, visited);
    req[0] = req[1] = 0; req[4] = (uint8_t)(revision + 1u);
    TEST_ASSERT_EQ_U(BVPV_ERR_STATE, do_req(BVPV_SVC_RUNTIME_SNAPSHOT_PULL, req, sizeof(req)));
    memset(req, 0, sizeof(req));
    TEST_ASSERT_EQ_U(BVPV_ERR_LENGTH, do_req(BVPV_SVC_RUNTIME_SNAPSHOT_PULL, req, sizeof(req)));
}

int main(void)
{
    TEST_RUN(test_hello);
    TEST_RUN(test_echo);
    TEST_RUN(test_tree_pull);

    TEST_RUN(test_attr_ext_read);
    TEST_RUN(test_value_read);
    TEST_RUN(test_value_write);
    TEST_RUN(test_value_read_batch);
    TEST_RUN(test_state_read);
    TEST_RUN(test_attr_snapshot_pull);
    TEST_RUN(test_dynamic_attributes);
    TEST_RUN(test_errtext_pull);
    TEST_RUN(test_connect_sequence);
    TEST_RUN(test_unknown_svc);
    TEST_RUN(test_firmware_app_dispatch);
    TEST_RUN(test_external_catalog_dispatch);
    TEST_RUN(test_runtime_snapshot);
    TEST_MAIN_END();
}
