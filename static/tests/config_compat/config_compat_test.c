#include "services/config_compat.h"

#include <assert.h>
#include <float.h>
#include <stdio.h>
#include <string.h>

#define V040_CHANNELS 4u

/* Frozen pre-v0.4.3 telemetry_channel_settings_t/config_channel_definition_t
 * shapes, kept local to this test and deliberately decoupled from the live
 * types in services/include -- exactly mirroring the frozen types
 * services/src/config_compat.c itself uses to decode old on-flash records.
 * This must never be changed to track the live struct: the whole point is
 * to prove the decoder still accepts the exact bytes an old firmware wrote,
 * regardless of how many new fields the live struct has gained since. */
typedef struct
{
    bool enabled;
    uint32_t sample_period_ms;
    uint32_t publish_period_ms;
    uint32_t history_interval_ms;
    double scale;
    double offset;
    double warning_threshold;
    double critical_threshold;
} frozen_channel_settings_t;

typedef struct
{
    uint8_t in_use;
    uint8_t kind;
    uint8_t value_type;
    uint8_t driver_id;
    uint8_t source_index;
    uint8_t source_flags;
    uint8_t has_minimum;
    uint8_t has_maximum;
    char id[CONFIG_SERVICE_CHANNEL_ID_MAX_LEN];
    char name[CONFIG_SERVICE_CHANNEL_NAME_MAX_LEN];
    char description[CONFIG_SERVICE_CHANNEL_DESCRIPTION_MAX_LEN];
    char unit[CONFIG_SERVICE_CHANNEL_UNIT_MAX_LEN];
    uint32_t flags;
    double minimum;
    double maximum;
    frozen_channel_settings_t settings;
} frozen_channel_definition_t;

typedef struct
{
    char manufacturer[CONFIG_SERVICE_TEXT_MAX_LEN];
    char model[CONFIG_SERVICE_TEXT_MAX_LEN];
    char serial_number[CONFIG_SERVICE_TEXT_MAX_LEN];
    char hw_revision[CONFIG_SERVICE_TEXT_MAX_LEN];
    char fw_revision[CONFIG_SERVICE_TEXT_MAX_LEN];
    char build_time[CONFIG_SERVICE_TEXT_MAX_LEN];
    char git_hash[CONFIG_SERVICE_TEXT_MAX_LEN];
    char device_name[CONFIG_SERVICE_TEXT_MAX_LEN];
    char opcua_hostname[CONFIG_SERVICE_TEXT_MAX_LEN];
    char ntp_server[CONFIG_SERVICE_TEXT_MAX_LEN];
    char mqtt_broker_url[CONFIG_SERVICE_TEXT_MAX_LEN];
    char mqtt_topic_root[CONFIG_SERVICE_TEXT_MAX_LEN];
    bool enable_heap_diagnostics;
    bool enable_task_diagnostics;
    bool enable_alarm_ack_method;
    bool enable_mqtt_pubsub;
    bool mqtt_pubsub_use_json;
    uint16_t opcua_port;
    int16_t timezone_offset_minutes;
    uint32_t watchdog_timeout_ms;
    uint8_t mac[6];
    network_mode_t network_mode;
    uint8_t static_ip[4];
    uint8_t static_netmask[4];
    uint8_t static_gateway[4];
    uint8_t static_dns[4];
    frozen_channel_definition_t channels[V040_CHANNELS];
    char product_instance_uri[CONFIG_SERVICE_URI_MAX_LEN];
    char asset_id[CONFIG_SERVICE_ASSET_ID_MAX_LEN];
    int64_t manufacture_date_utc;
    int64_t calibration_date_utc;
    char location[CONFIG_SERVICE_LOCATION_MAX_LEN];
    char asset_class[CONFIG_SERVICE_ASSET_CLASS_MAX_LEN];
    char hierarchical_location_path[CONFIG_SERVICE_LOCATION_PATH_MAX_LEN];
    char operational_location_path[CONFIG_SERVICE_LOCATION_PATH_MAX_LEN];
} v040_profile_t;

