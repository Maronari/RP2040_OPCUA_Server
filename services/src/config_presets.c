/**
 * @file
 * @brief Compile-time preset catalog (`g_config_preset_catalog`) and the
 * `config_presets_apply()` entry point declared in
 * `services/config_presets.h`.
 *
 * Responsibilities:
 * - `config_preset_sensor_node_populate_channels()`: the first preset proven
 *   end to end for v0.5.0 T8 -- a small read-only sensor set (internal
 *   temperature, one generic ADC voltage input, one GPIO digital input).
 * - `config_preset_remote_io_populate_channels()`: a discrete-I/O-heavy
 *   composition modelling a remote I/O node (many GPIO inputs, a couple of
 *   supervisory ADC inputs, no temperature channel).
 * - `config_preset_dev_benchmark_populate_channels()`: fills
 *   `PLATFORM_CHANNEL_LIMIT` exactly by mixing all three compiled driver
 *   types, every channel historized and alarm-enabled, for v0.5.2's
 *   benchmark work to load against.
 *   All three are built entirely from drivers already compiled into this
 *   firmware image (`driver_catalog.c`) -- ROADMAP.md's Modbus/remote-I/O
 *   hardware drivers don't exist yet (deferred to v0.5.1+), so `remote_io`
 *   and `dev_benchmark` are necessarily different compositions of the same
 *   three native drivers `sensor_node` uses, not new hardware integrations.
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

#include <stdio.h>
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

#define CONFIG_PRESET_REMOTE_IO_GPIO_COUNT 12u
#define CONFIG_PRESET_REMOTE_IO_ADC_COUNT 2u

/** @brief Populates the "remote_io" preset's channel layout: a discrete-I/O-
 * heavy remote node -- CONFIG_PRESET_REMOTE_IO_GPIO_COUNT GPIO digital
 * inputs (source_index 0..N-1, each a distinct pin so none collide under
 * driver_catalog_claims_are_compatible()) plus CONFIG_PRESET_REMOTE_IO_ADC_COUNT
 * ADC voltage inputs for supervisory measurement (source_index 0..1, well
 * inside the 4-input ADC ceiling native_adc_voltage_source_is_valid()
 * enforces -- see drivers/src/adc_voltage_channel_driver.c). No
 * internal-temperature channel: this preset models a remote I/O module, not
 * a sensor node. 12 + 2 == 14, leaving headroom under PLATFORM_CHANNEL_LIMIT
 * for the operator to add channels afterward via SetChannelConfig. */
