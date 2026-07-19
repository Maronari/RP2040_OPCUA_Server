#include "services/config_service.h"

#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include <config_store_limits.h>
#include "drivers/flash_storage_driver.h"
#include "services/config_store_service.h"
#include "pico/unique_id.h"

_Static_assert(sizeof(device_profile_config_t) <= CONFIG_STORE_MAX_PAYLOAD_SIZE_BYTES,
               "device_profile_config_t must fit in one config storage slot");

static device_profile_config_t g_running_config;
static device_profile_config_t g_staged_config;
static device_profile_config_t g_default_config;
static uint8_t g_persisted_payload[sizeof(device_profile_config_t)];
static device_profile_config_t g_persisted_config;
static bool g_default_config_valid = false;
static uint32_t g_running_revision = 0u;
static uint32_t g_staged_revision = 0u;

#define CONFIG_SERVICE_LEGACY_ASSET_ID_MAX_LEN 32u

static size_t config_service_align_up(size_t value, size_t alignment)
{
    return alignment == 0u ? value : ((value + alignment - 1u) / alignment) * alignment;
}

static bool config_service_decode_persisted(const uint8_t *payload,
                                            size_t payload_size,
                                            device_profile_config_t *out_config)
{
    const size_t asset_offset = offsetof(device_profile_config_t, asset_id);
    const size_t current_tail_offset = offsetof(device_profile_config_t, manufacture_date_utc);
    const size_t legacy_tail_offset =
        config_service_align_up(asset_offset + CONFIG_SERVICE_LEGACY_ASSET_ID_MAX_LEN,
                                _Alignof(int64_t));
    const size_t tail_size = sizeof(device_profile_config_t) - current_tail_offset;
    const size_t legacy_payload_size = legacy_tail_offset + tail_size;

    if (payload == NULL || out_config == NULL)
    {
        return false;
    }

    memset(out_config, 0, sizeof(*out_config));
    if (payload_size == sizeof(*out_config))
    {
        memcpy(out_config, payload, sizeof(*out_config));
        return true;
    }

    if (payload_size != legacy_payload_size)
    {
        return false;
    }

    memcpy(out_config, payload, asset_offset);
    memcpy(out_config->asset_id,
           payload + asset_offset,
           CONFIG_SERVICE_LEGACY_ASSET_ID_MAX_LEN);
    memcpy((uint8_t *)out_config + current_tail_offset,
           payload + legacy_tail_offset,
           tail_size);
    return true;
}

static bool config_service_is_valid_timezone_offset_minutes(int16_t value)
{
    return value >= -720 && value <= 840;
}

static bool config_service_is_valid_channel_kind(uint8_t value)
{
    return value <= (uint8_t)TELEMETRY_CHANNEL_KIND_COMPUTED;
}

static bool config_service_is_valid_channel_value_type(uint8_t value)
{
    return value <= (uint8_t)TELEMETRY_CHANNEL_VALUE_STRING;
}

static void config_service_force_terminated(char *text, size_t size)
{
    if (text == NULL || size == 0u)
    {
        return;
    }

    text[size - 1u] = '\0';
}

static bool config_service_ipv4_is_zero(const uint8_t address[4])
{
    return address == NULL ||
           (address[0] == 0u && address[1] == 0u && address[2] == 0u && address[3] == 0u);
}

static void config_service_normalize_text_fields(device_profile_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    config_service_force_terminated(config->manufacturer, sizeof(config->manufacturer));
    config_service_force_terminated(config->model, sizeof(config->model));
    config_service_force_terminated(config->serial_number, sizeof(config->serial_number));
    config_service_force_terminated(config->hw_revision, sizeof(config->hw_revision));
    config_service_force_terminated(config->fw_revision, sizeof(config->fw_revision));
    config_service_force_terminated(config->build_time, sizeof(config->build_time));
    config_service_force_terminated(config->git_hash, sizeof(config->git_hash));
    config_service_force_terminated(config->device_name, sizeof(config->device_name));
    config_service_force_terminated(config->opcua_hostname, sizeof(config->opcua_hostname));
    config_service_force_terminated(config->ntp_server, sizeof(config->ntp_server));
    config_service_force_terminated(config->mqtt_broker_url, sizeof(config->mqtt_broker_url));
    config_service_force_terminated(config->mqtt_topic_root, sizeof(config->mqtt_topic_root));
    config_service_force_terminated(config->product_instance_uri, sizeof(config->product_instance_uri));
    config_service_force_terminated(config->asset_id, sizeof(config->asset_id));
    config_service_force_terminated(config->location, sizeof(config->location));
    config_service_force_terminated(config->asset_class, sizeof(config->asset_class));
}

