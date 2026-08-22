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

/**
 * @brief Initialize the Flash-operation coordinator.
 *
 * Creates the internal FreeRTOS mutex that serializes every subsequent
 * erase/program call across the whole firmware. Must run before any
 * application Flash-writing service (config store, security store,
 * maintenance journal, history Flash backend) is initialized.
 *
 * @return true on success.
 */
bool flash_operation_coordinator_init(void);

/**
 * @brief Erase one 4 KiB Flash sector.
 *
 * Serialized against every other flash_operation_erase_sector()/
 * flash_operation_program_page() call via the coordinator's mutex; blocks
 * the calling task until the mutex and `flash_safe_execute()` complete.
 * Same-task re-entrant acquisition is detected and rejected rather than
 * deadlocking.
 *
 * @param flash_offset Sector-aligned offset from the start of Flash.
 * @return FLASH_OPERATION_OK on success; FLASH_OPERATION_TIMEOUT if the
 *         mutex could not be acquired in time; FLASH_OPERATION_NESTED on
 *         same-task re-entrant acquisition; FLASH_OPERATION_SAFE_EXEC_FAILED
 *         if the underlying `flash_safe_execute()` call failed.
 */
flash_operation_result_t flash_operation_erase_sector(uint32_t flash_offset);

/**
 * @brief Program one Flash page (must be freshly erased).
 *
 * @p page must point to a complete `FLASH_PAGE_SIZE` buffer owned by the
 * caller in internal SRAM, and must remain readable for the whole callback
 * while XIP Flash is unavailable. This is a caller-owned residency
 * invariant: rejecting addresses inside the executing firmware image (see
 * below) does not prove that any other pointer is internal SRAM — PSRAM is
 * not part of this contract. Future buffers must therefore be statically
 * owned in internal SRAM unless platform PSRAM-during-XIP-unavailability
 * support is proven first.
 *
 * Rejects any @p page pointer that overlaps the currently executing
 * firmware image, since that memory becomes unreadable mid-program while
 * XIP is suspended. Serialized identically to
 * flash_operation_erase_sector().
 *
 * @param flash_offset Page-aligned destination offset from the start of
 *        Flash.
 * @param page Source buffer, exactly `FLASH_PAGE_SIZE` bytes, in internal
 *        SRAM.
 * @return FLASH_OPERATION_OK on success; FLASH_OPERATION_INVALID_ARGUMENT if
 *         @p page overlaps the executing image; other codes as in
 *         flash_operation_erase_sector().
 */
flash_operation_result_t flash_operation_program_page(uint32_t flash_offset, const uint8_t *page);

#endif
