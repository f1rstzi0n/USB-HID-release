#include "fw_image.h"
#include "mbedtls/sha256.h"
#include <string.h>
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put16(uint8_t *p, uint16_t n) { p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); }
static void put32(uint8_t *p, uint32_t n) {
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); p[2] = (uint8_t)(n >> 16); p[3] = (uint8_t)(n >> 24);
}

uint8_t fw_manifest_verify(const uint8_t h[FW_MANIFEST_SIZE], const uint8_t key[64])
{
    static const uint8_t target[16] = FW_TARGET;
    uint8_t digest[32];
    unsigned i;
    bool nul = false;
    if (!h || !key) return FW_ERR_SIGNATURE;
    if (memcmp(h, "AFW1", 4) || get16(h + 4) != FW_MANIFEST_SIZE || get16(h + 6) != 1u)
        return 0x08u;
    if (memcmp(h + 8, target, sizeof(target))) return FW_ERR_TARGET;
    if (get32(h + 24) != FW_APP_BASE || get32(h + 28) < 8u || get32(h + 28) > FW_APP_CAPACITY)
        return 0x03u;
    if (get16(h + 96) != FW_BOOT_CONTRACT) return 0x0Au;
    for (i = 98; i < FW_MANIFEST_SIGNED_SIZE; ++i) if (h[i]) return 0x08u;
    for (i = 32; i < 64; ++i)
    {
        if (!h[i]) nul = true;
        else if (nul || h[i] < 0x20u || h[i] > 0x7Eu) return 0x08u;
    }
    if (!h[32] || !nul) return 0x08u;
    if (!fw_sha256(h, FW_MANIFEST_SIGNED_SIZE, digest)) return FW_ERR_SIGNATURE;
    return fw_p256_verify(key, digest, h + FW_MANIFEST_SIGNED_SIZE);
}
bool fw_image_read(const fw_image_t *f, uint32_t a, uint8_t *p, uint32_t n) {
    return f->io.read && a >= FW_HEADER_BASE && a <= FW_FLASH_END &&
        n <= FW_FLASH_END - a && f->io.read(f->io.ctx, a, p, n);
}
uint8_t fw_image_check(const fw_image_t *f, const uint8_t h[FW_MANIFEST_SIZE]) {
    mbedtls_sha256_context sha;
    uint8_t block[256], digest[32], vectors[8];
    uint32_t offset, length = get32(h + 28), sp, reset;
    int result;
    mbedtls_sha256_init(&sha);
    result = mbedtls_sha256_starts(&sha, 0);
    for (offset = 0; result == 0 && offset < length; offset += sizeof(block)) {
        uint32_t n = length - offset;
        if (n > sizeof(block)) n = sizeof(block);
        if (!fw_image_read(f, FW_APP_BASE + offset, block, n)) {
            mbedtls_sha256_free(&sha);
            return FW_ERR_FLASH;
        }
        result = mbedtls_sha256_update(&sha, block, n);
        if (!result) fw_validation_progress();
    }
    if (result == 0) result = mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (result || memcmp(digest, h + 64, 32)) return FW_ERR_DIGEST;
    if (!fw_image_read(f, FW_APP_BASE, vectors, sizeof(vectors))) return FW_ERR_FLASH;
    sp = get32(vectors);
    reset = get32(vectors + 4);
    if ((sp & 7u) || sp <= 0x20000000u || sp > 0x2001FFF8u || !(reset & 1u) ||
        (reset & ~1u) < FW_APP_BASE || (reset & ~1u) >= FW_APP_BASE + length) return 0x03u;
    return 0u;
}
void fw_image_init(fw_image_t *f, const fw_flash_io_t *io, const uint8_t key[64], const uint8_t uid[12]) {
    uint8_t commit[4], error;
    memset(f, 0, sizeof(*f));
    if (io) f->io = *io;
    if (key) memcpy(f->public_key, key, sizeof(f->public_key));
    if (uid) memcpy(f->uid, uid, sizeof(f->uid));
    if (!fw_image_read(f, FW_COMMIT_ADDRESS, commit, sizeof(commit)) || get32(commit) != FW_COMMIT_VALUE)
        return;
    if (!fw_image_read(f, FW_HEADER_BASE, f->active, sizeof(f->active))) {
        f->last_error = FW_ERR_FLASH;
        return;
    }
    error = fw_manifest_verify(f->active, f->public_key);
    if (!error) error = fw_image_check(f, f->active);
    if (!error && fw_sha256(f->active, FW_MANIFEST_SIGNED_SIZE, f->active_hash)) {
        f->valid_application = true;
    } else {
        f->last_error = error ? error : FW_ERR_SIGNATURE;
        memset(f->active, 0, sizeof(f->active));
    }
}
void fw_image_info(const fw_image_t *f, uint8_t mode, uint8_t out[FW_INFO_SIZE]) {
    memset(out, 0, FW_INFO_SIZE);
    out[1] = mode;
    out[2] = FW_IDLE;
    out[3] = f->valid_application ? 1u : 0u;
    out[4] = f->last_error;
    memcpy(out + 5, f->uid, sizeof(f->uid));
    memcpy(out + 17, FW_TARGET, sizeof(FW_TARGET));
    put16(out + 33, FW_BOOT_CONTRACT);
    put16(out + 35, FW_BLOCK_SIZE);
    put32(out + 37, FW_APP_BASE);
    put32(out + 41, FW_APP_CAPACITY);

    if (f->valid_application) {
        memcpy(out + 49, f->active_hash, 32);
        memcpy(out + 113, f->active + 32, 32);
        memcpy(out + 145, f->active + 28, 4);
    }
}
