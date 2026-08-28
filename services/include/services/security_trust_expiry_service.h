/**
 * @file
 * @brief Read-only diagnostic: reports how close the trusted-client
 * certificates in `security_service`'s trust list are to expiring.
 *
 * Responsibilities:
 * - Parses the trust list returned by `security_service_get_trust_list()`
 *   as a chain of X.509 certificates (via mbedTLS) and computes per-cert
 *   `notAfter` expiry against the current time from `time_service`.
 * - Classifies the overall trust list into a single
 *   `security_trust_expiry_service_state_t` summarizing the worst-case
 *   condition found (expired > expiring-soon-high > expiring-soon > valid).
 *
 * Ownership: stateless — holds no module-level state (confirmed by
 * `docs/code/state-ownership.md`); every call re-reads and re-parses the
 * current trust list from `security_service`.
 *
 * Concurrency: no internal locking; safe to call from any single task
 * context, but not against concurrent Flash writers of the trust list
 * (relies on `security_service`'s own Flash-store consistency).
 */
#ifndef SERVICES_SECURITY_TRUST_EXPIRY_SERVICE_H
#define SERVICES_SECURITY_TRUST_EXPIRY_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SECURITY_TRUST_EXPIRY_SERVICE_WARN_DAYS 30u
#define SECURITY_TRUST_EXPIRY_SERVICE_HIGH_WARN_DAYS 7u
#define SECURITY_TRUST_EXPIRY_SERVICE_SUMMARY_MAX_LEN 384u

/**
 * @brief Overall trust-list expiry classification, in ascending severity
 * within each branch of the state machine computed by
 * `security_trust_expiry_service_finalize_state()`.
 */
typedef enum
{
    /** No report has been computed yet (only used as the pre-init value inside a report struct). */
    SECURITY_TRUST_EXPIRY_STATE_NONE = 0,
    /** Trust list is empty (no trusted-client certificates configured). */
    SECURITY_TRUST_EXPIRY_STATE_EMPTY = 1,
    /** Trust list is non-empty but expiry cannot be determined (no valid system time, or no cert had a parseable `notAfter`). */
    SECURITY_TRUST_EXPIRY_STATE_UNKNOWN = 2,
    /** All certificates parse and none are expired or within the warning windows. */
    SECURITY_TRUST_EXPIRY_STATE_VALID = 3,
    /** At least one certificate expires within `SECURITY_TRUST_EXPIRY_SERVICE_WARN_DAYS`. */
    SECURITY_TRUST_EXPIRY_STATE_EXPIRES_SOON = 4,
    /** At least one certificate expires within `SECURITY_TRUST_EXPIRY_SERVICE_HIGH_WARN_DAYS`. */
    SECURITY_TRUST_EXPIRY_STATE_EXPIRES_SOON_HIGH = 5,
    /** At least one certificate's `notAfter` has already passed. */
    SECURITY_TRUST_EXPIRY_STATE_EXPIRED = 6,
    /** Trust list is non-empty but failed to parse as X.509 (no certificates recovered). */
    SECURITY_TRUST_EXPIRY_STATE_PARSE_FAILED = 7,
    /** Internal inconsistency (parse partially succeeded then flagged not-ok); should not occur in practice. */
    SECURITY_TRUST_EXPIRY_STATE_INTERNAL_ERROR = 8,
} security_trust_expiry_service_state_t;

/** Aggregate expiry report for the current trust list; see field comments for units. */
typedef struct
{
    bool have_trust_list;
    bool parse_ok;
    /** True only if `time_service` reported a non-`SERVICE_QUALITY_BAD` time when this report was built. */
    bool time_check_performed;
    uint32_t trusted_client_count;
    uint32_t trusted_client_with_known_expiry_count;
    /** Certificates expiring within `SECURITY_TRUST_EXPIRY_SERVICE_WARN_DAYS` (includes the high-warn subset). */
    uint32_t trusted_client_expiring_soon_count;
    /** Certificates expiring within `SECURITY_TRUST_EXPIRY_SERVICE_HIGH_WARN_DAYS` (subset of the above). */
    uint32_t trusted_client_expiring_high_count;
    uint32_t trusted_client_expired_count;
    uint32_t earliest_not_after_epoch_s;
    uint32_t days_until_earliest_expiry;
    security_trust_expiry_service_state_t state;
} security_trust_expiry_service_report_t;

/**
 * @brief Builds a fresh expiry report by re-reading and re-parsing the
 * current trust list.
 *
 * @return false only if @p out_report is NULL; all other conditions
 * (empty trust list, parse failure, no time source) are represented in the
 * report's `state` field, not a false return.
 * @note Allocates a temporary heap buffer (`pvPortMalloc`) when the trust
 * list is PEM-encoded, to obtain a NUL-terminated copy for mbedTLS; freed
 * before returning.
 */
bool security_trust_expiry_service_get_report(security_trust_expiry_service_report_t *out_report);
/**
 * @brief Formats a `security_trust_expiry_service_get_report()` result as a
 * compact JSON object.
 * @param buffer Destination buffer, recommended size
 * `SECURITY_TRUST_EXPIRY_SERVICE_SUMMARY_MAX_LEN`.
 * @param buffer_size Size of @p buffer.
 * @return false if @p buffer is NULL/zero-sized, the report could not be
 * built, or the formatted JSON would not fit in @p buffer_size.
 */
bool security_trust_expiry_service_get_summary_json(char *buffer, size_t buffer_size);
/** @return Static lowercase state name (e.g. "expires_soon"), or "unknown" for an out-of-range value. */
const char *security_trust_expiry_service_state_to_string(
    security_trust_expiry_service_state_t state);

#endif
