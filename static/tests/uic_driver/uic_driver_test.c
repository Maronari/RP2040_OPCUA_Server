/**
 * @file
 * @brief Host tests for the real drivers/src/uic_driver.c against a fake
 *        pio_edge_counter_t -- proves mode dispatch (digital input vs the
 *        three counter-based modes), the frequency/period delta-window
 *        math, and that digital-input mode never touches the counter
 *        backing at all, without needing real RP2350 PIO hardware.
 */
#include "drivers/uic_driver.h"

#include "hardware/gpio.h"

#include <assert.h>
#include <math.h>
#include <string.h>

#define UIC_BASE_GPIO 40u
#define UIC_CHANNEL_COUNT 4u

#define UIC_MODE_DIGITAL_INPUT 0u
#define UIC_MODE_PULSE_COUNT 1u
#define UIC_MODE_FREQUENCY 2u
#define UIC_MODE_PERIOD 3u

typedef struct
{
    bool started;
    uint32_t edges;
} fake_counter_channel_t;

static fake_counter_channel_t g_fake_counters[UIC_CHANNEL_COUNT];
static bool g_force_start_failure;

static void fake_counter_reset(void)
{
    memset(g_fake_counters, 0, sizeof(g_fake_counters));
    g_force_start_failure = false;
}

static telemetry_driver_result_t fake_start(void *context, uint8_t gpio)
{
    (void)context;
    if (g_force_start_failure)
    {
        return TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
    }
    g_fake_counters[gpio - UIC_BASE_GPIO].started = true;
    g_fake_counters[gpio - UIC_BASE_GPIO].edges = 0u;
    return TELEMETRY_DRIVER_RESULT_OK;
}

static void fake_stop(void *context, uint8_t gpio)
{
    (void)context;
    g_fake_counters[gpio - UIC_BASE_GPIO].started = false;
}

static uint32_t fake_read_edges(void *context, uint8_t gpio)
{
    (void)context;
    return g_fake_counters[gpio - UIC_BASE_GPIO].edges;
}

static const pio_edge_counter_vtbl_t g_fake_vtbl = {
    .start = fake_start,
    .stop = fake_stop,
    .read_edges = fake_read_edges,
};

static telemetry_channel_t make_channel(native_channel_source_t *source, telemetry_channel_value_type_t value_type)
{
    telemetry_channel_t channel = {0};
    channel.driver_context = source;
    channel.metadata.value_type = value_type;
    return channel;
}

static void assert_close(double a, double b)
{
    assert(fabs(a - b) < 0.001);
}

static void test_source_validation(void)
{
    assert(native_uic_source_is_valid(0u, 0x00u));
    assert(native_uic_source_is_valid(3u, 0x03u));
    assert(!native_uic_source_is_valid(4u, 0x00u));
    assert(!native_uic_source_is_valid(0u, 0x04u)); /* mode 4 (quadrature) is not defined this release */
    assert(!native_uic_source_is_valid(0u, 0x08u)); /* bits above the mode field must be 0 */
}

