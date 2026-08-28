/**
 * @file
 * @brief Chunked upload/stage/validate/apply pipeline for replacing the
 * device identity (certificate + private key) managed by `security_service`.
 *
 * Responsibilities:
 * - Runs a single-session block-transfer import identical in shape to
 *   `security_trust_import_service`, but for a packaged
 *   `SID1`-magic-prefixed blob containing a certificate and private key
 *   (see `security_identity_import_service_parse_package()`).
 * - Validates, beyond CRC/schema: certificate parses, private key parses,
 *   certificate/key form a matching pair
 *   (`UA_CertificateUtils_checkKeyPair`), certificate's application URI
 *   matches `config_service_get()->product_instance_uri`, and — only if
 *   `time_service` has a valid time — certificate validity window contains
 *   the current time.
 * - On successful apply, commits the new identity via
 *   `security_service_set_device_identity()` and advances the identity
 *   generation counters in `security_state_service`.
 *
 * Ownership: owns `g_security_identity_import_runtime`, a single static
 * session record (one import in flight at a time). The staging buffer is
 * heap-allocated for the session's lifetime.
 *
 * Concurrency: no internal locking; single-task-context use assumed, same
 * caveats as `security_trust_import_service`.
 */
#ifndef SERVICES_SECURITY_IDENTITY_IMPORT_SERVICE_H
#define SERVICES_SECURITY_IDENTITY_IMPORT_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SECURITY_IDENTITY_IMPORT_SERVICE_MAX_CHUNK_BYTES 4096u
#define SECURITY_IDENTITY_IMPORT_SERVICE_SCHEMA_VERSION 1u

/** Result/error code shared by every stage of the identity import pipeline. */
typedef enum
{
    /** Pre-init sentinel; never returned by an API call. */
    SECURITY_IDENTITY_IMPORT_RESULT_NONE = 0,
    SECURITY_IDENTITY_IMPORT_RESULT_OK = 1,
    /** `begin()` called while a session is already active. */
    SECURITY_IDENTITY_IMPORT_RESULT_BUSY = 2,
    /** A chunk/validate/apply/abort call was made with no active session. */
    SECURITY_IDENTITY_IMPORT_RESULT_NOT_ACTIVE = 3,
    SECURITY_IDENTITY_IMPORT_RESULT_INVALID_ARGUMENT = 4,
    /** A single `write_chunk()` call exceeded `SECURITY_IDENTITY_IMPORT_SERVICE_MAX_CHUNK_BYTES`. */
    SECURITY_IDENTITY_IMPORT_RESULT_CHUNK_TOO_LARGE = 5,
    /** Declared payload size exceeds the header plus max certificate/key sizes. */
    SECURITY_IDENTITY_IMPORT_RESULT_TOO_LARGE = 6,
    SECURITY_IDENTITY_IMPORT_RESULT_OUT_OF_MEMORY = 7,
    /** Validate/apply attempted before all declared bytes were written. */
    SECURITY_IDENTITY_IMPORT_RESULT_NOT_READY = 8,
    SECURITY_IDENTITY_IMPORT_RESULT_CRC_MISMATCH = 9,
    /** Declared schema version does not equal `SECURITY_IDENTITY_IMPORT_SERVICE_SCHEMA_VERSION`. */
    SECURITY_IDENTITY_IMPORT_RESULT_SCHEMA_MISMATCH = 10,
    /** Package header magic/length fields are malformed or inconsistent with the payload size. */
    SECURITY_IDENTITY_IMPORT_RESULT_PACKAGE_INVALID = 11,
    SECURITY_IDENTITY_IMPORT_RESULT_CERTIFICATE_PARSE_FAILED = 12,
    SECURITY_IDENTITY_IMPORT_RESULT_PRIVATE_KEY_PARSE_FAILED = 13,
    /** Certificate's public key does not match the supplied private key. */
    SECURITY_IDENTITY_IMPORT_RESULT_KEY_PAIR_MISMATCH = 14,
    /** Certificate's application URI does not match the device's configured `product_instance_uri`. */
    SECURITY_IDENTITY_IMPORT_RESULT_APPLICATION_URI_MISMATCH = 15,
    /** Current time (if known) is before the certificate's `notBefore`. */
    SECURITY_IDENTITY_IMPORT_RESULT_CERTIFICATE_NOT_YET_VALID = 16,
    /** Current time (if known) is at/after the certificate's `notAfter`. */
    SECURITY_IDENTITY_IMPORT_RESULT_CERTIFICATE_EXPIRED = 17,
    /** Validated identity failed to commit via `security_service_set_device_identity()`. */
    SECURITY_IDENTITY_IMPORT_RESULT_APPLY_FAILED = 18,
    SECURITY_IDENTITY_IMPORT_RESULT_INTERNAL_ERROR = 19,
} security_identity_import_service_result_t;

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
    security_identity_import_service_result_t last_result;
} security_identity_import_service_status_t;

