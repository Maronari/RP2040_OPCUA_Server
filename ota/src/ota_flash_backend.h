/**
 * @file
 * @brief Target-specific Flash erase/program backend interface used by
 *        `ota/src/ota_flash.c`.
 *
 * Responsibilities:
 * - Declare the two low-level Flash primitives (`ota_flash_backend_erase()`,
 *   `ota_flash_backend_program()`) that all higher-level OTA Flash
 *   operations (state record I/O, Bank B staging) are built on.
 *
 * Ownership:
 * No state; this is a pure function-pointer-free vtable-by-linkage
 * abstraction — exactly one implementation is compiled into any given
 * firmware image (see below), never both.
 *
 * Concurrency:
 * Implementation-defined; see the specific backend
 * (`ota_flash_backend_application.c` or `bootloader/main.c`) for its
 * synchronization guarantees.
 */

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

/**
 * @brief Erases @p byte_count bytes of Flash starting at @p flash_offset.
 *
 * @param flash_offset Flash offset (relative to XIP base), must be a
 *        multiple of the Flash erase-block size.
 * @param byte_count Number of bytes to erase, must be a non-zero multiple
 *        of the Flash erase-block size.
 * @return `OTA_FLASH_STATUS_OK` on success;
 *         `OTA_FLASH_STATUS_INVALID_ARGUMENT` if @p byte_count is 0 or
 *         either argument is not erase-block aligned;
 *         `OTA_FLASH_STATUS_FLASH_OP_FAILED` if any underlying erase
 *         operation fails.
 * @note Implementations erase sector-by-sector; a failure partway through a
 *       multi-sector request leaves the already-erased sectors erased (no
 *       rollback).
 * @note Blocking; implementations may briefly disable interrupts and/or
 *       pause the opposite core while Flash is busy (XIP is unavailable
 *       during the physical erase).
 */
ota_flash_status_t ota_flash_backend_erase(uint32_t flash_offset, size_t byte_count);

/**
 * @brief Programs @p byte_count bytes of @p data into Flash starting at
 *        @p flash_offset.
 *
 * @param flash_offset Flash offset (relative to XIP base), must be a
 *        multiple of the Flash program-page size. The target range must
 *        already be erased (0xFF) — this function does not erase.
 * @param data Source bytes to program. Must not be NULL.
 * @param byte_count Number of bytes to program, must be a non-zero multiple
 *        of the Flash program-page size.
 * @return `OTA_FLASH_STATUS_OK` on success;
 *         `OTA_FLASH_STATUS_INVALID_ARGUMENT` if @p data is NULL,
 *         @p byte_count is 0, or either @p flash_offset/@p byte_count is not
 *         page-aligned; `OTA_FLASH_STATUS_FLASH_OP_FAILED` if any underlying
 *         program operation fails.
 * @note Implementations program page-by-page; a failure partway through a
 *       multi-page request leaves the already-programmed pages programmed
 *       (no rollback).
 * @note Blocking; see `ota_flash_backend_erase()` for the same XIP/interrupt
 *       caveats.
 */
ota_flash_status_t ota_flash_backend_program(uint32_t flash_offset,
                                             const uint8_t *data,
                                             size_t byte_count);

#endif
