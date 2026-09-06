/**
 * @file
 * @brief Native channel driver for the `IPS2050HTR` protected high-side
 *        digital-output frontend.
 *
 * See drivers/include/drivers/ips2050_driver.h for the full contract and
 * HARDWARE_MVP.md section 3.4.4 for the GPIO pin mapping and hardware
 * safe-state interlock this implementation follows.
 *
 * Ownership: g_enable_asserted is a single file-static flag shared by both
 * IPS2050 channel instances -- it tracks the physical DO_ENABLE_REQ line (a
 * singleton resource), not any one channel; mirrors
 * adc_voltage_channel_driver.c's g_adc_ready convention.
 */
#include "drivers/ips2050_driver.h"

#include "hardware/gpio.h"

#define IPS2050_ENABLE_GPIO 10u
#define IPS2050_MAX_CHANNEL_INDEX 1u

static const uint8_t g_cmd_gpio[2] = {20u, 21u};
static const uint8_t g_fault_gpio[2] = {22u, 23u};

static bool g_enable_asserted;

/** @brief Reinterpret channel->driver_context as this driver's source selector, or NULL. */
static const native_channel_source_t *ips2050_source(const telemetry_channel_t *channel)
{
    return channel != NULL ? (const native_channel_source_t *)channel->driver_context : NULL;
}

bool native_ips2050_source_is_valid(uint8_t source_index, uint8_t source_flags)
{
    return source_index <= IPS2050_MAX_CHANNEL_INDEX && source_flags == 0u;
}

telemetry_driver_result_t native_ips2050_start(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = ips2050_source(channel);

    if (source == NULL || !native_ips2050_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    gpio_init(g_cmd_gpio[source->source_index]);
    gpio_disable_pulls(g_cmd_gpio[source->source_index]);
    gpio_put(g_cmd_gpio[source->source_index], false);
    gpio_set_dir(g_cmd_gpio[source->source_index], GPIO_OUT);
    gpio_init(g_fault_gpio[source->source_index]);
    gpio_set_dir(g_fault_gpio[source->source_index], GPIO_IN);
    gpio_disable_pulls(g_fault_gpio[source->source_index]);
    if (!g_enable_asserted)
    {
        gpio_init(IPS2050_ENABLE_GPIO);
        gpio_disable_pulls(IPS2050_ENABLE_GPIO);
        gpio_put(IPS2050_ENABLE_GPIO, true);
        gpio_set_dir(IPS2050_ENABLE_GPIO, GPIO_OUT);
        g_enable_asserted = true;
    }
    channel->runtime.value.boolean = false;
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_ips2050_stop(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = ips2050_source(channel);

    if (source == NULL || !native_ips2050_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    gpio_put(g_cmd_gpio[source->source_index], false);
    channel->runtime.value.boolean = false;
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_ips2050_write(telemetry_channel_t *channel, const telemetry_channel_value_t *value)
{
    const native_channel_source_t *source = ips2050_source(channel);

    if (source == NULL || value == NULL ||
        !native_ips2050_source_is_valid(source->source_index, source->source_flags) ||
        channel->metadata.value_type != TELEMETRY_CHANNEL_VALUE_BOOLEAN)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    gpio_put(g_cmd_gpio[source->source_index], value->boolean);
    channel->runtime.value.boolean = value->boolean;
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_ips2050_self_test(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = ips2050_source(channel);
    bool fault_asserted;

    if (source == NULL || !native_ips2050_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    fault_asserted = !gpio_get(g_fault_gpio[source->source_index]);
    return fault_asserted ? TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE : TELEMETRY_DRIVER_RESULT_OK;
}

bool native_ips2050_read_fault(uint8_t source_index, bool *out_fault_active)
{
    if (source_index > IPS2050_MAX_CHANNEL_INDEX || out_fault_active == NULL)
    {
        return false;
    }
    *out_fault_active = !gpio_get(g_fault_gpio[source_index]);
    return true;
}
