/**
 * @file
 * @brief Public API for configuration validation: semantic validation of an
 * in-memory `device_profile_config_t`, and end-to-end validation of a
 * staged config-import payload (binary or hand-written JSON).
 *
 * Responsibilities:
 * - `config_validate_service_validate_candidate()` is the single semantic
 *   gate every configuration candidate must pass before it can become
 *   running/staged/persisted (invoked from `config_service.c` and from
 *   `config_validate_service_validate_imported()` below).
 * - `config_validate_service_validate_imported()` drives the config-import
 *   session state machine (`services/config_import_service.h`) through
 *   CRC/completeness checks, sniffs whether the staged payload is JSON or
 *   the opaque binary schema, decodes it, and validates the result.
 * - A small hand-rolled recursive-descent JSON parser (`config_json_reader_t`
 *   and the `config_json_*`/`config_json_reader_*` static functions in
 *   `config_validate_service.c`) decodes the public JSON export/import
 *   schema into a `device_profile_config_t`.
 *
 * Ownership: this module holds no persistent state; all functions operate
 * on caller-supplied buffers/structs.
 *
 * Concurrency: no internal synchronization. Depends on
 * `config_import_service`'s single static import session, so
 * `config_validate_service_validate_imported()` is implicitly serialized by
 * however the caller serializes import-session access (not enforced here).
 */
#ifndef SERVICES_CONFIG_VALIDATE_SERVICE_H
#define SERVICES_CONFIG_VALIDATE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <services/config_service.h>

#define CONFIG_VALIDATE_SERVICE_MESSAGE_MAX_LEN 128u

/**
 * @brief Outcome of a configuration/import validation attempt.
 *
 * `CONFIG_VALIDATE_RESULT_OK` is the only success value; every other value
 * is a distinct, user-facing rejection reason (see
 * `config_validate_service_result_to_string()` for their wire names).
 * `NO_IMPORT_SESSION`/`INCOMPLETE_PAYLOAD`/`CRC_MISMATCH` are only produced
 * by `config_validate_service_validate_imported()` (import-session
 * preconditions); the rest can also be produced by
 * `config_validate_service_validate_candidate()` directly.
 * `CONFIG_VALIDATE_RESULT_UNKNOWN_DRIVER` is a channel referencing a
 * `driver_id` absent from the compiled driver catalog; it is distinct from
 * `CONFIG_VALIDATE_RESULT_INVALID_CHANNEL`, which still covers source/kind/
 * value_type failures on a channel whose driver *does* exist.
 */
typedef enum
{
    CONFIG_VALIDATE_RESULT_NONE = 0,
    CONFIG_VALIDATE_RESULT_OK = 1,
    CONFIG_VALIDATE_RESULT_NO_IMPORT_SESSION = 2,
    CONFIG_VALIDATE_RESULT_INCOMPLETE_PAYLOAD = 3,
    CONFIG_VALIDATE_RESULT_CRC_MISMATCH = 4,
    CONFIG_VALIDATE_RESULT_UNSUPPORTED_SCHEMA = 5,
    CONFIG_VALIDATE_RESULT_INVALID_SIZE = 6,
    CONFIG_VALIDATE_RESULT_INVALID_NETWORK = 7,
    CONFIG_VALIDATE_RESULT_INVALID_PORT = 8,
    CONFIG_VALIDATE_RESULT_INVALID_TIMEZONE = 9,
    CONFIG_VALIDATE_RESULT_INVALID_WATCHDOG = 10,
    CONFIG_VALIDATE_RESULT_DUPLICATE_CHANNEL_ID = 11,
    CONFIG_VALIDATE_RESULT_INVALID_CHANNEL = 12,
    CONFIG_VALIDATE_RESULT_INTERNAL_ERROR = 13,
    CONFIG_VALIDATE_RESULT_INVALID_JSON = 14,
    CONFIG_VALIDATE_RESULT_MISSING_FIELD = 15,
    CONFIG_VALIDATE_RESULT_INVALID_LOCATION_PATH = 16,
    CONFIG_VALIDATE_RESULT_RESOURCE_CONFLICT = 17,
    CONFIG_VALIDATE_RESULT_UNSUPPORTED_VALUE_TYPE = 18,
    CONFIG_VALIDATE_RESULT_UNKNOWN_DRIVER = 19,
} config_validate_service_result_t;

/**
 * @brief Human/machine-readable report of one validation attempt, as
 * produced by `config_validate_service_validate_imported()` and serialized
 * by `config_validate_service_write_report_json()`.
 */
typedef struct
{
    bool valid;
    config_validate_service_result_t result;
    uint32_t schema_version;
    uint32_t payload_size;
    uint32_t payload_crc32;
    char message[CONFIG_VALIDATE_SERVICE_MESSAGE_MAX_LEN];
} config_validate_service_report_t;

/**
 * @brief Validates the currently staged config-import payload end to end:
 * import-session state, CRC, schema detection (JSON vs. opaque binary),
 * decode, and semantic validation.
 *
 * @param out_report Filled with the outcome (result code, schema/size/CRC,
 * human-readable message); optional (may be NULL).
 * @param out_candidate Filled with the decoded, normalized-by-caller-later
 * `device_profile_config_t` only if the result is
 * `CONFIG_VALIDATE_RESULT_OK`; optional (may be NULL). Left untouched on
 * failure.
 * @return `CONFIG_VALIDATE_RESULT_OK` if the staged import payload is valid
 * and decodes to a semantically valid configuration; otherwise the specific
 * rejection reason (see `config_validate_service_result_t`).
 * @pre A config-import session must be active and complete
 * (`config_import_service_begin()`/`_write_chunk()`); otherwise this
 * returns `CONFIG_VALIDATE_RESULT_NO_IMPORT_SESSION`/
 * `_INCOMPLETE_PAYLOAD`/`_CRC_MISMATCH` without decoding anything.
 */
