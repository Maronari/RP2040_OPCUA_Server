#include "services/security_trust_expiry_service.h"

#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>

#include "mbedtls/x509_crt.h"
#include "services/security_service.h"
#include "services/time_service.h"

static int64_t security_trust_expiry_service_days_from_civil(int32_t year,
                                                             uint32_t month,
                                                             uint32_t day)
{
    const int32_t adjusted_year = year - (month <= 2u ? 1 : 0);
    const int32_t era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    const uint32_t year_of_era = (uint32_t)(adjusted_year - era * 400);
    const uint32_t shifted_month = (month > 2u) ? (month - 3u) : (month + 9u);
    const uint32_t day_of_year = (153u * shifted_month + 2u) / 5u + day - 1u;
    const uint32_t day_of_era = year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;

    return (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
}

static bool security_trust_expiry_service_x509_time_to_epoch(const mbedtls_x509_time *time,
                                                             uint32_t *out_epoch_s)
{
    int64_t days;
    int64_t seconds;

    if (time == NULL || out_epoch_s == NULL ||
        time->year < 1970 ||
        time->mon < 1 || time->mon > 12 ||
        time->day < 1 || time->day > 31 ||
        time->hour < 0 || time->hour > 23 ||
        time->min < 0 || time->min > 59 ||
        time->sec < 0 || time->sec > 60)
    {
        return false;
    }

    days = security_trust_expiry_service_days_from_civil(time->year,
                                                         (uint32_t)time->mon,
                                                         (uint32_t)time->day);
    if (days < 0)
    {
        return false;
    }

    seconds = days * 86400 +
              (int64_t)time->hour * 3600 +
              (int64_t)time->min * 60 +
              (int64_t)time->sec;
    if (seconds < 0 || (uint64_t)seconds > UINT32_MAX)
    {
        return false;
    }

    *out_epoch_s = (uint32_t)seconds;
    return true;
}

static bool security_trust_expiry_service_payload_is_pem(const uint8_t *payload,
                                                         size_t payload_size)
{
    static const char pem_prefix[] = "-----BEGIN";

    return payload != NULL &&
           payload_size >= (sizeof(pem_prefix) - 1u) &&
           memcmp(payload, pem_prefix, sizeof(pem_prefix) - 1u) == 0;
}

static bool security_trust_expiry_service_make_parse_buffer(const uint8_t *source,
                                                            size_t source_size,
                                                            const uint8_t **out_buffer,
                                                            size_t *out_size,
                                                            bool *out_needs_free)
{
    uint8_t *buffer;
    const bool pem = security_trust_expiry_service_payload_is_pem(source, source_size);

    if (source == NULL || source_size == 0u || out_buffer == NULL || out_size == NULL ||
        out_needs_free == NULL)
    {
        return false;
    }

    *out_buffer = source;
    *out_size = source_size;
    *out_needs_free = false;

    if (!pem)
    {
        return true;
    }

    buffer = (uint8_t *)pvPortMalloc(source_size + 1u);
    if (buffer == NULL)
    {
        return false;
    }

    memcpy(buffer, source, source_size);
    buffer[source_size] = '\0';
    *out_buffer = buffer;
    *out_size = source_size + 1u;
    *out_needs_free = true;
    return true;
}

static void security_trust_expiry_service_init_report(security_trust_expiry_service_report_t *report)
{
    if (report == NULL)
    {
        return;
    }

    memset(report, 0, sizeof(*report));
    report->state = SECURITY_TRUST_EXPIRY_STATE_NONE;
}

static security_trust_expiry_service_state_t security_trust_expiry_service_finalize_state(
    const security_trust_expiry_service_report_t *report)
{
    if (report == NULL)
    {
        return SECURITY_TRUST_EXPIRY_STATE_INTERNAL_ERROR;
    }

    if (!report->have_trust_list)
    {
        return SECURITY_TRUST_EXPIRY_STATE_EMPTY;
    }

    if (!report->parse_ok)
    {
        return (report->trusted_client_count == 0u)
                   ? SECURITY_TRUST_EXPIRY_STATE_PARSE_FAILED
                   : SECURITY_TRUST_EXPIRY_STATE_INTERNAL_ERROR;
    }

    if (!report->time_check_performed || report->trusted_client_with_known_expiry_count == 0u)
    {
        return SECURITY_TRUST_EXPIRY_STATE_UNKNOWN;
    }

    if (report->trusted_client_expired_count > 0u)
    {
        return SECURITY_TRUST_EXPIRY_STATE_EXPIRED;
    }

    if (report->trusted_client_expiring_high_count > 0u)
    {
        return SECURITY_TRUST_EXPIRY_STATE_EXPIRES_SOON_HIGH;
    }

    if (report->trusted_client_expiring_soon_count > 0u)
    {
        return SECURITY_TRUST_EXPIRY_STATE_EXPIRES_SOON;
    }

    return SECURITY_TRUST_EXPIRY_STATE_VALID;
}

const char *security_trust_expiry_service_state_to_string(
    security_trust_expiry_service_state_t state)
{
    switch (state)
    {
    case SECURITY_TRUST_EXPIRY_STATE_NONE:
        return "none";
    case SECURITY_TRUST_EXPIRY_STATE_EMPTY:
        return "empty";
    case SECURITY_TRUST_EXPIRY_STATE_UNKNOWN:
        return "unknown";
    case SECURITY_TRUST_EXPIRY_STATE_VALID:
        return "valid";
    case SECURITY_TRUST_EXPIRY_STATE_EXPIRES_SOON:
        return "expires_soon";
    case SECURITY_TRUST_EXPIRY_STATE_EXPIRES_SOON_HIGH:
        return "expires_soon_high";
    case SECURITY_TRUST_EXPIRY_STATE_EXPIRED:
        return "expired";
    case SECURITY_TRUST_EXPIRY_STATE_PARSE_FAILED:
        return "parse_failed";
    case SECURITY_TRUST_EXPIRY_STATE_INTERNAL_ERROR:
        return "internal_error";
    default:
        return "unknown";
    }
}

bool security_trust_expiry_service_get_report(security_trust_expiry_service_report_t *out_report)
{
    security_service_blob_view_t trust_list = {0};
    time_service_status_t time_status = {0};
    mbedtls_x509_crt certificates;
    const uint8_t *parse_buffer = NULL;
    size_t parse_size = 0u;
    bool parse_buffer_needs_free = false;
    uint32_t now_epoch_s = 0u;
    int rc;

    if (out_report == NULL)
    {
        return false;
    }

    security_trust_expiry_service_init_report(out_report);

    if (!security_service_get_trust_list(&trust_list) || trust_list.length == 0u)
    {
        out_report->state = SECURITY_TRUST_EXPIRY_STATE_EMPTY;
        return true;
    }

    out_report->have_trust_list = true;

    time_service_get_status(&time_status);
    if (time_status.quality != SERVICE_QUALITY_BAD)
    {
        out_report->time_check_performed = true;
        now_epoch_s = time_status.unix_time_s;
    }

    if (!security_trust_expiry_service_make_parse_buffer(trust_list.data,
                                                         trust_list.length,
                                                         &parse_buffer,
                                                         &parse_size,
                                                         &parse_buffer_needs_free))
    {
        out_report->state = SECURITY_TRUST_EXPIRY_STATE_INTERNAL_ERROR;
        return true;
    }

    mbedtls_x509_crt_init(&certificates);
    rc = mbedtls_x509_crt_parse(&certificates, parse_buffer, parse_size);
    if (rc != 0)
    {
        out_report->state = SECURITY_TRUST_EXPIRY_STATE_PARSE_FAILED;
        goto cleanup;
    }

    for (const mbedtls_x509_crt *cursor = &certificates; cursor != NULL; cursor = cursor->next)
    {
        uint32_t not_after_epoch_s = 0u;

        if (cursor->raw.len == 0u)
        {
            continue;
        }

        ++out_report->trusted_client_count;
        if (!security_trust_expiry_service_x509_time_to_epoch(&cursor->valid_to, &not_after_epoch_s) ||
            not_after_epoch_s == 0u)
        {
            continue;
        }

        ++out_report->trusted_client_with_known_expiry_count;
        if (out_report->earliest_not_after_epoch_s == 0u ||
            not_after_epoch_s < out_report->earliest_not_after_epoch_s)
        {
            out_report->earliest_not_after_epoch_s = not_after_epoch_s;
        }

        if (!out_report->time_check_performed)
        {
            continue;
        }

        if (now_epoch_s >= not_after_epoch_s)
        {
            ++out_report->trusted_client_expired_count;
            continue;
        }

        {
            const uint32_t remaining_days = (not_after_epoch_s - now_epoch_s + 86399u) / 86400u;

            if (remaining_days <= SECURITY_TRUST_EXPIRY_SERVICE_HIGH_WARN_DAYS)
            {
                ++out_report->trusted_client_expiring_soon_count;
                ++out_report->trusted_client_expiring_high_count;
            }
            else if (remaining_days <= SECURITY_TRUST_EXPIRY_SERVICE_WARN_DAYS)
            {
                ++out_report->trusted_client_expiring_soon_count;
            }
        }
    }

    out_report->parse_ok = (out_report->trusted_client_count > 0u);
    if (!out_report->parse_ok)
    {
        out_report->state = SECURITY_TRUST_EXPIRY_STATE_PARSE_FAILED;
        goto cleanup;
    }

    if (out_report->time_check_performed && out_report->earliest_not_after_epoch_s > now_epoch_s)
    {
        out_report->days_until_earliest_expiry =
            (out_report->earliest_not_after_epoch_s - now_epoch_s + 86399u) / 86400u;
    }

    out_report->state = security_trust_expiry_service_finalize_state(out_report);

cleanup:
    mbedtls_x509_crt_free(&certificates);
    if (parse_buffer_needs_free && parse_buffer != NULL)
    {
        vPortFree((void *)parse_buffer);
    }
    return true;
}

bool security_trust_expiry_service_get_summary_json(char *buffer, size_t buffer_size)
{
    security_trust_expiry_service_report_t report;
    const char quote = '"';
    int written;

    if (buffer == NULL || buffer_size == 0u)
    {
        return false;
    }

    if (!security_trust_expiry_service_get_report(&report))
    {
        return false;
    }

    written = snprintf(buffer,
                       buffer_size,
                       "{%chaveTrustList%c:%s,%cparseOk%c:%s,%ctimeCheckPerformed%c:%s,%ctrustedClientCount%c:%lu,%ctrustedClientWithKnownExpiryCount%c:%lu,%ctrustedClientCertsExpiringSoonCount%c:%lu,%ctrustedClientCertsExpiringHighCount%c:%lu,%ctrustedClientCertsExpiredCount%c:%lu,%cearliestTrustedClientCertExpiryEpochS%c:%lu,%cdaysUntilEarliestTrustedClientCertExpiry%c:%lu,%ctrustExpiryState%c:%c%s%c}",
                       quote,
                       quote,
                       report.have_trust_list ? "true" : "false",
                       quote,
                       quote,
                       report.parse_ok ? "true" : "false",
                       quote,
                       quote,
                       report.time_check_performed ? "true" : "false",
                       quote,
                       quote,
                       (unsigned long)report.trusted_client_count,
                       quote,
                       quote,
                       (unsigned long)report.trusted_client_with_known_expiry_count,
                       quote,
                       quote,
                       (unsigned long)report.trusted_client_expiring_soon_count,
                       quote,
                       quote,
                       (unsigned long)report.trusted_client_expiring_high_count,
                       quote,
                       quote,
                       (unsigned long)report.trusted_client_expired_count,
                       quote,
                       quote,
                       (unsigned long)report.earliest_not_after_epoch_s,
                       quote,
                       quote,
                       (unsigned long)report.days_until_earliest_expiry,
                       quote,
                       quote,
                       quote,
                       security_trust_expiry_service_state_to_string(report.state),
                       quote);
    return written >= 0 && (size_t)written < buffer_size;
}
