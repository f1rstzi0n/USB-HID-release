#include "test_harness.h"

#include "bvpv_catalog.h"
#include "test_catalog_data.h"

/* 测试套件：参数目录模型。
 * 模块职责：参数的三字节地址查找、类型尺寸、权限判定、地址合成、类型感知值比较。
 * 判定方式：对合成目录（14 参数）做已知结果的查询/判定/比较断言。 */

/* 用例：按地址查找。
 * 功能：对存在的地址返回有效索引，不存在的地址返回 -1。
 * 预期：0x010103/0x6801FE 命中（索引≥0），0xFFFFFF/0x000000 不命中（-1）。
 * 判据：find 返回值符合上述。 */
static void test_find(void)
{
    bvpv_service_ctx_t ctx;
    test_catalog_make_ctx(&ctx);

    TEST_ASSERT(bvpv_catalog_find(&ctx, 0x010103u) >= 0);
    TEST_ASSERT(bvpv_catalog_find(&ctx, 0x6801FEu) >= 0);
    TEST_ASSERT(bvpv_catalog_find(&ctx, 0xFFFFFFu) < 0);
    TEST_ASSERT(bvpv_catalog_find(&ctx, 0x000000u) < 0);
}

/* 用例：类型尺寸表。
 * 功能：每种数据类型返回正确的字节尺寸；COMPOSITE 返回 0（尺寸由目录 elem_size 决定）。
 * 预期：UINT8=1、UINT16/UFRAC16=2、UINT32/INT32/FLOAT32/FIXED=4、UINT64/TEXT8=8、
 *       TEXT16=16、TEXT32=32、COMPOSITE=0。
 * 判据：bvpv_type_size 逐类型返回值与上表一致。 */
static void test_type_size(void)
{
    TEST_ASSERT_EQ_U(1u,  bvpv_type_size(BVPV_TYPE_UINT8));
    TEST_ASSERT_EQ_U(2u,  bvpv_type_size(BVPV_TYPE_UINT16));
    TEST_ASSERT_EQ_U(2u,  bvpv_type_size(BVPV_TYPE_UFRAC16));
    TEST_ASSERT_EQ_U(4u,  bvpv_type_size(BVPV_TYPE_UINT32));
    TEST_ASSERT_EQ_U(4u,  bvpv_type_size(BVPV_TYPE_INT32));
    TEST_ASSERT_EQ_U(4u,  bvpv_type_size(BVPV_TYPE_FLOAT32));
    TEST_ASSERT_EQ_U(4u,  bvpv_type_size(BVPV_TYPE_FIXED16_16));
    TEST_ASSERT_EQ_U(8u,  bvpv_type_size(BVPV_TYPE_UINT64));
    TEST_ASSERT_EQ_U(8u,  bvpv_type_size(BVPV_TYPE_TEXT8));
    TEST_ASSERT_EQ_U(16u, bvpv_type_size(BVPV_TYPE_TEXT16));
    TEST_ASSERT_EQ_U(32u, bvpv_type_size(BVPV_TYPE_TEXT32));
    TEST_ASSERT_EQ_U(0u,  bvpv_type_size(BVPV_TYPE_COMPOSITE));
}

/* 用例：单元素尺寸。
 * 功能：COMPOSITE 取目录 elem_size，其余取类型尺寸。
 * 预期：AndromedaCMD(COMPOSITE)=16B；Display6(FLOAT32)=4B。
 * 判据：bvpv_param_elem_size 返回值分别为 16 和 4。 */
static void test_elem_size(void)
{
    bvpv_service_ctx_t ctx;
    int32_t i;
    test_catalog_make_ctx(&ctx);

    i = bvpv_catalog_find(&ctx, 0x6801FEu);   /* AndromedaCMD COMPOSITE */
    TEST_ASSERT(i >= 0);
    TEST_ASSERT_EQ_U(16u, bvpv_param_elem_size(&ctx.params[i]));

    i = bvpv_catalog_find(&ctx, 0x710106u);   /* Display6 FLOAT32 */
    TEST_ASSERT(i >= 0);
    TEST_ASSERT_EQ_U(4u, bvpv_param_elem_size(&ctx.params[i]));
}

/* 用例：权限判定。
 * 功能：五种权限（RWA/ROO/RWO/OWO/OOO）的可读/可写/EEPROM 持久判定。
 * 预期：RWA 全可；ROO 只读；RWO 可读写但不持久；OWO 只写；OOO 全禁。仅 RWA 持久。
 * 判据：readable/writable/eeprom_persistent 三个函数对五种权限的返回矩阵正确。 */
