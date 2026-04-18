#include "services/config_import_service.h"

#include <string.h>

#include <FreeRTOS.h>

#include <config_store_limits.h>
#include "ota/ota_state.h"

typedef struct
{
    bool session_active;
    uint8_t *payload_buffer;
    uint32_t expected_payload_size;
    uint32_t expected_payload_crc32;
    uint32_t schema_version;
    uint32_t bytes_written;
    config_import_service_result_t last_result;
} config_import_runtime_t;

static config_import_runtime_t g_config_import_runtime = {0};

static void config_import_service_clear_session(void)
{
    if (g_config_import_runtime.payload_buffer != NULL)
    {
        vPortFree(g_config_import_runtime.payload_buffer);
    }

    g_config_import_runtime.session_active = false;
    g_config_import_runtime.payload_buffer = NULL;
    g_config_import_runtime.expected_payload_size = 0u;
    g_config_import_runtime.expected_payload_crc32 = 0u;
    g_config_import_runtime.schema_version = 0u;
    g_config_import_runtime.bytes_written = 0u;
}

void config_import_service_init(void)
{
    memset(&g_config_import_runtime, 0, sizeof(g_config_import_runtime));
    g_config_import_runtime.last_result = CONFIG_IMPORT_RESULT_NONE;
}

config_import_service_result_t config_import_service_begin(uint32_t payload_size,
                                                           uint32_t payload_crc32,
                                                           uint32_t schema_version)
{
    uint8_t *payload_buffer = NULL;

    if (g_config_import_runtime.session_active)
    {
        g_config_import_runtime.last_result = CONFIG_IMPORT_RESULT_BUSY;
        return g_config_import_runtime.last_result;
    }

    if (payload_size == 0u)
    {
        g_config_import_runtime.last_result = CONFIG_IMPORT_RESULT_INVALID_ARGUMENT;
        return g_config_import_runtime.last_result;
    }

    if (payload_size > CONFIG_STORE_MAX_PAYLOAD_SIZE_BYTES)
    {
        g_config_import_runtime.last_result = CONFIG_IMPORT_RESULT_TOO_LARGE;
        return g_config_import_runtime.last_result;
    }

    payload_buffer = (uint8_t *)pvPortMalloc((size_t)payload_size + 1u);
    if (payload_buffer == NULL)
    {
        g_config_import_runtime.last_result = CONFIG_IMPORT_RESULT_OUT_OF_MEMORY;
        return g_config_import_runtime.last_result;
    }

    memset(payload_buffer, 0, (size_t)payload_size + 1u);
    g_config_import_runtime.session_active = true;
    g_config_import_runtime.payload_buffer = payload_buffer;
    g_config_import_runtime.expected_payload_size = payload_size;
    g_config_import_runtime.expected_payload_crc32 = payload_crc32;
    g_config_import_runtime.schema_version = schema_version;
    g_config_import_runtime.bytes_written = 0u;
    g_config_import_runtime.last_result = CONFIG_IMPORT_RESULT_OK;
    return g_config_import_runtime.last_result;
}

config_import_service_result_t config_import_service_write_chunk(const uint8_t *data, size_t size)
{
    if (!g_config_import_runtime.session_active)
    {
        g_config_import_runtime.last_result = CONFIG_IMPORT_RESULT_NOT_ACTIVE;
        return g_config_import_runtime.last_result;
    }

    if (data == NULL || size == 0u)
    {
        g_config_import_runtime.last_result = CONFIG_IMPORT_RESULT_INVALID_ARGUMENT;
        return g_config_import_runtime.last_result;
    }

    if (size > CONFIG_IMPORT_SERVICE_MAX_CHUNK_BYTES)
    {
        g_config_import_runtime.last_result = CONFIG_IMPORT_RESULT_CHUNK_TOO_LARGE;
        return g_config_import_runtime.last_result;
    }

    if ((size_t)g_config_import_runtime.bytes_written + size > g_config_import_runtime.expected_payload_size)
    {
        g_config_import_runtime.last_result = CONFIG_IMPORT_RESULT_INVALID_ARGUMENT;
        return g_config_import_runtime.last_result;
    }

    memcpy(&g_config_import_runtime.payload_buffer[g_config_import_runtime.bytes_written], data, size);
    g_config_import_runtime.bytes_written += (uint32_t)size;
    g_config_import_runtime.payload_buffer[g_config_import_runtime.bytes_written] = '\0';
    g_config_import_runtime.last_result = CONFIG_IMPORT_RESULT_OK;
    return g_config_import_runtime.last_result;
}

