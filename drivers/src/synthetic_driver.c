/**
 * @file
 * @brief Synthetic (no external hardware) benchmark/loopback driver
 *        implementation. See drivers/include/drivers/synthetic_driver.h
 *        for the full contract -- read that file first if you are using
 *        this driver as a template for a new one.
 *
 * Ownership: g_counters is process-wide file-static state, one entry per
 * source_index (0-7), mirroring ad5422_driver.c's g_last_code[] --
 * driver_context cannot carry extra mutable fields (a fixed,
 * driver-agnostic struct shared by every native driver).
 */
#include "drivers/synthetic_driver.h"

#define SYNTHETIC_MAX_CHANNEL_INDEX 7u

static uint32_t g_counters[SYNTHETIC_MAX_CHANNEL_INDEX + 1u];

/** @brief Reinterpret channel->driver_context as this driver's source selector, or NULL. */
static const native_channel_source_t *synthetic_source(const telemetry_channel_t *channel)
{
    return channel != NULL ? (const native_channel_source_t *)channel->driver_context : NULL;
}

bool native_synthetic_source_is_valid(uint8_t source_index, uint8_t source_flags)
{
    return source_index <= SYNTHETIC_MAX_CHANNEL_INDEX && source_flags == 0u;
}

telemetry_driver_result_t native_synthetic_start(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = synthetic_source(channel);

    if (source == NULL || !native_synthetic_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    g_counters[source->source_index] = 0u;
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_synthetic_stop(telemetry_channel_t *channel)
{
    (void)channel;
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_synthetic_read(telemetry_channel_t *channel, uint32_t now_ms)
{
    const native_channel_source_t *source = synthetic_source(channel);
    uint32_t value;

    (void)now_ms;
    if (source == NULL || !native_synthetic_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    value = ++g_counters[source->source_index];
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