static void config_service_normalize_channel(config_channel_definition_t *channel, size_t index)
{
    const driver_catalog_entry_t *driver = NULL;
    const bool analog_source = channel != NULL &&
                               (driver = driver_catalog_get_by_id(channel->driver_id)) != NULL &&
                               driver->analog;

    if (channel == NULL)
    {
        return;
    }

    config_service_force_terminated(channel->id, sizeof(channel->id));
    config_service_force_terminated(channel->name, sizeof(channel->name));
    config_service_force_terminated(channel->description, sizeof(channel->description));
    config_service_force_terminated(channel->unit, sizeof(channel->unit));

    if (channel->in_use == 0u)
    {
        memset(channel, 0, sizeof(*channel));
        return;
    }

    if (driver == NULL)
    {
        memset(channel, 0, sizeof(*channel));
        return;
    }

    if (driver->validate_source != NULL && !driver->validate_source(channel->source_index, channel->source_flags))
    {
        memset(channel, 0, sizeof(*channel));
        return;
    }

    if (channel->flags == 0u)
    {
        channel->kind = (uint8_t)driver->default_kind;
        channel->value_type = (uint8_t)driver->default_value_type;
        channel->flags = driver->default_flags;
    }
    else
    {
        if (!config_service_is_valid_channel_kind(channel->kind))
        {
            channel->kind = (uint8_t)driver->default_kind;
        }

        if (!config_service_is_valid_channel_value_type(channel->value_type))
        {
            channel->value_type = (uint8_t)driver->default_value_type;
        }
    }

    if (channel->id[0] == '\0')
    {
        (void)snprintf(channel->id, sizeof(channel->id), "channel.%u", (unsigned)(index + 1u));
    }

    if (channel->name[0] == '\0')
    {
        (void)snprintf(channel->name, sizeof(channel->name), "Channel%u", (unsigned)(index + 1u));
    }

    if (channel->description[0] == '\0')
    {
        (void)snprintf(channel->description, sizeof(channel->description), "Configurable telemetry channel %u", (unsigned)(index + 1u));
    }

    if (channel->unit[0] == '\0' && driver->default_unit != NULL)
    {
        strncpy(channel->unit, driver->default_unit, sizeof(channel->unit));
        channel->unit[sizeof(channel->unit) - 1u] = '\0';
    }

    if (channel->settings.sample_period_ms == 0u)
    {
        channel->settings.sample_period_ms = 1000u;
    }

    if (channel->settings.publish_period_ms == 0u)
    {
        channel->settings.publish_period_ms = channel->settings.sample_period_ms;
    }

    if (channel->settings.history_interval_ms == 0u)
    {
        channel->settings.history_interval_ms = channel->settings.sample_period_ms;
    }

    if (channel->settings.scale == 0.0 && analog_source)
    {
        channel->settings.scale = 1.0;
    }

    if (!analog_source)
    {
        channel->settings.scale = 1.0;
        channel->settings.offset = 0.0;
        channel->has_minimum = 0u;
        channel->has_maximum = 0u;
    }

    if (channel->settings.critical_threshold < channel->settings.warning_threshold)
    {
        channel->settings.critical_threshold = channel->settings.warning_threshold;
    }

    if (!channel->settings.enabled)
    {
        channel->flags &= ~TELEMETRY_CHANNEL_FLAG_ENABLED;
    }
}

static void config_service_populate_serial_number(device_profile_config_t *config)
{
    pico_unique_board_id_t board_id;
    size_t offset = 0;

    if (config == NULL || config->serial_number[0] != '\0')
    {
        return;
    }

    pico_get_unique_board_id(&board_id);

    for (size_t index = 0; index < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; ++index)
    {
        if ((offset + 2u) >= sizeof(config->serial_number))
        {
            break;
        }

        offset += (size_t)snprintf(&config->serial_number[offset],
                                   sizeof(config->serial_number) - offset,
                                   "%02X",
                                   board_id.id[index]);
    }
}