config_import_service_result_t config_import_service_abort(void)
{
    if (!g_config_import_runtime.session_active)
    {
        g_config_import_runtime.last_result = CONFIG_IMPORT_RESULT_NOT_ACTIVE;
        return g_config_import_runtime.last_result;
    }

    config_import_service_clear_session();
    g_config_import_runtime.last_result = CONFIG_IMPORT_RESULT_OK;
    return g_config_import_runtime.last_result;
}

void config_import_service_get_status(config_import_service_status_t *out_status)
{
    if (out_status == NULL)
    {
        return;
    }

    memset(out_status, 0, sizeof(*out_status));
    out_status->session_active = g_config_import_runtime.session_active;
    out_status->expected_payload_size = g_config_import_runtime.expected_payload_size;
    out_status->expected_payload_crc32 = g_config_import_runtime.expected_payload_crc32;
    out_status->schema_version = g_config_import_runtime.schema_version;
    out_status->bytes_written = g_config_import_runtime.bytes_written;
    out_status->remaining_bytes =
        (g_config_import_runtime.expected_payload_size >= g_config_import_runtime.bytes_written)
            ? (g_config_import_runtime.expected_payload_size - g_config_import_runtime.bytes_written)
            : 0u;
    out_status->payload_complete =
        g_config_import_runtime.session_active &&
        g_config_import_runtime.expected_payload_size > 0u &&
        g_config_import_runtime.bytes_written == g_config_import_runtime.expected_payload_size;
    out_status->payload_crc32_matches =
        out_status->payload_complete &&
        ota_crc32_compute(g_config_import_runtime.payload_buffer,
                          g_config_import_runtime.expected_payload_size) == g_config_import_runtime.expected_payload_crc32;
    out_status->last_result = g_config_import_runtime.last_result;
}

bool config_import_service_get_payload_view(const uint8_t **out_payload,
                                            size_t *out_payload_size,
                                            uint32_t *out_expected_crc32,
                                            uint32_t *out_schema_version)
{
    config_import_service_status_t status = {0};

    config_import_service_get_status(&status);
    if (!status.payload_complete || !status.payload_crc32_matches || g_config_import_runtime.payload_buffer == NULL)
    {
        return false;
    }

    if (out_payload != NULL)
    {
        *out_payload = g_config_import_runtime.payload_buffer;
    }

    if (out_payload_size != NULL)
    {
        *out_payload_size = g_config_import_runtime.expected_payload_size;
    }

    if (out_expected_crc32 != NULL)
    {
        *out_expected_crc32 = g_config_import_runtime.expected_payload_crc32;
    }

    if (out_schema_version != NULL)
    {
        *out_schema_version = g_config_import_runtime.schema_version;
    }

    return true;
}

const char *config_import_service_result_to_string(config_import_service_result_t result)
{
    switch (result)
    {
    case CONFIG_IMPORT_RESULT_NONE:
        return "NONE";
    case CONFIG_IMPORT_RESULT_OK:
        return "OK";
    case CONFIG_IMPORT_RESULT_BUSY:
        return "BUSY";
    case CONFIG_IMPORT_RESULT_NOT_ACTIVE:
        return "NOT_ACTIVE";
    case CONFIG_IMPORT_RESULT_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case CONFIG_IMPORT_RESULT_CHUNK_TOO_LARGE:
        return "CHUNK_TOO_LARGE";
    case CONFIG_IMPORT_RESULT_TOO_LARGE:
        return "TOO_LARGE";
    case CONFIG_IMPORT_RESULT_OUT_OF_MEMORY:
        return "OUT_OF_MEMORY";
    case CONFIG_IMPORT_RESULT_INTERNAL_ERROR:
        return "INTERNAL_ERROR";
    default:
        return "UNKNOWN";
    }
}
