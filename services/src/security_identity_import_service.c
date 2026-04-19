#include "services/security_identity_import_service.h"

#include <string.h>

#include <FreeRTOS.h>

#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "open62541.h"
#include "ota/ota_state.h"
#include "services/config_service.h"
#include "services/security_service.h"
#include "services/security_state_service.h"
#include "services/time_service.h"

#define SECURITY_IDENTITY_IMPORT_SERVICE_PACKAGE_MAGIC 0x31444953u /* "SID1" little-endian */

typedef struct
{
    bool session_active;
    uint8_t *payload_buffer;
    uint32_t expected_payload_size;
    uint32_t expected_payload_crc32;
    uint32_t schema_version;
    uint32_t bytes_written;
    security_identity_import_service_result_t last_result;
} security_identity_import_runtime_t;

typedef struct
{
    uint32_t schema_version;
    const uint8_t *certificate;
    uint32_t certificate_size;
    const uint8_t *private_key;
    uint32_t private_key_size;
} security_identity_import_package_view_t;

static security_identity_import_runtime_t g_security_identity_import_runtime = {0};

static void security_identity_import_service_clear_session(void)
{
    if (g_security_identity_import_runtime.payload_buffer != NULL)
    {
        vPortFree(g_security_identity_import_runtime.payload_buffer);
    }

    g_security_identity_import_runtime.session_active = false;
    g_security_identity_import_runtime.payload_buffer = NULL;
    g_security_identity_import_runtime.expected_payload_size = 0u;
    g_security_identity_import_runtime.expected_payload_crc32 = 0u;
    g_security_identity_import_runtime.schema_version = 0u;
    g_security_identity_import_runtime.bytes_written = 0u;
}

static bool security_identity_import_service_payload_complete(void)
{
    return g_security_identity_import_runtime.session_active &&
           g_security_identity_import_runtime.expected_payload_size > 0u &&
           g_security_identity_import_runtime.bytes_written == g_security_identity_import_runtime.expected_payload_size;
}

static bool security_identity_import_service_payload_crc32_matches(void)
{
    if (!security_identity_import_service_payload_complete() ||
        g_security_identity_import_runtime.payload_buffer == NULL)
    {
        return false;
    }

    return ota_crc32_compute(g_security_identity_import_runtime.payload_buffer,
                             g_security_identity_import_runtime.expected_payload_size) ==
           g_security_identity_import_runtime.expected_payload_crc32;
}

static bool security_identity_import_service_payload_is_pem(const uint8_t *payload, size_t payload_size)
{
    static const char pem_prefix[] = "-----BEGIN";

    return payload != NULL &&
           payload_size >= (sizeof(pem_prefix) - 1u) &&
           memcmp(payload, pem_prefix, sizeof(pem_prefix) - 1u) == 0;
}

static uint32_t security_identity_import_service_read_u32_le(const uint8_t *data)
{
    return ((uint32_t)data[0]) |
           ((uint32_t)data[1] << 8u) |
           ((uint32_t)data[2] << 16u) |
           ((uint32_t)data[3] << 24u);
}

