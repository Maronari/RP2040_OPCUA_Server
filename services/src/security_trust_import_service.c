#include "services/security_trust_import_service.h"

#include <string.h>

#include <FreeRTOS.h>

#include "mbedtls/x509_crt.h"
#include "ota/ota_state.h"
#include "services/security_service.h"

typedef struct
{
    bool session_active;
    uint8_t *payload_buffer;
    uint32_t expected_payload_size;
    uint32_t expected_payload_crc32;
    uint32_t schema_version;
    uint32_t bytes_written;
    security_trust_import_service_result_t last_result;
} security_trust_import_runtime_t;

static security_trust_import_runtime_t g_security_trust_import_runtime = {0};

static void security_trust_import_service_clear_session(void)
{
    if (g_security_trust_import_runtime.payload_buffer != NULL)
    {
        vPortFree(g_security_trust_import_runtime.payload_buffer);
    }

    g_security_trust_import_runtime.session_active = false;
    g_security_trust_import_runtime.payload_buffer = NULL;
    g_security_trust_import_runtime.expected_payload_size = 0u;
    g_security_trust_import_runtime.expected_payload_crc32 = 0u;
    g_security_trust_import_runtime.schema_version = 0u;
    g_security_trust_import_runtime.bytes_written = 0u;
}

static bool security_trust_import_service_payload_complete(void)
{
    return g_security_trust_import_runtime.session_active &&
           g_security_trust_import_runtime.expected_payload_size > 0u &&
           g_security_trust_import_runtime.bytes_written == g_security_trust_import_runtime.expected_payload_size;
}

static bool security_trust_import_service_payload_crc32_matches(void)
{
    if (!security_trust_import_service_payload_complete() ||
        g_security_trust_import_runtime.payload_buffer == NULL)
    {
        return false;
    }

    return ota_crc32_compute(g_security_trust_import_runtime.payload_buffer,
                             g_security_trust_import_runtime.expected_payload_size) ==
           g_security_trust_import_runtime.expected_payload_crc32;
}

static bool security_trust_import_service_payload_is_pem(const uint8_t *payload, size_t payload_size)
{
    static const char pem_prefix[] = "-----BEGIN";

    return payload != NULL &&
           payload_size >= (sizeof(pem_prefix) - 1u) &&
           memcmp(payload, pem_prefix, sizeof(pem_prefix) - 1u) == 0;
}

static security_trust_import_service_result_t security_trust_import_service_validate_payload(uint32_t *out_certificate_count)
{
    mbedtls_x509_crt certificates;
    uint32_t certificate_count = 0u;
    size_t parse_length;
    int rc;

    if (!g_security_trust_import_runtime.session_active ||
        g_security_trust_import_runtime.payload_buffer == NULL)
    {
        return SECURITY_TRUST_IMPORT_RESULT_NOT_ACTIVE;
    }

    if (!security_trust_import_service_payload_complete())
    {
        return SECURITY_TRUST_IMPORT_RESULT_NOT_READY;
    }

    if (!security_trust_import_service_payload_crc32_matches())
    {
        return SECURITY_TRUST_IMPORT_RESULT_CRC_MISMATCH;
    }

    if (g_security_trust_import_runtime.schema_version != SECURITY_TRUST_IMPORT_SERVICE_SCHEMA_VERSION)
    {
        return SECURITY_TRUST_IMPORT_RESULT_SCHEMA_MISMATCH;
    }

    parse_length = g_security_trust_import_runtime.expected_payload_size;
    if (security_trust_import_service_payload_is_pem(g_security_trust_import_runtime.payload_buffer,
                                                     parse_length))
    {
        parse_length += 1u;
    }

    mbedtls_x509_crt_init(&certificates);
    rc = mbedtls_x509_crt_parse(&certificates,
                                g_security_trust_import_runtime.payload_buffer,
                                parse_length);
    if (rc != 0)
    {
        mbedtls_x509_crt_free(&certificates);
        return SECURITY_TRUST_IMPORT_RESULT_PARSE_FAILED;
    }

    for (const mbedtls_x509_crt *cursor = &certificates; cursor != NULL; cursor = cursor->next)
    {
        if (cursor->raw.len > 0u)
        {
            ++certificate_count;
        }
    }

    mbedtls_x509_crt_free(&certificates);
    if (certificate_count == 0u)
    {
        return SECURITY_TRUST_IMPORT_RESULT_PARSE_FAILED;
    }

    if (out_certificate_count != NULL)
    {
        *out_certificate_count = certificate_count;
    }

    return SECURITY_TRUST_IMPORT_RESULT_OK;
}

void security_trust_import_service_init(void)
{
    memset(&g_security_trust_import_runtime, 0, sizeof(g_security_trust_import_runtime));
    g_security_trust_import_runtime.last_result = SECURITY_TRUST_IMPORT_RESULT_NONE;
}

