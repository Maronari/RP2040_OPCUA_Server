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
#include "drivers/board_fault_inputs.h"

#include <string.h>

#define ADC_VIN_PROTECTED_MON 4u
#define ADC_AO_15V_MON 5u
#define ADC_ANA_5V_MON 6u

void board_diagnostics_service_init(void)
{
    board_fault_inputs_init();
}

bool board_diagnostics_collect(board_diagnostics_snapshot_t *out_snapshot)
{
    uint8_t index;

    if (out_snapshot == NULL)
    {
        return false;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));

    const board_fault_inputs_t inputs = board_fault_inputs_read();
    out_snapshot->power_input_fault = inputs.power_input_fault;
    out_snapshot->board_fault = inputs.board_fault;
    out_snapshot->rail_3v3_fault = inputs.rail_3v3_fault;
    out_snapshot->rail_5v_fault = inputs.rail_5v_fault;

    for (index = 0u; index < 2u; ++index)
    {
        bool fault_active = false;

        if (native_ips2050_read_fault(index, &fault_active))
        {
            out_snapshot->do_fault[index] = fault_active;
        }
    }

    out_snapshot->rail_read_ok[0] =
        native_adc_voltage_read_raw(ADC_VIN_PROTECTED_MON, &out_snapshot->rail_vin_protected_volts);
    out_snapshot->rail_read_ok[1] = native_adc_voltage_read_raw(ADC_AO_15V_MON, &out_snapshot->rail_ao_15v_volts);
    out_snapshot->rail_read_ok[2] = native_adc_voltage_read_raw(ADC_ANA_5V_MON, &out_snapshot->rail_ana_5v_volts);
    return true;
}
