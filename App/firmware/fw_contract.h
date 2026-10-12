#ifndef FW_CONTRACT_H
#define FW_CONTRACT_H
#include <stdbool.h>
#include <stdint.h>
#define FW_HEADER_BASE 0x08010000u
#define FW_APP_BASE 0x08010200u
#define FW_FLASH_END 0x08080000u
#define FW_APP_CAPACITY (FW_FLASH_END - FW_APP_BASE)
#define FW_MANIFEST_SIZE 256u
#define FW_MANIFEST_SIGNED_SIZE 192u
#define FW_BLOCK_SIZE 1024u
#define FW_INFO_SIZE 149u
#define FW_COMMIT_ADDRESS (FW_HEADER_BASE + 508u)
#define FW_COMMIT_VALUE 0xA55AC33Cu
#define FW_TARGET "BV500-F446"
#define FW_BOOT_CONTRACT 1u
#define FW_MODE_APP 1u
#define FW_MODE_BOOT 2u

#define FW_SVC_INFO 0x13u
#define FW_SVC_BEGIN 0x20u
#define FW_SVC_BLOCK 0x21u
#define FW_SVC_FINISH 0x22u
#define FW_SVC_RUN_APP 0x23u
#define FW_ERR_SIGNATURE 0x0Cu
#define FW_ERR_TARGET 0x0Du
#define FW_ERR_FLASH 0x0Eu
#define FW_ERR_DIGEST 0x0Fu

typedef enum {
    FW_IDLE = 0, FW_AWAIT_ERASE = 1, FW_ERASING = 2, FW_RECEIVING = 3,
    FW_VERIFYING = 4, FW_COMMITTED = 5, FW_FAILED = 6
} fw_update_state_t;

typedef enum { FW_ACTION_NONE = 0, FW_ACTION_ERASE, FW_ACTION_RESET } fw_update_action_t;

typedef struct {
    void *ctx;
    bool (*read)(void *, uint32_t address, uint8_t *, uint32_t len);
    bool (*program)(void *, uint32_t address, const uint8_t *, uint32_t len);
    bool (*erase_sector)(void *, uint8_t sector);
} fw_flash_io_t;

#endif
