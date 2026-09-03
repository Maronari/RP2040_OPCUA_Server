#include "services/config_import_service.h"
#include "services/config_compat.h"
#include "services/config_export_service.h"
#include "services/config_store_service.h"
#include "services/config_validate_service.h"
#include "services/config_wire.h"
#include "services/driver_catalog.h"
#include "ota/ota_state.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool g_fail_allocation;
static device_profile_config_t g_export_profile;

const device_profile_config_t *config_service_get(void)
{
    return &g_export_profile;
}

const device_profile_config_t *config_service_get_staged(void)
{
    return &g_export_profile;
}

uint32_t config_service_get_running_revision(void)
{
    return 1u;
}

uint32_t config_service_get_staged_revision(void)
{
    return 1u;
}

bool config_store_get_active_info(config_store_record_info_t *out_info)
{
    (void)out_info;
    return false;
}

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
    return source_index == 0u &&
           (source_flags == 0u || source_flags == CONFIG_CHANNEL_SOURCE_FLAG_INVERT);
}

static const driver_catalog_entry_t g_known_driver = {
    .driver_id = CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP,
    .name = "host-test",
    .validate_source = validate_source,
    .supported_value_types = TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_FLOAT) |
                             TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_DOUBLE),
};

bool driver_catalog_supports_value_type(const driver_catalog_entry_t *entry,
                                        telemetry_channel_value_type_t value_type)
{
    return entry != NULL &&
           (entry->supported_value_types & TELEMETRY_CHANNEL_VALUE_TYPE_BIT(value_type)) != 0u;
}

bool driver_catalog_get_resource_claims(uint8_t driver_id,
                                        uint8_t source_index,
                                        uint8_t source_flags,
                                        driver_catalog_resource_claim_t *out_claims,
                                        size_t claims_capacity,
                                        size_t *out_claim_count)
{
    if (driver_catalog_get_by_id(driver_id) == NULL ||
        !validate_source(source_index, source_flags) || out_claims == NULL ||
        out_claim_count == NULL || claims_capacity < 1u)
    {
        return false;
    }
    *out_claim_count = 1u;
    out_claims[0] = (driver_catalog_resource_claim_t){
        .type = DRIVER_CATALOG_RESOURCE_ADC_INPUT,
        .index = source_index,
        .shared = source_flags == CONFIG_CHANNEL_SOURCE_FLAG_INVERT,
    };
    return true;
}

bool driver_catalog_claims_are_compatible(const driver_catalog_resource_claim_t *claims,
                                          size_t claim_count)
{
    if (claims == NULL && claim_count != 0u)
    {
        return false;
    }
    for (size_t index = 0u; index < claim_count; ++index)
    {
        for (size_t other = index + 1u; other < claim_count; ++other)
        {
            if (claims[index].type == claims[other].type &&
                claims[index].index == claims[other].index &&
                (!claims[index].shared || !claims[other].shared))
            {
                return false;
            }
        }
    }
    return true;
}

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
    const uint8_t *payload = (const uint8_t *)candidate;
    size_t offset = 0u;

    assert(config_import_service_begin((uint32_t)sizeof(*candidate), crc,
                                       CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY) ==
           CONFIG_IMPORT_RESULT_OK);
    while (offset < sizeof(*candidate))
    {
        size_t chunk_size = sizeof(*candidate) - offset;
        if (chunk_size > CONFIG_IMPORT_SERVICE_MAX_CHUNK_BYTES)
        {
            chunk_size = CONFIG_IMPORT_SERVICE_MAX_CHUNK_BYTES;
        }
        assert(config_import_service_write_chunk(payload + offset, chunk_size) ==
               CONFIG_IMPORT_RESULT_OK);
        offset += chunk_size;
    }
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
    const char *low_threshold;
    const char *low_low_threshold;
    const char *alarm_hysteresis;
    const char *alarm_debounce_ms;
    bool include_alarm_tuning;
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
        .low_threshold = "-1.25",
        .low_low_threshold = "-2.5",
        .alarm_hysteresis = "0.5",
        .alarm_debounce_ms = "1234",
        .include_alarm_tuning = true,
    };
}

