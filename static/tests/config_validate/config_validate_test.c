#include "services/config_import_service.h"
#include "services/config_validate_service.h"
#include "services/config_wire.h"
#include "services/driver_catalog.h"
#include "ota/ota_state.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static bool g_fail_allocation;

void *pvPortMalloc(size_t size)
{
    return g_fail_allocation ? NULL : malloc(size);
}

void vPortFree(void *pointer)
{
    free(pointer);
}

static bool validate_source(uint8_t source_index, uint8_t source_flags)
{
    return source_index == 0u && source_flags == 0u;
}

static const driver_catalog_entry_t g_known_driver = {
    .driver_id = CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP,
    .name = "host-test",
    .validate_source = validate_source,
};

const driver_catalog_entry_t *driver_catalog_get_by_id(uint8_t driver_id)
{
    return driver_id == g_known_driver.driver_id ? &g_known_driver : NULL;
}

const driver_catalog_entry_t *driver_catalog_get_by_name(const char *name)
{
    return name != NULL && strcmp(name, g_known_driver.name) == 0 ? &g_known_driver : NULL;
}

size_t driver_catalog_count(void)
{
    return 1u;
}

const driver_catalog_entry_t *driver_catalog_get_by_index(size_t index)
{
    return index == 0u ? &g_known_driver : NULL;
}

bool config_service_location_path_is_valid(const char *value)
{
    return value != NULL && (value[0] == '\0' || strcmp(value, "valid/location") == 0);
}

static device_profile_config_t valid_candidate(void)
{
    device_profile_config_t candidate = {0};

    candidate.opcua_port = 4840u;
    candidate.network_mode = NETWORK_MODE_DHCP;
    candidate.channels[0].in_use = 1u;
    candidate.channels[0].driver_id = CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP;
    candidate.channels[0].kind = TELEMETRY_CHANNEL_KIND_ANALOG;
    candidate.channels[0].value_type = TELEMETRY_CHANNEL_VALUE_FLOAT;
    strcpy(candidate.channels[0].id, "temperature");
    return candidate;
}

static void stage(const device_profile_config_t *candidate, uint32_t crc)
{
    assert(config_import_service_begin((uint32_t)sizeof(*candidate), crc,
                                       CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY) ==
           CONFIG_IMPORT_RESULT_OK);
    assert(config_import_service_write_chunk((const uint8_t *)candidate,
                                             sizeof(*candidate)) == CONFIG_IMPORT_RESULT_OK);
}

static void expect_rejected(const device_profile_config_t *candidate,
                            config_validate_service_result_t expected)
{
    config_validate_service_report_t report = {0};
    device_profile_config_t output;
    device_profile_config_t before;

    memset(&output, 0xa5, sizeof(output));
    before = output;
    stage(candidate, ota_crc32_compute(candidate, sizeof(*candidate)));
    assert(config_validate_service_validate_imported(&report, &output) == expected);
    assert(!report.valid && report.result == expected);
    assert(memcmp(&output, &before, sizeof(output)) == 0);
    assert(config_import_service_abort() == CONFIG_IMPORT_RESULT_OK);
}

static void test_import_bounds_and_staging(void)
{
    config_import_service_status_t status = {0};
    uint8_t byte = 0u;

    config_import_service_init();
    assert(config_import_service_begin(0u, 0u, 1u) == CONFIG_IMPORT_RESULT_INVALID_ARGUMENT);
    assert(config_import_service_begin(65536u, 0u, 1u) == CONFIG_IMPORT_RESULT_TOO_LARGE);
    g_fail_allocation = true;
    assert(config_import_service_begin(1u, 0u, 1u) == CONFIG_IMPORT_RESULT_OUT_OF_MEMORY);
    g_fail_allocation = false;
    assert(config_import_service_begin(2u, 0u, 1u) == CONFIG_IMPORT_RESULT_OK);
    assert(config_import_service_begin(2u, 0u, 1u) == CONFIG_IMPORT_RESULT_BUSY);
    assert(config_import_service_write_chunk(NULL, 1u) == CONFIG_IMPORT_RESULT_INVALID_ARGUMENT);
    assert(config_import_service_write_chunk(&byte, CONFIG_IMPORT_SERVICE_MAX_CHUNK_BYTES + 1u) ==
           CONFIG_IMPORT_RESULT_CHUNK_TOO_LARGE);
    assert(config_import_service_write_chunk(&byte, 1u) == CONFIG_IMPORT_RESULT_OK);
    config_import_service_get_status(&status);
    assert(status.session_active && !status.payload_complete && status.bytes_written == 1u);
    assert(config_import_service_abort() == CONFIG_IMPORT_RESULT_OK);
    assert(config_import_service_abort() == CONFIG_IMPORT_RESULT_NOT_ACTIVE);
}

