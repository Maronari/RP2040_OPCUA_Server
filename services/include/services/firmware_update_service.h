/**
 * @file
 * @brief Service-layer coordinator for a chunked OTA firmware update
 *        session, wrapping `ota/ota_flash.h`'s `ota_flash_writer_t`.
 *
 * Owns one global session flag (`session_active`) plus one
 * `ota_flash_writer_t`, enforcing exactly one firmware-update session at a
 * time (see docs/code/ota-bootloader.md, "Обёртка на уровне сервиса"). All
 * five API calls (begin/write_chunk/finalize/abort/get_status) are invoked
 * from OPC UA method callbacks in `opcua_model/src/model_ota.c` and are
 * expected to run on that single call context; no internal locking is
 * performed. Any failure at any step other than `get_status()` clears the
 * session (firmware_update_service_clear_session() in the `.c` file),
 * requiring the caller to `begin()` a new one.
 */
#ifndef SERVICES_FIRMWARE_UPDATE_SERVICE_H
#define SERVICES_FIRMWARE_UPDATE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ota/ota_state.h"

/** @brief Maximum bytes accepted by a single firmware_update_service_write_chunk() call. */
#define FIRMWARE_UPDATE_SERVICE_MAX_CHUNK_BYTES 4096u

/** @brief Result/error code for every `firmware_update_service_*` API, also mapped from `ota_flash_status_t`. */
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

/**
 * @brief Point-in-time snapshot of the current (or most recent) OTA update session.
 *
 * `expected_image_size`/`bytes_written` are read directly from the
 * embedded `ota_flash_writer_t`, not duplicated service-side state.
 * `ota_state`/`ota_state_valid` reflect the persisted `ota_state_record_t`
 * (see docs/code/ota-bootloader.md), read fresh from Flash on every
 * firmware_update_service_get_status() call.
 */
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

/**
 * @brief Resets the service to its idle state, discarding any in-progress session.
 *
 * Must be called once at boot before any other `firmware_update_service_*`
 * call. Not safe to call while a session may be concurrently in use.
 */
void firmware_update_service_init(void);

/**
 * @brief Starts a new OTA update session for an image of @p image_size
 *        bytes, verified on finalize against @p image_crc32.
 *
 * @param image_size Total expected image size in bytes; must be nonzero.
 * @param image_crc32 Expected whole-image CRC-32, checked by
 *        firmware_update_service_finalize().
 * @param version_number Firmware version number to record with the image.
 * @return FIRMWARE_UPDATE_RESULT_OK on success;
 *         FIRMWARE_UPDATE_RESULT_BUSY if a session is already active
 *         (session left untouched); FIRMWARE_UPDATE_RESULT_INVALID_ARGUMENT
 *         if @p image_size is 0; otherwise a flash-writer error mapped
 *         from `ota_flash_status_t`, and the (failed) session is cleared.
 * @note Begins writing to the OTA staging Flash bank via
 *       `ota_flash_writer_begin()`.
 */
firmware_update_service_result_t firmware_update_service_begin(uint32_t image_size,
                                                              uint32_t image_crc32,
                                                              uint32_t version_number);

/**
 * @brief Writes the next @p size bytes of firmware image data to the OTA staging bank.
 *
 * @param data Chunk bytes; must be non-NULL.
 * @param size Chunk length in bytes; must be nonzero and at most
 *        FIRMWARE_UPDATE_SERVICE_MAX_CHUNK_BYTES.
 * @return FIRMWARE_UPDATE_RESULT_OK on success;
 *         FIRMWARE_UPDATE_RESULT_NOT_ACTIVE if no session is active;
 *         FIRMWARE_UPDATE_RESULT_INVALID_ARGUMENT for NULL/empty @p data;
 *         FIRMWARE_UPDATE_RESULT_CHUNK_TOO_LARGE if @p size exceeds the
 *         maximum; otherwise a flash-writer error mapped from
 *         `ota_flash_status_t`, in which case the session is cleared and
 *         must be restarted with a fresh `begin()`.
 * @note Programs Flash pages via `ota_flash_writer_write()`.
 */
firmware_update_service_result_t firmware_update_service_write_chunk(const uint8_t *data, size_t size);

/**
 * @brief Finishes the current session: verifies the written image against
 *        the expected size/CRC and records it in OTA state.
 *
 * Always clears the session on return, whether verification succeeds or
 * fails.
 *
 * @return FIRMWARE_UPDATE_RESULT_OK on success;
 *         FIRMWARE_UPDATE_RESULT_NOT_ACTIVE if no session is active;
 *         FIRMWARE_UPDATE_RESULT_VERIFY_FAILED if the written image fails
 *         CRC/size verification; otherwise a flash-writer error mapped
 *         from `ota_flash_status_t`.
 * @note Writes the persisted `ota_state_record_t` via `ota_flash_writer_finish()`.
 */
firmware_update_service_result_t firmware_update_service_finalize(void);

/**
 * @brief Aborts any in-progress session and resets the underlying OTA flash state.
 *
 * Unlike the other lifecycle calls, this does not require a session to be
 * active - it is always safe to call as a reset/cleanup operation.
 *
 * @return FIRMWARE_UPDATE_RESULT_OK on success; otherwise a flash-writer
 *         error mapped from `ota_flash_status_t`. The session is cleared
 *         regardless of the result.
 * @note Calls `ota_flash_reset_state()`, which may modify persisted OTA state.
 */
firmware_update_service_result_t firmware_update_service_abort(void);

/**
 * @brief Copies the current session/OTA-state snapshot to @p out_status.
 *
 * @param out_status Destination; no-op if NULL.
 * @note Reads the persisted OTA state record from Flash
 *       (`ota_flash_read_state()`) on every call.
 */
void firmware_update_service_get_status(firmware_update_service_status_t *out_status);

/** @brief Returns a short, stable, human-readable name for @p result (e.g. "BUSY"), for logging/diagnostics. Never returns NULL. */
const char *firmware_update_service_result_to_string(firmware_update_service_result_t result);

#endif