static bool write_json_channel(char *buffer,
                               size_t buffer_size,
                               size_t slot,
                               bool in_use,
                               const json_tokens_t *tokens)
{
    char alarm_tuning[256] = {0};
    int tuning_written;
    int written;

    if (tokens->include_alarm_tuning)
    {
        tuning_written = snprintf(alarm_tuning,
                                  sizeof(alarm_tuning),
                                  ",\"lowThreshold\":%s,\"lowLowThreshold\":%s,"
                                  "\"alarmHysteresis\":%s,\"alarmDebounceMs\":%s",
                                  tokens->low_threshold,
                                  tokens->low_low_threshold,
                                  tokens->alarm_hysteresis,
                                  tokens->alarm_debounce_ms);
        if (tuning_written <= 0 || (size_t)tuning_written >= sizeof(alarm_tuning))
        {
            return false;
        }
    }

    written = snprintf(
        buffer,
        buffer_size,
        "{\"slot\":%lu,\"inUse\":%s,\"kind\":0,\"valueType\":0,\"driverId\":%u,"
        "\"sourceIndex\":0,\"sourceFlags\":0,\"id\":\"%s\",\"name\":\"host\","
        "\"description\":\"host parser coverage\",\"unit\":\"C\",\"flags\":4294967295,"
        "\"minimum\":%s,\"maximum\":%s,\"hasMinimum\":true,\"hasMaximum\":true,"
        "\"enabled\":true,\"samplePeriodMs\":4294967295,\"publishPeriodMs\":0,"
        "\"historyIntervalMs\":1,\"scale\":%s,\"offset\":%s,\"warningThreshold\":%s,"
        "\"criticalThreshold\":%s%s}",
        (unsigned long)slot,
        in_use ? "true" : "false",
        (unsigned)CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP,
        in_use ? "temperature" : "",
        tokens->minimum,
        tokens->maximum,
        tokens->scale,
        tokens->offset,
        tokens->warning_threshold,
        tokens->critical_threshold,
        alarm_tuning);

    return written > 0 && (size_t)written < buffer_size;
}

static bool build_json_payload(char *buffer, size_t buffer_size, const json_tokens_t *tokens)
{
    char channels[PLATFORM_CHANNEL_LIMIT][768] = {{0}};
    int written;

    if (buffer == NULL || tokens == NULL)
    {
        return false;
    }
    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
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
        "\"opcua\":{\"port\":%s,\"enableAlarmAckMethod\":true},\"channels\":["
        "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s]}",
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
        channels[3],
        channels[4],
        channels[5],
        channels[6],
        channels[7],
        channels[8],
        channels[9],
        channels[10],
        channels[11],
        channels[12],
        channels[13],
        channels[14],
        channels[15]);

    return written > 0 && (size_t)written < buffer_size;
}

/*
 * v0.4.0 exported the same document schema with exactly four channel slots.
 * Freeze that external shape here by cutting the current exporter-shaped
 * fixture at the requested slot boundary; v0.4.1 continues to emit 16 slots.
 */
static bool build_json_payload_with_channel_count(char *buffer,
                                                  size_t buffer_size,
                                                  const json_tokens_t *tokens,
                                                  size_t channel_count)
{
    char marker[32] = {0};
    char *tail;

    if (!build_json_payload(buffer, buffer_size, tokens))
    {
        return false;
    }
    if (channel_count == PLATFORM_CHANNEL_LIMIT)
    {
        return true;
    }
    if (channel_count == 0u || channel_count >= PLATFORM_CHANNEL_LIMIT)
    {
        return false;
    }
    assert(snprintf(marker, sizeof(marker), ",{\"slot\":%lu,", (unsigned long)channel_count) > 0);
    tail = strstr(buffer, marker);
    if (tail == NULL || (size_t)(tail - buffer) + 3u > buffer_size)
    {
        return false;
    }
    tail[0] = ']';
    tail[1] = '}';
    tail[2] = '\0';
    return true;
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
    char json[16384] = {0};
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
    case 6u: tokens->low_threshold = value; break;
    case 7u: tokens->low_low_threshold = value; break;
    case 8u: tokens->alarm_hysteresis = value; break;
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
    for (size_t field = 0u; field < 9u; ++field)
    {
        tokens = valid_json_tokens();
        set_double_token(&tokens, field, "1.7976931348623157e308");
        expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_OK, NULL);
        tokens = valid_json_tokens();
        set_double_token(&tokens, field, "-1.7976931348623157e308");
        expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_OK, NULL);
    }
    for (size_t field = 0u; field < 9u; ++field)
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

