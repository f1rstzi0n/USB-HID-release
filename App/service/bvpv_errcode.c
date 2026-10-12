#include "bvpv_errcode.h"

const bvpv_errtext_t bvpv_errtext_table[] = {
    { BVPV_ERR_OK,            "OK" },
    { BVPV_ERR_NOT_FOUND,     "Parameter not found" },
    { BVPV_ERR_ACCESS_DENIED, "Access denied" },
    { BVPV_ERR_OUT_OF_RANGE,  "Value out of range" },
    { BVPV_ERR_TYPE_MISMATCH, "Type mismatch" },
    { BVPV_ERR_INDEX_OOB,     "Element index out of bounds" },
    { BVPV_ERR_BUSY,          "Device busy" },
    { BVPV_ERR_CRC,           "Checksum failed" },
    { BVPV_ERR_LENGTH,        "Length error" },
    { BVPV_ERR_NOT_SUPPORTED, "Service not supported" },
    { BVPV_ERR_STATE,         "Operation not allowed in current state" },
    { BVPV_ERR_MSC_STATE,     "MSC update state error" },
    { BVPV_ERR_SIGNATURE,     "Firmware signature failed" },
    { BVPV_ERR_TARGET,        "Firmware target mismatch" },
    { BVPV_ERR_FLASH,         "Internal Flash failed" },
    { BVPV_ERR_IMAGE_DIGEST,  "Application digest failed" },
    { BVPV_ERR_UNKNOWN,       "Unknown error" },
};

const uint16_t bvpv_errtext_count =
    (uint16_t)(sizeof(bvpv_errtext_table) / sizeof(bvpv_errtext_table[0]));
