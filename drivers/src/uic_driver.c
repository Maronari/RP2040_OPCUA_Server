/**
 * @file
 * @brief Native channel driver for the `UIC0..UIC3` universal protected
 *        input channels.
 *
 * See drivers/include/drivers/uic_driver.h for the full contract,
 * including the deliberate quadrature-encoder scope decision.
 *
 * Ownership: g_counter is a single file-static binding shared by every UIC
 * channel instance (mirrors ads8688_driver.c's g_bus -- a genuine
 * board-wide singleton, not per-channel, so it stays file-static).
 * Per-channel frequency/period derived state lives in each channel's own
 * service-owned native_channel_source_t::scratch.uic_derived (v0.5.1
 * T19+ static-allocation audit correction), not a driver-private array --
 * a compiled-but-unconfigured UIC driver therefore holds none of it.
 */
#include "drivers/uic_driver.h"
#include "drivers/industrial_board_pins.h"

#include "hardware/gpio.h"

#include <stddef.h>

#define UIC_BASE_GPIO INDUSTRIAL_UIC_BASE_GPIO
#define UIC_MAX_CHANNEL_INDEX 3u
#define UIC_MODE_MASK 0x07u

typedef enum
{
    UIC_MODE_DIGITAL_INPUT = 0,
    UIC_MODE_PULSE_COUNT = 1,
    UIC_MODE_FREQUENCY = 2,
    UIC_MODE_PERIOD = 3
} uic_mode_t;

static pio_edge_counter_t g_counter;
static bool g_counter_bound;

/** @brief Reinterpret channel->driver_context as this driver's source selector, or NULL. */
static native_channel_source_t *uic_source(const telemetry_channel_t *channel)
{
    return channel != NULL ? (native_channel_source_t *)channel->driver_context : NULL;
}

static uint8_t uic_mode(uint8_t source_flags)
{
    return (uint8_t)(source_flags & UIC_MODE_MASK);
}

void native_uic_bind_counter(const pio_edge_counter_t *counter)
{
    if (counter != NULL)
    {
        g_counter = *counter;
        g_counter_bound = true;
    }
    else
    {
        g_counter_bound = false;
    }
}

bool native_uic_source_is_valid(uint8_t source_index, uint8_t source_flags)
{
    return source_index <= UIC_MAX_CHANNEL_INDEX && (source_flags & (uint8_t)~UIC_MODE_MASK) == 0u &&
           uic_mode(source_flags) <= UIC_MODE_PERIOD;
}

telemetry_driver_result_t native_uic_start(telemetry_channel_t *channel)
{
    native_channel_source_t *source = uic_source(channel);
    uint8_t gpio;

    if (source == NULL || !native_uic_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    gpio = (uint8_t)(UIC_BASE_GPIO + source->source_index);
    if (uic_mode(source->source_flags) == UIC_MODE_DIGITAL_INPUT)
    {
        gpio_init(gpio);
        gpio_set_dir(gpio, GPIO_IN);
        gpio_disable_pulls(gpio);
        return TELEMETRY_DRIVER_RESULT_OK;
    }
    if (!g_counter_bound)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    if (g_counter.vtbl->start(g_counter.context, gpio) != TELEMETRY_DRIVER_RESULT_OK)
    {
        return TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
    }
    source->scratch.uic_derived = (uic_derived_state_t){0};
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_uic_stop(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = uic_source(channel);
    uint8_t gpio;

    if (source == NULL || !native_uic_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (uic_mode(source->source_flags) != UIC_MODE_DIGITAL_INPUT && g_counter_bound)
    {
        gpio = (uint8_t)(UIC_BASE_GPIO + source->source_index);
        g_counter.vtbl->stop(g_counter.context, gpio);
    }
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_uic_read(telemetry_channel_t *channel, uint32_t now_ms)
{
    native_channel_source_t *source = uic_source(channel);
    uint8_t gpio;
    uint8_t mode;
    uint32_t current;
    uic_derived_state_t *derived;
    double value;

    if (source == NULL || !native_uic_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    mode = uic_mode(source->source_flags);
    gpio = (uint8_t)(UIC_BASE_GPIO + source->source_index);
    if (mode == UIC_MODE_DIGITAL_INPUT)
    {
        if (channel->metadata.value_type != TELEMETRY_CHANNEL_VALUE_BOOLEAN)
        {
            return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
        }
        channel->runtime.value.boolean = gpio_get(gpio) != 0;
        return TELEMETRY_DRIVER_RESULT_OK;
    }
    if (channel->metadata.value_type != TELEMETRY_CHANNEL_VALUE_DOUBLE)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (!g_counter_bound)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    current = g_counter.vtbl->read_edges(g_counter.context, gpio);
    if (mode == UIC_MODE_PULSE_COUNT)
    {
        channel->runtime.value.f64 = (double)current;
        return TELEMETRY_DRIVER_RESULT_OK;
    }
    derived = &source->scratch.uic_derived;
    if (!derived->have_baseline)
    {
        derived->have_baseline = true;
        derived->baseline_count = current;
        derived->baseline_time_ms = now_ms;
        channel->runtime.value.f64 = 0.0;
        return TELEMETRY_DRIVER_RESULT_OK;
    }
    {
        uint32_t delta_count = current - derived->baseline_count;
        uint32_t delta_time_ms = now_ms - derived->baseline_time_ms;
        double frequency_hz = delta_time_ms > 0u ? ((double)delta_count * 1000.0 / (double)delta_time_ms) : 0.0;

        derived->baseline_count = current;
        derived->baseline_time_ms = now_ms;
        value = (mode == UIC_MODE_FREQUENCY) ? frequency_hz : (frequency_hz > 0.0 ? 1.0 / frequency_hz : 0.0);
    }
    channel->runtime.value.f64 = value;
    return TELEMETRY_DRIVER_RESULT_OK;
}