static void config_service_populate_product_instance_uri(device_profile_config_t *config)
{
    if (config == NULL || config->product_instance_uri[0] != '\0')
    {
        return;
    }

    (void)snprintf(config->product_instance_uri,
                   sizeof(config->product_instance_uri),
                   "urn:rp2-opcua:%s",
                   config->serial_number[0] != '\0' ? config->serial_number : "device");
    config->product_instance_uri[sizeof(config->product_instance_uri) - 1u] = '\0';
}

static void config_service_normalize(device_profile_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    config_service_normalize_text_fields(config);

    if (!config_service_is_valid_timezone_offset_minutes(config->timezone_offset_minutes))
    {
        config->timezone_offset_minutes = 0;
    }

    if (config->opcua_port == 0u)
    {
        config->opcua_port = 4840u;
    }

    if (config->network_mode != NETWORK_MODE_DHCP && config->network_mode != NETWORK_MODE_STATIC)
    {
        config->network_mode = NETWORK_MODE_DHCP;
    }

    if (config_service_ipv4_is_zero(config->static_netmask))
    {
        config->static_netmask[0] = 255u;
        config->static_netmask[1] = 255u;
        config->static_netmask[2] = 255u;
        config->static_netmask[3] = 0u;
    }

    if (config_service_ipv4_is_zero(config->static_dns))
    {
        config->static_dns[0] = 8u;
        config->static_dns[1] = 8u;
        config->static_dns[2] = 8u;
        config->static_dns[3] = 8u;
    }

    if (config->device_name[0] == '\0' && config->opcua_hostname[0] != '\0')
    {
        strncpy(config->device_name, config->opcua_hostname, sizeof(config->device_name));
        config->device_name[sizeof(config->device_name) - 1] = '\0';
    }

    if (config->opcua_hostname[0] == '\0' && config->device_name[0] != '\0')
    {
        strncpy(config->opcua_hostname, config->device_name, sizeof(config->opcua_hostname));
        config->opcua_hostname[sizeof(config->opcua_hostname) - 1] = '\0';
    }

    if (config->ntp_server[0] == '\0')
    {
        strncpy(config->ntp_server, "pool.ntp.org", sizeof(config->ntp_server));
        config->ntp_server[sizeof(config->ntp_server) - 1] = '\0';
    }

    if (config->mqtt_broker_url[0] == '\0')
    {
        strncpy(config->mqtt_broker_url, "opc.mqtt://127.0.0.1:1883", sizeof(config->mqtt_broker_url));
        config->mqtt_broker_url[sizeof(config->mqtt_broker_url) - 1] = '\0';
    }

    if (config->mqtt_topic_root[0] == '\0')
    {
        (void)snprintf(config->mqtt_topic_root,
                       sizeof(config->mqtt_topic_root),
                       "devices/%s",
                       config->opcua_hostname[0] != '\0' ? config->opcua_hostname : "device");
        config->mqtt_topic_root[sizeof(config->mqtt_topic_root) - 1] = '\0';
    }

    for (size_t index = 0u; index < CONFIG_SERVICE_MAX_CHANNELS; ++index)
    {
        config_service_normalize_channel(&config->channels[index], index);
    }

    config_service_populate_serial_number(config);
    config_service_populate_product_instance_uri(config);
}

void config_service_init(const device_profile_config_t *defaults)
{
    memset(&g_running_config, 0, sizeof(g_running_config));
    memset(&g_staged_config, 0, sizeof(g_staged_config));
    memset(&g_default_config, 0, sizeof(g_default_config));
    g_default_config_valid = false;
    g_running_revision = 1u;
    g_staged_revision = 1u;

    if (defaults != NULL)
    {
        g_running_config = *defaults;
        g_staged_config = *defaults;
        g_default_config = *defaults;
        g_default_config_valid = true;
    }

    size_t persisted_payload_size = 0u;
    config_store_record_info_t persisted_info = {0};
    memset(g_persisted_payload, 0, sizeof(g_persisted_payload));
    memset(&g_persisted_config, 0, sizeof(g_persisted_config));
    if (config_store_load_active(g_persisted_payload,
                                 sizeof(g_persisted_payload),
                                 &persisted_payload_size,
                                 &persisted_info) &&
        persisted_info.payload_format == CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY &&
        config_service_decode_persisted(g_persisted_payload,
                                        persisted_payload_size,
                                        &g_persisted_config))
    {
        g_running_config = g_persisted_config;
        g_staged_config = g_persisted_config;
        g_running_revision = persisted_info.generation > 0u ? persisted_info.generation : 1u;
        g_staged_revision = g_running_revision;
    }
    else if (flash_storage_driver_load_partial(g_persisted_payload,
                                               sizeof(g_persisted_payload),
                                               &persisted_payload_size) &&
             config_service_decode_persisted(g_persisted_payload,
                                             persisted_payload_size,
                                             &g_persisted_config))
    {
        g_running_config = g_persisted_config;
        g_staged_config = g_persisted_config;
    }

    config_service_normalize(&g_running_config);
    config_service_normalize(&g_staged_config);
    if (g_default_config_valid)
    {
        config_service_normalize(&g_default_config);
    }
}