static void test_access(void)
{
    TEST_ASSERT(bvpv_access_readable(BVPV_ACC_RWA));
    TEST_ASSERT(bvpv_access_readable(BVPV_ACC_ROO));
    TEST_ASSERT(bvpv_access_readable(BVPV_ACC_RWO));
    TEST_ASSERT(!bvpv_access_readable(BVPV_ACC_OWO));
    TEST_ASSERT(!bvpv_access_readable(BVPV_ACC_OOO));

    TEST_ASSERT(bvpv_access_writable(BVPV_ACC_RWA));
    TEST_ASSERT(bvpv_access_writable(BVPV_ACC_RWO));
    TEST_ASSERT(bvpv_access_writable(BVPV_ACC_OWO));
    TEST_ASSERT(!bvpv_access_writable(BVPV_ACC_ROO));
    TEST_ASSERT(!bvpv_access_writable(BVPV_ACC_OOO));

    TEST_ASSERT(bvpv_access_eeprom_persistent(BVPV_ACC_RWA));
    TEST_ASSERT(!bvpv_access_eeprom_persistent(BVPV_ACC_RWO));
    TEST_ASSERT(!bvpv_access_eeprom_persistent(BVPV_ACC_ROO));
}

/* 用例：地址合成。
 * 功能：类/实例/属性三字节合成 24 位地址。
 * 预期：bvpv_addr(0x69,0x01,0x03)=0x690103；(0x68,0x01,0xFE)=0x6801FE。
 * 判据：返回值等于期望的 24 位地址。 */
static void test_addr(void)
{
    TEST_ASSERT_EQ_U(0x690103u, bvpv_addr(0x69u, 0x01u, 0x03u));
    TEST_ASSERT_EQ_U(0x6801FEu, bvpv_addr(0x68u, 0x01u, 0xFEu));
}

/* 用例：类型感知值比较（写范围校验的基础）。
 * 功能：各数值类型按各自语义比较 LE 字节；非数值类型（TEXT）返回 0（不可比较）。
 * 预期：UINT8 5<10；UINT16 0x1234<0x1235；INT32 -5<+5（含负数）；FLOAT32 50.0<100.0；
 *       相等返回 0；TEXT8 返回 0。
 * 判据：bvpv_value_compare 返回值的符号符合上述（负/正/零）。 */
static void test_value_compare(void)
{
    uint8_t u8a = 5u, u8b = 10u;
    uint8_t f50[4]  = { 0x00u, 0x00u, 0x48u, 0x42u };  /* 50.0f  = 0x42480000 */
    uint8_t f100[4] = { 0x00u, 0x00u, 0xC8u, 0x42u };  /* 100.0f = 0x42C80000 */
    uint8_t i32neg[4] = { 0xFBu, 0xFFu, 0xFFu, 0xFFu }; /* -5     = 0xFFFFFFFB */
    uint8_t i32pos[4] = { 0x05u, 0x00u, 0x00u, 0x00u }; /* +5 */
    uint8_t u16a[2] = { 0x34u, 0x12u };                 /* 0x1234 */
    uint8_t u16b[2] = { 0x35u, 0x12u };                 /* 0x1235 */

    TEST_ASSERT(bvpv_value_compare(BVPV_TYPE_UINT8, &u8a, &u8b) < 0);
    TEST_ASSERT(bvpv_value_compare(BVPV_TYPE_UINT8, &u8b, &u8a) > 0);
    TEST_ASSERT(bvpv_value_compare(BVPV_TYPE_UINT8, &u8a, &u8a) == 0);

    TEST_ASSERT(bvpv_value_compare(BVPV_TYPE_UINT16, u16a, u16b) < 0);

    TEST_ASSERT(bvpv_value_compare(BVPV_TYPE_INT32, i32neg, i32pos) < 0);
    TEST_ASSERT(bvpv_value_compare(BVPV_TYPE_INT32, i32pos, i32neg) > 0);

    TEST_ASSERT(bvpv_value_compare(BVPV_TYPE_FLOAT32, f50, f100) < 0);
    TEST_ASSERT(bvpv_value_compare(BVPV_TYPE_FLOAT32, f100, f50) > 0);
    TEST_ASSERT(bvpv_value_compare(BVPV_TYPE_FLOAT32, f50, f50) == 0);

    /* 不可比较类型返回 0 */
    TEST_ASSERT(bvpv_value_compare(BVPV_TYPE_TEXT8, u16a, u16b) == 0);
}

int main(void)
{
    TEST_RUN(test_find);
    TEST_RUN(test_type_size);
    TEST_RUN(test_elem_size);
    TEST_RUN(test_access);
    TEST_RUN(test_addr);
    TEST_RUN(test_value_compare);
    TEST_MAIN_END();
}
