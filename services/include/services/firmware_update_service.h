#ifndef SERVICES_FIRMWARE_UPDATE_SERVICE_H
#define SERVICES_FIRMWARE_UPDATE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ota/ota_state.h"

#define FIRMWARE_UPDATE_SERVICE_MAX_CHUNK_BYTES 4096u

typedef enum
{
    FIRMWARE_UPDATE_RESULT_NONE = 0,
    FIRMWARE_UPDATE_RESULT_OK = 1,
    FIRMWARE_UPDATE_RESULT_BUSY = 2,
    FIRMWARE_UPDATE_RESULT_NOT_ACTIVE = 3,
    FIRMWARE_UPDATE_RESULT_INVALID_ARGUMENT = 4,
    FIRMWARE_UPDATE_RESULT_CHUNK_TOO_LARGE = 5,
    FIRMWARE_UPDATE_RESULT_FLASH_ERROR = 6,
    FIRMWARE_UPDATE_RESULT_VERIFY_FAILED = 7,
    FIRMWARE_UPDATE_RESULT_INTERNAL_ERROR = 8,
} firmware_update_service_result_t;

typedef struct
{
    bool session_active;
    uint32_t expected_image_size;
    uint32_t expected_image_crc32;
    uint32_t version_number;
    uint32_t bytes_written;
    uint32_t remaining_bytes;
    firmware_update_service_result_t last_result;
    bool ota_state_valid;
    ota_state_record_t ota_state;
} firmware_update_service_status_t;

void firmware_update_service_init(void);
firmware_update_service_result_t firmware_update_service_begin(uint32_t image_size,
                                                              uint32_t image_crc32,
                                                              uint32_t version_number);
firmware_update_service_result_t firmware_update_service_write_chunk(const uint8_t *data, size_t size);
firmware_update_service_result_t firmware_update_service_finalize(void);
firmware_update_service_result_t firmware_update_service_abort(void);
void firmware_update_service_get_status(firmware_update_service_status_t *out_status);
const char *firmware_update_service_result_to_string(firmware_update_service_result_t result);

#endif
