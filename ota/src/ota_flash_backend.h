#ifndef OTA_FLASH_BACKEND_H
#define OTA_FLASH_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "ota/ota_flash.h"

/*
 * Exactly one target-specific implementation must be linked:
 * OTA_FLASH_BACKEND_APPLICATION for the scheduled application, or the
 * bootloader's bare-metal implementation.
 */
ota_flash_status_t ota_flash_backend_erase(uint32_t flash_offset, size_t byte_count);
ota_flash_status_t ota_flash_backend_program(uint32_t flash_offset,
                                             const uint8_t *data,
                                             size_t byte_count);

#endif
