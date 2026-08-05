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
