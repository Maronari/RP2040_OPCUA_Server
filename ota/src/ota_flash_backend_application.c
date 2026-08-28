/**
 * @file
 * @brief Application-side implementation of the OTA Flash backend
 *        (`ota_flash_backend.h`), used by the scheduled FreeRTOS
 *        application (never the bootloader).
 *
 * Responsibilities:
 * - Translate `ota_flash_backend_erase()`/`ota_flash_backend_program()`
 *   calls into sector-erase / page-program requests against
 *   `drivers/flash_operation_coordinator`, the shared serialization point
 *   for all Flash access on the application side.
 *
 * Ownership:
 * Stateless; holds no data of its own.
 *
 * Concurrency:
 * Safe to call from any FreeRTOS task context: `flash_operation_coordinator`
 * (see `drivers/include/drivers/flash_operation_coordinator.h`) serializes
 * every erase/program call across the whole application via a mutex and
 * blocks the calling task while Flash is busy. This is compiled only when
 * `OTA_FLASH_BACKEND_APPLICATION` is linked; the bootloader links its own
 * bare-metal implementation in `bootloader/main.c` instead (see
 * `ota_flash_backend.h`).
 */

#include "ota_flash_backend.h"

#include "drivers/flash_operation_coordinator.h"
#include "ota_layout.h"

ota_flash_status_t ota_flash_backend_erase(uint32_t flash_offset, size_t byte_count)
{
    if (byte_count == 0u ||
        (flash_offset % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES) != 0u ||
        (byte_count % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES) != 0u)
    {
        return OTA_FLASH_STATUS_INVALID_ARGUMENT;
    }

    for (size_t offset = 0u; offset < byte_count; offset += OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES)
    {
        if (flash_operation_erase_sector(flash_offset + (uint32_t)offset) != FLASH_OPERATION_OK)
        {
            return OTA_FLASH_STATUS_FLASH_OP_FAILED;
        }
    }
    return OTA_FLASH_STATUS_OK;
}

ota_flash_status_t ota_flash_backend_program(uint32_t flash_offset,
                                             const uint8_t *data,
                                             size_t byte_count)
{
    if (data == NULL || byte_count == 0u ||
        (flash_offset % OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES) != 0u ||
        (byte_count % OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES) != 0u)
    {
        return OTA_FLASH_STATUS_INVALID_ARGUMENT;
    }

    for (size_t offset = 0u; offset < byte_count; offset += OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES)
    {
        if (flash_operation_program_page(flash_offset + (uint32_t)offset, data + offset) !=
            FLASH_OPERATION_OK)
        {
            return OTA_FLASH_STATUS_FLASH_OP_FAILED;
        }
    }
    return OTA_FLASH_STATUS_OK;
}
