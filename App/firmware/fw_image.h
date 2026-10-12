/* 只读固件身份及已提交镜像验证。 */
#ifndef FW_IMAGE_H
#define FW_IMAGE_H
#include "fw_contract.h"
#include "fw_crypto.h"
typedef struct {
    fw_flash_io_t io;
    uint8_t public_key[64], uid[12];
    uint8_t active[FW_MANIFEST_SIZE], active_hash[32];
    uint8_t last_error;
    bool valid_application;
} fw_image_t;
uint8_t fw_manifest_verify(const uint8_t [FW_MANIFEST_SIZE], const uint8_t [64]);
bool fw_image_read(const fw_image_t *, uint32_t, uint8_t *, uint32_t);
uint8_t fw_image_check(const fw_image_t *, const uint8_t [FW_MANIFEST_SIZE]);
void fw_image_init(fw_image_t *, const fw_flash_io_t *, const uint8_t [64], const uint8_t [12]);
void fw_image_info(const fw_image_t *, uint8_t, uint8_t [FW_INFO_SIZE]);
#endif
