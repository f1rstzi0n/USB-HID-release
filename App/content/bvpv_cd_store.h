/* 单槽内容仓：硬件 adapter 只负责有界 I/O，提交与恢复语义集中在本模块。 */
#ifndef BVPV_CD_STORE_H
#define BVPV_CD_STORE_H
#include "bvpv_cd_meta.h"

#define BVPV_CD_INFO_DATA_LEN 24u
#define BVPV_CD_IDLE_TIMEOUT_MS 30000u

typedef enum {
    BVPV_CD_READ_OK = 0,
    BVPV_CD_READ_BUSY = 1,
    BVPV_CD_READ_ERROR = 2
} bvpv_cd_read_result_t;

typedef struct
{
    void *ctx;
    bvpv_cd_read_result_t (*read)(void *, uint32_t, uint8_t *, uint32_t);
    bool (*erase_start)(void *, uint32_t);
    bool (*program_start)(void *, uint32_t, const uint8_t *, uint16_t);
    /* 0=完成，1=进行中，-1=器件故障/超时；adapter 必须有超时。 */
    int (*poll)(void *);
    bool (*reserve)(void *, bool);
} bvpv_cd_io_t;

typedef enum
{
    BVPV_CD_IDLE = 0,
    BVPV_CD_RECEIVING = 1,
    BVPV_CD_WRITING = 2,
    BVPV_CD_VERIFYING = 3,
    BVPV_CD_COMMITTING = 4,
    BVPV_CD_CLEANUP = 5,
    BVPV_CD_FAULT = 6
} bvpv_cd_update_state_t;

typedef struct
{
    bvpv_cd_io_t io;
    bvpv_cd_record_t active, candidate;
    bool available, fault, session, cancelled, reply_ready, committed, preparing_begin, booting;
    uint8_t record_index, target_record, phase, last_error, boot_phase;
    uint8_t boot_raw[2][BVPV_CD_RECORD_LEN];
    uint32_t high_sequence, offset, cursor, crc, last_activity;
    uint16_t page_offset, reply_len;
    uint8_t block[BVPV_CD_BLOCK_SIZE], scratch[256], meta[BVPV_CD_RECORD_LEN], reply[BVPV_CD_INFO_DATA_LEN + 1u];
} bvpv_cd_store_t;

void bvpv_cd_store_init(bvpv_cd_store_t *, const bvpv_cd_io_t *);
/* 启动仅排队校验，不擦写；USB 启动后每 task 至多读取 256B。返回值为当前 ready。 */
bool bvpv_cd_store_boot(bvpv_cd_store_t *);
bool bvpv_cd_store_ready(const bvpv_cd_store_t *);
bvpv_cd_read_result_t bvpv_cd_store_read(bvpv_cd_store_t *, uint32_t offset, uint8_t *, uint32_t len);
bvpv_cd_update_state_t bvpv_cd_store_state(const bvpv_cd_store_t *);
/* 一个未完成命令；载荷在返回前复制，调用方无需长期持有请求缓冲。 */
void bvpv_cd_store_request(bvpv_cd_store_t *, uint8_t svc, const uint8_t *, uint16_t len,
                           uint32_t now);
void bvpv_cd_store_task(bvpv_cd_store_t *, uint32_t now);
bool bvpv_cd_store_take_reply(bvpv_cd_store_t *, uint8_t *, uint16_t *);
void bvpv_cd_store_cancel(bvpv_cd_store_t *);
#endif
