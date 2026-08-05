#ifndef DRIVERS_FLASH_OPERATION_COORDINATOR_H
#define DRIVERS_FLASH_OPERATION_COORDINATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    FLASH_OPERATION_OK = 0,
    FLASH_OPERATION_NOT_INITIALIZED,
    FLASH_OPERATION_INVALID_ARGUMENT,
    FLASH_OPERATION_NESTED,
    FLASH_OPERATION_TIMEOUT,
    FLASH_OPERATION_SAFE_EXEC_FAILED
} flash_operation_result_t;

/* Must run before any application Flash-writing service is initialized. */
bool flash_operation_coordinator_init(void);
flash_operation_result_t flash_operation_erase_sector(uint32_t flash_offset);
/*
 * page must point to a complete FLASH_PAGE_SIZE buffer owned by the caller in
 * internal SRAM. It must remain readable for the whole callback while XIP
 * Flash is unavailable. This is a caller-owned residency invariant:
 * rejecting executable-Flash addresses does not prove that any other pointer
 * is internal SRAM. PSRAM is not
 * part of this contract. Future journal page buffers must therefore be
 * statically owned in internal SRAM unless platform support is proven first.
 */
flash_operation_result_t flash_operation_program_page(uint32_t flash_offset, const uint8_t *page);

#endif
