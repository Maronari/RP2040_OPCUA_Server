#include "services/config_service.h"

#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "drivers/flash_storage_driver.h"
#include "pico/unique_id.h"

static device_profile_config_t g_running_config;
static device_profile_config_t g_staged_config;
static device_profile_config_t g_default_config;
static bool g_default_config_valid = false;
static uint32_t g_running_revision = 0u;
static uint32_t g_staged_revision = 0u;

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

static void config_service_normalize(device_profile_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    if (!config_service_is_valid_timezone_offset_minutes(config->timezone_offset_minutes))
    {
        config->timezone_offset_minutes = 0;
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

    device_profile_config_t persisted = {0};
    if (flash_storage_driver_load(&persisted, sizeof(persisted)))
    {
        g_running_config = persisted;
        g_staged_config = persisted;
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

bool config_service_stage_set_mqtt_pubsub_enabled(bool enabled)
{
    taskENTER_CRITICAL();
    g_staged_config.enable_mqtt_pubsub = enabled;
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

    return flash_storage_driver_save(&normalized_config, sizeof(normalized_config));
}
