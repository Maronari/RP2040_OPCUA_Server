#ifndef OTA_OTA_STATE_H
#define OTA_OTA_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_STATE_MAGIC 0x3141544Fu /* "OTA1" */
#define OTA_STATE_FORMAT_VERSION 1u

typedef enum {
    OTA_IMAGE_STATE_IDLE = 0u,
    OTA_IMAGE_STATE_STAGED_IN_BANK_B = 1u,
    OTA_IMAGE_STATE_COPYING_TO_BANK_A = 2u,
    OTA_IMAGE_STATE_READY_TO_BOOT = 3u,
    OTA_IMAGE_STATE_FAILED = 4u,
} ota_image_state_t;

typedef enum {
    OTA_IMAGE_ERROR_NONE = 0u,
    OTA_IMAGE_ERROR_INVALID_RECORD = 1u,
    OTA_IMAGE_ERROR_INVALID_STAGED_IMAGE = 2u,
    OTA_IMAGE_ERROR_COPY_FAILED = 3u,
    OTA_IMAGE_ERROR_VERIFY_FAILED = 4u,
    OTA_IMAGE_ERROR_BOOT_FAILED = 5u,
} ota_image_error_t;

typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint16_t state;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t staged_bank_offset;
    uint32_t staged_bank_size;
    uint32_t version_number;
    uint32_t boot_attempts;
    uint32_t last_error;
    uint32_t reserved[6];
    uint32_t record_crc32;
} ota_state_record_t;

uint32_t ota_crc32_compute(const void *data, size_t size);
void ota_state_reset_record(ota_state_record_t *record);
void ota_state_finalize(ota_state_record_t *record);
bool ota_state_validate(const ota_state_record_t *record);
const char *ota_state_to_string(uint16_t state);
const char *ota_error_to_string(uint32_t error_code);

#endif