static void test_validation_results_and_no_output_mutation(void)
{
    device_profile_config_t candidate = valid_candidate();
    config_validate_service_report_t report = {0};
    device_profile_config_t output = {0};

    config_import_service_init();
    assert(config_validate_service_validate_imported(&report, &output) ==
           CONFIG_VALIDATE_RESULT_NO_IMPORT_SESSION);

    assert(config_import_service_begin((uint32_t)sizeof(candidate), 0u,
                                       CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY) ==
           CONFIG_IMPORT_RESULT_OK);
    assert(config_validate_service_validate_imported(&report, &output) ==
           CONFIG_VALIDATE_RESULT_INCOMPLETE_PAYLOAD);
    assert(config_import_service_abort() == CONFIG_IMPORT_RESULT_OK);

    stage(&candidate, 0u);
    assert(config_validate_service_validate_imported(&report, &output) ==
           CONFIG_VALIDATE_RESULT_CRC_MISMATCH);
    assert(config_import_service_abort() == CONFIG_IMPORT_RESULT_OK);

    stage(&candidate, ota_crc32_compute(&candidate, sizeof(candidate)));
    assert(config_validate_service_validate_imported(&report, &output) == CONFIG_VALIDATE_RESULT_OK);
    assert(report.valid && memcmp(&output, &candidate, sizeof(output)) == 0);
    assert(config_import_service_abort() == CONFIG_IMPORT_RESULT_OK);

    candidate.opcua_port = 0u;
    expect_rejected(&candidate, CONFIG_VALIDATE_RESULT_INVALID_PORT);
    candidate = valid_candidate();
    candidate.timezone_offset_minutes = 841;
    expect_rejected(&candidate, CONFIG_VALIDATE_RESULT_INVALID_TIMEZONE);
    candidate = valid_candidate();
    candidate.watchdog_timeout_ms = 999u;
    expect_rejected(&candidate, CONFIG_VALIDATE_RESULT_INVALID_WATCHDOG);
    candidate = valid_candidate();
    candidate.network_mode = NETWORK_MODE_STATIC;
    expect_rejected(&candidate, CONFIG_VALIDATE_RESULT_INVALID_NETWORK);
    candidate = valid_candidate();
    candidate.channels[0].driver_id = 99u;
    expect_rejected(&candidate, CONFIG_VALIDATE_RESULT_INVALID_CHANNEL);
    candidate = valid_candidate();
    candidate.channels[0].source_index = 1u;
    expect_rejected(&candidate, CONFIG_VALIDATE_RESULT_INVALID_CHANNEL);
    candidate = valid_candidate();
    candidate.channels[1] = candidate.channels[0];
    expect_rejected(&candidate, CONFIG_VALIDATE_RESULT_DUPLICATE_CHANNEL_ID);
    candidate = valid_candidate();
    strcpy(candidate.hierarchical_location_path, "invalid/location/path");
    expect_rejected(&candidate, CONFIG_VALIDATE_RESULT_INVALID_LOCATION_PATH);
}

int main(void)
{
    test_import_bounds_and_staging();
    test_validation_results_and_no_output_mutation();
    return 0;
}
