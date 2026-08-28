/**
 * @file
 * @brief On-die RP2350 temperature sensor driver (ADC input backed).
 *
 * Responsibilities:
 * - Enable the ADC peripheral's internal temperature-sensor input.
 * - Read and convert one raw ADC sample to degrees Celsius using the
 *   datasheet reference curve.
 *
 * Ownership: no persistent state of its own -- the ADC peripheral itself is
 * the only owned hardware resource, shared with adc_voltage_channel_driver.c.
 *
 * Concurrency: none; see temp_sensor_driver_read_celsius()'s header doc for
 * the ADC-sharing caveat.
 */
#include "drivers/temp_sensor_driver.h"

#include <stddef.h>

#include "hardware/adc.h"

bool temp_sensor_driver_init(void)
{
    adc_init();
    adc_set_temp_sensor_enabled(true);
    return true;
}

bool temp_sensor_driver_read_celsius(float *out_temperature_c)
{
    if (out_temperature_c == NULL)
    {
        return false;
    }

    /* The temperature mux is input 4 on RP2040/RP2350A and input 8 on
     * RP2350B. Keep package selection owned by the Pico SDK. */
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    uint16_t raw = adc_read();
    const float conversion_factor = 3.3f / 4095.0f;
    const float voltage = raw * conversion_factor;

    *out_temperature_c = 27.0f - ((voltage - 0.706f) / 0.001721f);
    return true;
}


