/* 极简 host 测试框架：零外部依赖，每个测试可执行文件为单一 .c。
 * TEST_ASSERT* 记录失败并继续；main 末尾用 TEST_MAIN_END 汇总并返回退出码。 */
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <string.h>

static int g_checks   = 0;
static int g_failures = 0;

#define TEST_ASSERT(cond)                                                        \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                        \
    } while (0)

#define TEST_ASSERT_EQ_U(expected, actual)                                       \
    do {                                                                         \
        unsigned long e_ = (unsigned long)(expected);                            \
        unsigned long a_ = (unsigned long)(actual);                              \
        g_checks++;                                                              \
        if (e_ != a_) {                                                          \
            g_failures++;                                                        \
            printf("FAIL %s:%d: expected 0x%lX (%lu), got 0x%lX (%lu)\n",       \
                   __FILE__, __LINE__, e_, e_, a_, a_);                          \
        }                                                                        \
    } while (0)

#define TEST_ASSERT_MEMEQ(expected, actual, len)                                 \
    do {                                                                         \
        g_checks++;                                                              \
        if (memcmp((expected), (actual), (len)) != 0) {                          \
            g_failures++;                                                        \
            printf("FAIL %s:%d: memory differs (%d bytes)\n",                   \
                   __FILE__, __LINE__, (int)(len));                              \
        }                                                                        \
    } while (0)

#define TEST_RUN(fn)                                                             \
    do {                                                                         \
        int before_ = g_failures;                                                \
        printf("  %-34s", #fn);                                                  \
        fn();                                                                    \
        printf("%s\n", (g_failures == before_) ? "ok" : "FAILED");              \
    } while (0)

#define TEST_MAIN_END()                                                          \
    do {                                                                         \
        printf("checks=%d failures=%d\n", g_checks, g_failures);                \
        return (g_failures != 0) ? 1 : 0;                                        \
    } while (0)

#endif /* TEST_HARNESS_H */
