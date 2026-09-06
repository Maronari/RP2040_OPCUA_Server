/**
 * @file
 * @brief Board-diagnostics-service implementation. See
 *        services/include/services/board_diagnostics_service.h for the
 *        full contract, scope note (driver/vtable-internal only, no
 *        OPC UA nodes), and the raw-vs-real rail-voltage caveat.
 */
#include "services/board_diagnostics_service.h"

#include "drivers/ips2050_driver.h"
#include "drivers/native_channel_driver.h"
#include "hardware/gpio.h"
#include "services/board_power_service.h"

#include <string.h>

#define ADC_VIN_PROTECTED_MON 4u
#define ADC_AO_15V_MON 5u
#define ADC_ANA_5V_MON 6u

#define PWR_IN_FAULT_N_GPIO 11u
#define BOARD_FAULT_N_GPIO 19u
#define PWR_3V3_PGOOD_N_GPIO 34u
#define PWR_5V_PGOOD_N_GPIO 35u

static void init_active_low_input(uint8_t gpio)
{
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_IN);
    gpio_disable_pulls(gpio);
}

void board_diagnostics_service_init(void)
{
    init_active_low_input(PWR_IN_FAULT_N_GPIO);
    init_active_low_input(BOARD_FAULT_N_GPIO);
    init_active_low_input(PWR_3V3_PGOOD_N_GPIO);
    init_active_low_input(PWR_5V_PGOOD_N_GPIO);
}

bool board_diagnostics_collect(board_diagnostics_snapshot_t *out_snapshot)
{
    uint8_t index;

    if (out_snapshot == NULL)
    {
        return false;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));

    out_snapshot->power_input_fault = !gpio_get(PWR_IN_FAULT_N_GPIO);
    out_snapshot->board_fault = !gpio_get(BOARD_FAULT_N_GPIO);
    out_snapshot->rail_3v3_fault = !gpio_get(PWR_3V3_PGOOD_N_GPIO);
    out_snapshot->rail_5v_fault = !gpio_get(PWR_5V_PGOOD_N_GPIO);

    for (index = 0u; index < 2u; ++index)
    {
        bool fault_active = false;

        if (native_ips2050_read_fault(index, &fault_active))
        {
            out_snapshot->do_fault[index] = fault_active;
        }
    }
    for (index = 0u; index < 4u; ++index)
    {
        bool fault_active = false;

        if (board_power_get_sensor_fault((board_power_sensor_port_t)index, &fault_active))
        {
            out_snapshot->sensor_power_fault[index] = fault_active;
        }
    }
    for (index = 0u; index < 2u; ++index)
    {
        bool ready = false;

        if (board_power_get_i2c_ready((board_power_i2c_isolation_bus_t)index, &ready))
        {
            out_snapshot->i2c_isolation_ready[index] = ready;
        }
    }

    out_snapshot->rail_read_ok[0] =
        native_adc_voltage_read_raw(ADC_VIN_PROTECTED_MON, &out_snapshot->rail_vin_protected_volts);
    out_snapshot->rail_read_ok[1] = native_adc_voltage_read_raw(ADC_AO_15V_MON, &out_snapshot->rail_ao_15v_volts);
    out_snapshot->rail_read_ok[2] = native_adc_voltage_read_raw(ADC_ANA_5V_MON, &out_snapshot->rail_ana_5v_volts);
    return true;
}
