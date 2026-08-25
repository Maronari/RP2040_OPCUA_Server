/**
 * @file
 * @brief Native channel driver adapting temp_sensor_driver.c's on-die
 *        temperature sensor to the telemetry_driver_vtbl_t contract.
 *
 * Responsibilities: implement the start/stop/read slots for the
 * internal-temperature channel (see
 * drivers/include/drivers/native_channel_driver.h for the shared vtable
 * contract) by delegating the actual hardware access to
 * drivers/temp_sensor_driver.c.
 *
 * Ownership: no persistent state of its own.
 *
 * Concurrency: none; per the header's file-level doc, all calls arrive
 * serialized from SensorTask.
 */
#include "drivers/native_channel_driver.h"

#include "drivers/temp_sensor_driver.h"

bool native_internal_temperature_source_is_valid(uint8_t source_index, uint8_t source_flags)
{
    return source_index == 0u && source_flags == 0u;
}

telemetry_driver_result_t native_internal_temperature_start(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = channel != NULL ? channel->driver_context : NULL;

    if (source == NULL || !native_internal_temperature_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    return temp_sensor_driver_init() ? TELEMETRY_DRIVER_RESULT_OK : TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
}

telemetry_driver_result_t native_internal_temperature_stop(telemetry_channel_t *channel)
{
    (void)channel;
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_internal_temperature_read(telemetry_channel_t *channel, uint32_t now_ms)
{
    float value;

    (void)now_ms;
    if (channel == NULL)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (!temp_sensor_driver_read_celsius(&value))
    {
        return TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
    }
    value = (value * (float)channel->settings.scale) + (float)channel->settings.offset;
    if (channel->metadata.value_type == TELEMETRY_CHANNEL_VALUE_FLOAT)
    {
        channel->runtime.value.f32 = value;
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
