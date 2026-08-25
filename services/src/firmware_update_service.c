/**
 * @file
 * @brief OTA update session coordinator; see
 *        services/include/services/firmware_update_service.h for the
 *        public API contract.
 *
 * Ownership: `g_firmware_update_runtime` (session flag, embedded
 * `ota_flash_writer_t`, expected CRC/version, last result) is owned
 * exclusively by this module.
 *
 * Concurrency: no internal locking; relies on all callers running from the
 * OPC UA method-callback context (see file-level note in the header) so
 * only one session is ever manipulated at a time.
 */
#include "services/firmware_update_service.h"

#include <string.h>

#include "ota/ota_flash.h"

/** @brief Runtime state for the single in-flight (or most recently completed) OTA session. */
typedef struct
{
    bool session_active;
    ota_flash_writer_t writer;
    uint32_t expected_image_crc32;
    uint32_t version_number;
    firmware_update_service_result_t last_result;
} firmware_update_runtime_t;

static firmware_update_runtime_t g_firmware_update_runtime = {0};

/** @brief Maps an `ota_flash_status_t` from the OTA flash writer to the service's result vocabulary. */
static firmware_update_service_result_t firmware_update_service_map_flash_status(ota_flash_status_t status)
{
    switch (status)
    {
    case OTA_FLASH_STATUS_OK:
        return FIRMWARE_UPDATE_RESULT_OK;
    case OTA_FLASH_STATUS_INVALID_ARGUMENT:
    case OTA_FLASH_STATUS_OUT_OF_RANGE:
        return FIRMWARE_UPDATE_RESULT_INVALID_ARGUMENT;
    case OTA_FLASH_STATUS_FLASH_OP_FAILED:
        return FIRMWARE_UPDATE_RESULT_FLASH_ERROR;
    case OTA_FLASH_STATUS_VERIFY_FAILED:
        return FIRMWARE_UPDATE_RESULT_VERIFY_FAILED;
    case OTA_FLASH_STATUS_WRITER_STATE_ERROR:
    default:
        return FIRMWARE_UPDATE_RESULT_INTERNAL_ERROR;
    }
}

/** @brief Resets the writer and clears session/CRC/version fields, without touching `last_result`. */
static void firmware_update_service_clear_session(void)
{
    ota_flash_writer_init(&g_firmware_update_runtime.writer);
    g_firmware_update_runtime.session_active = false;
    g_firmware_update_runtime.expected_image_crc32 = 0u;
    g_firmware_update_runtime.version_number = 0u;
}

void firmware_update_service_init(void)
{
    memset(&g_firmware_update_runtime, 0, sizeof(g_firmware_update_runtime));
    ota_flash_writer_init(&g_firmware_update_runtime.writer);
    g_firmware_update_runtime.last_result = FIRMWARE_UPDATE_RESULT_NONE;
}

firmware_update_service_result_t firmware_update_service_begin(uint32_t image_size,
                                                              uint32_t image_crc32,
                                                              uint32_t version_number)
{
    ota_flash_status_t flash_status;

    if (g_firmware_update_runtime.session_active)
    {
        g_firmware_update_runtime.last_result = FIRMWARE_UPDATE_RESULT_BUSY;
        return g_firmware_update_runtime.last_result;
    }

    if (image_size == 0u)
    {
        g_firmware_update_runtime.last_result = FIRMWARE_UPDATE_RESULT_INVALID_ARGUMENT;
        return g_firmware_update_runtime.last_result;
    }

    flash_status = ota_flash_writer_begin(&g_firmware_update_runtime.writer, image_size);
    g_firmware_update_runtime.last_result = firmware_update_service_map_flash_status(flash_status);
    if (flash_status != OTA_FLASH_STATUS_OK)
    {
        firmware_update_service_clear_session();
        return g_firmware_update_runtime.last_result;
    }

    g_firmware_update_runtime.session_active = true;
    g_firmware_update_runtime.expected_image_crc32 = image_crc32;
    g_firmware_update_runtime.version_number = version_number;
    g_firmware_update_runtime.last_result = FIRMWARE_UPDATE_RESULT_OK;
    return g_firmware_update_runtime.last_result;
}

