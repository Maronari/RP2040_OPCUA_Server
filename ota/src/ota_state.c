#include "ota/ota_state.h"

#include <stddef.h>
#include <string.h>

uint32_t ota_crc32_compute(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;

    if (bytes == NULL)
    {
        return 0u;
    }

    for (size_t index = 0; index < size; ++index)
    {
        crc ^= bytes[index];
        for (uint32_t bit = 0; bit < 8u; ++bit)
        {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }

    return ~crc;
}

void ota_state_reset_record(ota_state_record_t *record)
{
    if (record == NULL)
    {
        return;
    }

    memset(record, 0, sizeof(*record));
    record->magic = OTA_STATE_MAGIC;
    record->format_version = OTA_STATE_FORMAT_VERSION;
    record->state = OTA_IMAGE_STATE_IDLE;
    record->last_error = OTA_IMAGE_ERROR_NONE;
    ota_state_finalize(record);
}

void ota_state_finalize(ota_state_record_t *record)
{
    if (record == NULL)
    {
        return;
    }

    record->record_crc32 = ota_crc32_compute(record, offsetof(ota_state_record_t, record_crc32));
}

bool ota_state_validate(const ota_state_record_t *record)
{
    if (record == NULL)
    {
        return false;
    }

    if (record->magic != OTA_STATE_MAGIC || record->format_version != OTA_STATE_FORMAT_VERSION)
    {
        return false;
    }

    return record->record_crc32 == ota_crc32_compute(record, offsetof(ota_state_record_t, record_crc32));
}

const char *ota_state_to_string(uint16_t state)
{
    switch ((ota_image_state_t)state)
    {
    case OTA_IMAGE_STATE_IDLE:
        return "IDLE";
    case OTA_IMAGE_STATE_STAGED_IN_BANK_B:
        return "STAGED_IN_BANK_B";
    case OTA_IMAGE_STATE_COPYING_TO_BANK_A:
        return "COPYING_TO_BANK_A";
    case OTA_IMAGE_STATE_READY_TO_BOOT:
        return "READY_TO_BOOT";
    case OTA_IMAGE_STATE_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

const char *ota_error_to_string(uint32_t error_code)
{
    switch ((ota_image_error_t)error_code)
    {
    case OTA_IMAGE_ERROR_NONE:
        return "NONE";
    case OTA_IMAGE_ERROR_INVALID_RECORD:
        return "INVALID_RECORD";
    case OTA_IMAGE_ERROR_INVALID_STAGED_IMAGE:
        return "INVALID_STAGED_IMAGE";
    case OTA_IMAGE_ERROR_COPY_FAILED:
        return "COPY_FAILED";
    case OTA_IMAGE_ERROR_VERIFY_FAILED:
        return "VERIFY_FAILED";
    case OTA_IMAGE_ERROR_BOOT_FAILED:
        return "BOOT_FAILED";
    default:
        return "UNKNOWN";
    }
}