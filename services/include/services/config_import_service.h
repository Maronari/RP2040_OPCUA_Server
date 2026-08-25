/**
 * @file
 * @brief Public API for the config-import chunked-upload session: a single
 * in-heap staging buffer that an OPC UA/HTTP caller writes a configuration
 * payload into before `config_validate_service_validate_imported()`
 * validates and `config_service_replace_and_save()` commits it.
 *
 * Responsibilities:
 * - Own exactly one import session at a time (`g_config_import_runtime`,
 *   `config_import_service.c`): begin (allocate) -> write_chunk* ->
 *   (validate elsewhere) -> abort (free), or replaced by a fresh
 *   `config_import_service_begin()` after the previous session's payload
 *   was consumed and the caller aborts/re-begins.
 * - Bound payload size (`CONFIG_STORE_MAX_PAYLOAD_SIZE_BYTES`) and
 *   per-chunk size (`CONFIG_IMPORT_SERVICE_MAX_CHUNK_BYTES`).
 * - Track completeness and CRC32 match without itself deciding whether the
 *   payload is a *valid configuration* — that is
 *   `config_validate_service`'s job, via
 *   `config_import_service_get_payload_view()`.
 *
 * Ownership: `g_config_import_runtime.payload_buffer` is heap-allocated
 * (`pvPortMalloc()`/`vPortFree()`) and owned exclusively by this module;
 * `config_import_service_get_payload_view()` lends out a read-only pointer
 * into it that is only valid until the next
 * `config_import_service_begin()`/`_abort()` call.
 *
 * Concurrency: no internal synchronization — a single static session struct
 * with no mutex/critical-section guarding its fields. Callers must
 * serialize access themselves (in practice, the OPC UA method-call
 * execution context that drives config import).
 */
#ifndef SERVICES_CONFIG_IMPORT_SERVICE_H
#define SERVICES_CONFIG_IMPORT_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Maximum bytes accepted per `config_import_service_write_chunk()` call. */
#define CONFIG_IMPORT_SERVICE_MAX_CHUNK_BYTES 4096u

/**
 * @brief Outcome of an import-session operation, also cached as
 * `config_import_service_status_t::last_result`.
 * @note `CONFIG_IMPORT_RESULT_INTERNAL_ERROR` is defined and mapped by
 * `config_import_service_result_to_string()` but not statically confirmed
 * to be returned anywhere in this module's current implementation.
 */
typedef enum
{
    CONFIG_IMPORT_RESULT_NONE = 0,
    CONFIG_IMPORT_RESULT_OK = 1,
    CONFIG_IMPORT_RESULT_BUSY = 2,
    CONFIG_IMPORT_RESULT_NOT_ACTIVE = 3,
    CONFIG_IMPORT_RESULT_INVALID_ARGUMENT = 4,
    CONFIG_IMPORT_RESULT_CHUNK_TOO_LARGE = 5,
    CONFIG_IMPORT_RESULT_TOO_LARGE = 6,
    CONFIG_IMPORT_RESULT_OUT_OF_MEMORY = 7,
    CONFIG_IMPORT_RESULT_INTERNAL_ERROR = 8,
} config_import_service_result_t;

/**
 * @brief Snapshot of the current import session, as returned by
 * `config_import_service_get_status()`.
 * @note `payload_crc32_matches` is only meaningful (and only computed) once
 * `payload_complete` is true; it recomputes the CRC32 over the full
 * buffer on every status query rather than caching it.
 */
typedef struct
{
    bool session_active;
    uint32_t expected_payload_size;
    uint32_t expected_payload_crc32;
    uint32_t schema_version;
    uint32_t bytes_written;
    uint32_t remaining_bytes;
    bool payload_complete;
    bool payload_crc32_matches;
    config_import_service_result_t last_result;
} config_import_service_status_t;

/**
 * @brief Resets the import service to its initial (no session, no
 * allocation) state.
 * @note Does **not** free a pending session's buffer — callers must ensure
 * no session is active (or accept the leak) before calling this. In
 * practice only used at startup, before any session could exist.
 */
void config_import_service_init(void);

