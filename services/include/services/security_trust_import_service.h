/**
 * @file
 * @brief Chunked upload/stage/validate/apply pipeline for replacing
 * `security_service`'s trust list.
 *
 * Responsibilities:
 * - Runs a single-session, block-transfer-style import: `begin()` allocates
 *   a heap staging buffer sized to the declared payload, `write_chunk()`
 *   appends bytes, `validate_staged()` parses the buffered bytes as an X.509
 *   chain (mbedTLS) and checks CRC32/schema version, `apply_staged()`
 *   commits the validated bytes into `security_service`'s trust list via
 *   `security_service_set_trust_list()`.
 * - Mirrors the block-upload shape used elsewhere in the codebase (see
 *   `config_import_service`, per `docs/code/state-ownership.md`).
 *
 * Ownership: owns `g_security_trust_import_runtime`, a single static session
 * record (one import in flight at a time — a second `begin()` while active
 * returns `SECURITY_TRUST_IMPORT_RESULT_BUSY`). The staging buffer is
 * heap-allocated (`pvPortMalloc`) for the session's lifetime and freed on
 * abort or successful apply.
 *
 * Concurrency: no internal locking; assumes single-task-context use (the
 * task servicing the OPC UA/config-service import method calls). Not
 * reentrant — a second concurrent caller would corrupt `bytes_written`.
 */
#ifndef SERVICES_SECURITY_TRUST_IMPORT_SERVICE_H
#define SERVICES_SECURITY_TRUST_IMPORT_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SECURITY_TRUST_IMPORT_SERVICE_MAX_CHUNK_BYTES 4096u
#define SECURITY_TRUST_IMPORT_SERVICE_SCHEMA_VERSION 1u

/** Result/error code shared by every stage of the trust-list import pipeline. */
typedef enum
{
    /** Pre-init sentinel; never returned by an API call. */
    SECURITY_TRUST_IMPORT_RESULT_NONE = 0,
    SECURITY_TRUST_IMPORT_RESULT_OK = 1,
    /** `begin()` called while a session is already active. */
    SECURITY_TRUST_IMPORT_RESULT_BUSY = 2,
    /** A chunk/validate/apply/abort call was made with no active session. */
    SECURITY_TRUST_IMPORT_RESULT_NOT_ACTIVE = 3,
    SECURITY_TRUST_IMPORT_RESULT_INVALID_ARGUMENT = 4,
    /** A single `write_chunk()` call exceeded `SECURITY_TRUST_IMPORT_SERVICE_MAX_CHUNK_BYTES`. */
    SECURITY_TRUST_IMPORT_RESULT_CHUNK_TOO_LARGE = 5,
    /** Declared payload size exceeds `SECURITY_SERVICE_TRUST_LIST_MAX_LEN`. */
    SECURITY_TRUST_IMPORT_RESULT_TOO_LARGE = 6,
    SECURITY_TRUST_IMPORT_RESULT_OUT_OF_MEMORY = 7,
    /** Validate/apply attempted before all declared bytes were written. */
    SECURITY_TRUST_IMPORT_RESULT_NOT_READY = 8,
    SECURITY_TRUST_IMPORT_RESULT_CRC_MISMATCH = 9,
    /** Declared schema version does not equal `SECURITY_TRUST_IMPORT_SERVICE_SCHEMA_VERSION`. */
    SECURITY_TRUST_IMPORT_RESULT_SCHEMA_MISMATCH = 10,
    /** Staged bytes did not parse as at least one valid X.509 certificate. */
    SECURITY_TRUST_IMPORT_RESULT_PARSE_FAILED = 11,
    /** Validated payload failed to commit via `security_service_set_trust_list()`. */
    SECURITY_TRUST_IMPORT_RESULT_APPLY_FAILED = 12,
    SECURITY_TRUST_IMPORT_RESULT_INTERNAL_ERROR = 13,
} security_trust_import_service_result_t;

/** Point-in-time snapshot of the in-progress upload session, for polling by a client. */
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
    security_trust_import_service_result_t last_result;
} security_trust_import_service_status_t;

/** Result of `validate_staged()`/`apply_staged()`, including parse-derived metadata. */
typedef struct
{
    bool session_active;
    bool payload_complete;
    bool payload_crc32_matches;
    /** True iff `result == SECURITY_TRUST_IMPORT_RESULT_OK`. */
    bool valid;
    uint32_t payload_size;
    uint32_t expected_payload_crc32;
    uint32_t schema_version;
    /** Number of X.509 certificates recovered from the staged payload; only meaningful when `valid`. */
    uint32_t certificate_count;
    security_trust_import_service_result_t result;
} security_trust_import_service_report_t;

