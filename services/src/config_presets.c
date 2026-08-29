/**
 * @file
 * @brief Compile-time preset catalog (`g_config_preset_catalog`) and the
 * `config_presets_apply()` entry point declared in
 * `services/config_presets.h`.
 *
 * Responsibilities:
 * - `config_preset_sensor_node_populate_channels()`: the one preset proven
 *   end to end for v0.5.0 T8 -- a small read-only sensor set (internal
 *   temperature, one generic ADC voltage input, one GPIO digital input)
 *   built entirely from drivers already compiled into this firmware image
 *   (`driver_catalog.c`).
 * - `config_presets_build()`/`config_presets_apply()`: resolve a preset id
 *   and feed it into `config_service_replace_and_save()`, the existing
 *   transaction pipeline -- no parallel apply path.
 *
 * Ownership: `g_config_preset_catalog` has static storage duration for the
 * process lifetime and is never mutated after startup.
 *
 * Concurrency: none needed; every function here is a pure/read-only helper
 * over caller-supplied storage, aside from the one call into
 * `config_service_replace_and_save()` (which owns its own synchronization).
 */
#include "services/config_presets.h"

#include <string.h>

#include "services/device_channels.h"
#include "services/driver_catalog.h"
#include "services/platform_limits.h"

/* Operator-adjustable via SetChannelConfig/ClearChannelConfig after apply --
 * these are a reasonable starting layout for the reference board, not a
 * claim about any particular wiring. */
#define CONFIG_PRESET_SENSOR_NODE_ADC_CHANNEL_ID "channel.adc.1"
#define CONFIG_PRESET_SENSOR_NODE_GPIO_CHANNEL_ID "channel.gpio.1"

/** @brief Populates the "sensor_node" preset's channel layout: internal
 * temperature (reusing the same channel id as the compiled default profile,
 * see app/basic_profile/main.c), one ADC voltage input, one GPIO digital
 * input. */
static void config_preset_sensor_node_populate_channels(config_channel_definition_t *channels)
{
    memset(channels, 0, sizeof(*channels) * PLATFORM_CHANNEL_LIMIT);

    channels[0] = (config_channel_definition_t){
        .in_use = 1u,
        .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_ANALOG,
        .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_FLOAT,
        .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP,
        .source_index = 0u,
        .id = DEVICE_CHANNEL_ID_TEMPERATURE_1,
        .name = DEVICE_CHANNEL_NAME_TEMPERATURE_1,
        .description = "Internal temperature telemetry channel",
        .unit = "degC",
        .flags = TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_HISTORIZED |
                 TELEMETRY_CHANNEL_FLAG_ALARM_ENABLED,
        .settings = {
            .enabled = true,
            .sample_period_ms = 1000u,
            .publish_period_ms = 1000u,
            .history_interval_ms = 1000u,
            .scale = 1.0,
            .offset = 0.0,
            .warning_threshold = 60.0,
            .critical_threshold = 75.0,
        },
    };

    channels[1] = (config_channel_definition_t){
        .in_use = 1u,
        .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_ANALOG,
        .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_FLOAT,
        .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE,
        .source_index = 0u,
        .id = CONFIG_PRESET_SENSOR_NODE_ADC_CHANNEL_ID,
        .name = "AdcVoltage1",
        .description = "Generic ADC voltage telemetry channel",
        .unit = "V",
        .flags = TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_HISTORIZED,
        .settings = {
            .enabled = true,
            .sample_period_ms = 1000u,
            .publish_period_ms = 1000u,
            .history_interval_ms = 1000u,
            .scale = 1.0,
            .offset = 0.0,
            .warning_threshold = 3.3,
            .critical_threshold = 3.3,
        },
    };

    channels[2] = (config_channel_definition_t){
        .in_use = 1u,
        .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_DISCRETE,
        .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_BOOLEAN,
        .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_GPIO_INPUT,
        .source_index = 16u,
        .id = CONFIG_PRESET_SENSOR_NODE_GPIO_CHANNEL_ID,
        .name = "GpioInput1",
        .description = "General-purpose digital input channel",
        .unit = "",
        .flags = TELEMETRY_CHANNEL_FLAG_ENABLED,
        .settings = {
            .enabled = true,
            .sample_period_ms = 500u,
            .publish_period_ms = 500u,
            .history_interval_ms = 1000u,
            .scale = 1.0,
            .offset = 0.0,
        },
    };
}

/* Fixed array of descriptors, mirroring driver_catalog.c's g_driver_catalog
 * shape. Append future presets (remote_io, dev_benchmark) here -- see the
 * config_preset_id_t doc comment in config_presets.h. */
static const config_preset_catalog_entry_t g_config_preset_catalog[] = {
    {
        .preset_id = (uint8_t)CONFIG_PRESET_ID_SENSOR_NODE,
        .name = "sensor_node",
        .description = "Small read-only sensor set: internal temperature, one ADC voltage input, one GPIO digital input.",
        .populate_channels = config_preset_sensor_node_populate_channels,
    },
};

size_t config_presets_count(void)
{
    return sizeof(g_config_preset_catalog) / sizeof(g_config_preset_catalog[0]);
}

const config_preset_catalog_entry_t *config_presets_get_by_index(size_t index)
{
    return index < config_presets_count() ? &g_config_preset_catalog[index] : NULL;
}

const config_preset_catalog_entry_t *config_presets_get_by_id(uint8_t preset_id)
{
    for (size_t index = 0u; index < config_presets_count(); ++index)
    {
        if (g_config_preset_catalog[index].preset_id == preset_id)
        {
            return &g_config_preset_catalog[index];
        }
    }
    return NULL;
}

const config_preset_catalog_entry_t *config_presets_get_by_name(const char *name)
{
    if (name == NULL)
    {
        return NULL;
    }
    for (size_t index = 0u; index < config_presets_count(); ++index)
    {
        if (strcmp(g_config_preset_catalog[index].name, name) == 0)
        {
            return &g_config_preset_catalog[index];
        }
    }
    return NULL;
}

bool config_presets_build(uint8_t preset_id,
                          const device_profile_config_t *base,
                          device_profile_config_t *out_config)
{
    const config_preset_catalog_entry_t *entry = config_presets_get_by_id(preset_id);

    if (entry == NULL || base == NULL || out_config == NULL || entry->populate_channels == NULL)
    {
        return false;
    }

    *out_config = *base;
    entry->populate_channels(out_config->channels);
    return true;
}

bool config_presets_apply(uint8_t preset_id)
{
    device_profile_config_t candidate;

    if (!config_presets_build(preset_id, config_service_get(), &candidate))
    {
        return false;
    }

    return config_service_replace_and_save(&candidate, true);
}
