#ifndef DRIVERS_TEMP_SENSOR_DRIVER_H
#define DRIVERS_TEMP_SENSOR_DRIVER_H

#include <stdbool.h>

/**
 * @brief Initialize the ADC peripheral and enable the on-die temperature sensor.
 *
 * Shares the single RP2350 ADC peripheral with native_adc_voltage_start()
 * (drivers/native_channel_driver.h) -- both call `adc_init()`, which the
 * Pico SDK tolerates being called more than once. Unlike the ADC-voltage
 * driver, this driver does not track its own "already initialized" flag.
 *
 * @return Always true.
 * @note Hardware side effect: enables the internal temperature sensor input.
 */
bool temp_sensor_driver_init(void);

/**
 * @brief Read the on-die temperature sensor and convert to degrees Celsius.
 *
 * Uses the RP2040/RP2350 datasheet reference conversion (27 degC at 0.706 V,
 * -1.721 mV/degC slope) against a fixed 3.3 V/4095-count ADC scale.
 *
 * @param out_temperature_c Destination for the converted value; untouched
 *        on failure.
 * @return false only if @p out_temperature_c is NULL; true otherwise.
 * @pre temp_sensor_driver_init() must have been called. Shares the single
 *      ADC peripheral with native_adc_voltage_read() (drivers/native_channel_driver.h);
 *      both are dispatched from the same SensorTask read path
 *      (channel_manager.c) so callers never overlap in practice, but this
 *      driver itself performs no locking against a different call pattern.
 * @note Hardware side effect: selects ADC input for the temperature mux and
 *       performs a blocking conversion.
 */
bool temp_sensor_driver_read_celsius(float *out_temperature_c);

#endif