/**
 * @brief Resets the import session to idle, discarding (without freeing —
 * see note) any in-progress buffer state.
 * @note This zero-initializes the runtime struct directly rather than
 * calling the free-then-clear helper; if called while a session is active
 * with an allocated buffer, the buffer pointer is dropped without being
 * freed. Intended for one-time startup initialization, not for aborting an
 * active session (use `security_trust_import_service_abort()` for that).
 */
void security_trust_import_service_init(void);
/**
 * @brief Starts a new import session and allocates a `payload_size + 1`
 * byte staging buffer.
 * @param payload_size Total expected payload size in bytes, `1..SECURITY_SERVICE_TRUST_LIST_MAX_LEN`.
 * @param payload_crc32 CRC32 the completed payload must match.
 * @param schema_version Caller-declared schema version, checked against
 * `SECURITY_TRUST_IMPORT_SERVICE_SCHEMA_VERSION` at validation time (not here).
 * @return `SECURITY_TRUST_IMPORT_RESULT_OK` on success; `_BUSY` if a session
 * is already active; `_INVALID_ARGUMENT` if `payload_size == 0`; `_TOO_LARGE`
 * if it exceeds the trust-list maximum; `_OUT_OF_MEMORY` if the staging
 * buffer could not be allocated.
 */
security_trust_import_service_result_t security_trust_import_service_begin(uint32_t payload_size,
                                                                          uint32_t payload_crc32,
                                                                          uint32_t schema_version);
/**
 * @brief Appends a chunk of bytes to the staging buffer.
 * @param data Chunk bytes.
 * @param size Chunk length, `1..SECURITY_TRUST_IMPORT_SERVICE_MAX_CHUNK_BYTES`;
 * writing past the declared `payload_size` is rejected.
 * @return `SECURITY_TRUST_IMPORT_RESULT_OK`, `_NOT_ACTIVE`,
 * `_INVALID_ARGUMENT` (NULL/zero-size, or would overflow the declared size),
 * or `_CHUNK_TOO_LARGE`.
 */
security_trust_import_service_result_t security_trust_import_service_write_chunk(const uint8_t *data,
                                                                                size_t size);
/**
 * @brief Cancels the active session, freeing the staging buffer.
 * @return `SECURITY_TRUST_IMPORT_RESULT_OK`, or `_NOT_ACTIVE` if no session
 * was in progress.
 */
security_trust_import_service_result_t security_trust_import_service_abort(void);
/** @brief Fills @p out_status with the current session's progress; no-op if @p out_status is NULL. */
void security_trust_import_service_get_status(security_trust_import_service_status_t *out_status);
/**
 * @brief Validates the fully-uploaded staged payload (CRC32, schema version,
 * X.509 parse) without applying it.
 * @param out_report Optional; filled with session/validation details even on failure.
 * @return `SECURITY_TRUST_IMPORT_RESULT_OK` if the payload is a valid,
 * complete, CRC-matching, schema-matching, parseable certificate chain;
 * otherwise the specific failure code (`_NOT_ACTIVE`, `_NOT_READY`,
 * `_CRC_MISMATCH`, `_SCHEMA_MISMATCH`, `_PARSE_FAILED`).
 */
security_trust_import_service_result_t security_trust_import_service_validate_staged(
    security_trust_import_service_report_t *out_report);
/**
 * @brief Validates the staged payload and, if valid, commits it as the new
 * trust list via `security_service_set_trust_list()`, then clears the session.
 * @param out_report Optional; filled with the outcome either way.
 * @return `SECURITY_TRUST_IMPORT_RESULT_OK` on success (session is cleared);
 * any validation failure code from `validate_staged()`; or `_APPLY_FAILED`
 * if `security_service_set_trust_list()` rejects the commit (session is
 * left active/staged in that case, not cleared).
 * @note Triggers a Flash write in `security_service` (trust list store +
 * version bump) on success.
 */
security_trust_import_service_result_t security_trust_import_service_apply_staged(
    security_trust_import_service_report_t *out_report);
/** @return Static uppercase result name, or "UNKNOWN" for an out-of-range value. */
const char *security_trust_import_service_result_to_string(security_trust_import_service_result_t result);

#endif