/* Frozen v0.4.1/v0.4.2 shape: same as the live device_profile_config_t
 * except telemetry_channel_settings_t has not yet grown the v0.4.3
 * alarm-tuning fields (low/low-low thresholds, hysteresis, debounce). */
typedef struct
{
    char manufacturer[CONFIG_SERVICE_TEXT_MAX_LEN];
    char model[CONFIG_SERVICE_TEXT_MAX_LEN];
    char serial_number[CONFIG_SERVICE_TEXT_MAX_LEN];
    char hw_revision[CONFIG_SERVICE_TEXT_MAX_LEN];
    char fw_revision[CONFIG_SERVICE_TEXT_MAX_LEN];
    char build_time[CONFIG_SERVICE_TEXT_MAX_LEN];
    char git_hash[CONFIG_SERVICE_TEXT_MAX_LEN];
    char device_name[CONFIG_SERVICE_TEXT_MAX_LEN];
    char opcua_hostname[CONFIG_SERVICE_TEXT_MAX_LEN];
    char ntp_server[CONFIG_SERVICE_TEXT_MAX_LEN];
    char mqtt_broker_url[CONFIG_SERVICE_TEXT_MAX_LEN];
    char mqtt_topic_root[CONFIG_SERVICE_TEXT_MAX_LEN];
    bool enable_heap_diagnostics;
    bool enable_task_diagnostics;
    bool enable_alarm_ack_method;
    bool enable_mqtt_pubsub;
    bool mqtt_pubsub_use_json;
    uint16_t opcua_port;
    int16_t timezone_offset_minutes;
    uint32_t watchdog_timeout_ms;
    uint8_t mac[6];
    network_mode_t network_mode;
    uint8_t static_ip[4];
    uint8_t static_netmask[4];
    uint8_t static_gateway[4];
    uint8_t static_dns[4];
    frozen_channel_definition_t channels[PLATFORM_CHANNEL_LIMIT];
    char product_instance_uri[CONFIG_SERVICE_URI_MAX_LEN];
    char asset_id[CONFIG_SERVICE_ASSET_ID_MAX_LEN];
    int64_t manufacture_date_utc;
    int64_t calibration_date_utc;
    char location[CONFIG_SERVICE_LOCATION_MAX_LEN];
    char asset_class[CONFIG_SERVICE_ASSET_CLASS_MAX_LEN];
    char hierarchical_location_path[CONFIG_SERVICE_LOCATION_PATH_MAX_LEN];
    char operational_location_path[CONFIG_SERVICE_LOCATION_PATH_MAX_LEN];
} v042_profile_t;