static void test_digital_input_mode_never_touches_the_counter(void)
{
    native_channel_source_t source = {.source_index = 2u, .source_flags = UIC_MODE_DIGITAL_INPUT};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_BOOLEAN);

    gpio_stub_reset();
    fake_counter_reset();
    native_uic_bind_counter(NULL);
    assert(native_uic_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_gpio_stub_pulls_disabled[UIC_BASE_GPIO + 2u]);

    /* Active-low frontend: a HIGH pin is an idle field input. */
    g_gpio_stub_external_level[UIC_BASE_GPIO + 2u] = true;
    assert(native_uic_read(&channel, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(channel.runtime.value.boolean == false);

    g_gpio_stub_external_level[UIC_BASE_GPIO + 2u] = false;
    assert(native_uic_read(&channel, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(channel.runtime.value.boolean == true);
    assert(native_uic_stop(&channel) == TELEMETRY_DRIVER_RESULT_OK);
}

static void test_pulse_count_mode(void)
{
    native_channel_source_t source = {.source_index = 0u, .source_flags = UIC_MODE_PULSE_COUNT};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_DOUBLE);
    pio_edge_counter_t counter = {.vtbl = &g_fake_vtbl, .context = NULL};

    gpio_stub_reset();
    fake_counter_reset();
    native_uic_bind_counter(&counter);
    assert(native_uic_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_fake_counters[0].started);

    g_fake_counters[0].edges = 42u;
    assert(native_uic_read(&channel, 1000u) == TELEMETRY_DRIVER_RESULT_OK);
    assert_close(channel.runtime.value.f64, 42.0);

    assert(native_uic_stop(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert(!g_fake_counters[0].started);
}

static void test_frequency_and_period_modes(void)
{
    native_channel_source_t freq_source = {.source_index = 1u, .source_flags = UIC_MODE_FREQUENCY};
    native_channel_source_t period_source = {.source_index = 2u, .source_flags = UIC_MODE_PERIOD};
    telemetry_channel_t freq_channel = make_channel(&freq_source, TELEMETRY_CHANNEL_VALUE_DOUBLE);
    telemetry_channel_t period_channel = make_channel(&period_source, TELEMETRY_CHANNEL_VALUE_DOUBLE);
    pio_edge_counter_t counter = {.vtbl = &g_fake_vtbl, .context = NULL};

    gpio_stub_reset();
    fake_counter_reset();
    native_uic_bind_counter(&counter);
    assert(native_uic_start(&freq_channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_uic_start(&period_channel) == TELEMETRY_DRIVER_RESULT_OK);

    /* First read after start() only captures a baseline -- no prior sample
     * exists to diff against yet. */
    assert(native_uic_read(&freq_channel, 1000u) == TELEMETRY_DRIVER_RESULT_OK);
    assert_close(freq_channel.runtime.value.f64, 0.0);
    assert(native_uic_read(&period_channel, 1000u) == TELEMETRY_DRIVER_RESULT_OK);
    assert_close(period_channel.runtime.value.f64, 0.0);

    /* 100 edges over the next 1000 ms -> 100 Hz / 10 ms period. */
    g_fake_counters[1].edges = 100u;
    g_fake_counters[2].edges = 100u;
    assert(native_uic_read(&freq_channel, 2000u) == TELEMETRY_DRIVER_RESULT_OK);
    assert_close(freq_channel.runtime.value.f64, 100.0);
    assert(native_uic_read(&period_channel, 2000u) == TELEMETRY_DRIVER_RESULT_OK);
    assert_close(period_channel.runtime.value.f64, 0.01);

    /* No new edges since the last read -> 0 Hz, and period reports 0.0
     * rather than dividing by zero. */
    assert(native_uic_read(&freq_channel, 3000u) == TELEMETRY_DRIVER_RESULT_OK);
    assert_close(freq_channel.runtime.value.f64, 0.0);
    assert(native_uic_read(&period_channel, 3000u) == TELEMETRY_DRIVER_RESULT_OK);
    assert_close(period_channel.runtime.value.f64, 0.0);
}

static void test_no_counter_bound(void)
{
    native_channel_source_t source = {.source_index = 3u, .source_flags = UIC_MODE_PULSE_COUNT};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_DOUBLE);

    gpio_stub_reset();
    fake_counter_reset();
    native_uic_bind_counter(NULL);
    assert(native_uic_start(&channel) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
    assert(native_uic_read(&channel, 0u) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
}

static void test_start_hardware_failure(void)
{
    native_channel_source_t source = {.source_index = 0u, .source_flags = UIC_MODE_PULSE_COUNT};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_DOUBLE);
    pio_edge_counter_t counter = {.vtbl = &g_fake_vtbl, .context = NULL};

    gpio_stub_reset();
    fake_counter_reset();
    native_uic_bind_counter(&counter);
    g_force_start_failure = true;
    assert(native_uic_start(&channel) == TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE);
}

static void test_value_type_mismatch_rejected(void)
{
    native_channel_source_t digital_source = {.source_index = 0u, .source_flags = UIC_MODE_DIGITAL_INPUT};
    native_channel_source_t counter_source = {.source_index = 1u, .source_flags = UIC_MODE_PULSE_COUNT};
    telemetry_channel_t wrong_digital = make_channel(&digital_source, TELEMETRY_CHANNEL_VALUE_DOUBLE);
    telemetry_channel_t wrong_counter = make_channel(&counter_source, TELEMETRY_CHANNEL_VALUE_BOOLEAN);
    pio_edge_counter_t counter = {.vtbl = &g_fake_vtbl, .context = NULL};

    gpio_stub_reset();
    fake_counter_reset();
    native_uic_bind_counter(&counter);
    assert(native_uic_start(&wrong_digital) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_uic_read(&wrong_digital, 0u) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
    assert(native_uic_start(&wrong_counter) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_uic_read(&wrong_counter, 0u) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
}

/*
 * Regression: HARDWARE_MVP.md section 3.5.1 freezes this board's UIC
 * frontend as inverting -- "field HIGH -> UICx_IN = LOW" -- so a channel
 * must report the field state, not the pin state. Until 2026-09-12 the
 * driver returned the pin level directly, reporting every digital UIC
 * channel inverted.
 */
static void test_digital_input_is_active_low(void)
{
    gpio_stub_reset();
    fake_counter_reset();
    native_uic_bind_counter(NULL);

    for (uint8_t index = 0u; index < UIC_CHANNEL_COUNT; ++index)
    {
        native_channel_source_t source = {.source_index = index, .source_flags = UIC_MODE_DIGITAL_INPUT};
        telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_BOOLEAN);

        assert(native_uic_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);

        g_gpio_stub_external_level[UIC_BASE_GPIO + index] = false; /* field asserted */
        assert(native_uic_read(&channel, 0u) == TELEMETRY_DRIVER_RESULT_OK);
        assert(channel.runtime.value.boolean == true);

        g_gpio_stub_external_level[UIC_BASE_GPIO + index] = true; /* field idle */
        assert(native_uic_read(&channel, 0u) == TELEMETRY_DRIVER_RESULT_OK);
        assert(channel.runtime.value.boolean == false);
    }
}

int main(void)
{
    test_source_validation();
    test_digital_input_mode_never_touches_the_counter();
    test_digital_input_is_active_low();
    test_pulse_count_mode();
    test_frequency_and_period_modes();
    test_no_counter_bound();
    test_start_hardware_failure();
    test_value_type_mismatch_rejected();
    return 0;
}