static void config_preset_remote_io_populate_channels(config_channel_definition_t *channels)
{
    memset(channels, 0, sizeof(*channels) * PLATFORM_CHANNEL_LIMIT);

    for (uint8_t index = 0u; index < CONFIG_PRESET_REMOTE_IO_GPIO_COUNT; ++index)
    {
        channels[index] = (config_channel_definition_t){
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_DISCRETE,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_BOOLEAN,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_GPIO_INPUT,
            .source_index = index,
            .description = "Remote discrete input channel",
            .flags = TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_HISTORIZED,
            .settings = {
                .enabled = true,
                .sample_period_ms = 500u,
                .publish_period_ms = 500u,
                .history_interval_ms = 1000u,
                .scale = 1.0,
                .offset = 0.0,
            },
        };
        (void)snprintf(channels[index].id, sizeof(channels[index].id), "channel.gpio.%u", (unsigned)(index + 1u));
        (void)snprintf(channels[index].name, sizeof(channels[index].name), "GpioInput%u", (unsigned)(index + 1u));
    }

    for (uint8_t index = 0u; index < CONFIG_PRESET_REMOTE_IO_ADC_COUNT; ++index)
    {
        const uint8_t slot = CONFIG_PRESET_REMOTE_IO_GPIO_COUNT + index;

        channels[slot] = (config_channel_definition_t){
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_ANALOG,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_FLOAT,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE,
            .source_index = index,
            .description = "Remote supervisory ADC voltage channel",
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
        (void)snprintf(channels[slot].id, sizeof(channels[slot].id), "channel.adc.%u", (unsigned)(index + 1u));
        (void)snprintf(channels[slot].name, sizeof(channels[slot].name), "AdcVoltage%u", (unsigned)(index + 1u));
    }
}

#define CONFIG_PRESET_DEV_BENCHMARK_ADC_COUNT 4u
#define CONFIG_PRESET_DEV_BENCHMARK_GPIO_COUNT 11u

/** @brief Populates the "dev_benchmark" preset's channel layout: one
 * internal-temperature channel, CONFIG_PRESET_DEV_BENCHMARK_ADC_COUNT ADC
 * voltage channels (the native ADC driver's full source_index 0..3 range --
 * see drivers/src/adc_voltage_channel_driver.c), and
 * CONFIG_PRESET_DEV_BENCHMARK_GPIO_COUNT GPIO digital inputs -- 1 + 4 + 11
 * == PLATFORM_CHANNEL_LIMIT (16 on the reference profile), mixing all three
 * compiled driver types at the platform's compiled channel ceiling. Every
 * channel is historized and alarm-enabled so this is a genuine load for
 * v0.5.2's benchmark work, not just a filler channel count. */
static void config_preset_dev_benchmark_populate_channels(config_channel_definition_t *channels)
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

    for (uint8_t index = 0u; index < CONFIG_PRESET_DEV_BENCHMARK_ADC_COUNT; ++index)
    {
        const uint8_t slot = 1u + index;

        channels[slot] = (config_channel_definition_t){
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_ANALOG,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_FLOAT,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE,
            .source_index = index,
            .description = "Benchmark ADC voltage channel",
            .unit = "V",
            .flags = TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_HISTORIZED |
                     TELEMETRY_CHANNEL_FLAG_ALARM_ENABLED,
            .settings = {
                .enabled = true,
                .sample_period_ms = 500u,
                .publish_period_ms = 500u,
                .history_interval_ms = 500u,
                .scale = 1.0,
                .offset = 0.0,
                .warning_threshold = 3.3,
                .critical_threshold = 3.3,
            },
        };
        (void)snprintf(channels[slot].id, sizeof(channels[slot].id), "channel.adc.%u", (unsigned)(index + 1u));
        (void)snprintf(channels[slot].name, sizeof(channels[slot].name), "AdcVoltage%u", (unsigned)(index + 1u));
    }

    for (uint8_t index = 0u; index < CONFIG_PRESET_DEV_BENCHMARK_GPIO_COUNT; ++index)
    {
        const uint8_t slot = 1u + CONFIG_PRESET_DEV_BENCHMARK_ADC_COUNT + index;

        channels[slot] = (config_channel_definition_t){
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_DISCRETE,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_BOOLEAN,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_GPIO_INPUT,
            .source_index = index,
            .description = "Benchmark discrete input channel",
            .flags = TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_HISTORIZED |
                     TELEMETRY_CHANNEL_FLAG_ALARM_ENABLED,
            .settings = {
                .enabled = true,
                .sample_period_ms = 200u,
                .publish_period_ms = 200u,
                .history_interval_ms = 200u,
                .scale = 1.0,
                .offset = 0.0,
            },
        };
        (void)snprintf(channels[slot].id, sizeof(channels[slot].id), "channel.gpio.%u", (unsigned)(index + 1u));
        (void)snprintf(channels[slot].name, sizeof(channels[slot].name), "GpioInput%u", (unsigned)(index + 1u));
    }
}

/* Fixed array of descriptors, mirroring driver_catalog.c's g_driver_catalog
 * shape. */
static const config_preset_catalog_entry_t g_config_preset_catalog[] = {
    {
        .preset_id = (uint8_t)CONFIG_PRESET_ID_SENSOR_NODE,
        .name = "sensor_node",
        .description = "Small read-only sensor set: internal temperature, one ADC voltage input, one GPIO digital input.",
        .populate_channels = config_preset_sensor_node_populate_channels,
    },
    {
        .preset_id = (uint8_t)CONFIG_PRESET_ID_REMOTE_IO,
        .name = "remote_io",
        .description = "Discrete-I/O-heavy remote node: 12 GPIO digital inputs plus 2 supervisory ADC voltage inputs.",
        .populate_channels = config_preset_remote_io_populate_channels,
    },
    {
        .preset_id = (uint8_t)CONFIG_PRESET_ID_DEV_BENCHMARK,
        .name = "dev_benchmark",
        .description = "Maximum-density benchmark load: internal temperature, 4 ADC voltage inputs, 11 GPIO digital inputs (16 channels total, all historized and alarm-enabled).",
        .populate_channels = config_preset_dev_benchmark_populate_channels,
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