static void test_v040_json_channel_array_migration(void)
{
    char json[16384] = {0};
    json_tokens_t tokens = valid_json_tokens();
    config_validate_service_report_t report = {0};
    device_profile_config_t parsed;

    /* Historical JSON export had neither the v0.4.3 tuning keys nor more
     * than four channels.  Omission must preserve high-only semantics. */
    tokens.include_alarm_tuning = false;
    assert(build_json_payload_with_channel_count(json, sizeof(json), &tokens,
                                                 CONFIG_COMPAT_V040_CHANNEL_LIMIT));
    memset(&parsed, 0xa5, sizeof(parsed));
    stage_json(json);
    assert(config_validate_service_validate_imported(&report, &parsed) == CONFIG_VALIDATE_RESULT_OK);
    assert(report.valid);
    assert(parsed.channels[0].in_use != 0u);
    assert(parsed.channels[0].settings.low_threshold == DBL_MAX);
    assert(parsed.channels[0].settings.low_low_threshold == DBL_MAX);
    for (size_t index = CONFIG_COMPAT_V040_CHANNEL_LIMIT; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        config_channel_definition_t empty = {0};
        assert(memcmp(&parsed.channels[index], &empty, sizeof(empty)) == 0);
    }
    assert(config_import_service_abort() == CONFIG_IMPORT_RESULT_OK);

    assert(build_json_payload_with_channel_count(json, sizeof(json), &tokens,
                                                 CONFIG_COMPAT_V040_CHANNEL_LIMIT + 1u));
    stage_json(json);
    assert(config_validate_service_validate_imported(&report, &parsed) == CONFIG_VALIDATE_RESULT_INVALID_JSON);
    assert(config_import_service_abort() == CONFIG_IMPORT_RESULT_OK);
}

static void test_json_alarm_tuning_round_trip(void)
{
    char json[16384] = {0};
    config_validate_service_report_t report = {0};
    device_profile_config_t parsed = {0};

    g_export_profile = valid_candidate();
    g_export_profile.channels[0].settings.enabled = true;
    g_export_profile.channels[0].settings.sample_period_ms = 1000u;
    g_export_profile.channels[0].settings.publish_period_ms = 1000u;
    g_export_profile.channels[0].settings.history_interval_ms = 1000u;
    g_export_profile.channels[0].settings.scale = 1.0;
    g_export_profile.channels[0].settings.warning_threshold = 60.0;
    g_export_profile.channels[0].settings.critical_threshold = 75.0;

    /* Migrated high-only compatibility sentinel must survive a finite JSON
     * export/import round trip rather than becoming an active zero low side. */
    g_export_profile.channels[0].settings.low_threshold = DBL_MAX;
    g_export_profile.channels[0].settings.low_low_threshold = DBL_MAX;
    g_export_profile.channels[0].settings.alarm_hysteresis = 0.0;
    g_export_profile.channels[0].settings.alarm_debounce_ms = 0u;
    assert(config_export_service_write_running_json(json, sizeof(json)));
    assert(strstr(json, "\"lowThreshold\":") != NULL);
    assert(strstr(json, "\"lowLowThreshold\":") != NULL);
    assert(strstr(json, "\"alarmHysteresis\":") != NULL);
    assert(strstr(json, "\"alarmDebounceMs\":0") != NULL);
    stage_json(json);
    assert(config_validate_service_validate_imported(&report, &parsed) == CONFIG_VALIDATE_RESULT_OK);
    assert(parsed.channels[0].settings.low_threshold == DBL_MAX);
    assert(parsed.channels[0].settings.low_low_threshold == DBL_MAX);
    assert(parsed.channels[0].settings.alarm_hysteresis == 0.0);
    assert(parsed.channels[0].settings.alarm_debounce_ms == 0u);
    assert(config_import_service_abort() == CONFIG_IMPORT_RESULT_OK);

    /* A native explicit zero lower threshold is distinct from the migration
     * sentinel and must retain its low-low, hysteresis, and debounce tuning. */
    g_export_profile.channels[0].settings.low_threshold = 0.0;
    g_export_profile.channels[0].settings.low_low_threshold = -2.5;
    g_export_profile.channels[0].settings.alarm_hysteresis = 0.75;
    g_export_profile.channels[0].settings.alarm_debounce_ms = 1234u;
    assert(config_export_service_write_running_json(json, sizeof(json)));
    stage_json(json);
    assert(config_validate_service_validate_imported(&report, &parsed) == CONFIG_VALIDATE_RESULT_OK);
    assert(parsed.channels[0].settings.low_threshold == 0.0);
    assert(parsed.channels[0].settings.low_low_threshold == -2.5);
    assert(parsed.channels[0].settings.alarm_hysteresis == 0.75);
    assert(parsed.channels[0].settings.alarm_debounce_ms == 1234u);
    assert(config_import_service_abort() == CONFIG_IMPORT_RESULT_OK);
}