static bool security_identity_import_service_x509_time_to_epoch(const mbedtls_x509_time *time,
                                                                uint32_t *out_epoch_s)
{
    int64_t days;
    int64_t seconds;
    int32_t adjusted_year;
    int32_t era;
    uint32_t year_of_era;
    uint32_t shifted_month;
    uint32_t day_of_year;
    uint32_t day_of_era;

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

    adjusted_year = time->year - (time->mon <= 2 ? 1 : 0);
    era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    year_of_era = (uint32_t)(adjusted_year - era * 400);
    shifted_month = (time->mon > 2) ? ((uint32_t)time->mon - 3u) : ((uint32_t)time->mon + 9u);
    day_of_year = (153u * shifted_month + 2u) / 5u + (uint32_t)time->day - 1u;
    day_of_era = year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;

    days = (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
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

static bool security_identity_import_service_make_parse_buffer(const uint8_t *source,
                                                               size_t source_size,
                                                               uint8_t **out_buffer,
                                                               size_t *out_size)
{
    uint8_t *buffer;
    const bool pem = security_identity_import_service_payload_is_pem(source, source_size);

    if (source == NULL || source_size == 0u || out_buffer == NULL || out_size == NULL)
    {
        return false;
    }

    buffer = (uint8_t *)pvPortMalloc(source_size + 1u);
    if (buffer == NULL)
    {
        return false;
    }

    memcpy(buffer, source, source_size);
    buffer[source_size] = '\0';
    *out_buffer = buffer;
    *out_size = pem ? (source_size + 1u) : source_size;
    return true;
}

static security_identity_import_service_result_t security_identity_import_service_parse_package(
    security_identity_import_package_view_t *out_view)
{
    const uint8_t *payload = g_security_identity_import_runtime.payload_buffer;
    const uint32_t payload_size = g_security_identity_import_runtime.expected_payload_size;
    const uint32_t header_size = 20u;
    uint32_t magic;
    uint32_t schema_version;
    uint32_t certificate_size;
    uint32_t private_key_size;
    uint64_t total_size;

    if (out_view != NULL)
    {
        memset(out_view, 0, sizeof(*out_view));
    }

    if (payload == NULL || payload_size < header_size)
    {
        return SECURITY_IDENTITY_IMPORT_RESULT_PACKAGE_INVALID;
    }

    magic = security_identity_import_service_read_u32_le(&payload[0]);
    schema_version = security_identity_import_service_read_u32_le(&payload[4]);
    certificate_size = security_identity_import_service_read_u32_le(&payload[8]);
    private_key_size = security_identity_import_service_read_u32_le(&payload[12]);

    if (magic != SECURITY_IDENTITY_IMPORT_SERVICE_PACKAGE_MAGIC)
    {
        return SECURITY_IDENTITY_IMPORT_RESULT_PACKAGE_INVALID;
    }

    if (schema_version != g_security_identity_import_runtime.schema_version ||
        schema_version != SECURITY_IDENTITY_IMPORT_SERVICE_SCHEMA_VERSION)
    {
        return SECURITY_IDENTITY_IMPORT_RESULT_SCHEMA_MISMATCH;
    }

    if (certificate_size == 0u ||
        private_key_size == 0u ||
        certificate_size > SECURITY_SERVICE_CERT_MAX_LEN ||
        private_key_size > SECURITY_SERVICE_KEY_MAX_LEN)
    {
        return SECURITY_IDENTITY_IMPORT_RESULT_PACKAGE_INVALID;
    }

    total_size = (uint64_t)header_size + (uint64_t)certificate_size + (uint64_t)private_key_size;
    if (total_size != payload_size)
    {
        return SECURITY_IDENTITY_IMPORT_RESULT_PACKAGE_INVALID;
    }

    if (out_view != NULL)
    {
        out_view->schema_version = schema_version;
        out_view->certificate = &payload[header_size];
        out_view->certificate_size = certificate_size;
        out_view->private_key = &payload[header_size + certificate_size];
        out_view->private_key_size = private_key_size;
    }

    return SECURITY_IDENTITY_IMPORT_RESULT_OK;
}

static uint32_t security_identity_import_service_next_generation(const security_state_service_snapshot_t *snapshot)
{
    uint32_t current = 0u;

    if (snapshot != NULL)
    {
        current = snapshot->active_identity_generation;
        if (snapshot->pending_identity_generation > current)
        {
            current = snapshot->pending_identity_generation;
        }
    }

    return current + 1u;
}

static security_identity_import_service_result_t security_identity_import_service_validate_payload(
    security_identity_import_service_report_t *out_report)
{
    security_identity_import_package_view_t package_view = {0};
    security_identity_import_service_result_t result;
    uint8_t *certificate_buffer = NULL;
    uint8_t *private_key_buffer = NULL;
    size_t certificate_parse_size = 0u;
    size_t private_key_parse_size = 0u;
    UA_ByteString certificate_bytestring = UA_BYTESTRING_NULL;
    UA_ByteString private_key_bytestring = UA_BYTESTRING_NULL;
    const device_profile_config_t *config = NULL;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context private_key;
    time_service_status_t time_status = {0};
    int rc;

    mbedtls_x509_crt_init(&certificate);
    mbedtls_pk_init(&private_key);

    if (!g_security_identity_import_runtime.session_active ||
        g_security_identity_import_runtime.payload_buffer == NULL)
    {
        return SECURITY_IDENTITY_IMPORT_RESULT_NOT_ACTIVE;
    }

    if (!security_identity_import_service_payload_complete())
    {
        return SECURITY_IDENTITY_IMPORT_RESULT_NOT_READY;
    }

    if (!security_identity_import_service_payload_crc32_matches())
    {
        return SECURITY_IDENTITY_IMPORT_RESULT_CRC_MISMATCH;
    }

    if (g_security_identity_import_runtime.schema_version != SECURITY_IDENTITY_IMPORT_SERVICE_SCHEMA_VERSION)
    {
        return SECURITY_IDENTITY_IMPORT_RESULT_SCHEMA_MISMATCH;
    }

    result = security_identity_import_service_parse_package(&package_view);
    if (result != SECURITY_IDENTITY_IMPORT_RESULT_OK)
    {
        return result;
    }

    if (out_report != NULL)
    {
        out_report->certificate_size = package_view.certificate_size;
        out_report->private_key_size = package_view.private_key_size;
    }

    if (!security_identity_import_service_make_parse_buffer(package_view.certificate,
                                                            package_view.certificate_size,
                                                            &certificate_buffer,
                                                            &certificate_parse_size))
    {
        result = SECURITY_IDENTITY_IMPORT_RESULT_OUT_OF_MEMORY;
        goto cleanup;
    }

    if (!security_identity_import_service_make_parse_buffer(package_view.private_key,
                                                            package_view.private_key_size,
                                                            &private_key_buffer,
                                                            &private_key_parse_size))
    {
        result = SECURITY_IDENTITY_IMPORT_RESULT_OUT_OF_MEMORY;
        goto cleanup_certificate;
    }

    rc = mbedtls_x509_crt_parse(&certificate, certificate_buffer, certificate_parse_size);
    if (rc != 0)
    {
        result = SECURITY_IDENTITY_IMPORT_RESULT_CERTIFICATE_PARSE_FAILED;
        goto cleanup_certificate;
    }

#if MBEDTLS_VERSION_NUMBER >= 0x02060000 && MBEDTLS_VERSION_NUMBER < 0x03000000
    rc = mbedtls_pk_parse_key(&private_key,
                              private_key_buffer,
                              private_key_parse_size,
                              NULL,
                              0u);
#else
    {
        mbedtls_entropy_context entropy;

        mbedtls_entropy_init(&entropy);
        rc = mbedtls_pk_parse_key(&private_key,
                                  private_key_buffer,
                                  private_key_parse_size,
                                  NULL,
                                  0u,
                                  mbedtls_entropy_func,
                                  &entropy);
        mbedtls_entropy_free(&entropy);
    }
#endif
    if (rc != 0)
    {
        result = SECURITY_IDENTITY_IMPORT_RESULT_PRIVATE_KEY_PARSE_FAILED;
        goto cleanup_key;
    }

    certificate_bytestring.data = certificate_buffer;
    certificate_bytestring.length = certificate_parse_size;
    private_key_bytestring.data = private_key_buffer;
    private_key_bytestring.length = private_key_parse_size;

    if (UA_CertificateUtils_checkKeyPair(&certificate_bytestring, &private_key_bytestring) != UA_STATUSCODE_GOOD)
    {
        result = SECURITY_IDENTITY_IMPORT_RESULT_KEY_PAIR_MISMATCH;
        goto cleanup_key;
    }

    config = config_service_get();
    if (config == NULL || config->product_instance_uri[0] == '\0')
    {
        result = SECURITY_IDENTITY_IMPORT_RESULT_INTERNAL_ERROR;
        goto cleanup_key;
    }

    {
        UA_String application_uri = UA_STRING((char *)config->product_instance_uri);

        if (UA_CertificateUtils_verifyApplicationUri(&certificate_bytestring,
                                                     &application_uri) != UA_STATUSCODE_GOOD)
        {
            result = SECURITY_IDENTITY_IMPORT_RESULT_APPLICATION_URI_MISMATCH;
            goto cleanup_key;
        }
    }

    (void)security_identity_import_service_x509_time_to_epoch(&certificate.valid_from,
                                                              out_report != NULL ? &out_report->not_before_epoch_s : &(uint32_t){0u});
    (void)security_identity_import_service_x509_time_to_epoch(&certificate.valid_to,
                                                              out_report != NULL ? &out_report->not_after_epoch_s : &(uint32_t){0u});

    time_service_get_status(&time_status);
    if (time_status.quality != SERVICE_QUALITY_BAD)
    {
        uint32_t not_before_epoch_s = 0u;
        uint32_t not_after_epoch_s = 0u;

        if (out_report != NULL)
        {
            out_report->time_check_performed = true;
        }

        if (security_identity_import_service_x509_time_to_epoch(&certificate.valid_from, &not_before_epoch_s) &&
            time_status.unix_time_s < not_before_epoch_s)
        {
            if (out_report != NULL)
            {
                out_report->not_before_epoch_s = not_before_epoch_s;
            }
            result = SECURITY_IDENTITY_IMPORT_RESULT_CERTIFICATE_NOT_YET_VALID;
            goto cleanup_key;
        }

        if (security_identity_import_service_x509_time_to_epoch(&certificate.valid_to, &not_after_epoch_s) &&
            time_status.unix_time_s >= not_after_epoch_s)
        {
            if (out_report != NULL)
            {
                out_report->not_after_epoch_s = not_after_epoch_s;
            }
            result = SECURITY_IDENTITY_IMPORT_RESULT_CERTIFICATE_EXPIRED;
            goto cleanup_key;
        }
    }

    result = SECURITY_IDENTITY_IMPORT_RESULT_OK;

cleanup_key:
    mbedtls_pk_free(&private_key);
cleanup_certificate:
    mbedtls_x509_crt_free(&certificate);
cleanup:
    if (certificate_buffer != NULL)
    {
        vPortFree(certificate_buffer);
    }
    if (private_key_buffer != NULL)
    {
        vPortFree(private_key_buffer);
    }
    return result;
}

void security_identity_import_service_init(void)
{
    memset(&g_security_identity_import_runtime, 0, sizeof(g_security_identity_import_runtime));
    g_security_identity_import_runtime.last_result = SECURITY_IDENTITY_IMPORT_RESULT_NONE;
}

security_identity_import_service_result_t security_identity_import_service_begin(uint32_t payload_size,
                                                                                uint32_t payload_crc32,
                                                                                uint32_t schema_version)
{
    uint8_t *payload_buffer = NULL;

    if (g_security_identity_import_runtime.session_active)
    {
        g_security_identity_import_runtime.last_result = SECURITY_IDENTITY_IMPORT_RESULT_BUSY;
        return g_security_identity_import_runtime.last_result;
    }

    if (payload_size == 0u)
    {
        g_security_identity_import_runtime.last_result = SECURITY_IDENTITY_IMPORT_RESULT_INVALID_ARGUMENT;
        return g_security_identity_import_runtime.last_result;
    }

    if (payload_size > (20u + SECURITY_SERVICE_CERT_MAX_LEN + SECURITY_SERVICE_KEY_MAX_LEN))
    {
        g_security_identity_import_runtime.last_result = SECURITY_IDENTITY_IMPORT_RESULT_TOO_LARGE;
        return g_security_identity_import_runtime.last_result;
    }

    payload_buffer = (uint8_t *)pvPortMalloc((size_t)payload_size + 1u);
    if (payload_buffer == NULL)
    {
        g_security_identity_import_runtime.last_result = SECURITY_IDENTITY_IMPORT_RESULT_OUT_OF_MEMORY;
        return g_security_identity_import_runtime.last_result;
    }

    memset(payload_buffer, 0, (size_t)payload_size + 1u);
    g_security_identity_import_runtime.session_active = true;
    g_security_identity_import_runtime.payload_buffer = payload_buffer;
    g_security_identity_import_runtime.expected_payload_size = payload_size;
    g_security_identity_import_runtime.expected_payload_crc32 = payload_crc32;
    g_security_identity_import_runtime.schema_version = schema_version;
    g_security_identity_import_runtime.bytes_written = 0u;
    g_security_identity_import_runtime.last_result = SECURITY_IDENTITY_IMPORT_RESULT_OK;
    return g_security_identity_import_runtime.last_result;
}

security_identity_import_service_result_t security_identity_import_service_write_chunk(const uint8_t *data,
                                                                                      size_t size)
{
    if (!g_security_identity_import_runtime.session_active)
    {
        g_security_identity_import_runtime.last_result = SECURITY_IDENTITY_IMPORT_RESULT_NOT_ACTIVE;
        return g_security_identity_import_runtime.last_result;
    }

    if (data == NULL || size == 0u)
    {
        g_security_identity_import_runtime.last_result = SECURITY_IDENTITY_IMPORT_RESULT_INVALID_ARGUMENT;
        return g_security_identity_import_runtime.last_result;
    }

    if (size > SECURITY_IDENTITY_IMPORT_SERVICE_MAX_CHUNK_BYTES)
    {
        g_security_identity_import_runtime.last_result = SECURITY_IDENTITY_IMPORT_RESULT_CHUNK_TOO_LARGE;
        return g_security_identity_import_runtime.last_result;
    }

    if ((size_t)g_security_identity_import_runtime.bytes_written + size >
        g_security_identity_import_runtime.expected_payload_size)
    {
        g_security_identity_import_runtime.last_result = SECURITY_IDENTITY_IMPORT_RESULT_INVALID_ARGUMENT;
        return g_security_identity_import_runtime.last_result;
    }

    memcpy(&g_security_identity_import_runtime.payload_buffer[g_security_identity_import_runtime.bytes_written],
           data,
           size);
    g_security_identity_import_runtime.bytes_written += (uint32_t)size;
    g_security_identity_import_runtime.payload_buffer[g_security_identity_import_runtime.bytes_written] = '\0';
    g_security_identity_import_runtime.last_result = SECURITY_IDENTITY_IMPORT_RESULT_OK;
    return g_security_identity_import_runtime.last_result;
}

security_identity_import_service_result_t security_identity_import_service_abort(void)
{
    if (!g_security_identity_import_runtime.session_active)
    {
        g_security_identity_import_runtime.last_result = SECURITY_IDENTITY_IMPORT_RESULT_NOT_ACTIVE;
        return g_security_identity_import_runtime.last_result;
    }

    security_identity_import_service_clear_session();
    g_security_identity_import_runtime.last_result = SECURITY_IDENTITY_IMPORT_RESULT_OK;
    return g_security_identity_import_runtime.last_result;
}

void security_identity_import_service_get_status(security_identity_import_service_status_t *out_status)
{
    if (out_status == NULL)
    {
        return;
    }

    memset(out_status, 0, sizeof(*out_status));
    out_status->session_active = g_security_identity_import_runtime.session_active;
    out_status->expected_payload_size = g_security_identity_import_runtime.expected_payload_size;
    out_status->expected_payload_crc32 = g_security_identity_import_runtime.expected_payload_crc32;
    out_status->schema_version = g_security_identity_import_runtime.schema_version;
    out_status->bytes_written = g_security_identity_import_runtime.bytes_written;
    out_status->remaining_bytes =
        (g_security_identity_import_runtime.expected_payload_size >= g_security_identity_import_runtime.bytes_written)
            ? (g_security_identity_import_runtime.expected_payload_size - g_security_identity_import_runtime.bytes_written)
            : 0u;
    out_status->payload_complete = security_identity_import_service_payload_complete();
    out_status->payload_crc32_matches = security_identity_import_service_payload_crc32_matches();
    out_status->last_result = g_security_identity_import_runtime.last_result;
}

security_identity_import_service_result_t security_identity_import_service_validate_staged(
    security_identity_import_service_report_t *out_report)
{
    security_identity_import_service_result_t result;

    if (out_report != NULL)
    {
        memset(out_report, 0, sizeof(*out_report));
        out_report->session_active = g_security_identity_import_runtime.session_active;
        out_report->payload_complete = security_identity_import_service_payload_complete();
        out_report->payload_crc32_matches = security_identity_import_service_payload_crc32_matches();
        out_report->payload_size = g_security_identity_import_runtime.expected_payload_size;
        out_report->expected_payload_crc32 = g_security_identity_import_runtime.expected_payload_crc32;
        out_report->schema_version = g_security_identity_import_runtime.schema_version;
    }

    result = security_identity_import_service_validate_payload(out_report);
    g_security_identity_import_runtime.last_result = result;

    if (out_report != NULL)
    {
        out_report->result = result;
        out_report->valid = (result == SECURITY_IDENTITY_IMPORT_RESULT_OK);
    }

    return result;
}

security_identity_import_service_result_t security_identity_import_service_apply_staged(
    security_identity_import_service_report_t *out_report)
{
    security_identity_import_service_report_t report = {0};
    security_identity_import_package_view_t package_view = {0};
    security_state_service_snapshot_t snapshot = {0};
    security_identity_import_service_result_t result =
        security_identity_import_service_validate_staged(&report);

    if (result != SECURITY_IDENTITY_IMPORT_RESULT_OK)
    {
        if (out_report != NULL)
        {
            *out_report = report;
        }
        return result;
    }

    result = security_identity_import_service_parse_package(&package_view);
    if (result != SECURITY_IDENTITY_IMPORT_RESULT_OK)
    {
        g_security_identity_import_runtime.last_result = result;
        report.valid = false;
        report.result = result;
        if (out_report != NULL)
        {
            *out_report = report;
        }
        return result;
    }

    if (!security_service_set_device_identity(package_view.certificate,
                                              package_view.certificate_size,
                                              package_view.private_key,
                                              package_view.private_key_size))
    {
        g_security_identity_import_runtime.last_result = SECURITY_IDENTITY_IMPORT_RESULT_APPLY_FAILED;
        report.valid = false;
        report.result = SECURITY_IDENTITY_IMPORT_RESULT_APPLY_FAILED;
        if (out_report != NULL)
        {
            *out_report = report;
        }
        return report.result;
    }

    if (security_state_service_get_snapshot(&snapshot))
    {
        const uint32_t next_generation = security_identity_import_service_next_generation(&snapshot);

        (void)security_state_service_set_identity_generations(snapshot.active_identity_generation,
                                                              next_generation);
    }

    security_identity_import_service_clear_session();
    g_security_identity_import_runtime.last_result = SECURITY_IDENTITY_IMPORT_RESULT_OK;
    report.result = SECURITY_IDENTITY_IMPORT_RESULT_OK;
    report.valid = true;
    report.reboot_required = true;
    if (out_report != NULL)
    {
        *out_report = report;
    }
    return report.result;
}

const char *security_identity_import_service_result_to_string(security_identity_import_service_result_t result)
{
    switch (result)
    {
    case SECURITY_IDENTITY_IMPORT_RESULT_NONE:
        return "NONE";
    case SECURITY_IDENTITY_IMPORT_RESULT_OK:
        return "OK";
    case SECURITY_IDENTITY_IMPORT_RESULT_BUSY:
        return "BUSY";
    case SECURITY_IDENTITY_IMPORT_RESULT_NOT_ACTIVE:
        return "NOT_ACTIVE";
    case SECURITY_IDENTITY_IMPORT_RESULT_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case SECURITY_IDENTITY_IMPORT_RESULT_CHUNK_TOO_LARGE:
        return "CHUNK_TOO_LARGE";
    case SECURITY_IDENTITY_IMPORT_RESULT_TOO_LARGE:
        return "TOO_LARGE";
    case SECURITY_IDENTITY_IMPORT_RESULT_OUT_OF_MEMORY:
        return "OUT_OF_MEMORY";
    case SECURITY_IDENTITY_IMPORT_RESULT_NOT_READY:
        return "NOT_READY";
    case SECURITY_IDENTITY_IMPORT_RESULT_CRC_MISMATCH:
        return "CRC_MISMATCH";
    case SECURITY_IDENTITY_IMPORT_RESULT_SCHEMA_MISMATCH:
        return "SCHEMA_MISMATCH";
    case SECURITY_IDENTITY_IMPORT_RESULT_PACKAGE_INVALID:
        return "PACKAGE_INVALID";
    case SECURITY_IDENTITY_IMPORT_RESULT_CERTIFICATE_PARSE_FAILED:
        return "CERTIFICATE_PARSE_FAILED";
    case SECURITY_IDENTITY_IMPORT_RESULT_PRIVATE_KEY_PARSE_FAILED:
        return "PRIVATE_KEY_PARSE_FAILED";
    case SECURITY_IDENTITY_IMPORT_RESULT_KEY_PAIR_MISMATCH:
        return "KEY_PAIR_MISMATCH";
    case SECURITY_IDENTITY_IMPORT_RESULT_APPLICATION_URI_MISMATCH:
        return "APPLICATION_URI_MISMATCH";
    case SECURITY_IDENTITY_IMPORT_RESULT_CERTIFICATE_NOT_YET_VALID:
        return "CERTIFICATE_NOT_YET_VALID";
    case SECURITY_IDENTITY_IMPORT_RESULT_CERTIFICATE_EXPIRED:
        return "CERTIFICATE_EXPIRED";
    case SECURITY_IDENTITY_IMPORT_RESULT_APPLY_FAILED:
        return "APPLY_FAILED";
    case SECURITY_IDENTITY_IMPORT_RESULT_INTERNAL_ERROR:
        return "INTERNAL_ERROR";
    default:
        return "UNKNOWN";
    }
}
