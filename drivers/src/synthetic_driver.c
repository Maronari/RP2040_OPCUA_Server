/**
 * @file
 * @brief Synthetic (no external hardware) benchmark/loopback driver
 *        implementation. See drivers/include/drivers/synthetic_driver.h
 *        for the full contract -- read that file first if you are using
 *        this driver as a template for a new one.
 *
 * Ownership: each service-owned native_channel_source_t holds its own
 * counter. Same-index channels have separate contexts, including the two
 * transaction banks. This driver reserves no file-static RAM.
 */
#include "drivers/synthetic_driver.h"

#define SYNTHETIC_MAX_CHANNEL_INDEX 7u

/** @brief Reinterpret channel->driver_context as this driver's source selector, or NULL. */
static native_channel_source_t *synthetic_source(const telemetry_channel_t *channel)
{
    return channel != NULL ? (native_channel_source_t *)channel->driver_context : NULL;
}

bool native_synthetic_source_is_valid(uint8_t source_index, uint8_t source_flags)
{
    return source_index <= SYNTHETIC_MAX_CHANNEL_INDEX && source_flags == 0u;
}

telemetry_driver_result_t native_synthetic_start(telemetry_channel_t *channel)
{
    native_channel_source_t *source = synthetic_source(channel);

    if (source == NULL || !native_synthetic_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    source->synthetic_read_count = 0u;
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_synthetic_stop(telemetry_channel_t *channel)
{
    (void)channel;
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_synthetic_read(telemetry_channel_t *channel, uint32_t now_ms)
{
    native_channel_source_t *source = synthetic_source(channel);
    uint32_t value;

    (void)now_ms;
    if (source == NULL || !native_synthetic_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (channel->metadata.value_type != TELEMETRY_CHANNEL_VALUE_FLOAT &&
        channel->metadata.value_type != TELEMETRY_CHANNEL_VALUE_DOUBLE)
    {
        return TELEMETRY_DRIVER_RESULT_UNSUPPORTED;
    }
    value = ++source->synthetic_read_count;
    if (channel->metadata.value_type == TELEMETRY_CHANNEL_VALUE_FLOAT)
    {
        channel->runtime.value.f32 = (float)value;
    }
    else if (channel->metadata.value_type == TELEMETRY_CHANNEL_VALUE_DOUBLE)
    {
        channel->runtime.value.f64 = (double)value;
    }
    else
    {
        return TELEMETRY_DRIVER_RESULT_UNSUPPORTED;
    }
    return TELEMETRY_DRIVER_RESULT_OK;
}