const device_profile_config_t *config_service_get(void)
{
    return &g_running_config;
}

const device_profile_config_t *config_service_get_staged(void)
{
    return &g_staged_config;
}

bool config_service_reset_to_defaults(void)
{
    if (!g_default_config_valid)
    {
        return false;
    }

    taskENTER_CRITICAL();
    g_staged_config = g_default_config;
    g_staged_revision++;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_apply(void)
{
    device_profile_config_t normalized_config;

    taskENTER_CRITICAL();
    normalized_config = g_staged_config;
    taskEXIT_CRITICAL();

    config_service_normalize(&normalized_config);

    taskENTER_CRITICAL();
    g_running_config = normalized_config;
    g_running_revision++;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_set_timezone_offset_minutes(int16_t value)
{
    if (!config_service_is_valid_timezone_offset_minutes(value))
    {
        return false;
    }

    taskENTER_CRITICAL();
    g_running_config.timezone_offset_minutes = value;
    g_staged_config.timezone_offset_minutes = value;
    g_running_revision++;
    g_staged_revision++;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_set_asset_id(const char *value)
{
    device_profile_config_t persisted_config;
    size_t value_length;

    if (value == NULL)
    {
        return false;
    }

    value_length = strlen(value);
    if (value_length >= CONFIG_SERVICE_ASSET_ID_MAX_LEN)
    {
        return false;
    }

    taskENTER_CRITICAL();
    persisted_config = g_running_config;
    taskEXIT_CRITICAL();
    memset(persisted_config.asset_id, 0, sizeof(persisted_config.asset_id));
    memcpy(persisted_config.asset_id, value, value_length + 1u);

    if (!config_store_save_payload(&persisted_config,
                                   sizeof(persisted_config),
                                   CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY,
                                   CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY,
                                   NULL))
    {
        return false;
    }

    taskENTER_CRITICAL();
    memset(g_running_config.asset_id, 0, sizeof(g_running_config.asset_id));
    memset(g_staged_config.asset_id, 0, sizeof(g_staged_config.asset_id));
    memcpy(g_running_config.asset_id, value, value_length + 1u);
    memcpy(g_staged_config.asset_id, value, value_length + 1u);
    g_running_revision++;
    g_staged_revision++;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_stage_set_mqtt_pubsub_enabled(bool enabled)
{
    taskENTER_CRITICAL();
    g_staged_config.enable_mqtt_pubsub = enabled;
    g_staged_revision++;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_stage_set_identity(const device_identity_config_t *identity)
{
    device_profile_config_t updated_config;

    if (identity == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();
    updated_config = g_staged_config;
    taskEXIT_CRITICAL();

    memcpy(updated_config.device_name, identity->device_name, sizeof(updated_config.device_name));
    memcpy(updated_config.opcua_hostname, identity->opcua_hostname, sizeof(updated_config.opcua_hostname));
    memcpy(updated_config.product_instance_uri, identity->product_instance_uri, sizeof(updated_config.product_instance_uri));
    memcpy(updated_config.serial_number, identity->serial_number, sizeof(updated_config.serial_number));
    memcpy(updated_config.asset_id, identity->asset_id, sizeof(updated_config.asset_id));
    memcpy(updated_config.hw_revision, identity->hw_revision, sizeof(updated_config.hw_revision));
    memcpy(updated_config.fw_revision, identity->fw_revision, sizeof(updated_config.fw_revision));
    updated_config.manufacture_date_utc = identity->manufacture_date_utc;
    updated_config.calibration_date_utc = identity->calibration_date_utc;
    memcpy(updated_config.location, identity->location, sizeof(updated_config.location));
    memcpy(updated_config.asset_class, identity->asset_class, sizeof(updated_config.asset_class));
    config_service_normalize(&updated_config);

    taskENTER_CRITICAL();
    g_staged_config = updated_config;
    g_staged_revision++;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_stage_set_network(const device_network_config_t *network)
{
    device_profile_config_t updated_config;

    if (network == NULL || network->opcua_port == 0u)
    {
        return false;
    }

    taskENTER_CRITICAL();
    updated_config = g_staged_config;
    taskEXIT_CRITICAL();

    updated_config.network_mode = network->use_dhcp ? NETWORK_MODE_DHCP : NETWORK_MODE_STATIC;
    memcpy(updated_config.mac, network->mac, sizeof(updated_config.mac));
    memcpy(updated_config.opcua_hostname, network->opcua_hostname, sizeof(updated_config.opcua_hostname));
    updated_config.opcua_port = network->opcua_port;
    memcpy(updated_config.ntp_server, network->ntp_server, sizeof(updated_config.ntp_server));
    memcpy(updated_config.static_ip, network->static_ip, sizeof(updated_config.static_ip));
    memcpy(updated_config.static_netmask, network->static_netmask, sizeof(updated_config.static_netmask));
    memcpy(updated_config.static_gateway, network->static_gateway, sizeof(updated_config.static_gateway));
    memcpy(updated_config.static_dns, network->static_dns, sizeof(updated_config.static_dns));
    config_service_normalize(&updated_config);

    taskENTER_CRITICAL();
    g_staged_config = updated_config;
    g_staged_revision++;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_stage_copy_channel(size_t index, config_channel_definition_t *out_channel)
{
    if (out_channel == NULL || index >= CONFIG_SERVICE_MAX_CHANNELS)
    {
        return false;
    }

    taskENTER_CRITICAL();
    *out_channel = g_staged_config.channels[index];
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_stage_set_channel(size_t index, const config_channel_definition_t *channel)
{
    device_profile_config_t updated_config;

    if (channel == NULL || index >= CONFIG_SERVICE_MAX_CHANNELS)
    {
        return false;
    }

    taskENTER_CRITICAL();
    updated_config = g_staged_config;
    taskEXIT_CRITICAL();

    updated_config.channels[index] = *channel;
    config_service_normalize_channel(&updated_config.channels[index], index);

    taskENTER_CRITICAL();
    g_staged_config = updated_config;
    g_staged_revision++;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_stage_clear_channel(size_t index)
{
    config_channel_definition_t cleared_channel = {0};
    return config_service_stage_set_channel(index, &cleared_channel);
}

bool config_service_replace_and_save(const device_profile_config_t *config, bool apply_running)
{
    device_profile_config_t normalized_config;

    if (config == NULL)
    {
        return false;
    }

    normalized_config = *config;
    config_service_normalize(&normalized_config);

    if (!config_store_save_payload(&normalized_config,
                                   sizeof(normalized_config),
                                   CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY,
                                   CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY,
                                   NULL))
    {
        return false;
    }

    taskENTER_CRITICAL();
    g_staged_config = normalized_config;
    g_staged_revision++;
    if (apply_running)
    {
        g_running_config = normalized_config;
        g_running_revision++;
    }
    taskEXIT_CRITICAL();
    return true;
}

uint32_t config_service_get_running_revision(void)
{
    uint32_t revision = 0u;

    taskENTER_CRITICAL();
    revision = g_running_revision;
    taskEXIT_CRITICAL();
    return revision;
}

uint32_t config_service_get_staged_revision(void)
{
    uint32_t revision = 0u;

    taskENTER_CRITICAL();
    revision = g_staged_revision;
    taskEXIT_CRITICAL();
    return revision;
}

bool config_service_save(void)
{
    device_profile_config_t normalized_config;

    taskENTER_CRITICAL();
    normalized_config = g_staged_config;
    taskEXIT_CRITICAL();

    config_service_normalize(&normalized_config);

    taskENTER_CRITICAL();
    g_staged_config = normalized_config;
    g_staged_revision++;
    taskEXIT_CRITICAL();

    return config_store_save_payload(&normalized_config,
                                     sizeof(normalized_config),
                                     CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY,
                                     CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY,
                                     NULL);
}
