#include "services/config_compat.h"

#include <stddef.h>
#include <string.h>

#define CONFIG_COMPAT_LEGACY_ASSET_ID_MAX_LEN 32u

/*
 * Frozen native payload written by v0.4.0 on the same ARM ABI.  This is a
 * compatibility decoder only; new records always use device_profile_config_t.
 */
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
    config_channel_definition_t channels[CONFIG_COMPAT_V040_CHANNEL_LIMIT];
    char product_instance_uri[CONFIG_SERVICE_URI_MAX_LEN];
    char asset_id[CONFIG_SERVICE_ASSET_ID_MAX_LEN];
    int64_t manufacture_date_utc;
    int64_t calibration_date_utc;
    char location[CONFIG_SERVICE_LOCATION_MAX_LEN];
    char asset_class[CONFIG_SERVICE_ASSET_CLASS_MAX_LEN];
    char hierarchical_location_path[CONFIG_SERVICE_LOCATION_PATH_MAX_LEN];
    char operational_location_path[CONFIG_SERVICE_LOCATION_PATH_MAX_LEN];
} config_compat_v040_profile_t;

static size_t config_compat_align_up(size_t value, size_t alignment)
{
    return alignment == 0u ? value : ((value + alignment - 1u) / alignment) * alignment;
}

static bool config_compat_expand_v040(const config_compat_v040_profile_t *old_config,
                                      device_profile_config_t *out_config)
{
    const size_t prefix_size = offsetof(config_compat_v040_profile_t, channels);
    const size_t copied_channels = PLATFORM_CHANNEL_LIMIT < CONFIG_COMPAT_V040_CHANNEL_LIMIT ?
                                       PLATFORM_CHANNEL_LIMIT : CONFIG_COMPAT_V040_CHANNEL_LIMIT;

    for (size_t index = copied_channels; index < CONFIG_COMPAT_V040_CHANNEL_LIMIT; ++index)
    {
        if (old_config->channels[index].in_use != 0u)
        {
            return false;
        }
    }

    memset(out_config, 0, sizeof(*out_config));
    memcpy(out_config, old_config, prefix_size);
    memcpy(out_config->channels, old_config->channels,
           copied_channels * sizeof(old_config->channels[0]));
    memcpy(out_config->product_instance_uri, old_config->product_instance_uri,
           sizeof(out_config->product_instance_uri));
    memcpy(out_config->asset_id, old_config->asset_id, sizeof(out_config->asset_id));
    out_config->manufacture_date_utc = old_config->manufacture_date_utc;
    out_config->calibration_date_utc = old_config->calibration_date_utc;
    memcpy(out_config->location, old_config->location, sizeof(out_config->location));
    memcpy(out_config->asset_class, old_config->asset_class, sizeof(out_config->asset_class));
    memcpy(out_config->hierarchical_location_path, old_config->hierarchical_location_path,
           sizeof(out_config->hierarchical_location_path));
    memcpy(out_config->operational_location_path, old_config->operational_location_path,
           sizeof(out_config->operational_location_path));
    return true;
}

static bool config_compat_decode_v040_legacy(const uint8_t *payload,
                                             size_t payload_size,
                                             device_profile_config_t *out_config)
{
    config_compat_v040_profile_t old_config = {0};
    const size_t asset_offset = offsetof(config_compat_v040_profile_t, asset_id);
    const size_t tail_offset = offsetof(config_compat_v040_profile_t, manufacture_date_utc);
    const size_t previous_size = offsetof(config_compat_v040_profile_t,
                                          hierarchical_location_path);
    const size_t legacy_tail_offset =
        config_compat_align_up(asset_offset + CONFIG_COMPAT_LEGACY_ASSET_ID_MAX_LEN,
                               _Alignof(int64_t));
    const size_t legacy_size = legacy_tail_offset + previous_size - tail_offset;

    if (payload_size == previous_size)
    {
        memcpy(&old_config, payload, previous_size);
        return config_compat_expand_v040(&old_config, out_config);
    }
    if (payload_size != legacy_size)
    {
        return false;
    }

    memcpy(&old_config, payload, asset_offset);
    memcpy(old_config.asset_id, payload + asset_offset,
           CONFIG_COMPAT_LEGACY_ASSET_ID_MAX_LEN);
    memcpy((uint8_t *)&old_config + tail_offset, payload + legacy_tail_offset,
           previous_size - tail_offset);
    return config_compat_expand_v040(&old_config, out_config);
}

bool config_compat_decode_binary(const uint8_t *payload,
                                 size_t payload_size,
                                 device_profile_config_t *out_config,
                                 bool *out_migrated)
{
    config_compat_v040_profile_t old_config;

    if (payload == NULL || out_config == NULL)
    {
        return false;
    }
    if (out_migrated != NULL)
    {
        *out_migrated = false;
    }
    if (payload_size == sizeof(*out_config))
    {
        if ((const void *)out_config != (const void *)payload)
        {
            memmove(out_config, payload, sizeof(*out_config));
        }
        return true;
    }
    memset(out_config, 0, sizeof(*out_config));
    if (payload_size == sizeof(old_config))
    {
        memcpy(&old_config, payload, sizeof(old_config));
        if (!config_compat_expand_v040(&old_config, out_config))
        {
            return false;
        }
    }
    else if (!config_compat_decode_v040_legacy(payload, payload_size, out_config))
    {
        return false;
    }

    if (out_migrated != NULL)
    {
        *out_migrated = true;
    }
    return true;
}

size_t config_compat_v040_payload_size(void)
{
    return sizeof(config_compat_v040_profile_t);
}
