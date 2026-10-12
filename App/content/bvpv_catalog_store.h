#ifndef BVPV_CATALOG_STORE_H
#define BVPV_CATALOG_STORE_H
#include "bvpv_catalog_format.h"
#include "bvpv_cd_store.h"
#include "bvpv_catalog.h"
#include "mbedtls/sha256.h"

typedef struct {
    uint16_t index;
    uint8_t flags, reserved;
    uint8_t min[8], max[8];
} bvpv_catalog_range_t;
typedef struct {
    bvpv_cd_io_t io;
    bvpv_service_ctx_t *runtime;
    const char *app_identity;
    uint8_t key[64], contract_sha[32];
    uint8_t header[512], active_header[512], meta[96], boot_meta[2][96];
    uint8_t block[3500], record[3500], scratch[256], reply[8];
    uint8_t record_prefix[2];
    uint64_t text_hashes[BVPV_CATALOG_MAX_PARAMS];
    uint16_t text_count;
    uint8_t seen[(BVPV_CATALOG_MAX_PARAMS + 7u) / 8u];
    uint8_t policy_access[BVPV_CATALOG_MAX_PARAMS], policy_flags[BVPV_CATALOG_MAX_PARAMS];
    uint16_t param_ordinal[BVPV_CATALOG_MAX_PARAMS];
    bvpv_catalog_range_t ranges[BVPV_CATALOG_MAX_RANGES];
    mbedtls_sha256_context body_hash, expected_index_hash, actual_index_hash;
    uint32_t generation, session_id, next_offset, cursor, block_start, last_activity;
    uint32_t record_start, signature_stage, contract_cursor;
    uint16_t block_len, block_cursor, record_len, record_cursor, ordinal, group_count, tree_params;
    uint16_t ui_count, range_count, candidate_range_count, reply_len, io_count;
    uint8_t phase, section, meta_index, meta_target, meta_kind, last_error;
    bool available, fault, booting, session, preflight, preflight_ready, cancelled;
    bool reply_ready, committed, apply_policy, signing, hashes_started, header_programming;
} bvpv_catalog_store_t;

/* 初始化不访问 Flash；boot/task 在 USB 可用后逐步校验，每 task 至多 256B。 */
void bvpv_catalog_store_init(bvpv_catalog_store_t *, const bvpv_cd_io_t *,
    bvpv_service_ctx_t *, const uint8_t key[64], const char *app_identity);
void bvpv_catalog_store_boot(bvpv_catalog_store_t *);
void bvpv_catalog_store_task(bvpv_catalog_store_t *, uint32_t now);
bool bvpv_catalog_store_ready(const bvpv_catalog_store_t *);
bool bvpv_catalog_store_request(bvpv_catalog_store_t *, uint8_t svc,
    const uint8_t *, uint16_t len, uint8_t *out, uint16_t *out_len, uint16_t cap, uint32_t now);
bool bvpv_catalog_store_take_reply(bvpv_catalog_store_t *, uint8_t *, uint16_t *, uint16_t cap);
void bvpv_catalog_store_cancel(bvpv_catalog_store_t *);
uint8_t bvpv_catalog_store_policy(void *, uint16_t, const bvpv_param_t *, bvpv_effective_attr_t *);
/* 有界读取已验证的静态 wire 记录，索引为包内目录顺序。 */
uint8_t bvpv_catalog_store_record(bvpv_catalog_store_t *, uint8_t section,
    uint16_t index, uint8_t *out, uint16_t *len, uint16_t cap);
#endif