security_trust_import_service_result_t security_trust_import_service_begin(uint32_t payload_size,
                                                                          uint32_t payload_crc32,
                                                                          uint32_t schema_version)
{
    uint8_t *payload_buffer = NULL;

    if (g_security_trust_import_runtime.session_active)
    {
        g_security_trust_import_runtime.last_result = SECURITY_TRUST_IMPORT_RESULT_BUSY;
        return g_security_trust_import_runtime.last_result;
    }

    if (payload_size == 0u)
    {
        g_security_trust_import_runtime.last_result = SECURITY_TRUST_IMPORT_RESULT_INVALID_ARGUMENT;
        return g_security_trust_import_runtime.last_result;
    }

    if (payload_size > SECURITY_SERVICE_TRUST_LIST_MAX_LEN)
    {
        g_security_trust_import_runtime.last_result = SECURITY_TRUST_IMPORT_RESULT_TOO_LARGE;
        return g_security_trust_import_runtime.last_result;
    }

    payload_buffer = (uint8_t *)pvPortMalloc((size_t)payload_size + 1u);
    if (payload_buffer == NULL)
    {
        g_security_trust_import_runtime.last_result = SECURITY_TRUST_IMPORT_RESULT_OUT_OF_MEMORY;
        return g_security_trust_import_runtime.last_result;
    }

    memset(payload_buffer, 0, (size_t)payload_size + 1u);
    g_security_trust_import_runtime.session_active = true;
    g_security_trust_import_runtime.payload_buffer = payload_buffer;
    g_security_trust_import_runtime.expected_payload_size = payload_size;
    g_security_trust_import_runtime.expected_payload_crc32 = payload_crc32;
    g_security_trust_import_runtime.schema_version = schema_version;
    g_security_trust_import_runtime.bytes_written = 0u;
    g_security_trust_import_runtime.last_result = SECURITY_TRUST_IMPORT_RESULT_OK;
    return g_security_trust_import_runtime.last_result;
}

security_trust_import_service_result_t security_trust_import_service_write_chunk(const uint8_t *data,
                                                                                size_t size)
{
    if (!g_security_trust_import_runtime.session_active)
    {
        g_security_trust_import_runtime.last_result = SECURITY_TRUST_IMPORT_RESULT_NOT_ACTIVE;
        return g_security_trust_import_runtime.last_result;
    }

    if (data == NULL || size == 0u)
    {
        g_security_trust_import_runtime.last_result = SECURITY_TRUST_IMPORT_RESULT_INVALID_ARGUMENT;
        return g_security_trust_import_runtime.last_result;
    }

    if (size > SECURITY_TRUST_IMPORT_SERVICE_MAX_CHUNK_BYTES)
    {
        g_security_trust_import_runtime.last_result = SECURITY_TRUST_IMPORT_RESULT_CHUNK_TOO_LARGE;
        return g_security_trust_import_runtime.last_result;
    }

    if ((size_t)g_security_trust_import_runtime.bytes_written + size >
        g_security_trust_import_runtime.expected_payload_size)
    {
        g_security_trust_import_runtime.last_result = SECURITY_TRUST_IMPORT_RESULT_INVALID_ARGUMENT;
        return g_security_trust_import_runtime.last_result;
    }

    memcpy(&g_security_trust_import_runtime.payload_buffer[g_security_trust_import_runtime.bytes_written],
           data,
           size);
    g_security_trust_import_runtime.bytes_written += (uint32_t)size;
    g_security_trust_import_runtime.payload_buffer[g_security_trust_import_runtime.bytes_written] = '\0';
    g_security_trust_import_runtime.last_result = SECURITY_TRUST_IMPORT_RESULT_OK;
    return g_security_trust_import_runtime.last_result;
}

security_trust_import_service_result_t security_trust_import_service_abort(void)
{
    if (!g_security_trust_import_runtime.session_active)
    {
        g_security_trust_import_runtime.last_result = SECURITY_TRUST_IMPORT_RESULT_NOT_ACTIVE;
        return g_security_trust_import_runtime.last_result;
    }

    security_trust_import_service_clear_session();
    g_security_trust_import_runtime.last_result = SECURITY_TRUST_IMPORT_RESULT_OK;
    return g_security_trust_import_runtime.last_result;
}

