#ifndef DRIVERS_NATIVE_CHANNEL_DRIVER_H
#define DRIVERS_NATIVE_CHANNEL_DRIVER_H

#include <stdint.h>

#include "services/telemetry_driver.h"

/* This is configuration data copied into sensor-manager-owned storage before
 * activation.  Native drivers never retain configuration text or service
 * registry pointers. */
typedef struct
{
    uint8_t source_index;
    uint8_t source_flags;
} native_channel_source_t;

bool native_internal_temperature_source_is_valid(uint8_t source_index, uint8_t source_flags);
bool native_adc_voltage_source_is_valid(uint8_t source_index, uint8_t source_flags);
bool native_gpio_input_source_is_valid(uint8_t source_index, uint8_t source_flags);

telemetry_driver_result_t native_internal_temperature_start(telemetry_channel_t *channel);
telemetry_driver_result_t native_internal_temperature_stop(telemetry_channel_t *channel);
telemetry_driver_result_t native_internal_temperature_read(telemetry_channel_t *channel, uint32_t now_ms);

telemetry_driver_result_t native_adc_voltage_start(telemetry_channel_t *channel);
telemetry_driver_result_t native_adc_voltage_stop(telemetry_channel_t *channel);
telemetry_driver_result_t native_adc_voltage_read(telemetry_channel_t *channel, uint32_t now_ms);

telemetry_driver_result_t native_gpio_input_start(telemetry_channel_t *channel);
telemetry_driver_result_t native_gpio_input_stop(telemetry_channel_t *channel);
telemetry_driver_result_t native_gpio_input_read(telemetry_channel_t *channel, uint32_t now_ms);

#endif
