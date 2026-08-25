/**
 * @file
 * @brief Persistent OTA lifecycle record: format, state machine values, and
 *        CRC32/validation helpers shared by the OTA writer (`ota_flash.c`),
 *        the bootloader (`bootloader/main.c`), and the application
 *        (`app/basic_profile/main.c`).
 *
 * Responsibilities:
 * - Define the on-Flash layout of the single OTA state record
 *   (`ota_state_record_t`), stored in the dedicated `OTA_LAYOUT_STATE_*`
 *   Flash region (see `ota_layout.h`).
 * - Define the OTA image lifecycle state (`ota_image_state_t`) and last-error
 *   (`ota_image_error_t`) enumerations that drive bootloader decisions.
 * - Provide the CRC32 implementation used both to protect the record itself
 *   (`record_crc32`) and to verify staged/copied firmware images.
 *
 * Ownership:
 * There is exactly one live copy of `ota_state_record_t` on Flash at a time,
 * at `OTA_LAYOUT_STATE_ORIGIN`. Callers read it into a local stack copy via
 * `ota_flash_read_state()` (see `ota_flash.h`) and write updated copies back
 * via `ota_flash_write_state()` / `ota_flash_reset_state()`; this header only
 * defines the record shape and pure helper functions, it does not perform any
 * I/O itself.
 *
 * Concurrency:
 * These functions are pure/stateless (no globals) and are safe to call from
 * any single execution context (bootloader `main()`, FreeRTOS `InitTask`).
 * They provide no synchronization of their own; callers that share a record
 * across tasks/interrupts must serialize access themselves. In practice only
 * the bootloader (single-threaded, pre-scheduler) and the application's
 * `InitTask`/OTA service touch the record, never concurrently.
 */

#ifndef OTA_OTA_STATE_H
#define OTA_OTA_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Magic value stamped into `ota_state_record_t::magic`; ASCII "OTA1"
 * (byte-reversed by the `0x31 'A' 'T' 'O'` encoding). Used by
 * `ota_state_validate()` to reject an erased (all-0xFF) or foreign Flash
 * region before trusting the rest of the record. */
#define OTA_STATE_MAGIC 0x3141544Fu /* "OTA1" */
/** Layout version of `ota_state_record_t`. Bumped whenever the record shape
 * changes in a way that is not backward compatible; checked by
 * `ota_state_validate()` alongside `OTA_STATE_MAGIC`. */
#define OTA_STATE_FORMAT_VERSION 1u

/**
 * @brief OTA image lifecycle states, persisted in
 *        `ota_state_record_t::state`.
 *
 * Transitions (see `docs/code/ota-bootloader.md` for the full state
 * diagram):
 * - `IDLE` -> `STAGED_IN_BANK_B`: `ota_flash_writer_finish()` after a
 *   successful CRC32 check of the freshly written Bank B image.
 * - `STAGED_IN_BANK_B`/`COPYING_TO_BANK_A` -> `READY_TO_BOOT` or `FAILED`:
 *   decided only by the bootloader (`bootloader/main.c`), which copies
 *   Bank B into Bank A and re-verifies before booting.
 * - `READY_TO_BOOT` -> `IDLE`: `app_acknowledge_ota_boot_if_needed()`
 *   unconditionally, on the first application boot after a successful copy
 *   (no health-check window; see ota-bootloader.md gap notes).
 * - `READY_TO_BOOT` -> `FAILED`: the bootloader detects a watchdog reboot
 *   with `boot_attempts > 0` while still `READY_TO_BOOT`, meaning the
 *   application never reached the acknowledgement step.
 */
typedef enum {
    OTA_IMAGE_STATE_IDLE = 0u,
    OTA_IMAGE_STATE_STAGED_IN_BANK_B = 1u,
    OTA_IMAGE_STATE_COPYING_TO_BANK_A = 2u,
    OTA_IMAGE_STATE_READY_TO_BOOT = 3u,
    OTA_IMAGE_STATE_FAILED = 4u,
} ota_image_state_t;

/**
 * @brief Last-error code persisted in `ota_state_record_t::last_error`,
 *        set alongside `OTA_IMAGE_STATE_FAILED` transitions to record why
 *        the update failed. Diagnostic only; does not itself gate the
 *        bootloader's boot decision (see `bootloader_is_valid_bank_a_image()`
 *        in `bootloader/main.c`).
 */
typedef enum {
    OTA_IMAGE_ERROR_NONE = 0u,
    OTA_IMAGE_ERROR_INVALID_RECORD = 1u,
    OTA_IMAGE_ERROR_INVALID_STAGED_IMAGE = 2u,
    OTA_IMAGE_ERROR_COPY_FAILED = 3u,
    OTA_IMAGE_ERROR_VERIFY_FAILED = 4u,
    OTA_IMAGE_ERROR_BOOT_FAILED = 5u,
} ota_image_error_t;