config_validate_service_result_t config_validate_service_validate_imported(config_validate_service_report_t *out_report,
                                                                          device_profile_config_t *out_candidate);

/**
 * @brief Semantic validation gate for any configuration candidate: timezone
 * range, location-path syntax, OPC UA port, watchdog timeout bound, network
 * mode/static-address completeness, per-channel driver/kind/value-type/
 * resource-claim validity, writable-channel driver-capability backing, and
 * cross-channel duplicate-ID/hardware-resource-conflict checks.
 *
 * @param candidate Configuration to validate; must not be NULL.
 * @param message Optional buffer to receive a human-readable explanation of
 * the result (success or failure); may be NULL.
 * @param message_size Capacity of @p message; ignored if @p message is
 * NULL.
 * @return `CONFIG_VALIDATE_RESULT_OK` if every check passes; otherwise the
 * first rejection reason encountered (checks are short-circuiting, in a
 * fixed order — this is not an exhaustive "list every problem" validator;
 * for exhaustive missing-`driver_id` reporting see
 * `config_validate_service_validate_candidate_collect_missing_drivers()`).
 * @note Calls into `driver_catalog` (channel/driver/resource-claim checks)
 * but never touches Flash or hardware itself.
 */
config_validate_service_result_t config_validate_service_validate_candidate(
    const device_profile_config_t *candidate,
    char *message,
    size_t message_size);

/**
 * @brief One channel slot whose configured `driver_id` has no match in the
 * compiled driver catalog, as collected by
 * `config_validate_service_validate_candidate_collect_missing_drivers()`.
 */
typedef struct
{
    uint8_t channel_index;
    uint8_t driver_id;
} config_validate_service_missing_driver_t;

/**
 * @brief Exhaustive report of every channel slot with an unknown
 * `driver_id`, filled by
 * `config_validate_service_validate_candidate_collect_missing_drivers()`.
 */
typedef struct
{
    /** Entries in ascending channel-slot order; only the first
     * `PLATFORM_CHANNEL_LIMIT` missing channels can ever exist, so this
     * never overflows. */
    config_validate_service_missing_driver_t entries[PLATFORM_CHANNEL_LIMIT];
    size_t count;
    /** True if the channels that DO have a known driver also fail the
     * cross-channel hardware resource-claim compatibility check — lets a
     * candidate with both problems surface both, deterministically, in one
     * call instead of only the first one found. */
    bool resource_conflict;
} config_validate_service_missing_driver_report_t;

/**
 * @brief Like `config_validate_service_validate_candidate()`, but collects
 * every channel slot with an unknown `driver_id` instead of stopping at the
 * first one. Additive sibling of `config_validate_service_validate_candidate()`
 * — that function's signature/behavior is unchanged; this is a distinct
 * function existing callers do not need to adopt.
 *
 * Conditions unrelated to missing drivers (source validation, out-of-range
 * `kind`/`value_type`, writable-capability backing, resource-claim decode,
 * duplicate channel IDs) are unchanged from
 * `config_validate_service_validate_candidate()` and still short-circuit on
 * the first one encountered, with their existing result codes.
 *
 * @param candidate Configuration to validate; must not be NULL.
 * @param message Optional buffer to receive a human-readable explanation of
 * the result; may be NULL. When the result is
 * `CONFIG_VALIDATE_RESULT_UNKNOWN_DRIVER`, enumerates every missing channel
 * (best-effort, may truncate for a very large @p message_size-constrained
 * buffer — use @p out_missing_drivers for a complete, structured list).
 * @param message_size Capacity of @p message; ignored if @p message is
 * NULL.
 * @param out_missing_drivers Optional; filled with every channel slot whose
 * `driver_id` is unknown to the catalog, and whether the channels that DO
 * have a known driver also produce a resource conflict; may be NULL.
 * @return `CONFIG_VALIDATE_RESULT_UNKNOWN_DRIVER` if any channel had an
 * unknown `driver_id` (see @p out_missing_drivers for the full list);
 * otherwise identical to `config_validate_service_validate_candidate()`.
 */
config_validate_service_result_t config_validate_service_validate_candidate_collect_missing_drivers(
    const device_profile_config_t *candidate,
    char *message,
    size_t message_size,
    config_validate_service_missing_driver_report_t *out_missing_drivers);

/**
 * @brief Serializes @p report as a single-line JSON object.
 * @param buffer Destination buffer; must not be NULL.
 * @param buffer_size Capacity of @p buffer.
 * @param report Report to serialize; must not be NULL.
 * @return true if the serialized JSON fit in @p buffer; false on NULL
 * arguments or truncation (no partial/invalid JSON is left in @p buffer's
 * logical content in that case, but the buffer itself may contain a
 * truncated string).
 */
bool config_validate_service_write_report_json(char *buffer,
                                               size_t buffer_size,
                                               const config_validate_service_report_t *report);

/**
 * @brief Maps a `config_validate_service_result_t` to its stable wire name
 * (e.g. `"INVALID_JSON"`), used in JSON reports and diagnostics.
 * @return A static, non-NULL string; `"UNKNOWN"` for an unrecognized value.
 */
const char *config_validate_service_result_to_string(config_validate_service_result_t result);

#endif