/** Result of `validate_staged()`/`apply_staged()`, including parse-derived metadata. */
typedef struct
{
    bool session_active;
    bool payload_complete;
    bool payload_crc32_matches;
    /** True iff `result == SECURITY_IDENTITY_IMPORT_RESULT_OK`. */
    bool valid;
    /** True iff a valid system time was available to check certificate validity window. */
    bool time_check_performed;
    /** True only on a successful `apply_staged()` — caller must reboot the device to pick up the new identity. */
    bool reboot_required;
    uint32_t payload_size;
    uint32_t expected_payload_crc32;
    uint32_t schema_version;
    uint32_t certificate_size;
    uint32_t private_key_size;
    /** Certificate `notBefore`, populated whenever the X.509 field could be parsed (even if `time_check_performed` is false). */
    uint32_t not_before_epoch_s;
    /** Certificate `notAfter`, populated whenever the X.509 field could be parsed. */
    uint32_t not_after_epoch_s;
    security_identity_import_service_result_t result;
} security_identity_import_service_report_t;

/**
 * @brief Resets the import session to idle.
 * @note Same caveat as `security_trust_import_service_init()`: zero-fills
 * the runtime struct directly without freeing a possibly-allocated staging
 * buffer. Intended for one-time startup initialization.
 */
void security_identity_import_service_init(void);
/**
 * @brief Starts a new import session and allocates a `payload_size + 1`
 * byte staging buffer.
 * @param payload_size Total expected packaged-blob size in bytes;
 * `1..(20 + SECURITY_SERVICE_CERT_MAX_LEN + SECURITY_SERVICE_KEY_MAX_LEN)`
 * (20-byte package header plus certificate/key maxima).
 * @param payload_crc32 CRC32 the completed payload must match.
 * @param schema_version Caller-declared schema version, cross-checked
 * against both `SECURITY_IDENTITY_IMPORT_SERVICE_SCHEMA_VERSION` and the
 * package header's own schema field at validation time.
 * @return `SECURITY_IDENTITY_IMPORT_RESULT_OK`, `_BUSY`,
 * `_INVALID_ARGUMENT` (zero size), `_TOO_LARGE`, or `_OUT_OF_MEMORY`.
 */
security_identity_import_service_result_t security_identity_import_service_begin(uint32_t payload_size,
                                                                                uint32_t payload_crc32,
                                                                                uint32_t schema_version);
/**
 * @brief Appends a chunk of bytes to the staging buffer.
 * @param data Chunk bytes.
 * @param size Chunk length, `1..SECURITY_IDENTITY_IMPORT_SERVICE_MAX_CHUNK_BYTES`;
 * writing past the declared payload size is rejected.
 * @return `SECURITY_IDENTITY_IMPORT_RESULT_OK`, `_NOT_ACTIVE`,
 * `_INVALID_ARGUMENT`, or `_CHUNK_TOO_LARGE`.
 */
security_identity_import_service_result_t security_identity_import_service_write_chunk(const uint8_t *data,
                                                                                      size_t size);
/**
 * @brief Cancels the active session, freeing the staging buffer.
 * @return `SECURITY_IDENTITY_IMPORT_RESULT_OK`, or `_NOT_ACTIVE` if no
 * session was in progress.
 */
security_identity_import_service_result_t security_identity_import_service_abort(void);
/** @brief Fills @p out_status with the current session's progress; no-op if @p out_status is NULL. */
void security_identity_import_service_get_status(security_identity_import_service_status_t *out_status);
/**
 * @brief Validates the fully-uploaded staged package: CRC32, schema
 * version, package header, certificate parse, private key parse, key-pair
 * match, application-URI match, and (if a time source is available)
 * certificate validity window — without applying it.
 * @param out_report Optional; filled with session/validation details
 * (including whichever of certificate size/key size/validity dates were
 * successfully parsed) even on failure.
 * @return `SECURITY_IDENTITY_IMPORT_RESULT_OK` if every check passes;
 * otherwise the specific first failure encountered.
 */
security_identity_import_service_result_t security_identity_import_service_validate_staged(
    security_identity_import_service_report_t *out_report);
/**
 * @brief Validates the staged package and, if valid, commits it as the new
 * device identity via `security_service_set_device_identity()`, advances
 * the identity generation counters in `security_state_service`, and clears
 * the session.
 * @param out_report Optional; filled with the outcome either way. On
 * success, `reboot_required` is set true.
 * @return `SECURITY_IDENTITY_IMPORT_RESULT_OK` on success; any validation
 * failure code from `validate_staged()`; or `_APPLY_FAILED` if the commit
 * to `security_service` fails (session left active in that case).
 * @note Triggers Flash writes in both `security_service` (identity store)
 * and `security_state_service` (generation counters) on success. The new
 * identity does not take effect in the running open62541 server until the
 * device reboots.
 */
security_identity_import_service_result_t security_identity_import_service_apply_staged(
    security_identity_import_service_report_t *out_report);
/** @return Static uppercase result name, or "UNKNOWN" for an out-of-range value. */
const char *security_identity_import_service_result_to_string(security_identity_import_service_result_t result);

#endif
