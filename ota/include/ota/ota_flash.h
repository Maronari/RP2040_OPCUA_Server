/**
 * @file
 * @brief Public API for the OTA state record I/O and the Bank B (staging)
 *        Flash writer.
 *
 * Responsibilities:
 * - Read/write/reset the single on-Flash `ota_state_record_t` (see
 *   `ota/ota_state.h`).
 * - Erase, program, and CRC32-verify the Bank B staging region.
 * - Provide a stateful, chunked `ota_flash_writer_t` that a caller (the
 *   application's `services/firmware_update_service.c`, driven by the
 *   `BeginFirmwareUpdate`/`WriteFirmwareChunk`/`FinalizeFirmwareUpdate` OPC
 *   UA methods) uses to stream an arbitrarily-chunked firmware image into
 *   Bank B without needing to know the underlying Flash program-page size.
 *
 * Ownership:
 * `ota_flash_writer_t` instances are owned by the caller (one lives inside
 * `firmware_update_service.c`'s session state); this module only mutates the
 * instance passed in by pointer and never allocates or retains one itself.
 *
 * Concurrency:
 * None of these functions are reentrant or task-safe for the *same*
 * `ota_flash_writer_t` or the same Flash region — the caller (currently
 * `firmware_update_service.c`, which allows only one OTA session at a time)
 * is responsible for serializing calls. All Flash access goes through
 * `ota_flash_backend_erase()`/`ota_flash_backend_program()`
 * (`ota_flash_backend.h`), whose application-side implementation is itself
 * serialized by `drivers/flash_operation_coordinator`.
 */

#ifndef OTA_OTA_FLASH_H
#define OTA_OTA_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ota/ota_state.h"

/** Size in bytes of `ota_flash_writer_t::page_buffer`. Must equal the
 * target's Flash program-page size (`OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES`,
 * enforced by a `_Static_assert` in `ota_flash.c`) so that every flushed
 * page is a single well-aligned Flash program operation. */
#define OTA_FLASH_WRITER_PAGE_BUFFER_SIZE 256u

/**
 * @brief Result/error codes returned by every function in this header.
 *
 * `OTA_FLASH_STATUS_OK` is always 0 so callers can treat any non-zero value
 * as failure. See `ota_flash_status_to_string()` for logging.
 */
typedef enum {
    OTA_FLASH_STATUS_OK = 0,
    OTA_FLASH_STATUS_INVALID_ARGUMENT = 1,
    OTA_FLASH_STATUS_OUT_OF_RANGE = 2,
    OTA_FLASH_STATUS_FLASH_OP_FAILED = 3,
    OTA_FLASH_STATUS_VERIFY_FAILED = 4,
    OTA_FLASH_STATUS_WRITER_STATE_ERROR = 5,
} ota_flash_status_t;

/**
 * @brief Stateful chunked writer for staging a firmware image into Bank B.
 *
 * Lifetime/ownership: caller-allocated (typically embedded in
 * `firmware_update_service.c`'s session struct); must be initialized with
 * `ota_flash_writer_init()` before use and started with
 * `ota_flash_writer_begin()` before any `ota_flash_writer_write()` calls.
 * Not safe to share across tasks; exactly one write session should be
 * active per writer instance at a time.
 *
 * @var ota_flash_writer_t::image_size
 * Total expected image size in bytes, fixed at `ota_flash_writer_begin()`.
 * @var ota_flash_writer_t::bytes_written
 * Cumulative bytes accepted via `ota_flash_writer_write()` so far
 * (including bytes still sitting in `page_buffer`, not yet flushed).
 * @var ota_flash_writer_t::buffered_bytes
 * Bytes currently held in `page_buffer`, not yet flushed to Flash.
 * @var ota_flash_writer_t::active
 * True between a successful `ota_flash_writer_begin()` and either
 * `ota_flash_writer_finish()` or a failed write/flush; false otherwise.
 * Guards against use of a not-yet-started or already-finished/failed
 * writer.
 * @var ota_flash_writer_t::page_buffer
 * Accumulates bytes until a full Flash program page
 * (`OTA_FLASH_WRITER_PAGE_BUFFER_SIZE`) is ready, then is flushed as one
 * `ota_flash_backend_program()` call. Padded with `0xFF` for a short final
 * page.
 */
typedef struct {
    uint32_t image_size;
    uint32_t bytes_written;
    size_t buffered_bytes;
    bool active;
    uint8_t page_buffer[OTA_FLASH_WRITER_PAGE_BUFFER_SIZE];
} ota_flash_writer_t;

/**
 * @brief Reads and validates the on-Flash OTA state record.
 *
 * Copies the record from `OTA_LAYOUT_STATE_ORIGIN` into @p out_record and
 * runs `ota_state_validate()` on it. Always fully populates @p out_record
 * (even on validation failure) so callers can inspect raw fields if needed,
 * but the return value is the authoritative validity signal.
 *
 * @param out_record Destination for the record contents. Must not be NULL.
 * @return true if @p out_record is non-NULL and the record passes
 *         `ota_state_validate()`; false otherwise (including on an erased
 *         or never-written state region).
 * @note Performs a Flash read only (XIP memory access), no erase/program.
 */
