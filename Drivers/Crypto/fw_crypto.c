#include "fw_crypto.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/memory_buffer_alloc.h"
#include "mbedtls/sha256.h"
#include <string.h>
/* 固定 arena 不依赖应用堆，也不接受私钥；验签串行执行。 */
static union { uint64_t align; uint8_t bytes[16384]; } crypto_arena;
static bool crypto_ready;
bool fw_sha256(const uint8_t *p, uint32_t n, uint8_t hash[32]) {
    return mbedtls_sha256(p, n, hash, 0) == 0;
}

static mbedtls_ecp_group verify_group;
static mbedtls_ecp_point verify_q;
static mbedtls_mpi verify_r, verify_s;
static mbedtls_ecdsa_restart_ctx verify_restart;
static uint8_t verify_digest[32];
static bool verify_active;
void fw_p256_cancel(void)
{
    if (!verify_active) return;
    mbedtls_ecdsa_restart_free(&verify_restart);
    mbedtls_mpi_free(&verify_s);
    mbedtls_mpi_free(&verify_r);
    mbedtls_ecp_point_free(&verify_q);
    mbedtls_ecp_group_free(&verify_group);
    verify_active = false;
}
uint8_t fw_p256_start(const uint8_t key[64], const uint8_t digest[32], const uint8_t signature[64])
{
    if (!key || !digest || !signature || verify_active) return FW_CRYPTO_INVALID;
    if (!crypto_ready)
    {
        mbedtls_memory_buffer_alloc_init(crypto_arena.bytes, sizeof(crypto_arena.bytes));
        crypto_ready = true;
    }
    mbedtls_ecp_group_init(&verify_group);
    mbedtls_ecp_point_init(&verify_q);
    mbedtls_mpi_init(&verify_r);
    mbedtls_mpi_init(&verify_s);
    mbedtls_ecdsa_restart_init(&verify_restart);
    verify_active = true;
    memcpy(verify_digest, digest, 32);
    if (mbedtls_ecp_group_load(&verify_group, MBEDTLS_ECP_DP_SECP256R1) ||
        mbedtls_mpi_read_binary(&verify_q.MBEDTLS_PRIVATE(X), key, 32) ||
        mbedtls_mpi_read_binary(&verify_q.MBEDTLS_PRIVATE(Y), key + 32, 32) ||
        mbedtls_mpi_lset(&verify_q.MBEDTLS_PRIVATE(Z), 1) ||
        mbedtls_ecp_check_pubkey(&verify_group, &verify_q) ||
        mbedtls_mpi_read_binary(&verify_r, signature, 32) ||
        mbedtls_mpi_read_binary(&verify_s, signature + 32, 32))
    {
        fw_p256_cancel();
        return FW_CRYPTO_INVALID;
    }
    /* 有限运算预算，具体墙钟响应仍由实板验收校准。 */
    mbedtls_ecp_set_max_ops(64);
    return 0;
}
int fw_p256_poll(void)
{
    int result;
    if (!verify_active) return -1;
    result = mbedtls_ecdsa_verify_restartable(&verify_group, verify_digest, 32,
        &verify_q, &verify_r, &verify_s, &verify_restart);
    if (result == MBEDTLS_ERR_ECP_IN_PROGRESS) return 1;
    fw_p256_cancel();
    return result == 0 ? 0 : -1;
}
uint8_t fw_p256_verify(const uint8_t key[64], const uint8_t digest[32], const uint8_t signature[64])
{
    int result;
    if (fw_p256_start(key, digest, signature)) return FW_CRYPTO_INVALID;
    do { result = fw_p256_poll(); fw_validation_progress(); } while (result > 0);
    return result == 0 ? 0 : FW_CRYPTO_INVALID;
}

__attribute__((weak)) void fw_validation_progress(void) {}
