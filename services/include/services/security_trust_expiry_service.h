#ifndef SERVICES_SECURITY_TRUST_EXPIRY_SERVICE_H
#define SERVICES_SECURITY_TRUST_EXPIRY_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SECURITY_TRUST_EXPIRY_SERVICE_WARN_DAYS 30u
#define SECURITY_TRUST_EXPIRY_SERVICE_HIGH_WARN_DAYS 7u
#define SECURITY_TRUST_EXPIRY_SERVICE_SUMMARY_MAX_LEN 384u

typedef enum
{
    SECURITY_TRUST_EXPIRY_STATE_NONE = 0,
    SECURITY_TRUST_EXPIRY_STATE_EMPTY = 1,
    SECURITY_TRUST_EXPIRY_STATE_UNKNOWN = 2,
    SECURITY_TRUST_EXPIRY_STATE_VALID = 3,
    SECURITY_TRUST_EXPIRY_STATE_EXPIRES_SOON = 4,
    SECURITY_TRUST_EXPIRY_STATE_EXPIRES_SOON_HIGH = 5,
    SECURITY_TRUST_EXPIRY_STATE_EXPIRED = 6,
    SECURITY_TRUST_EXPIRY_STATE_PARSE_FAILED = 7,
    SECURITY_TRUST_EXPIRY_STATE_INTERNAL_ERROR = 8,
} security_trust_expiry_service_state_t;

typedef struct
{
    bool have_trust_list;
    bool parse_ok;
    bool time_check_performed;
    uint32_t trusted_client_count;
    uint32_t trusted_client_with_known_expiry_count;
    uint32_t trusted_client_expiring_soon_count;
    uint32_t trusted_client_expiring_high_count;
    uint32_t trusted_client_expired_count;
    uint32_t earliest_not_after_epoch_s;
    uint32_t days_until_earliest_expiry;
    security_trust_expiry_service_state_t state;
} security_trust_expiry_service_report_t;

bool security_trust_expiry_service_get_report(security_trust_expiry_service_report_t *out_report);
bool security_trust_expiry_service_get_summary_json(char *buffer, size_t buffer_size);
const char *security_trust_expiry_service_state_to_string(
    security_trust_expiry_service_state_t state);

#endif
