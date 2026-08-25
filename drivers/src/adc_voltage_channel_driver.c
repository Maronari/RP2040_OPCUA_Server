/**
 * @file
 * @brief Native channel driver for external ADC-voltage inputs 0-3.
 *
 * Responsibilities: implement the start/stop/read slots of
 * telemetry_driver_vtbl_t for ADC-voltage channels (see
 * drivers/include/drivers/native_channel_driver.h for the shared vtable
 * contract).
 *
 * Ownership: g_adc_ready is a single file-static flag shared by every
 * ADC-voltage channel instance -- it tracks the physical ADC peripheral
 * (a singleton resource), not any one channel.
 *
 * Concurrency: none; per the header's file-level doc, all calls arrive
 * serialized from SensorTask.
 */
#include "drivers/native_channel_driver.h"

#include "hardware/adc.h"

/** True once any ADC-voltage channel has called adc_init(); shared across all instances. */
static bool g_adc_ready;

bool native_adc_voltage_source_is_valid(uint8_t source_index, uint8_t source_flags)
{
    return source_index <= 3u && source_flags == 0u;
}

/** @brief Reinterpret channel->driver_context as this driver's source selector, or NULL. */
static const native_channel_source_t *adc_source(const telemetry_channel_t *channel)
{
    return channel != NULL ? (const native_channel_source_t *)channel->driver_context : NULL;
}

telemetry_driver_result_t native_adc_voltage_start(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = adc_source(channel);

    if (source == NULL || !native_adc_voltage_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (!g_adc_ready)
    {
        adc_init();
        g_adc_ready = true;
    }
    adc_gpio_init((uint)(ADC_BASE_PIN + source->source_index));
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_adc_voltage_stop(telemetry_channel_t *channel)
{
    (void)channel;
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_adc_voltage_read(telemetry_channel_t *channel, uint32_t now_ms)
{
    const native_channel_source_t *source = adc_source(channel);
    double value;

    (void)now_ms;
    if (source == NULL || !native_adc_voltage_source_is_valid(source->source_index, source->source_flags) || !g_adc_ready)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    adc_select_input(source->source_index);
    value = (((double)adc_read() * (3.3 / 4095.0)) * channel->settings.scale) + channel->settings.offset;
    if (channel->metadata.value_type == TELEMETRY_CHANNEL_VALUE_FLOAT)
    {
        channel->runtime.value.f32 = (float)value;
    }
    else if (channel->metadata.value_type == TELEMETRY_CHANNEL_VALUE_DOUBLE)
    {
        channel->runtime.value.f64 = value;
    }
    else
    {
        return TELEMETRY_DRIVER_RESULT_UNSUPPORTED;
    }
    return TELEMETRY_DRIVER_RESULT_OK;
}
