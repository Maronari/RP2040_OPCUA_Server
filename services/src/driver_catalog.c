#include "services/driver_catalog.h"

#include <string.h>

#include "hardware/adc.h"
#include "hardware/gpio.h"

#include "drivers/temp_sensor_driver.h"
#include "services/history_service.h"
#include "services/time_service.h"

static bool g_adc_initialized = false;

static void driver_catalog_store_numeric_value(telemetry_channel_t *channel, double value)
{
    if (channel == NULL)
    {
        return;
    }

    switch (channel->metadata.value_type)
    {
    case TELEMETRY_CHANNEL_VALUE_DOUBLE:
        channel->runtime.value.f64 = value;
        break;
    case TELEMETRY_CHANNEL_VALUE_INT32:
        channel->runtime.value.i32 = (int32_t)value;
        break;
    case TELEMETRY_CHANNEL_VALUE_UINT32:
        channel->runtime.value.u32 = value < 0.0 ? 0u : (uint32_t)value;
        break;
    case TELEMETRY_CHANNEL_VALUE_BOOLEAN:
        channel->runtime.value.boolean = value != 0.0;
        break;
    case TELEMETRY_CHANNEL_VALUE_FLOAT:
    default:
        channel->runtime.value.f32 = (float)value;
        break;
    }
}

static float driver_catalog_runtime_value_as_float(const telemetry_channel_t *channel)
{
    if (channel == NULL)
    {
        return 0.0f;
    }

    switch (channel->metadata.value_type)
    {
    case TELEMETRY_CHANNEL_VALUE_DOUBLE:
        return (float)channel->runtime.value.f64;
    case TELEMETRY_CHANNEL_VALUE_INT32:
        return (float)channel->runtime.value.i32;
    case TELEMETRY_CHANNEL_VALUE_UINT32:
        return (float)channel->runtime.value.u32;
    case TELEMETRY_CHANNEL_VALUE_BOOLEAN:
        return channel->runtime.value.boolean ? 1.0f : 0.0f;
    case TELEMETRY_CHANNEL_VALUE_FLOAT:
    default:
        return channel->runtime.value.f32;
    }
}

static void driver_catalog_finalize_sample(telemetry_channel_t *channel, uint32_t now_ms)
{
    if (channel == NULL)
    {
        return;
    }

    channel->runtime.source_timestamp_epoch_s = get_system_time();
    channel->runtime.sample_count++;
    channel->runtime.quality = SERVICE_QUALITY_GOOD;
    channel->runtime.last_update_ms = now_ms;

    if ((channel->metadata.flags & TELEMETRY_CHANNEL_FLAG_HISTORIZED) != 0u)
    {
        history_service_record_channel_sample(channel->metadata.id,
                                              driver_catalog_runtime_value_as_float(channel),
                                              channel->runtime.quality,
                                              channel->runtime.source_timestamp_epoch_s,
                                              channel->settings.history_interval_ms);
    }
}

static bool driver_catalog_validate_internal_temp(uint8_t source_index, uint8_t source_flags)
{
    return source_index == 0u && source_flags == 0u;
}

static bool driver_catalog_validate_adc(uint8_t source_index, uint8_t source_flags)
{
    return source_index <= 3u && source_flags == 0u;
}

static bool driver_catalog_validate_gpio(uint8_t source_index, uint8_t source_flags)
{
    const uint8_t allowed_flags = CONFIG_CHANNEL_SOURCE_FLAG_INVERT |
                                  CONFIG_CHANNEL_SOURCE_FLAG_PULLUP |
                                  CONFIG_CHANNEL_SOURCE_FLAG_PULLDOWN;

    if (source_index >= NUM_BANK0_GPIOS)
    {
        return false;
    }

    if ((source_flags & (uint8_t)(~allowed_flags)) != 0u)
    {
        return false;
    }

    return (source_flags & CONFIG_CHANNEL_SOURCE_FLAG_PULLUP) == 0u ||
           (source_flags & CONFIG_CHANNEL_SOURCE_FLAG_PULLDOWN) == 0u;
}

static bool driver_catalog_init_internal_temp(telemetry_channel_t *channel,
                                              uint8_t source_index,
                                              uint8_t source_flags)
{
    (void)channel;
    return driver_catalog_validate_internal_temp(source_index, source_flags) && temp_sensor_driver_init();
}

static bool driver_catalog_init_adc(telemetry_channel_t *channel,
                                    uint8_t source_index,
                                    uint8_t source_flags)
{
    (void)channel;

    if (!driver_catalog_validate_adc(source_index, source_flags))
    {
        return false;
    }

    if (!g_adc_initialized)
    {
        adc_init();
        g_adc_initialized = true;
    }

    adc_gpio_init((uint)(ADC_BASE_PIN + source_index));
    return true;
}

static bool driver_catalog_init_gpio(telemetry_channel_t *channel,
                                     uint8_t source_index,
                                     uint8_t source_flags)
{
    (void)channel;

    if (!driver_catalog_validate_gpio(source_index, source_flags))
    {
        return false;
    }

    gpio_init(source_index);
    gpio_set_dir(source_index, GPIO_IN);
    if ((source_flags & CONFIG_CHANNEL_SOURCE_FLAG_PULLUP) != 0u)
    {
        gpio_pull_up(source_index);
    }
    else if ((source_flags & CONFIG_CHANNEL_SOURCE_FLAG_PULLDOWN) != 0u)
    {
        gpio_pull_down(source_index);
    }
    else
    {
        gpio_disable_pulls(source_index);
    }

    return true;
}

