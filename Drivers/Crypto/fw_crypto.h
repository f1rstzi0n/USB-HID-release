/* SHA-256 与串行 P-256 验签；不依赖应用、Boot 或 RTOS。 */
#ifndef FW_CRYPTO_H
#define FW_CRYPTO_H
#include <stdbool.h>
#include <stdint.h>
#define FW_CRYPTO_INVALID 0x0Cu
bool fw_sha256(const uint8_t *, uint32_t, uint8_t [32]);
uint8_t fw_p256_start(const uint8_t [64], const uint8_t [32], const uint8_t [64]);
int fw_p256_poll(void);
void fw_p256_cancel(void);
uint8_t fw_p256_verify(const uint8_t [64], const uint8_t [32], const uint8_t [64]);
/* 同步验证的有限步骤由平台提供服务点；默认空实现。 */
void fw_validation_progress(void);
#endif