/**
 * @brief On-Flash OTA lifecycle record. The single source of truth for OTA
 *        progress across bootloader and application boots.
 *
 * Persistence: stored verbatim (whole-struct `memcpy`) at
 * `OTA_LAYOUT_STATE_ORIGIN`/`OTA_LAYOUT_STATE_OFFSET_BYTES`, a dedicated
 * erase-block-sized Flash region reserved solely for this record (see
 * `ota_layout.h`). Written via `ota_flash_write_state()`
 * (`ota/src/ota_flash.c`), which always erases the whole sector before
 * reprogramming it — there is no wear-leveling or history; every write
 * costs one Flash erase cycle.
 *
 * Integrity: `record_crc32` covers every preceding field (computed by
 * `ota_state_finalize()`) and is checked by `ota_state_validate()` together
 * with `magic`/`format_version`. This only detects a corrupted/erased
 * record, it is not a firmware-image authentication mechanism.
 *
 * @var ota_state_record_t::magic
 * Must equal `OTA_STATE_MAGIC`; distinguishes a valid record from erased
 * (0xFF-filled) or uninitialized Flash.
 * @var ota_state_record_t::format_version
 * Must equal `OTA_STATE_FORMAT_VERSION`.
 * @var ota_state_record_t::state
 * Current `ota_image_state_t`, stored as `uint16_t`.
 * @var ota_state_record_t::image_size
 * Size in bytes of the image staged in Bank B / being copied to Bank A.
 * @var ota_state_record_t::image_crc32
 * Client-supplied CRC32 of the full image, checked against the actual
 * Flash contents at both staging time and post-copy time.
 * @var ota_state_record_t::staged_bank_offset
 * Flash offset of the staging bank (Bank B); expected to equal
 * `OTA_LAYOUT_BANK_B_OFFSET_BYTES` — validated by the bootloader before
 * trusting a staged image.
 * @var ota_state_record_t::staged_bank_size
 * Size of the staging bank; expected to equal `OTA_LAYOUT_BANK_B_SIZE_BYTES`.
 * @var ota_state_record_t::version_number
 * Opaque application-supplied firmware version identifier, informational
 * only (diagnostics/logging), not interpreted by the bootloader.
 * @var ota_state_record_t::boot_attempts
 * Incremented by the bootloader each time it hands off to a
 * `READY_TO_BOOT` image; used only to detect that a previous boot attempt
 * did reach `READY_TO_BOOT` before a watchdog reboot (there is no attempt
 * limit that blocks further boots).
 * @var ota_state_record_t::last_error
 * Most recent `ota_image_error_t`, stored as `uint32_t`.
 * @var ota_state_record_t::reserved
 * Zero-filled padding reserved for future fields; not interpreted by any
 * current code.
 * @var ota_state_record_t::record_crc32
 * CRC32 of all preceding bytes (see `ota_state_finalize()`); must be the
 * last field so `offsetof(ota_state_record_t, record_crc32)` covers
 * everything else.
 */
typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint16_t state;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t staged_bank_offset;
    uint32_t staged_bank_size;
    uint32_t version_number;
    uint32_t boot_attempts;
    uint32_t last_error;
    uint32_t reserved[6];
    uint32_t record_crc32;
} ota_state_record_t;

/**
 * @brief Computes the standard CRC-32 (IEEE 802.3, poly 0xEDB88320,
 *        reflected, init/xorout 0xFFFFFFFF) over a byte buffer.
 *
 * Used both to protect `ota_state_record_t::record_crc32` and to verify
 * staged/copied firmware image contents against a caller-supplied expected
 * value; this function itself performs no comparison or persistence.
 *
 * @param data Buffer to checksum. May be Flash-mapped XIP memory.
 * @param size Number of bytes to checksum.
 * @return CRC32 of the buffer, or 0 if @p data is NULL.
 */
uint32_t ota_crc32_compute(const void *data, size_t size);

/**
 * @brief Resets @p record to the canonical `IDLE` state and finalizes its
 *        CRC32, in memory only.
 *
 * Sets `magic`/`format_version` to the current constants, zeroes every
 * other field (including `reserved`), sets `state` to
 * `OTA_IMAGE_STATE_IDLE` and `last_error` to `OTA_IMAGE_ERROR_NONE`, then
 * calls `ota_state_finalize()`. Does not touch Flash; callers that want the
 * reset persisted must write @p record out themselves (see
 * `ota_flash_reset_state()`).
 *
 * @param record Record to reset. No-op if NULL.
 */
void ota_state_reset_record(ota_state_record_t *record);

/**
 * @brief Recomputes and stores `record->record_crc32` over every preceding
 *        field.
 *
 * Must be called (directly or via `ota_state_reset_record()`) after any
 * modification to @p record and before it is written to Flash or validated,
 * otherwise `ota_state_validate()` will reject it.
 *
 * @param record Record to finalize in place. No-op if NULL.
 */
void ota_state_finalize(ota_state_record_t *record);

/**
 * @brief Validates that @p record is a well-formed, uncorrupted OTA state
 *        record.
 *
 * Checks `magic == OTA_STATE_MAGIC`, `format_version ==
 * OTA_STATE_FORMAT_VERSION`, and that `record_crc32` matches a freshly
 * computed CRC32 of the rest of the record. Does not validate any
 * relationship to actual Flash bank contents (e.g. staged image CRC) — see
 * `ota_flash_verify_bank_b()` for that.
 *
 * @param record Record to validate.
 * @return true if @p record is non-NULL and passes all checks; false
 *         otherwise (including on a NULL pointer, an erased/all-0xFF Flash
 *         region, or bit corruption).
 */
bool ota_state_validate(const ota_state_record_t *record);

/**
 * @brief Returns a static, human-readable name for an `ota_image_state_t`
 *        value, for logging only.
 *
 * @param state Raw state value (as stored in `ota_state_record_t::state`).
 * @return Constant string; `"UNKNOWN"` for any value outside
 *         `ota_image_state_t`. Never NULL.
 */
const char *ota_state_to_string(uint16_t state);

/**
 * @brief Returns a static, human-readable name for an `ota_image_error_t`
 *        value, for logging only.
 *
 * @param error_code Raw error value (as stored in
 *        `ota_state_record_t::last_error`).
 * @return Constant string; `"UNKNOWN"` for any value outside
 *         `ota_image_error_t`. Never NULL.
 */
const char *ota_error_to_string(uint32_t error_code);

#endif