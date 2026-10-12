#include "test_harness.h"

#include "bvpv_usb_crc16.h"

/* 测试套件：CRC16-CCITT-FALSE 校验模块。
 * 模块职责：为 L2 帧计算/校验 16 位循环冗余校验（多项式 0x1021、初值 0xFFFF、不反射）。
 * 判定方式：与公开的标准校验值比对；分段与一次性计算结果一致性比对。 */

/* 用例：标准校验值。
 * 功能：对国际公认输入 "123456789" 计算 CRC。
 * 预期：结果 = 0x29B1（CRC16-CCITT-FALSE 的标准 check value）。
 * 判据：bvpv_crc16("123456789", 9) == 0x29B1。 */
static void test_known_vector(void)
{
    const char *s = "123456789";
    TEST_ASSERT_EQ_U(0x29B1u, bvpv_crc16((const uint8_t *)s, 9u));
}

/* 用例：空输入。
 * 功能：长度为 0 时不应改变初值；data 为 NULL 且 len=0 时不应崩溃。
 * 预期：返回初值 0xFFFF。
 * 判据：两种空调用都返回 0xFFFF。 */
static void test_empty(void)
{
    TEST_ASSERT_EQ_U(0xFFFFu, bvpv_crc16((const uint8_t *)"", 0u));
    TEST_ASSERT_EQ_U(0xFFFFu, bvpv_crc16(0, 0u));   /* data 为 NULL 且 len=0 */
}

/* 用例：增量计算与一次性计算一致。
 * 功能：把同一输入分两段用 update 累积，结果须等于一次性计算。
 * 预期：分段结果 = 一次性结果 = 0x29B1。
 * 判据：两段 update 累积值 == 0x29B1。 */
static void test_incremental(void)
{
    const char *s = "123456789";
    uint16_t crc = 0xFFFFu;
    crc = bvpv_crc16_update(crc, (const uint8_t *)s, 4u);
    crc = bvpv_crc16_update(crc, (const uint8_t *)s + 4, 5u);
    TEST_ASSERT_EQ_U(0x29B1u, crc);
}

/* 用例：单字节 0x00 的已知结果。
 * 功能：覆盖最简单的非空输入路径。
 * 预期：[0x00] → 0xE1F0（CRC 目录标准值）。
 * 判据：bvpv_crc16({0x00}, 1) == 0xE1F0。 */
static void test_single_byte_zero(void)
{
    uint8_t b = 0x00u;
    TEST_ASSERT_EQ_U(0xE1F0u, bvpv_crc16(&b, 1u));
}

int main(void)
{
    TEST_RUN(test_known_vector);
    TEST_RUN(test_empty);
    TEST_RUN(test_incremental);
    TEST_RUN(test_single_byte_zero);
    TEST_MAIN_END();
}
