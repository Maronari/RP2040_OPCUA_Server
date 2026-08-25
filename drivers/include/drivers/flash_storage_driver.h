/**
 * @file
 * @brief Legacy, read-only configuration importer for the pre-slot single
 *        Flash sector layout.
 *
 * This is a read-only compatibility path only: it never erases or programs
 * Flash (unlike flash_operation_coordinator-backed drivers), reading
 * directly from the XIP-mapped legacy sector at the end of Flash. The
 * legacy sector physically overlaps the OTA-state allocation in the
 * current layout, which is exactly why nothing here may ever write to it.
 * See docs/code/persistence.md for the region map and
 * config_service.c:514-539 for the one-time fallback caller.
 */
#ifndef DRIVERS_FLASH_STORAGE_DRIVER_H
#define DRIVERS_FLASH_STORAGE_DRIVER_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Load a legacy configuration payload of an exact expected size.
 *
 * Validates the stored header (magic, version, exact payload_size match)
 * and CRC32 before copying into @p buffer. Not currently called anywhere
 * in this repository (flash_storage_driver_load_partial() is the one
 * production caller); kept for exact-size use cases.
 *
 * @param buffer Destination, must be at least @p size bytes.
 * @param size Exact expected payload size in bytes; a stored payload of any
 *        other size is rejected.
 * @return true only if the header, size, and CRC32 all validate and the
 *         copy succeeded; false otherwise (buffer left unmodified on
 *         failure other than a successful copy).
 * @note Read-only: performs a direct XIP read, never erases or programs.
 */
bool flash_storage_driver_load(void *buffer, size_t size);

/**
 * @brief Load a legacy configuration payload of unknown (bounded) size.
 *
 * Validates the stored header and CRC32, then zero-fills @p buffer before
 * copying the payload (any unused tail bytes are deterministically zero,
 * not left with stale caller data).
 *
 * @param buffer Destination, must be at least @p buffer_size bytes.
 * @param buffer_size Capacity of @p buffer; load fails if the stored
 *        payload is larger than this.
 * @param out_payload_size Optional; set to the actual stored payload size
 *        on success.
 * @return true only if the header and CRC32 validate and the payload fits;
 *         false otherwise.
 * @note Read-only: performs a direct XIP read, never erases or programs.
 *       This is the sole production caller path (config_service.c's
 *       last-resort fallback when both authoritative config-store slots
 *       are invalid).
 */
bool flash_storage_driver_load_partial(void *buffer, size_t buffer_size, size_t *out_payload_size);

#endif