static void test_worst_case_json_export_is_bounded(void)
{
    enum
    {
        export_buffer_size = 24576u,
        compact_export_limit = 12288u
    };
    char json[export_buffer_size] = {0};
    size_t required_size;
    clock_t started_at;
    clock_t elapsed;

    g_export_profile = valid_candidate();
    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        config_channel_definition_t *channel = &g_export_profile.channels[index];

        channel->in_use = 1u;
        channel->driver_id = CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP;
        channel->kind = TELEMETRY_CHANNEL_KIND_ANALOG;
        channel->value_type = TELEMETRY_CHANNEL_VALUE_FLOAT;
        (void)snprintf(channel->id, sizeof(channel->id), "worst.%u", (unsigned)index);
        memset(channel->name, 'n', sizeof(channel->name) - 1u);
        memset(channel->description, 'd', sizeof(channel->description) - 1u);
        memset(channel->unit, 'u', sizeof(channel->unit) - 1u);
        channel->minimum = -DBL_MAX;
        channel->maximum = DBL_MAX;
        channel->settings.scale = 1.0;
        channel->settings.offset = 0.0;
        channel->settings.warning_threshold = -40.0;
        channel->settings.critical_threshold = 125.0;
        channel->settings.low_threshold = DBL_MAX;
        channel->settings.low_low_threshold = DBL_MAX;
        channel->settings.alarm_hysteresis = 1.0;
    }

    started_at = clock();
    assert(config_export_service_write_running_json(json, sizeof(json)));
    elapsed = clock() - started_at;
    required_size = strlen(json) + 1u;

    /* The compact representation prevents a DBL_MAX fixed-point expansion
     * from consuming the model's entire export scratch. This bound also
     * exercises all 16 slots and their longest bounded text fields. */
    assert(required_size < compact_export_limit);
    assert(elapsed < CLOCKS_PER_SEC);
    assert(strstr(json, "\"minimum\":-1.7976931348623157e308") != NULL);
    assert(strstr(json, "\"lowThreshold\":1.7976931348623157e308") != NULL);
    assert(strstr(json, "\"warningThreshold\":-40,\"criticalThreshold\":125") != NULL);

    /* Exact one-byte shortage must fail rather than return partial JSON as
     * success; the caller can map false to its deterministic OPC UA error. */
    assert(required_size > 1u);
    assert(config_export_service_write_running_json(json, required_size));
    assert(!config_export_service_write_running_json(json, required_size - 1u));
    assert(!config_export_service_write_running_json(NULL, required_size));

    /* A persisted fixed-width field without a terminator must fail in a
     * bounded scan, never walk into adjacent profile/PSRAM memory. */
    g_export_profile = valid_candidate();
    memset(g_export_profile.asset_id, 'x', sizeof(g_export_profile.asset_id));
    assert(!config_export_service_write_running_json(json, sizeof(json)));

    g_export_profile = valid_candidate();
    g_export_profile.channels[0].settings.scale = NAN;
    assert(!config_export_service_write_running_json(json, sizeof(json)));
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

