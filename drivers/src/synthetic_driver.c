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
    source->scratch.synthetic_read_count = 0u;
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
    value = ++source->scratch.synthetic_read_count;
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

telemetry_driver_result_t native_synthetic_write(telemetry_channel_t *channel,
                                                  const telemetry_channel_value_t *value)
{
    native_channel_source_t *source = synthetic_source(channel);
    double requested;
    uint32_t counter;

    if (source == NULL || value == NULL ||
        !native_synthetic_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (channel->metadata.value_type == TELEMETRY_CHANNEL_VALUE_FLOAT)
    {
        requested = (double)value->f32;
    }
    else if (channel->metadata.value_type == TELEMETRY_CHANNEL_VALUE_DOUBLE)
    {
        requested = value->f64;
    }
    else
    {
        return TELEMETRY_DRIVER_RESULT_UNSUPPORTED;
    }
    /* These comparisons also reject NaN. Check the range before conversion
     * so the double-to-uint32 cast is always defined. */
    if (!(requested >= 0.0 && requested <= (double)UINT32_MAX))
    {
        return TELEMETRY_DRIVER_RESULT_OUT_OF_RANGE;
    }
    counter = (uint32_t)requested;
    if ((double)counter != requested)
    {
        return TELEMETRY_DRIVER_RESULT_OUT_OF_RANGE;
    }

    source->scratch.synthetic_read_count = counter;
    if (channel->metadata.value_type == TELEMETRY_CHANNEL_VALUE_FLOAT)
    {
        channel->runtime.value.f32 = value->f32;
    }
    else
    {
        channel->runtime.value.f64 = value->f64;
    }
    return TELEMETRY_DRIVER_RESULT_OK;
}