static void test_v040(void)
{
    v040_profile_t old_config = {0};
    device_profile_config_t decoded;
    bool migrated = false;

    assert(sizeof(old_config) == config_compat_v040_payload_size());
    strcpy(old_config.device_name, "v040-device");
    strcpy(old_config.asset_id, "asset-v040");
    strcpy(old_config.hierarchical_location_path, "Plant/Line1");
    old_config.opcua_port = 4840u;
    old_config.channels[0].in_use = 1u;
    old_config.channels[0].driver_id = CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP;
    strcpy(old_config.channels[0].id, "channel.temperature.1");
    strcpy(old_config.channels[0].name, "Temperature1");
    old_config.channels[0].settings.warning_threshold = 60.0;
    old_config.channels[0].settings.critical_threshold = 75.0;
    old_config.channels[3].in_use = 1u;
    strcpy(old_config.channels[3].id, "channel.four");

#if PLATFORM_CHANNEL_LIMIT < V040_CHANNELS
    memset(&decoded, 0, sizeof(decoded));
    strcpy(decoded.device_name, "known-good");
    assert(!config_compat_decode_binary((const uint8_t *)&old_config,
                                        sizeof(old_config),
                                        &decoded,
                                        &migrated));
    assert(strcmp(decoded.device_name, "known-good") == 0);
    old_config.channels[3].in_use = 0u;
#endif
    assert(config_compat_decode_binary((const uint8_t *)&old_config,
                                       sizeof(old_config),
                                       &decoded,
                                       &migrated));
    /* A v0.4.0 payload always differs in size from the live struct now,
     * regardless of PLATFORM_CHANNEL_LIMIT: v0.4.0 always used exactly
     * V040_CHANNELS channels with the pre-v0.4.3 (smaller) settings shape,
     * while the live struct's channel settings grew in v0.4.3. */
    assert(migrated);
    assert(strcmp(decoded.device_name, old_config.device_name) == 0);
    assert(strcmp(decoded.asset_id, old_config.asset_id) == 0);
    assert(strcmp(decoded.hierarchical_location_path,
                  old_config.hierarchical_location_path) == 0);
    assert(decoded.opcua_port == old_config.opcua_port);
    assert(strcmp(decoded.channels[0].id, old_config.channels[0].id) == 0);
    /* The absent v0.4.0 lower side is disabled with a finite sentinel, so
     * only the original high-side thresholds are active after migration. */
    assert(decoded.channels[0].settings.warning_threshold == 60.0);
    assert(decoded.channels[0].settings.critical_threshold == 75.0);
    assert(decoded.channels[0].settings.low_threshold == DBL_MAX);
    assert(decoded.channels[0].settings.low_low_threshold == DBL_MAX);
    assert(decoded.channels[0].settings.alarm_hysteresis == 0.0);
    assert(decoded.channels[0].settings.alarm_debounce_ms == 0u);
#if PLATFORM_CHANNEL_LIMIT > 3u
    assert(strcmp(decoded.channels[3].id, old_config.channels[3].id) == 0);
#endif
    for (size_t index = V040_CHANNELS; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        assert(decoded.channels[index].in_use == 0u);
    }

    migrated = true;
    assert(config_compat_decode_binary((const uint8_t *)&decoded,
                                       sizeof(decoded),
                                       &decoded,
                                       &migrated));
    assert(!migrated);
    assert(strcmp(decoded.device_name, old_config.device_name) == 0);
    assert(!config_compat_decode_binary((const uint8_t *)&old_config,
                                        sizeof(old_config) - 1u,
                                        &decoded,
                                        NULL));
    assert(!config_compat_decode_binary((const uint8_t *)&old_config,
                                        sizeof(old_config) + 1u,
                                        &decoded,
                                        NULL));
    printf("platform_limit=%u current_profile=%u v040_profile=%u channel_definition=%u\n",
           (unsigned)PLATFORM_CHANNEL_LIMIT,
           (unsigned)sizeof(device_profile_config_t),
           (unsigned)sizeof(v040_profile_t),
           (unsigned)sizeof(config_channel_definition_t));
}

static void assert_v040_partial_tail(const device_profile_config_t *decoded,
                                     const v040_profile_t *old_config)
{
    assert(strcmp(decoded->product_instance_uri, old_config->product_instance_uri) == 0);
    assert(strcmp(decoded->asset_id, old_config->asset_id) == 0);
    assert(decoded->manufacture_date_utc == old_config->manufacture_date_utc);
    assert(decoded->calibration_date_utc == old_config->calibration_date_utc);
    assert(strcmp(decoded->location, old_config->location) == 0);
    assert(strcmp(decoded->asset_class, old_config->asset_class) == 0);
    assert(decoded->hierarchical_location_path[0] == '\0');
    assert(decoded->operational_location_path[0] == '\0');
    assert(decoded->channels[0].settings.low_threshold == DBL_MAX);
    assert(decoded->channels[0].settings.low_low_threshold == DBL_MAX);
}