static bool driver_catalog_poll_internal_temp(telemetry_channel_t *channel,
                                              uint8_t source_index,
                                              uint8_t source_flags,
                                              uint32_t now_ms)
{
    float raw_temperature_c = 0.0f;

    if (!driver_catalog_validate_internal_temp(source_index, source_flags) ||
        !temp_sensor_driver_read_celsius(&raw_temperature_c))
    {
        if (channel != NULL)
        {
            channel->runtime.error_count++;
            channel->runtime.quality = SERVICE_QUALITY_BAD;
        }
        return false;
    }

    driver_catalog_store_numeric_value(channel,
                                       (raw_temperature_c * channel->settings.scale) + channel->settings.offset);
    driver_catalog_finalize_sample(channel, now_ms);
    return true;
}

static bool driver_catalog_poll_adc(telemetry_channel_t *channel,
                                    uint8_t source_index,
                                    uint8_t source_flags,
                                    uint32_t now_ms)
{
    const double conversion_factor = 3.3 / 4095.0;
    uint16_t raw = 0u;

    if (!driver_catalog_validate_adc(source_index, source_flags))
    {
        if (channel != NULL)
        {
            channel->runtime.error_count++;
            channel->runtime.quality = SERVICE_QUALITY_BAD;
        }
        return false;
    }

    adc_select_input(source_index);
    raw = adc_read();
    driver_catalog_store_numeric_value(channel,
                                       (((double)raw * conversion_factor) * channel->settings.scale) +
                                           channel->settings.offset);
    driver_catalog_finalize_sample(channel, now_ms);
    return true;
}

static bool driver_catalog_poll_gpio(telemetry_channel_t *channel,
                                     uint8_t source_index,
                                     uint8_t source_flags,
                                     uint32_t now_ms)
{
    bool value = false;

    if (!driver_catalog_validate_gpio(source_index, source_flags))
    {
        if (channel != NULL)
        {
            channel->runtime.error_count++;
            channel->runtime.quality = SERVICE_QUALITY_BAD;
        }
        return false;
    }

    value = gpio_get(source_index) != 0;
    if ((source_flags & CONFIG_CHANNEL_SOURCE_FLAG_INVERT) != 0u)
    {
        value = !value;
    }

    driver_catalog_store_numeric_value(channel, value ? 1.0 : 0.0);
    driver_catalog_finalize_sample(channel, now_ms);
    return true;
}

static const driver_catalog_entry_t g_driver_catalog[] = {
    {
        .driver_id = CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP,
        .name = "internal_temp",
        .default_unit = "degC",
        .default_kind = TELEMETRY_CHANNEL_KIND_ANALOG,
        .default_value_type = TELEMETRY_CHANNEL_VALUE_FLOAT,
        .default_flags = TELEMETRY_CHANNEL_FLAG_ENABLED |
                         TELEMETRY_CHANNEL_FLAG_HISTORIZED |
                         TELEMETRY_CHANNEL_FLAG_ALARM_ENABLED,
        .analog = true,
        .validate_source = driver_catalog_validate_internal_temp,
        .init = driver_catalog_init_internal_temp,
        .poll = driver_catalog_poll_internal_temp,
    },
    {
        .driver_id = CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE,
        .name = "adc_voltage",
        .default_unit = "V",
        .default_kind = TELEMETRY_CHANNEL_KIND_ANALOG,
        .default_value_type = TELEMETRY_CHANNEL_VALUE_FLOAT,
        .default_flags = TELEMETRY_CHANNEL_FLAG_ENABLED |
                         TELEMETRY_CHANNEL_FLAG_HISTORIZED,
        .analog = true,
        .validate_source = driver_catalog_validate_adc,
        .init = driver_catalog_init_adc,
        .poll = driver_catalog_poll_adc,
    },
    {
        .driver_id = CONFIG_CHANNEL_DRIVER_GPIO_INPUT,
        .name = "gpio_input",
        .default_unit = "",
        .default_kind = TELEMETRY_CHANNEL_KIND_DISCRETE,
        .default_value_type = TELEMETRY_CHANNEL_VALUE_BOOLEAN,
        .default_flags = TELEMETRY_CHANNEL_FLAG_ENABLED,
        .analog = false,
        .validate_source = driver_catalog_validate_gpio,
        .init = driver_catalog_init_gpio,
        .poll = driver_catalog_poll_gpio,
    },
};

const driver_catalog_entry_t *driver_catalog_get_by_id(uint8_t driver_id)
{
    for (size_t index = 0u; index < (sizeof(g_driver_catalog) / sizeof(g_driver_catalog[0])); ++index)
    {
        if (g_driver_catalog[index].driver_id == driver_id)
        {
            return &g_driver_catalog[index];
        }
    }

    return NULL;
}

const driver_catalog_entry_t *driver_catalog_get_by_name(const char *name)
{
    if (name == NULL)
    {
        return NULL;
    }

    for (size_t index = 0u; index < (sizeof(g_driver_catalog) / sizeof(g_driver_catalog[0])); ++index)
    {
        if (strcmp(g_driver_catalog[index].name, name) == 0)
        {
            return &g_driver_catalog[index];
        }
    }

    return NULL;
}

size_t driver_catalog_count(void)
{
    return sizeof(g_driver_catalog) / sizeof(g_driver_catalog[0]);
}

const driver_catalog_entry_t *driver_catalog_get_by_index(size_t index)
{
    return index < driver_catalog_count() ? &g_driver_catalog[index] : NULL;
}
