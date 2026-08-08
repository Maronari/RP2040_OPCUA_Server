#include "services/config_import_service.h"
#include "services/config_validate_service.h"
#include "services/config_wire.h"
#include "services/driver_catalog.h"
#include "ota/ota_state.h"

#include <assert.h>
#include <stdio.h>
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

typedef struct
{
    const char *schema_version;
    const char *manufacture_date_utc;
    const char *calibration_date_utc;
    const char *mac;
    const char *static_ip;
    const char *static_netmask;
    const char *static_gateway;
    const char *static_dns;
    const char *timezone_offset_minutes;
    const char *watchdog_timeout_ms;
    const char *opcua_port;
    const char *minimum;
    const char *maximum;
    const char *scale;
    const char *offset;
    const char *warning_threshold;
    const char *critical_threshold;
} json_tokens_t;

static json_tokens_t valid_json_tokens(void)
{
    return (json_tokens_t){
        .schema_version = "1",
        .manufacture_date_utc = "-9223372036854775808",
        .calibration_date_utc = "9223372036854775807",
        .mac = "00:11:22:33:44:55",
        .static_ip = "0.0.0.0",
        .static_netmask = "255.255.255.255",
        .static_gateway = "1.2.3.4",
        .static_dns = "127.0.0.1",
        .timezone_offset_minutes = "-720",
        .watchdog_timeout_ms = "1000",
        .opcua_port = "65535",
        .minimum = "-1.7976931348623157e308",
        .maximum = "1.7976931348623157e308",
        .scale = "4.9406564584124654e-324",
        .offset = "-0",
        .warning_threshold = "1.25",
        .critical_threshold = "2.5",
    };
}

static bool write_json_channel(char *buffer,
                               size_t buffer_size,
                               size_t slot,
                               bool in_use,
                               const json_tokens_t *tokens)
{
    int written = snprintf(
        buffer,
        buffer_size,
        "{\"slot\":%lu,\"inUse\":%s,\"kind\":0,\"valueType\":0,\"driverId\":%u,"
        "\"sourceIndex\":0,\"sourceFlags\":0,\"id\":\"%s\",\"name\":\"host\","
        "\"description\":\"host parser coverage\",\"unit\":\"C\",\"flags\":4294967295,"
        "\"minimum\":%s,\"maximum\":%s,\"hasMinimum\":true,\"hasMaximum\":true,"
        "\"enabled\":true,\"samplePeriodMs\":4294967295,\"publishPeriodMs\":0,"
        "\"historyIntervalMs\":1,\"scale\":%s,\"offset\":%s,\"warningThreshold\":%s,"
        "\"criticalThreshold\":%s}",
        (unsigned long)slot,
        in_use ? "true" : "false",
        (unsigned)CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP,
        in_use ? "temperature" : "",
        tokens->minimum,
        tokens->maximum,
        tokens->scale,
        tokens->offset,
        tokens->warning_threshold,
        tokens->critical_threshold);

    return written > 0 && (size_t)written < buffer_size;
}