bool ota_flash_read_state(ota_state_record_t *out_record);

/**
 * @brief Persists @p record as the new OTA state, replacing whatever was
 *        there before.
 *
 * Finalizes a local copy of @p record (recomputing `record_crc32` via
 * `ota_state_finalize()`), then erases the entire OTA state Flash sector
 * and reprograms it with the new record. This is an erase-then-program
 * sequence with **no atomicity/rollback**: if power is lost or a fault
 * occurs between the erase and the program completing, the state region is
 * left erased (read back as invalid by `ota_flash_read_state()`) rather
 * than reverted to the previous record.
 *
 * @param record Record to persist. Must not be NULL; its `record_crc32` is
 *        recomputed internally so callers do not need to call
 *        `ota_state_finalize()` first.
 * @return `OTA_FLASH_STATUS_OK` on success; `OTA_FLASH_STATUS_INVALID_ARGUMENT`
 *         if @p record is NULL; `OTA_FLASH_STATUS_FLASH_OP_FAILED` if the
 *         underlying erase or program operation fails.
 * @note Every call costs one Flash sector erase, regardless of how much of
 *       the record actually changed.
 */
ota_flash_status_t ota_flash_write_state(const ota_state_record_t *record);

/**
 * @brief Resets the OTA state record to `OTA_IMAGE_STATE_IDLE` and persists
 *        it.
 *
 * Convenience wrapper: builds an in-memory record via
 * `ota_state_reset_record()` and writes it out via `ota_flash_write_state()`.
 * Called by `ota_flash_writer_begin()` (to clear any leftover state before a
 * new staging session) and by the application after a successful boot
 * acknowledgement (see `app_acknowledge_ota_boot_if_needed()`).
 *
 * @return Same return semantics as `ota_flash_write_state()`.
 */
ota_flash_status_t ota_flash_reset_state(void);

/**
 * @brief Erases enough of Bank B to hold an image of @p image_size bytes.
 *
 * Rounds @p image_size up to the Flash erase-block size and erases that
 * many bytes starting at the beginning of Bank B. Any previously staged
 * image data beyond the erased range is left untouched (stale) — callers
 * must not rely on Bank B being fully blank beyond what this call erases.
 *
 * @param image_size Size of the image to be staged, in bytes.
 * @return `OTA_FLASH_STATUS_OK` on success;
 *         `OTA_FLASH_STATUS_INVALID_ARGUMENT` if @p image_size is 0 or
 *         exceeds Bank B's capacity; `OTA_FLASH_STATUS_FLASH_OP_FAILED` if
 *         the erase fails.
 * @note Typically called only via `ota_flash_writer_begin()`, not directly.
 */
ota_flash_status_t ota_flash_prepare_bank_b(uint32_t image_size);

/**
 * @brief Programs a page-aligned chunk of data into Bank B at a given image
 *        offset.
 *
 * @param image_offset Offset within Bank B, in bytes. Must be a multiple of
 *        the Flash program-page size.
 * @param data Bytes to program. Must not be NULL.
 * @param size Number of bytes; must be a multiple of the Flash program-page
 *        size.
 * @return `OTA_FLASH_STATUS_OK` on success;
 *         `OTA_FLASH_STATUS_INVALID_ARGUMENT` if @p data is NULL, @p size is
 *         0, or either @p image_offset/@p size is not page-aligned;
 *         `OTA_FLASH_STATUS_OUT_OF_RANGE` if the range falls outside Bank B;
 *         `OTA_FLASH_STATUS_FLASH_OP_FAILED` on a backend Flash failure.
 * @note Callers wanting arbitrary (non-page-aligned) chunk sizes should use
 *       `ota_flash_writer_write()` instead, which buffers internally.
 */
ota_flash_status_t ota_flash_program_bank_b(uint32_t image_offset,
                                            const void *data,
                                            size_t size);

/**
 * @brief Verifies the CRC32 of the first @p image_size bytes of Bank B
 *        against @p expected_crc32.
 *
 * Reads Bank B directly via XIP-mapped memory (no separate Flash read
 * operation) and computes the CRC32 in place; performs no Flash writes.
 *
 * @param image_size Number of bytes to checksum, starting at the beginning
 *        of Bank B.
 * @param expected_crc32 CRC32 value to compare against (client-supplied at
 *        `BeginFirmwareUpdate` time; not independently derived from a
 *        trusted source — see `docs/code/ota-bootloader.md#integrity-checking`).
 * @return `OTA_FLASH_STATUS_OK` if the CRC32 matches;
 *         `OTA_FLASH_STATUS_INVALID_ARGUMENT` if @p image_size is 0 or
 *         exceeds Bank B's capacity; `OTA_FLASH_STATUS_VERIFY_FAILED` on a
 *         CRC32 mismatch.
 */
ota_flash_status_t ota_flash_verify_bank_b(uint32_t image_size, uint32_t expected_crc32);