static void test_v040_partial_payloads(void)
{
    v040_profile_t old_config = {0};
    const size_t asset_offset = offsetof(v040_profile_t, asset_id);
    const size_t tail_offset = offsetof(v040_profile_t, manufacture_date_utc);
    const size_t previous_size = offsetof(v040_profile_t, hierarchical_location_path);
    const size_t legacy_tail_offset =
        ((asset_offset + 32u + _Alignof(int64_t) - 1u) / _Alignof(int64_t)) * _Alignof(int64_t);
    const size_t legacy_size = legacy_tail_offset + previous_size - tail_offset;
    uint8_t previous_payload[previous_size];
    uint8_t previous_unaligned[previous_size + 1u];
    uint8_t legacy_payload[legacy_size];
    uint8_t legacy_unaligned[legacy_size + 1u];
    device_profile_config_t decoded;
    bool migrated = false;

    old_config.channels[0].in_use = 1u;
    old_config.channels[0].settings.warning_threshold = 60.0;
    old_config.channels[0].settings.critical_threshold = 75.0;
    strcpy(old_config.channels[0].id, "channel.temperature.1");
    strcpy(old_config.product_instance_uri, "urn:partial:v040");
    strcpy(old_config.asset_id, "legacy-asset-1234567890");
    old_config.manufacture_date_utc = INT64_C(1700000000000000);
    old_config.calibration_date_utc = INT64_C(1701000000000000);
    strcpy(old_config.location, "PlantA-Rack7");
    strcpy(old_config.asset_class, "Pump");
    strcpy(old_config.hierarchical_location_path, "must-not-be-read");
    strcpy(old_config.operational_location_path, "must-not-be-read");

    /* Original v0.4.0 partial payload: all bytes through asset_class, then
     * no hierarchical or operational location fields. */
    memcpy(previous_payload, &old_config, previous_size);
    memcpy(previous_unaligned + 1u, previous_payload, previous_size);
    assert(config_compat_decode_binary(previous_unaligned + 1u,
                                       previous_size,
                                       &decoded,
                                       &migrated));
    assert(migrated);
    assert_v040_partial_tail(&decoded, &old_config);

    /* Earlier v0.4.0 payload: asset_id was exactly 32 bytes and the tail was
     * shifted to the next int64_t-aligned offset.  Build its byte layout
     * explicitly rather than relying on a live C struct. */
    memset(legacy_payload, 0, sizeof(legacy_payload));
    memcpy(legacy_payload, &old_config, asset_offset);
    memcpy(legacy_payload + asset_offset, old_config.asset_id, 32u);
    memcpy(legacy_payload + legacy_tail_offset,
           (const uint8_t *)&old_config + tail_offset,
           previous_size - tail_offset);
    memcpy(legacy_unaligned + 1u, legacy_payload, legacy_size);
    assert(config_compat_decode_binary(legacy_unaligned + 1u,
                                       legacy_size,
                                       &decoded,
                                       &migrated));
    assert(migrated);
    assert_v040_partial_tail(&decoded, &old_config);

    /* Invalid/truncated legacy records must not replace the caller's last
     * known-good configuration. */
    memset(&decoded, 0, sizeof(decoded));
    strcpy(decoded.device_name, "known-good-previous");
    assert(!config_compat_decode_binary(previous_unaligned + 1u,
                                        previous_size - 1u,
                                        &decoded,
                                        &migrated));
    assert(strcmp(decoded.device_name, "known-good-previous") == 0);
    strcpy(decoded.device_name, "known-good-shifted");
    assert(!config_compat_decode_binary(legacy_unaligned + 1u,
                                        legacy_size - 1u,
                                        &decoded,
                                        &migrated));
    assert(strcmp(decoded.device_name, "known-good-shifted") == 0);

#if PLATFORM_CHANNEL_LIMIT < V040_CHANNELS
    /* Capacity rejection applies to both partial v0.4.0 byte layouts before
     * mutating the caller's known-good output. */
    {
        const size_t excess_in_use_offset = offsetof(v040_profile_t, channels) +
                                           (V040_CHANNELS - 1u) * sizeof(frozen_channel_definition_t) +
                                           offsetof(frozen_channel_definition_t, in_use);

        previous_payload[excess_in_use_offset] = 1u;
        memcpy(previous_unaligned + 1u, previous_payload, previous_size);
        strcpy(decoded.device_name, "known-good-previous-capacity");
        assert(!config_compat_decode_binary(previous_unaligned + 1u,
                                            previous_size,
                                            &decoded,
                                            &migrated));
        assert(strcmp(decoded.device_name, "known-good-previous-capacity") == 0);

        legacy_payload[excess_in_use_offset] = 1u;
        memcpy(legacy_unaligned + 1u, legacy_payload, legacy_size);
        strcpy(decoded.device_name, "known-good-shifted-capacity");
        assert(!config_compat_decode_binary(legacy_unaligned + 1u,
                                            legacy_size,
                                            &decoded,
                                            &migrated));
        assert(strcmp(decoded.device_name, "known-good-shifted-capacity") == 0);
    }
#endif
}