static bool build_json_payload(char *buffer, size_t buffer_size, const json_tokens_t *tokens)
{
    char channels[CONFIG_SERVICE_MAX_CHANNELS][768] = {{0}};
    int written;

    if (buffer == NULL || tokens == NULL)
    {
        return false;
    }
    for (size_t index = 0u; index < CONFIG_SERVICE_MAX_CHANNELS; ++index)
    {
        if (!write_json_channel(channels[index], sizeof(channels[index]), index, index == 0u, tokens))
        {
            return false;
        }
    }

    written = snprintf(
        buffer,
        buffer_size,
        "{\"schemaVersion\":%s,\"device\":{"
        "\"manufacturer\":\"acme\",\"model\":\"rp2350\",\"deviceName\":\"host-device\","
        "\"opcuaHostname\":\"host\",\"serialNumber\":\"serial\","
        "\"productInstanceUri\":\"urn:host\",\"assetId\":\"asset\",\"hwRevision\":\"hw\","
        "\"fwRevision\":\"fw\",\"buildTime\":\"build\",\"gitHash\":\"hash\","
        "\"location\":\"lab\",\"assetClass\":\"test\",\"manufactureDateUtc\":%s,"
        "\"calibrationDateUtc\":%s},\"network\":{\"mode\":\"dhcp\",\"mac\":\"%s\","
        "\"staticIp\":\"%s\",\"staticNetmask\":\"%s\",\"staticGateway\":\"%s\","
        "\"staticDns\":\"%s\"},\"time\":{\"ntpServer\":\"ntp\","
        "\"timezoneOffsetMinutes\":%s},\"mqtt\":{\"enabled\":false,\"useJson\":true,"
        "\"brokerUrl\":\"\",\"topicRoot\":\"\"},\"diagnostics\":{"
        "\"enableHeapDiagnostics\":true,\"enableTaskDiagnostics\":true,\"watchdogTimeoutMs\":%s},"
        "\"opcua\":{\"port\":%s,\"enableAlarmAckMethod\":true},\"channels\":[%s,%s,%s,%s]}",
        tokens->schema_version,
        tokens->manufacture_date_utc,
        tokens->calibration_date_utc,
        tokens->mac,
        tokens->static_ip,
        tokens->static_netmask,
        tokens->static_gateway,
        tokens->static_dns,
        tokens->timezone_offset_minutes,
        tokens->watchdog_timeout_ms,
        tokens->opcua_port,
        channels[0],
        channels[1],
        channels[2],
        channels[3]);

    return written > 0 && (size_t)written < buffer_size;
}

static void stage_json(const char *json)
{
    size_t length = strlen(json);
    size_t written = 0u;

    assert(length > 0u && length <= UINT32_MAX);
    assert(config_import_service_begin((uint32_t)length,
                                       ota_crc32_compute(json, length),
                                       CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY) == CONFIG_IMPORT_RESULT_OK);
    while (written < length)
    {
        size_t chunk_size = length - written;
        if (chunk_size > CONFIG_IMPORT_SERVICE_MAX_CHUNK_BYTES)
        {
            chunk_size = CONFIG_IMPORT_SERVICE_MAX_CHUNK_BYTES;
        }
        assert(config_import_service_write_chunk((const uint8_t *)&json[written], chunk_size) ==
               CONFIG_IMPORT_RESULT_OK);
        written += chunk_size;
    }
}

static void expect_json_result(const json_tokens_t *tokens,
                               config_validate_service_result_t expected,
                               device_profile_config_t *out_candidate)
{
    char json[4096] = {0};
    config_validate_service_report_t report = {0};
    device_profile_config_t local_candidate;
    device_profile_config_t before;
    device_profile_config_t *candidate = out_candidate != NULL ? out_candidate : &local_candidate;
    config_validate_service_result_t result;

    assert(build_json_payload(json, sizeof(json), tokens));
    memset(candidate, 0xa5, sizeof(*candidate));
    before = *candidate;
    stage_json(json);
    result = config_validate_service_validate_imported(&report, candidate);
    if (result != expected)
    {
        (void)fprintf(stderr, "json result %d expected %d: %s\n", (int)result, (int)expected, report.message);
    }
    assert(result == expected);
    assert(report.valid == (expected == CONFIG_VALIDATE_RESULT_OK));
    assert(report.result == expected);
    if (expected != CONFIG_VALIDATE_RESULT_OK)
    {
        assert(memcmp(candidate, &before, sizeof(*candidate)) == 0);
    }
    assert(config_import_service_abort() == CONFIG_IMPORT_RESULT_OK);
}

static void set_double_token(json_tokens_t *tokens, size_t field, const char *value)
{
    switch (field)
    {
    case 0u: tokens->minimum = value; break;
    case 1u: tokens->maximum = value; break;
    case 2u: tokens->scale = value; break;
    case 3u: tokens->offset = value; break;
    case 4u: tokens->warning_threshold = value; break;
    case 5u: tokens->critical_threshold = value; break;
    default: assert(false); break;
    }
}

