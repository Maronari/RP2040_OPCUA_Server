#include "drivers/native_channel_driver.h"

#include "hardware/gpio.h"

#define NATIVE_GPIO_SOURCE_INVERT (1u << 0u)
#define NATIVE_GPIO_SOURCE_PULLUP (1u << 1u)
#define NATIVE_GPIO_SOURCE_PULLDOWN (1u << 2u)

static const native_channel_source_t *gpio_source(const telemetry_channel_t *channel)
{
    return channel != NULL ? (const native_channel_source_t *)channel->driver_context : NULL;
}

bool native_gpio_input_source_is_valid(uint8_t source_index, uint8_t source_flags)
{
    const uint8_t allowed = NATIVE_GPIO_SOURCE_INVERT | NATIVE_GPIO_SOURCE_PULLUP | NATIVE_GPIO_SOURCE_PULLDOWN;

    return source_index < NUM_BANK0_GPIOS && (source_flags & (uint8_t)~allowed) == 0u &&
           ((source_flags & NATIVE_GPIO_SOURCE_PULLUP) == 0u ||
            (source_flags & NATIVE_GPIO_SOURCE_PULLDOWN) == 0u);
}

telemetry_driver_result_t native_gpio_input_start(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = gpio_source(channel);
    if (source == NULL || !native_gpio_input_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    gpio_init(source->source_index);
    gpio_set_dir(source->source_index, GPIO_IN);
    if ((source->source_flags & NATIVE_GPIO_SOURCE_PULLUP) != 0u)
    {
        gpio_pull_up(source->source_index);
    }
    else if ((source->source_flags & NATIVE_GPIO_SOURCE_PULLDOWN) != 0u)
    {
        gpio_pull_down(source->source_index);
    }
    else
    {
        gpio_disable_pulls(source->source_index);
    }
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_gpio_input_stop(telemetry_channel_t *channel)
{
    (void)channel;
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_gpio_input_read(telemetry_channel_t *channel, uint32_t now_ms)
{
    const native_channel_source_t *source = gpio_source(channel);
    bool value;

    (void)now_ms;
    if (source == NULL || !native_gpio_input_source_is_valid(source->source_index, source->source_flags) ||
        channel->metadata.value_type != TELEMETRY_CHANNEL_VALUE_BOOLEAN)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    value = gpio_get(source->source_index) != 0;
    if ((source->source_flags & NATIVE_GPIO_SOURCE_INVERT) != 0u)
    {
        value = !value;
    }
    channel->runtime.value.boolean = value;
    return TELEMETRY_DRIVER_RESULT_OK;
}
