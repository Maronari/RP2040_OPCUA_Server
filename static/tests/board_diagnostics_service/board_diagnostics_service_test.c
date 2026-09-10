/**
 * @file
 * @brief Host tests for the real
 *        services/src/board_diagnostics_service.c against a stateful GPIO
 *        stub plus a fake native_ips2050_read_fault() implementation --
 *        proves the exact GPIO polarity/pin mapping and that a failing
 *        underlying read leaves its field false rather than aborting the
 *        whole collection.
 */
#include "services/board_diagnostics_service.h"

#include "drivers/ips2050_driver.h"
#include "drivers/native_channel_driver.h"
#include "hardware/gpio.h"

#include <assert.h>
#include <math.h>

#define PWR_IN_FAULT_N_GPIO 11u
#define BOARD_FAULT_N_GPIO 19u
#define PWR_3V3_PGOOD_N_GPIO 34u
#define PWR_5V_PGOOD_N_GPIO 35u

static bool g_do_fault[2];
static bool g_do_fault_read_ok[2] = {true, true};
static double g_adc_volts[8];
static bool g_adc_read_ok[8] = {true, true, true, true, true, true, true, true};

bool native_ips2050_read_fault(uint8_t source_index, bool *out_fault_active)
{
    if (source_index > 1u || out_fault_active == NULL || !g_do_fault_read_ok[source_index])
    {
        return false;
    }
    *out_fault_active = g_do_fault[source_index];
    return true;
}

bool native_adc_voltage_read_raw(uint8_t adc_index, double *out_volts)
{
    if (adc_index > 7u || out_volts == NULL || !g_adc_read_ok[adc_index])
    {
        return false;
    }
    *out_volts = g_adc_volts[adc_index];
    return true;
}

static void reset_fakes(void)
{
    size_t index;

    gpio_stub_reset();
    for (index = 0u; index < 2u; ++index)
    {
        g_do_fault[index] = false;
        g_do_fault_read_ok[index] = true;
    }
    for (index = 0u; index < 8u; ++index)
    {
        g_adc_volts[index] = 0.0;
        g_adc_read_ok[index] = true;
    }
}

static void assert_close(double a, double b)
{
    assert(fabs(a - b) < 0.001);
}

static void test_direct_gpio_polarity_and_mapping(void)
{
    board_diagnostics_snapshot_t snapshot;

    reset_fakes();
    board_diagnostics_service_init();
    assert(g_gpio_stub_pulls_disabled[PWR_IN_FAULT_N_GPIO]);
    assert(g_gpio_stub_pulls_disabled[BOARD_FAULT_N_GPIO]);
    assert(g_gpio_stub_pulls_disabled[PWR_3V3_PGOOD_N_GPIO]);
    assert(g_gpio_stub_pulls_disabled[PWR_5V_PGOOD_N_GPIO]);

    assert(board_diagnostics_collect(&snapshot));
    assert(!snapshot.power_input_fault && !snapshot.board_fault && !snapshot.rail_3v3_fault &&
           !snapshot.rail_5v_fault);

    g_gpio_stub_external_level[PWR_3V3_PGOOD_N_GPIO] = false; /* PGOOD deasserted -> fault */
    assert(board_diagnostics_collect(&snapshot));
    assert(snapshot.rail_3v3_fault == true);
    /* Unrelated fields unaffected. */
    assert(snapshot.power_input_fault == false && snapshot.rail_5v_fault == false &&
           snapshot.board_fault == false);
}

static void test_do_fields_pass_through(void)
{
    board_diagnostics_snapshot_t snapshot;

    reset_fakes();
    board_diagnostics_service_init();
    g_do_fault[1] = true;

    assert(board_diagnostics_collect(&snapshot));
    assert(snapshot.do_fault[0] == false && snapshot.do_fault[1] == true);
}

static void test_failing_underlying_read_leaves_field_false(void)
{
    board_diagnostics_snapshot_t snapshot;

    reset_fakes();
    board_diagnostics_service_init();
    g_do_fault[0] = true;
    g_do_fault_read_ok[0] = false; /* simulate the read itself failing */

    assert(board_diagnostics_collect(&snapshot));
    assert(snapshot.do_fault[0] == false); /* not the true value -- the read never succeeded */
    /* Every other field still collected normally. */
    assert(snapshot.do_fault[1] == false);
}

static void test_null_rejected(void)
{
    reset_fakes();
    board_diagnostics_service_init();
    assert(!board_diagnostics_collect(NULL));
}

static void test_rail_voltages_pass_through_correct_adc_index(void)
{
    board_diagnostics_snapshot_t snapshot;

    reset_fakes();
    board_diagnostics_service_init();
    g_adc_volts[4] = 3.1;  /* VIN_PROTECTED_MON */
    g_adc_volts[5] = 1.65; /* AO_15V_MON */
    g_adc_volts[6] = 0.9;  /* ANA_5V_MON */
    g_adc_volts[0] = 2.5;  /* unrelated ADC index -- must not leak into any rail field */

    assert(board_diagnostics_collect(&snapshot));
    assert(snapshot.rail_read_ok[0] && snapshot.rail_read_ok[1] && snapshot.rail_read_ok[2]);
    assert_close(snapshot.rail_vin_protected_volts, 3.1);
    assert_close(snapshot.rail_ao_15v_volts, 1.65);
    assert_close(snapshot.rail_ana_5v_volts, 0.9);
}

static void test_rail_read_failure_leaves_field_false_and_zero(void)
{
    board_diagnostics_snapshot_t snapshot;

    reset_fakes();
    board_diagnostics_service_init();
    g_adc_volts[5] = 1.65;
    g_adc_read_ok[5] = false; /* simulate the read itself failing */

    assert(board_diagnostics_collect(&snapshot));
    assert(snapshot.rail_read_ok[0] == true);
    assert(snapshot.rail_read_ok[1] == false);
    assert(snapshot.rail_ao_15v_volts == 0.0); /* not the true value -- the read never succeeded */
    assert(snapshot.rail_read_ok[2] == true);
}

int main(void)
{
    test_direct_gpio_polarity_and_mapping();
    test_do_fields_pass_through();
    test_failing_underlying_read_leaves_field_false();
    test_null_rejected();
    test_rail_voltages_pass_through_correct_adc_index();
    test_rail_read_failure_leaves_field_false_and_zero();
    return 0;
}