static void test_json_numeric_and_network_parsing(void)
{
    static const char *const invalid_double_values[] = {
        "not-a-number", "nan", "inf", "-inf", "1e309", "1.0junk"
    };
    json_tokens_t tokens = valid_json_tokens();
    device_profile_config_t parsed = {0};

    /* This one document executes every strtoull/strtol/strtoll/strtod field. */
    expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_OK, &parsed);
    assert(parsed.opcua_port == UINT16_MAX);
    assert(parsed.timezone_offset_minutes == -720);
    assert(parsed.manufacture_date_utc == INT64_MIN);
    assert(parsed.calibration_date_utc == INT64_MAX);
    assert(parsed.channels[0].minimum < -1.0e307);
    assert(parsed.channels[0].maximum > 1.0e307);
    assert(parsed.channels[0].settings.scale > 0.0);
    assert(parsed.static_netmask[0] == UINT8_MAX && parsed.static_gateway[3] == 4u);

    /* Each strtod field accepts the finite upper boundary. */
    for (size_t field = 0u; field < 6u; ++field)
    {
        tokens = valid_json_tokens();
        set_double_token(&tokens, field, "1.7976931348623157e308");
        expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_OK, NULL);
        tokens = valid_json_tokens();
        set_double_token(&tokens, field, "-1.7976931348623157e308");
        expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_OK, NULL);
    }
    for (size_t field = 0u; field < 6u; ++field)
    {
        for (size_t value = 0u; value < sizeof(invalid_double_values) / sizeof(invalid_double_values[0]); ++value)
        {
            tokens = valid_json_tokens();
            set_double_token(&tokens, field, invalid_double_values[value]);
            expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_INVALID_JSON, NULL);
        }
    }

    tokens = valid_json_tokens();
    tokens.timezone_offset_minutes = "840";
    expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_OK, NULL);
    tokens = valid_json_tokens();
    tokens.schema_version = "4294967296";
    expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_INVALID_JSON, NULL);
    tokens = valid_json_tokens();
    tokens.timezone_offset_minutes = "32768";
    expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_INVALID_JSON, NULL);
    tokens = valid_json_tokens();
    tokens.watchdog_timeout_ms = "not-a-number";
    expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_INVALID_JSON, NULL);
    tokens = valid_json_tokens();
    tokens.opcua_port = "65535junk";
    expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_INVALID_JSON, NULL);
    tokens = valid_json_tokens();
    tokens.calibration_date_utc = "9223372036854775807junk";
    expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_INVALID_JSON, NULL);

    tokens = valid_json_tokens();
    tokens.static_ip = "1.2.3";
    expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_INVALID_JSON, NULL);
    tokens = valid_json_tokens();
    tokens.static_gateway = "1.2.3.256";
    expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_INVALID_JSON, NULL);
    tokens = valid_json_tokens();
    tokens.mac = "00-11-22-33-44-55";
    expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_INVALID_JSON, NULL);
}

static void test_json_i64_overflow_is_currently_saturated(void)
{
    json_tokens_t tokens = valid_json_tokens();
    device_profile_config_t parsed = {0};

    /* Characterization: config_json_reader_parse_i64 does not inspect ERANGE. */
    tokens.calibration_date_utc = "9223372036854775808";
    expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_OK, &parsed);
    assert(parsed.calibration_date_utc == INT64_MAX);
}

static void test_json_sscanf_trailing_junk_is_currently_accepted(void)
{
    json_tokens_t tokens = valid_json_tokens();
    device_profile_config_t parsed = {0};

    /* Characterization: the current sscanf formats do not consume string suffixes. */
    tokens.static_ip = "10.20.30.40extra";
    expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_OK, &parsed);
    assert(parsed.static_ip[0] == 10u && parsed.static_ip[3] == 40u);

    tokens = valid_json_tokens();
    tokens.mac = "01:02:03:04:05:06extra";
    expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_OK, &parsed);
    assert(parsed.mac[0] == 1u && parsed.mac[5] == 6u);

    /* %2x also turns an overlong final byte into accepted trailing input. */
    tokens = valid_json_tokens();
    tokens.mac = "01:02:03:04:05:1ff";
    expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_OK, &parsed);
    assert(parsed.mac[5] == 0x1fu);
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
    test_json_numeric_and_network_parsing();
    test_json_i64_overflow_is_currently_saturated();
    test_json_sscanf_trailing_junk_is_currently_accepted();
    return 0;
}
