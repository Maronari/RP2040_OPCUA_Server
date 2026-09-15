/**
 * @file
 * @brief Native channel driver for discrete/boolean GPIO output (writable)
 *        channels.
 *
 * Responsibilities: implement the start/stop/write/configure/self_test
 * slots of telemetry_driver_vtbl_t for GPIO-output channels (see
 * drivers/include/drivers/native_channel_driver.h for the shared vtable
 * contract). This is the first native driver to implement write/configure/
 * self_test -- see docs/releases/v0.5.1.md T1.
 *
 * Ownership: no persistent state beyond the pin itself -- each operation
 * reconfigures/drives the pin identified in the channel's driver_context.
 *
 * Concurrency: none; per the header's file-level doc, all calls arrive
 * serialized from SensorTask.
 */
#include "drivers/native_channel_driver.h"

#include "hardware/gpio.h"

#define NATIVE_GPIO_OUTPUT_SOURCE_INVERT (1u << 0u)
#define NATIVE_GPIO_OUTPUT_SOURCE_OPEN_DRAIN (1u << 1u)

/** @brief Reinterpret channel->driver_context as this driver's source selector, or NULL. */
static const native_channel_source_t *gpio_source(const telemetry_channel_t *channel)
{
    return channel != NULL ? (const native_channel_source_t *)channel->driver_context : NULL;
}

bool native_gpio_output_source_is_valid(uint8_t source_index, uint8_t source_flags)
{
    const uint8_t allowed = NATIVE_GPIO_OUTPUT_SOURCE_INVERT | NATIVE_GPIO_OUTPUT_SOURCE_OPEN_DRAIN;

    return source_index < NUM_BANK0_GPIOS && (source_flags & (uint8_t)~allowed) == 0u;
}

telemetry_driver_result_t native_gpio_output_write(telemetry_channel_t *channel,
                                                    const telemetry_channel_value_t *value)
{
    const native_channel_source_t *source = gpio_source(channel);
    bool asserted;

    if (source == NULL || value == NULL ||
        !native_gpio_output_source_is_valid(source->source_index, source->source_flags) ||
        channel->metadata.value_type != TELEMETRY_CHANNEL_VALUE_BOOLEAN)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    asserted = value->boolean;
    if ((source->source_flags & NATIVE_GPIO_OUTPUT_SOURCE_INVERT) != 0u)
    {
        asserted = !asserted;
    }
    if ((source->source_flags & NATIVE_GPIO_OUTPUT_SOURCE_OPEN_DRAIN) != 0u)
    {
        if (asserted)
        {
            /* Set the driven level before enabling the output so the pad
             * never briefly reflects a stale GPIO_OUT bit. */
            gpio_put(source->source_index, 0u);
            gpio_set_dir(source->source_index, GPIO_OUT);
        }
        else
        {
            gpio_set_dir(source->source_index, GPIO_IN);
        }
    }
    else
    {
        gpio_put(source->source_index, asserted);
    }
    channel->runtime.value.boolean = value->boolean;
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_gpio_output_start(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = gpio_source(channel);
    const telemetry_channel_value_t released = {.boolean = false};

    if (source == NULL || !native_gpio_output_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    gpio_init(source->source_index);
    if ((source->source_flags & NATIVE_GPIO_OUTPUT_SOURCE_OPEN_DRAIN) != 0u)
    {
        /* Weak pull-up so the released (high-impedance) state reads a sane
         * logic-high even with no external pull-up wired. */
        gpio_pull_up(source->source_index);
    }
    else
    {
        gpio_disable_pulls(source->source_index);
        gpio_set_dir(source->source_index, GPIO_OUT);
    }
    return native_gpio_output_write(channel, &released);
}

telemetry_driver_result_t native_gpio_output_stop(telemetry_channel_t *channel)
{
    (void)channel;
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_gpio_output_configure(telemetry_channel_t *channel, const void *config)
{
    const native_channel_source_t *source = gpio_source(channel);
    const native_gpio_output_config_t *output_config = (const native_gpio_output_config_t *)config;
    telemetry_channel_value_t value;

    if (source == NULL || !native_gpio_output_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    /* sensor_manager.c's real call sites (sensor_manager_start_candidates(),
     * sensor_manager_restart_active()) always invoke configure() with
     * config == NULL; that is the normal case, not an error -- apply the
     * driver's own released/safe default. A non-NULL payload (no current
     * caller supplies one) overrides that default. */
    value.boolean = (output_config != NULL) ? output_config->safe_level : false;
    return native_gpio_output_write(channel, &value);
}

telemetry_driver_result_t native_gpio_output_self_test(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = gpio_source(channel);
    bool asserted;

    if (source == NULL || !native_gpio_output_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    asserted = channel->runtime.value.boolean;
    if ((source->source_flags & NATIVE_GPIO_OUTPUT_SOURCE_INVERT) != 0u)
    {
        asserted = !asserted;
    }
    if ((source->source_flags & NATIVE_GPIO_OUTPUT_SOURCE_OPEN_DRAIN) != 0u)
    {
        return gpio_get_dir(source->source_index) == (asserted ? GPIO_OUT : GPIO_IN)
                   ? TELEMETRY_DRIVER_RESULT_OK
                   : TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
    }
    if (gpio_get_dir(source->source_index) != GPIO_OUT)
    {
        return TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
    }
    return gpio_get(source->source_index) == asserted ? TELEMETRY_DRIVER_RESULT_OK
                                                       : TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
}
