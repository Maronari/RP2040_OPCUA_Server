#include "services/config_compat.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define V040_CHANNELS 4u

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
    config_channel_definition_t channels[V040_CHANNELS];
    char product_instance_uri[CONFIG_SERVICE_URI_MAX_LEN];
    char asset_id[CONFIG_SERVICE_ASSET_ID_MAX_LEN];
    int64_t manufacture_date_utc;
    int64_t calibration_date_utc;
    char location[CONFIG_SERVICE_LOCATION_MAX_LEN];
    char asset_class[CONFIG_SERVICE_ASSET_CLASS_MAX_LEN];
    char hierarchical_location_path[CONFIG_SERVICE_LOCATION_PATH_MAX_LEN];
    char operational_location_path[CONFIG_SERVICE_LOCATION_PATH_MAX_LEN];
} v040_profile_t;

int main(void)
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
    old_config.channels[3].in_use = 1u;
    strcpy(old_config.channels[3].id, "channel.four");

#if PLATFORM_CHANNEL_LIMIT < V040_CHANNELS
    assert(!config_compat_decode_binary((const uint8_t *)&old_config,
                                        sizeof(old_config),
                                        &decoded,
                                        &migrated));
    old_config.channels[3].in_use = 0u;
#endif
    assert(config_compat_decode_binary((const uint8_t *)&old_config,
                                       sizeof(old_config),
                                       &decoded,
                                       &migrated));
#if PLATFORM_CHANNEL_LIMIT == V040_CHANNELS
    assert(!migrated);
#else
    assert(migrated);
#endif
    assert(strcmp(decoded.device_name, old_config.device_name) == 0);
    assert(strcmp(decoded.asset_id, old_config.asset_id) == 0);
    assert(strcmp(decoded.hierarchical_location_path,
                  old_config.hierarchical_location_path) == 0);
    assert(decoded.opcua_port == old_config.opcua_port);
    assert(strcmp(decoded.channels[0].id, old_config.channels[0].id) == 0);
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
    return 0;
}