firmware_update_service_result_t firmware_update_service_write_chunk(const uint8_t *data, size_t size)
{
    ota_flash_status_t flash_status;

    if (!g_firmware_update_runtime.session_active)
    {
        g_firmware_update_runtime.last_result = FIRMWARE_UPDATE_RESULT_NOT_ACTIVE;
        return g_firmware_update_runtime.last_result;
    }

    if (data == NULL || size == 0u)
    {
        g_firmware_update_runtime.last_result = FIRMWARE_UPDATE_RESULT_INVALID_ARGUMENT;
        return g_firmware_update_runtime.last_result;
    }

    if (size > FIRMWARE_UPDATE_SERVICE_MAX_CHUNK_BYTES)
    {
        g_firmware_update_runtime.last_result = FIRMWARE_UPDATE_RESULT_CHUNK_TOO_LARGE;
        return g_firmware_update_runtime.last_result;
    }

    flash_status = ota_flash_writer_write(&g_firmware_update_runtime.writer, data, size);
    g_firmware_update_runtime.last_result = firmware_update_service_map_flash_status(flash_status);
    if (flash_status != OTA_FLASH_STATUS_OK)
    {
        firmware_update_service_clear_session();
        return g_firmware_update_runtime.last_result;
    }

    g_firmware_update_runtime.last_result = FIRMWARE_UPDATE_RESULT_OK;
    return g_firmware_update_runtime.last_result;
}

firmware_update_service_result_t firmware_update_service_finalize(void)
{
    ota_flash_status_t flash_status;

    if (!g_firmware_update_runtime.session_active)
    {
        g_firmware_update_runtime.last_result = FIRMWARE_UPDATE_RESULT_NOT_ACTIVE;
        return g_firmware_update_runtime.last_result;
    }

    flash_status = ota_flash_writer_finish(&g_firmware_update_runtime.writer,
                                           g_firmware_update_runtime.expected_image_crc32,
                                           g_firmware_update_runtime.version_number);
    g_firmware_update_runtime.last_result = firmware_update_service_map_flash_status(flash_status);
    firmware_update_service_clear_session();
    return g_firmware_update_runtime.last_result;
}

firmware_update_service_result_t firmware_update_service_abort(void)
{
    ota_flash_status_t flash_status;

    flash_status = ota_flash_reset_state();
    g_firmware_update_runtime.last_result = firmware_update_service_map_flash_status(flash_status);
    firmware_update_service_clear_session();
    return g_firmware_update_runtime.last_result;
}

void firmware_update_service_get_status(firmware_update_service_status_t *out_status)
{
    if (out_status == NULL)
    {
        return;
    }

    memset(out_status, 0, sizeof(*out_status));
    out_status->session_active = g_firmware_update_runtime.session_active;
    out_status->expected_image_size = g_firmware_update_runtime.writer.image_size;
    out_status->expected_image_crc32 = g_firmware_update_runtime.expected_image_crc32;
    out_status->version_number = g_firmware_update_runtime.version_number;
    out_status->bytes_written = g_firmware_update_runtime.writer.bytes_written;
    out_status->remaining_bytes =
        (g_firmware_update_runtime.writer.image_size >= g_firmware_update_runtime.writer.bytes_written)
            ? (g_firmware_update_runtime.writer.image_size - g_firmware_update_runtime.writer.bytes_written)
            : 0u;
    out_status->last_result = g_firmware_update_runtime.last_result;
    out_status->ota_state_valid = ota_flash_read_state(&out_status->ota_state);
}

const char *firmware_update_service_result_to_string(firmware_update_service_result_t result)
{
    switch (result)
    {
    case FIRMWARE_UPDATE_RESULT_NONE:
        return "NONE";
    case FIRMWARE_UPDATE_RESULT_OK:
        return "OK";
    case FIRMWARE_UPDATE_RESULT_BUSY:
        return "BUSY";
    case FIRMWARE_UPDATE_RESULT_NOT_ACTIVE:
        return "NOT_ACTIVE";
    case FIRMWARE_UPDATE_RESULT_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case FIRMWARE_UPDATE_RESULT_CHUNK_TOO_LARGE:
        return "CHUNK_TOO_LARGE";
    case FIRMWARE_UPDATE_RESULT_FLASH_ERROR:
        return "FLASH_ERROR";
    case FIRMWARE_UPDATE_RESULT_VERIFY_FAILED:
        return "VERIFY_FAILED";
    case FIRMWARE_UPDATE_RESULT_INTERNAL_ERROR:
        return "INTERNAL_ERROR";
    default:
        return "UNKNOWN";
    }
}
