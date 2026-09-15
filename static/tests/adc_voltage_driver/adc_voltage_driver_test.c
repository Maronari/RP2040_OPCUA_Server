/**
 * @file
 * @brief Host tests for the real
 *        drivers/src/adc_voltage_channel_driver.c's
 *        native_adc_voltage_read_raw() against a stateful fake ADC
 *        peripheral -- proves the actual GPIO/channel selection sequence
 *        and voltage conversion, not just that the call dispatches.
 */
#include "drivers/native_channel_driver.h"

#include "hardware/adc.h"

#include <assert.h>
#include <math.h>

static void assert_close(double a, double b)
{
    assert(fabs(a - b) < 0.001);
}

static void test_out_of_range_and_null_rejected(void)
{
    double volts = 0.0;

    adc_stub_reset();
    assert(!native_adc_voltage_read_raw(8u, &volts));
    assert(!native_adc_voltage_read_raw(0u, NULL));
    assert(!g_adc_stub_init_called); /* rejected before touching hardware */
}

static void test_lazy_init_and_channel_selection(void)
{
    double volts = 0.0;

    adc_stub_reset();
    g_adc_stub_channel_code[5] = 2048u; /* ~1.65 V */
    assert(native_adc_voltage_read_raw(5u, &volts));
    assert(g_adc_stub_init_called);
    assert(g_adc_stub_gpio_init_called[ADC_BASE_PIN + 5u]);
    assert(g_adc_stub_selected_input == 5u);
    assert_close(volts, 2048.0 * 3.3 / 4095.0);
}

static void test_shared_init_runs_only_once(void)
{
    double volts = 0.0;
    unsigned int call_count_after_first;

    /* g_adc_ready (inside the driver under test) is a one-time-latched
     * file-static flag that this test file cannot reset -- only
     * g_adc_stub_init_call_count (this stub's own counter) is reset here.
     * So this test captures its own baseline after the first call within
     * this test, rather than assuming a fresh-process call count of zero
     * (a prior test in this same binary may have already latched the
     * driver's internal flag). */
    adc_stub_reset();
    assert(native_adc_voltage_read_raw(4u, &volts));
    call_count_after_first = g_adc_stub_init_call_count;
    assert(native_adc_voltage_read_raw(6u, &volts));
    assert(g_adc_stub_init_call_count == call_count_after_first); /* second call must not re-init */
}

static void test_reads_full_scale_and_zero(void)
{
    double volts = 0.0;

    adc_stub_reset();
    g_adc_stub_channel_code[0] = 4095u;
    assert(native_adc_voltage_read_raw(0u, &volts));
    assert_close(volts, 3.3);

    g_adc_stub_channel_code[7] = 0u;
    assert(native_adc_voltage_read_raw(7u, &volts));
    assert_close(volts, 0.0);
}

int main(void)
{
    test_out_of_range_and_null_rejected();
    test_lazy_init_and_channel_selection();
    test_shared_init_runs_only_once();
    test_reads_full_scale_and_zero();
    return 0;
}