/**
 * @brief Begins a new import session: allocates a
 * `payload_size + 1`-byte heap buffer to stage the incoming payload.
 * @param payload_size Total expected payload size in bytes; must be
 * non-zero and `<= CONFIG_STORE_MAX_PAYLOAD_SIZE_BYTES`.
 * @param payload_crc32 Expected CRC32 of the complete payload, checked
 * lazily by `config_import_service_get_status()`/`_get_payload_view()`
 * once all bytes are written.
 * @param schema_version Caller-declared schema version, echoed back in
 * status and cross-checked by `config_validate_service` against a JSON
 * document's own `schemaVersion` field.
 * @return `CONFIG_IMPORT_RESULT_OK` on success;
 * `CONFIG_IMPORT_RESULT_BUSY` if a session is already active;
 * `CONFIG_IMPORT_RESULT_INVALID_ARGUMENT` for `payload_size == 0`;
 * `CONFIG_IMPORT_RESULT_TOO_LARGE` if @p payload_size exceeds the config
 * store's payload limit; `CONFIG_IMPORT_RESULT_OUT_OF_MEMORY` if the
 * allocation fails.
 * @note Allocates heap memory via `pvPortMalloc()`.
 */
config_import_service_result_t config_import_service_begin(uint32_t payload_size,
                                                           uint32_t payload_crc32,
                                                           uint32_t schema_version);

/**
 * @brief Appends one chunk of payload bytes to the active session.
 * @param data Chunk bytes; must not be NULL.
 * @param size Chunk length; must be non-zero,
 * `<= CONFIG_IMPORT_SERVICE_MAX_CHUNK_BYTES`, and not exceed the session's
 * remaining expected bytes.
 * @return `CONFIG_IMPORT_RESULT_OK` on success;
 * `CONFIG_IMPORT_RESULT_NOT_ACTIVE` if no session is active;
 * `CONFIG_IMPORT_RESULT_INVALID_ARGUMENT` for NULL/zero-length @p data or a
 * write that would overrun the declared payload size;
 * `CONFIG_IMPORT_RESULT_CHUNK_TOO_LARGE` if @p size exceeds the per-chunk
 * limit.
 * @note Chunks must be written in order and exactly sum to the session's
 * declared `payload_size` — there is no seek/random-access capability.
 */
config_import_service_result_t config_import_service_write_chunk(const uint8_t *data, size_t size);

/**
 * @brief Aborts and frees the active import session.
 * @return `CONFIG_IMPORT_RESULT_OK` on success;
 * `CONFIG_IMPORT_RESULT_NOT_ACTIVE` if no session was active.
 * @note Frees the staging buffer via `vPortFree()`.
 */
config_import_service_result_t config_import_service_abort(void);

/**
 * @brief Fills @p out_status with a snapshot of the current session state.
 * @param out_status Destination; no-op if NULL. Zeroed then populated even
 * if no session is active (in which case every field reads as its "no
 * session" default).
 * @note Recomputes the payload CRC32 (`ota_crc32_compute()`) on every call
 * once the payload is complete — not free, but bounded by the payload size
 * limit.
 */
void config_import_service_get_status(config_import_service_status_t *out_status);

/**
 * @brief Returns a read-only view of the completed, CRC-verified payload
 * for the active session.
 * @param out_payload Set to the internal staging buffer pointer on success;
 * optional (may be NULL).
 * @param out_payload_size Set to the payload size on success; optional.
 * @param out_expected_crc32 Set to the session's expected CRC32 on success;
 * optional.
 * @param out_schema_version Set to the session's declared schema version on
 * success; optional.
 * @return true if a session is active, its payload is fully written, and
 * its CRC32 matches; false otherwise (no outputs are written on failure).
 * @note The returned `*out_payload` pointer is only valid until the next
 * `config_import_service_begin()` or `config_import_service_abort()` call
 * (which reallocates/frees the buffer).
 */
bool config_import_service_get_payload_view(const uint8_t **out_payload,
                                            size_t *out_payload_size,
                                            uint32_t *out_expected_crc32,
                                            uint32_t *out_schema_version);

/**
 * @brief Maps a `config_import_service_result_t` to its stable wire name
 * (e.g. `"CHUNK_TOO_LARGE"`).
 * @return A static, non-NULL string; `"UNKNOWN"` for an unrecognized value.
 */
const char *config_import_service_result_to_string(config_import_service_result_t result);

#endif