void security_trust_import_service_get_status(security_trust_import_service_status_t *out_status)
{
    if (out_status == NULL)
    {
        return;
    }

    memset(out_status, 0, sizeof(*out_status));
    out_status->session_active = g_security_trust_import_runtime.session_active;
    out_status->expected_payload_size = g_security_trust_import_runtime.expected_payload_size;
    out_status->expected_payload_crc32 = g_security_trust_import_runtime.expected_payload_crc32;
    out_status->schema_version = g_security_trust_import_runtime.schema_version;
    out_status->bytes_written = g_security_trust_import_runtime.bytes_written;
    out_status->remaining_bytes =
        (g_security_trust_import_runtime.expected_payload_size >= g_security_trust_import_runtime.bytes_written)
            ? (g_security_trust_import_runtime.expected_payload_size - g_security_trust_import_runtime.bytes_written)
            : 0u;
    out_status->payload_complete = security_trust_import_service_payload_complete();
    out_status->payload_crc32_matches = security_trust_import_service_payload_crc32_matches();
    out_status->last_result = g_security_trust_import_runtime.last_result;
}

security_trust_import_service_result_t security_trust_import_service_validate_staged(
    security_trust_import_service_report_t *out_report)
{
    security_trust_import_service_result_t result;
    uint32_t certificate_count = 0u;

    if (out_report != NULL)
    {
        memset(out_report, 0, sizeof(*out_report));
        out_report->session_active = g_security_trust_import_runtime.session_active;
        out_report->payload_complete = security_trust_import_service_payload_complete();
        out_report->payload_crc32_matches = security_trust_import_service_payload_crc32_matches();
        out_report->payload_size = g_security_trust_import_runtime.expected_payload_size;
        out_report->expected_payload_crc32 = g_security_trust_import_runtime.expected_payload_crc32;
        out_report->schema_version = g_security_trust_import_runtime.schema_version;
    }

    result = security_trust_import_service_validate_payload(&certificate_count);
    g_security_trust_import_runtime.last_result = result;

    if (out_report != NULL)
    {
        out_report->result = result;
        out_report->valid = (result == SECURITY_TRUST_IMPORT_RESULT_OK);
        out_report->certificate_count = certificate_count;
    }

    return result;
}

security_trust_import_service_result_t security_trust_import_service_apply_staged(
    security_trust_import_service_report_t *out_report)
{
    security_trust_import_service_report_t report = {0};
    security_trust_import_service_result_t result =
        security_trust_import_service_validate_staged(&report);

    if (result != SECURITY_TRUST_IMPORT_RESULT_OK)
    {
        if (out_report != NULL)
        {
            *out_report = report;
        }
        return result;
    }

    if (!security_service_set_trust_list(g_security_trust_import_runtime.payload_buffer,
                                         g_security_trust_import_runtime.expected_payload_size))
    {
        g_security_trust_import_runtime.last_result = SECURITY_TRUST_IMPORT_RESULT_APPLY_FAILED;
        report.valid = false;
        report.result = SECURITY_TRUST_IMPORT_RESULT_APPLY_FAILED;
        if (out_report != NULL)
        {
            *out_report = report;
        }
        return report.result;
    }

    security_trust_import_service_clear_session();
    g_security_trust_import_runtime.last_result = SECURITY_TRUST_IMPORT_RESULT_OK;
    report.result = SECURITY_TRUST_IMPORT_RESULT_OK;
    report.valid = true;
    if (out_report != NULL)
    {
        *out_report = report;
    }
    return report.result;
}

const char *security_trust_import_service_result_to_string(security_trust_import_service_result_t result)
{
    switch (result)
    {
    case SECURITY_TRUST_IMPORT_RESULT_NONE:
        return "NONE";
    case SECURITY_TRUST_IMPORT_RESULT_OK:
        return "OK";
    case SECURITY_TRUST_IMPORT_RESULT_BUSY:
        return "BUSY";
    case SECURITY_TRUST_IMPORT_RESULT_NOT_ACTIVE:
        return "NOT_ACTIVE";
    case SECURITY_TRUST_IMPORT_RESULT_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case SECURITY_TRUST_IMPORT_RESULT_CHUNK_TOO_LARGE:
        return "CHUNK_TOO_LARGE";
    case SECURITY_TRUST_IMPORT_RESULT_TOO_LARGE:
        return "TOO_LARGE";
    case SECURITY_TRUST_IMPORT_RESULT_OUT_OF_MEMORY:
        return "OUT_OF_MEMORY";
    case SECURITY_TRUST_IMPORT_RESULT_NOT_READY:
        return "NOT_READY";
    case SECURITY_TRUST_IMPORT_RESULT_CRC_MISMATCH:
        return "CRC_MISMATCH";
    case SECURITY_TRUST_IMPORT_RESULT_SCHEMA_MISMATCH:
        return "SCHEMA_MISMATCH";
    case SECURITY_TRUST_IMPORT_RESULT_PARSE_FAILED:
        return "PARSE_FAILED";
    case SECURITY_TRUST_IMPORT_RESULT_APPLY_FAILED:
        return "APPLY_FAILED";
    case SECURITY_TRUST_IMPORT_RESULT_INTERNAL_ERROR:
        return "INTERNAL_ERROR";
    default:
        return "UNKNOWN";
    }
}