/* An unrecognized JSON schemaVersion must be rejected with a diagnostic
 * (CONFIG_VALIDATE_RESULT_UNSUPPORTED_SCHEMA) rather than parsed as if it
 * were the current schema -- config_validate_service_parse_json_payload()
 * now sources this check from config_compat_schema_version_is_supported(),
 * the same migration-table source of truth the Flash-binary path uses. */
static void test_json_rejects_unsupported_schema_version(void)
{
    json_tokens_t tokens = valid_json_tokens();

    tokens.schema_version = "999";
    expect_json_result(&tokens, CONFIG_VALIDATE_RESULT_UNSUPPORTED_SCHEMA, NULL);
}

/* Mirrors the JSON case above for the binary-import path
 * (config_validate_service_validate_binary_payload()): a staged binary
 * payload whose session schema_version has no migration-table entry must be
 * refused before config_compat_decode_versioned() even looks at the bytes,
 * and must not disturb the caller's prior output. */
static void test_binary_import_rejects_unsupported_schema_version(void)
{
    device_profile_config_t candidate = valid_candidate();
    config_validate_service_report_t report = {0};
    device_profile_config_t output;
    device_profile_config_t before;
    const uint8_t *payload = (const uint8_t *)&candidate;
    size_t offset = 0u;

    memset(&output, 0xa5, sizeof(output));
    before = output;
    assert(config_import_service_begin((uint32_t)sizeof(candidate),
                                       ota_crc32_compute(&candidate, sizeof(candidate)),
                                       CONFIG_STORE_SCHEMA_VERSION_CURRENT + 1u) ==
           CONFIG_IMPORT_RESULT_OK);
    while (offset < sizeof(candidate))
    {
        size_t chunk_size = sizeof(candidate) - offset;
        if (chunk_size > CONFIG_IMPORT_SERVICE_MAX_CHUNK_BYTES)
        {
            chunk_size = CONFIG_IMPORT_SERVICE_MAX_CHUNK_BYTES;
        }
        assert(config_import_service_write_chunk(payload + offset, chunk_size) == CONFIG_IMPORT_RESULT_OK);
        offset += chunk_size;
    }
    assert(config_validate_service_validate_imported(&report, &output) ==
           CONFIG_VALIDATE_RESULT_UNSUPPORTED_SCHEMA);
    assert(!report.valid && report.result == CONFIG_VALIDATE_RESULT_UNSUPPORTED_SCHEMA);
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
    expect_rejected(&candidate, CONFIG_VALIDATE_RESULT_UNKNOWN_DRIVER);
    candidate = valid_candidate();
    candidate.channels[0].source_index = 1u;
    expect_rejected(&candidate, CONFIG_VALIDATE_RESULT_INVALID_CHANNEL);
    candidate = valid_candidate();
    candidate.channels[1] = candidate.channels[0];
    expect_rejected(&candidate, CONFIG_VALIDATE_RESULT_DUPLICATE_CHANNEL_ID);
    candidate = valid_candidate();
    candidate.channels[0].value_type = TELEMETRY_CHANNEL_VALUE_UINT64;
    expect_rejected(&candidate, CONFIG_VALIDATE_RESULT_UNSUPPORTED_VALUE_TYPE);
    candidate = valid_candidate();
    candidate.channels[0].flags |= TELEMETRY_CHANNEL_FLAG_HISTORIZED;
    candidate.channels[0].value_type = TELEMETRY_CHANNEL_VALUE_DOUBLE;
    assert(config_validate_service_validate_candidate(&candidate, NULL, 0u) ==
           CONFIG_VALIDATE_RESULT_OK);
    candidate = valid_candidate();
    candidate.channels[0].flags |= TELEMETRY_CHANNEL_FLAG_HISTORIZED;
    candidate.channels[0].value_type = TELEMETRY_CHANNEL_VALUE_FLOAT;
    assert(config_validate_service_validate_candidate(&candidate, NULL, 0u) ==
           CONFIG_VALIDATE_RESULT_OK);
    candidate = valid_candidate();
    candidate.channels[1] = candidate.channels[0];
    strcpy(candidate.channels[1].id, "temperature.2");
    expect_rejected(&candidate, CONFIG_VALIDATE_RESULT_RESOURCE_CONFLICT);
    candidate = valid_candidate();
    candidate.channels[0].source_flags = CONFIG_CHANNEL_SOURCE_FLAG_INVERT;
    candidate.channels[1] = candidate.channels[0];
    strcpy(candidate.channels[1].id, "temperature.2");
    stage(&candidate, ota_crc32_compute(&candidate, sizeof(candidate)));
    assert(config_validate_service_validate_imported(&report, &output) == CONFIG_VALIDATE_RESULT_OK);
    assert(config_import_service_abort() == CONFIG_IMPORT_RESULT_OK);
    candidate = valid_candidate();
    strcpy(candidate.hierarchical_location_path, "invalid/location/path");
    expect_rejected(&candidate, CONFIG_VALIDATE_RESULT_INVALID_LOCATION_PATH);
}