/**
 * @brief Zero-initializes @p writer, leaving it inactive.
 *
 * Must be called (directly, or transitively via `ota_flash_writer_begin()`)
 * before any other writer function is used on a fresh or reused
 * `ota_flash_writer_t` instance.
 *
 * @param writer Writer to initialize. No-op if NULL.
 */
void ota_flash_writer_init(ota_flash_writer_t *writer);

/**
 * @brief Begins a new Bank B staging session.
 *
 * Resets @p writer, resets the OTA state record to `IDLE` (discarding any
 * previous in-progress or failed session's persisted state), erases Bank B
 * for @p image_size bytes, then marks the writer active. On any failure
 * partway through, @p writer is left in its reset (inactive) state.
 *
 * @param writer Writer to start. Must not be NULL.
 * @param image_size Total size of the image to be staged, in bytes; must be
 *        non-zero and fit within Bank B.
 * @return `OTA_FLASH_STATUS_OK` on success; `OTA_FLASH_STATUS_INVALID_ARGUMENT`
 *         if @p writer is NULL or @p image_size is invalid; propagates
 *         `ota_flash_reset_state()`/`ota_flash_prepare_bank_b()` failures
 *         otherwise.
 * @pre No other write session should be active concurrently on Bank B.
 * @note Performs a full Flash-sector erase pass over Bank B; this is the
 *       expensive step of an OTA session.
 */
ota_flash_status_t ota_flash_writer_begin(ota_flash_writer_t *writer, uint32_t image_size);

/**
 * @brief Appends @p size bytes of arbitrary-chunked image data to the
 *        staging session.
 *
 * Buffers data in `writer->page_buffer`, flushing a full Flash program page
 * to Bank B each time the buffer fills, regardless of the caller's chunk
 * boundaries. On any Flash-program failure, @p writer is marked inactive
 * (`active = false`) and further writes/finish calls will fail until a new
 * `ota_flash_writer_begin()`.
 *
 * @param writer Active writer (from a successful `ota_flash_writer_begin()`).
 * @param data Bytes to append. Must not be NULL.
 * @param size Number of bytes to append; must be non-zero.
 * @return `OTA_FLASH_STATUS_OK` on success; `OTA_FLASH_STATUS_INVALID_ARGUMENT`
 *         if @p writer is NULL/inactive or @p data/@p size is invalid;
 *         `OTA_FLASH_STATUS_OUT_OF_RANGE` if the write would exceed
 *         `writer->image_size`; propagates Flash program failures otherwise.
 * @pre `ota_flash_writer_begin()` must have succeeded and `finish()` must
 *      not yet have been called.
 */
ota_flash_status_t ota_flash_writer_write(ota_flash_writer_t *writer,
                                          const void *data,
                                          size_t size);

/**
 * @brief Completes a staging session: flushes any buffered partial page,
 *        verifies the full image CRC32, and records the image as
 *        `STAGED_IN_BANK_B` in the OTA state.
 *
 * Requires that exactly `writer->image_size` bytes were written via
 * `ota_flash_writer_write()`. Flushes a short final page (padded with
 * `0xFF`), verifies the whole Bank B image against @p expected_crc32 via
 * `ota_flash_verify_bank_b()`, and — only if that passes — writes a new OTA
 * state record with `state = OTA_IMAGE_STATE_STAGED_IN_BANK_B`,
 * `image_size`/`image_crc32`/`staged_bank_offset`/`staged_bank_size` set,
 * `version_number = version_number`, and `boot_attempts = 0`. The writer is
 * marked inactive in all cases (success or failure) once this returns.
 *
 * @param writer Active writer with all image bytes already written.
 * @param expected_crc32 CRC32 the completed image must match.
 * @param version_number Opaque version identifier to persist into the OTA
 *        state record (informational only; see `ota_state_record_t::version_number`).
 * @return `OTA_FLASH_STATUS_OK` only if the flush, CRC32 verification, and
 *         state-record write all succeed;
 *         `OTA_FLASH_STATUS_INVALID_ARGUMENT` if @p writer is NULL/inactive;
 *         `OTA_FLASH_STATUS_WRITER_STATE_ERROR` if fewer than
 *         `writer->image_size` bytes were written;
 *         `OTA_FLASH_STATUS_VERIFY_FAILED` on a CRC32 mismatch (no state
 *         transition occurs — the record is left as-is);
 *         other Flash-related statuses on I/O failure.
 * @post On success, the OTA state record is `STAGED_IN_BANK_B` and the
 *       bootloader will attempt to copy Bank B to Bank A on the next boot.
 */
ota_flash_status_t ota_flash_writer_finish(ota_flash_writer_t *writer,
                                           uint32_t expected_crc32,
                                           uint32_t version_number);

/**
 * @brief Returns a static, human-readable name for an `ota_flash_status_t`
 *        value, for logging only.
 *
 * @param status Status value to describe.
 * @return Constant string; `"UNKNOWN"` for any value outside
 *         `ota_flash_status_t`. Never NULL.
 */
const char *ota_flash_status_to_string(ota_flash_status_t status);

#endif