static void test_v042(void)
{
    v042_profile_t old_config = {0};
    device_profile_config_t decoded;
    uint8_t unaligned_payload[sizeof(old_config) + 1u] = {0};
    bool migrated = false;

    assert(sizeof(old_config) == config_compat_v042_payload_size());
    assert(sizeof(old_config) != sizeof(device_profile_config_t));
    strcpy(old_config.device_name, "v042-device");
    strcpy(old_config.asset_id, "asset-v042");
    old_config.opcua_port = 4840u;
    old_config.channels[0].in_use = 1u;
    old_config.channels[0].driver_id = CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP;
    strcpy(old_config.channels[0].id, "channel.temperature.1");
    strcpy(old_config.channels[0].name, "Temperature1");
    old_config.channels[0].settings.enabled = true;
    old_config.channels[0].settings.warning_threshold = 60.0;
    old_config.channels[0].settings.critical_threshold = 75.0;
    old_config.channels[0].settings.scale = 1.0;

    assert(config_compat_decode_binary((const uint8_t *)&old_config,
                                       sizeof(old_config),
                                       &decoded,
                                       &migrated));
    assert(migrated);
    assert(strcmp(decoded.device_name, old_config.device_name) == 0);
    assert(strcmp(decoded.asset_id, old_config.asset_id) == 0);
    assert(decoded.opcua_port == old_config.opcua_port);
    assert(strcmp(decoded.channels[0].id, old_config.channels[0].id) == 0);
    assert(decoded.channels[0].settings.enabled);
    assert(decoded.channels[0].settings.warning_threshold == 60.0);
    assert(decoded.channels[0].settings.critical_threshold == 75.0);
    assert(decoded.channels[0].settings.scale == 1.0);
    /* v0.4.2 had no lower side; it is disabled independently of the valid
     * native v0.4.3 numeric value zero. */
    assert(decoded.channels[0].settings.low_threshold == DBL_MAX);
    assert(decoded.channels[0].settings.low_low_threshold == DBL_MAX);
    assert(decoded.channels[0].settings.alarm_hysteresis == 0.0);
    assert(decoded.channels[0].settings.alarm_debounce_ms == 0u);

    /* Flash/config import payloads are byte streams, not guaranteed to have
     * the alignment of the frozen C struct.  The decoder must not cast and
     * dereference this source. */
    memcpy(unaligned_payload + 1u, &old_config, sizeof(old_config));
    assert(config_compat_decode_binary(unaligned_payload + 1u,
                                       sizeof(old_config),
                                       &decoded,
                                       &migrated));
    assert(migrated);
    assert(strcmp(decoded.device_name, old_config.device_name) == 0);
    assert(decoded.channels[0].settings.critical_threshold == 75.0);

    /* One byte off the exact v0.4.2 size must not be misread as anything. */
    assert(!config_compat_decode_binary((const uint8_t *)&old_config,
                                        sizeof(old_config) - 1u,
                                        &decoded,
                                        NULL));
}

int main(void)
{
    test_v040();
    test_v040_partial_payloads();
    test_v042();
    printf("config compat host suite: PASS\n");
    return 0;
}