static void test_collect_missing_drivers(void)
{
    device_profile_config_t candidate;
    config_validate_service_missing_driver_report_t report;
    config_validate_service_result_t result;
    char message[CONFIG_VALIDATE_SERVICE_MESSAGE_MAX_LEN];

    /* A single missing driver_id gets the new dedicated result code, not
     * the generic CONFIG_VALIDATE_RESULT_INVALID_CHANNEL it used to share
     * with source/kind/value_type failures. */
    candidate = valid_candidate();
    candidate.channels[0].driver_id = 99u;
    memset(&report, 0xa5, sizeof(report));
    result = config_validate_service_validate_candidate_collect_missing_drivers(
        &candidate, message, sizeof(message), &report);
    assert(result == CONFIG_VALIDATE_RESULT_UNKNOWN_DRIVER);
    assert(report.count == 1u);
    assert(report.entries[0].channel_index == 0u);
    assert(report.entries[0].driver_id == 99u);
    assert(!report.resource_conflict);
    assert(strstr(message, "channel 0 driver_id 99") != NULL);

    /* message/out_missing_drivers stay optional. */
    assert(config_validate_service_validate_candidate_collect_missing_drivers(
               &candidate, NULL, 0u, NULL) == CONFIG_VALIDATE_RESULT_UNKNOWN_DRIVER);

    /* Adding the collecting sibling must not change
     * config_validate_service_validate_candidate()'s own signature/behavior:
     * it still reports only the first failure, now with the new code. */
    assert(config_validate_service_validate_candidate(&candidate, NULL, 0u) ==
           CONFIG_VALIDATE_RESULT_UNKNOWN_DRIVER);

    /* Multiple missing driver_ids across the whole candidate are ALL
     * reported in one call, in ascending channel-slot order -- this is
     * exactly what config_validate_service_validate_candidate() cannot do,
     * since it still stops at the first failure of any kind. */
    memset(&candidate, 0, sizeof(candidate));
    candidate.opcua_port = 4840u;
    candidate.network_mode = NETWORK_MODE_DHCP;
    candidate.channels[0].in_use = 1u;
    candidate.channels[0].driver_id = 50u;
    candidate.channels[0].kind = TELEMETRY_CHANNEL_KIND_ANALOG;
    candidate.channels[0].value_type = TELEMETRY_CHANNEL_VALUE_FLOAT;
    strcpy(candidate.channels[0].id, "missing.0");
    candidate.channels[1].in_use = 1u;
    candidate.channels[1].driver_id = 51u;
    candidate.channels[1].kind = TELEMETRY_CHANNEL_KIND_ANALOG;
    candidate.channels[1].value_type = TELEMETRY_CHANNEL_VALUE_FLOAT;
    strcpy(candidate.channels[1].id, "missing.1");
    candidate.channels[2].in_use = 1u;
    candidate.channels[2].driver_id = 52u;
    candidate.channels[2].kind = TELEMETRY_CHANNEL_KIND_ANALOG;
    candidate.channels[2].value_type = TELEMETRY_CHANNEL_VALUE_FLOAT;
    strcpy(candidate.channels[2].id, "missing.2");
    memset(&report, 0xa5, sizeof(report));
    result = config_validate_service_validate_candidate_collect_missing_drivers(
        &candidate, message, sizeof(message), &report);
    assert(result == CONFIG_VALIDATE_RESULT_UNKNOWN_DRIVER);
    assert(report.count == 3u);
    assert(report.entries[0].channel_index == 0u && report.entries[0].driver_id == 50u);
    assert(report.entries[1].channel_index == 1u && report.entries[1].driver_id == 51u);
    assert(report.entries[2].channel_index == 2u && report.entries[2].driver_id == 52u);
    assert(!report.resource_conflict);
    assert(strstr(message, "channel 0 driver_id 50") != NULL);
    assert(strstr(message, "channel 1 driver_id 51") != NULL);
    assert(strstr(message, "channel 2 driver_id 52") != NULL);

    /* A candidate with BOTH a missing driver AND a resource conflict must
     * produce both diagnostics deterministically in one call: the missing
     * driver via the result code/report, the conflict via
     * report.resource_conflict (computed from the channels whose driver DID
     * resolve). */
    memset(&candidate, 0, sizeof(candidate));
    candidate.opcua_port = 4840u;
    candidate.network_mode = NETWORK_MODE_DHCP;
    candidate.channels[0].in_use = 1u;
    candidate.channels[0].driver_id = CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP;
    candidate.channels[0].kind = TELEMETRY_CHANNEL_KIND_ANALOG;
    candidate.channels[0].value_type = TELEMETRY_CHANNEL_VALUE_FLOAT;
    strcpy(candidate.channels[0].id, "a");
    candidate.channels[1] = candidate.channels[0];
    strcpy(candidate.channels[1].id, "b");
    candidate.channels[2].in_use = 1u;
    candidate.channels[2].driver_id = 77u;
    candidate.channels[2].kind = TELEMETRY_CHANNEL_KIND_ANALOG;
    candidate.channels[2].value_type = TELEMETRY_CHANNEL_VALUE_FLOAT;
    strcpy(candidate.channels[2].id, "c");
    memset(&report, 0xa5, sizeof(report));
    result = config_validate_service_validate_candidate_collect_missing_drivers(
        &candidate, message, sizeof(message), &report);
    assert(result == CONFIG_VALIDATE_RESULT_UNKNOWN_DRIVER);
    assert(report.count == 1u);
    assert(report.entries[0].channel_index == 2u && report.entries[0].driver_id == 77u);
    assert(report.resource_conflict);
    assert(strstr(message, "channel 2 driver_id 77") != NULL);
    assert(strstr(message, "resource claims also conflict") != NULL);

    /* Confirm the same two channels alone (no missing driver) still hit the
     * pre-existing, untouched resource-conflict path/code. */
    candidate.channels[2].in_use = 0u;
    assert(config_validate_service_validate_candidate(&candidate, NULL, 0u) ==
           CONFIG_VALIDATE_RESULT_RESOURCE_CONFLICT);
}

int main(void)
{
    test_import_bounds_and_staging();
    test_validation_results_and_no_output_mutation();
    test_collect_missing_drivers();
    test_json_numeric_and_network_parsing();
    test_v040_json_channel_array_migration();
    test_json_alarm_tuning_round_trip();
    test_worst_case_json_export_is_bounded();
    test_json_i64_overflow_is_currently_saturated();
    test_json_sscanf_trailing_junk_is_currently_accepted();
    test_json_rejects_unsupported_schema_version();
    test_binary_import_rejects_unsupported_schema_version();
    return 0;
}
