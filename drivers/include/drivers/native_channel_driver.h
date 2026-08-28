/**
 * @file
 * @brief Native (first-party, hardware-backed) channel driver operation
 *        table implementations: internal-temperature, ADC-voltage, and
 *        GPIO-input channels.
 *
 * This header declares the concrete functions that
 * services/src/driver_catalog.c wires into telemetry_driver_vtbl_t
 * (services/include/services/telemetry_driver.h) instances -- one vtable
 * per driver, bound at compile time. Each driver here implements exactly
 * the vtable's `start`/`stop`/`read` slots; none implements
 * `write`/`configure`/`self_test` (all three are read-only, non-writable
 * hardware sources).
 *
 * Vtable contract (see telemetry_driver_vtbl_t):
 * - `start(channel)`: validate `channel->driver_context` (cast to
 *   native_channel_source_t) and claim/configure the hardware resource
 *   (GPIO direction+pulls, ADC input enablement). Must be idempotent enough
 *   to be safely retried on TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT.
 * - `stop(channel)`: release logical ownership. None of these three drivers
 *   actually deconfigures hardware on stop (they are all no-ops beyond
 *   returning TELEMETRY_DRIVER_RESULT_OK) since the underlying peripherals
 *   (ADC, GPIO bank) have no meaningful "off" state to restore here.
 * - `read(channel, now_ms)`: perform one hardware sample and write the
 *   converted value into `channel->runtime.value` (the union member
 *   matching `channel->metadata.value_type`). Runs synchronously/blocking
 *   on the calling task; never sleeps or yields.
 *
 * Execution context: per docs/code/hardware.md, `start`/`stop` are
 * dispatched from sensor_manager.c and `read` from channel_manager.c:545,
 * always from SensorTask, never from an ISR or open62541 callback context.
 *
 * Concurrency: none of these drivers use locks. Hardware resource conflicts
 * (e.g. two channels claiming the same ADC input) are caught earlier, at
 * configuration time, by services/src/driver_catalog.c's resource-claim
 * bookkeeping -- this header's functions assume the caller has already
 * ensured no two active channels contend for the same physical resource.
 */
#ifndef DRIVERS_NATIVE_CHANNEL_DRIVER_H
#define DRIVERS_NATIVE_CHANNEL_DRIVER_H

#include <stdint.h>

#include "services/telemetry_driver.h"

/**
 * @brief Per-channel hardware source selector copied into driver_context.
 *
 * Configuration data copied into sensor-manager-owned storage before
 * channel activation; native drivers never retain configuration text or
 * service-registry pointers, only this compact struct via
 * `channel->driver_context`.
 *
 * @var source_index Driver-specific selector: ADC input 0-3 for
 *      native_adc_voltage_*, always 0 for native_internal_temperature_*,
 *      GPIO pin number for native_gpio_input_*.
 * @var source_flags Driver-specific bit flags (unused/must be 0 for
 *      internal-temperature and ADC-voltage; GPIO input invert/pull-up/
 *      pull-down bits for native_gpio_input_*).
 */
typedef struct
{
    uint8_t source_index;
    uint8_t source_flags;
} native_channel_source_t;

/**
 * @brief Validate an internal-temperature channel's source selector.
 * @return true only for source_index == 0 and source_flags == 0 (the
 *         single on-die sensor has no selectable index or flags).
 */
bool native_internal_temperature_source_is_valid(uint8_t source_index, uint8_t source_flags);

/**
 * @brief Validate an ADC-voltage channel's source selector.
 * @return true only for source_index <= 3 (external ADC inputs 0-3) and
 *         source_flags == 0.
 */
bool native_adc_voltage_source_is_valid(uint8_t source_index, uint8_t source_flags);

/**
 * @brief Validate a GPIO-input channel's source selector.
 * @return true only for source_index < NUM_BANK0_GPIOS, source_flags
 *         containing no unknown bits, and pull-up/pull-down not both set.
 */
bool native_gpio_input_source_is_valid(uint8_t source_index, uint8_t source_flags);

/**
 * @brief Start (claim/configure) the internal-temperature ADC channel.
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid;
 *         TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE if the ADC/sensor fails
 *         to initialize; TELEMETRY_DRIVER_RESULT_OK otherwise.
 * @note Hardware side effect: enables the ADC's internal temperature input.
 */
telemetry_driver_result_t native_internal_temperature_start(telemetry_channel_t *channel);
/** @brief No-op; always returns TELEMETRY_DRIVER_RESULT_OK (see file-level vtable contract). */
telemetry_driver_result_t native_internal_temperature_stop(telemetry_channel_t *channel);
/**
 * @brief Sample the on-die temperature sensor and store scaled value in @p channel.
 * @return TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE if the sensor read fails;
 *         TELEMETRY_DRIVER_RESULT_UNSUPPORTED if the channel's configured
 *         value type is neither FLOAT nor DOUBLE; TELEMETRY_DRIVER_RESULT_OK
 *         otherwise, with `channel->runtime.value` updated.
 * @note Applies `channel->settings.scale`/`offset` to the raw reading.
 */
telemetry_driver_result_t native_internal_temperature_read(telemetry_channel_t *channel, uint32_t now_ms);

/**
 * @brief Start (claim/configure) one external ADC-voltage input.
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid;
 *         TELEMETRY_DRIVER_RESULT_OK otherwise.
 * @note Hardware side effect: lazily calls `adc_init()` once (shared file-static
 *       flag across all ADC-voltage channel instances) and configures the
 *       selected pin as an ADC input via `adc_gpio_init()`.
 */
telemetry_driver_result_t native_adc_voltage_start(telemetry_channel_t *channel);
/** @brief No-op; always returns TELEMETRY_DRIVER_RESULT_OK (see file-level vtable contract). */
telemetry_driver_result_t native_adc_voltage_stop(telemetry_channel_t *channel);
/**
 * @brief Sample the configured ADC input and store scaled voltage in @p channel.
 * @return TELEMETRY_DRIVER_RESULT_INVALID_STATE if the source is invalid or
 *         native_adc_voltage_start() has not run; TELEMETRY_DRIVER_RESULT_UNSUPPORTED
 *         if the channel's value type is neither FLOAT nor DOUBLE;
 *         TELEMETRY_DRIVER_RESULT_OK otherwise.
 * @note Converts the 12-bit raw code to volts (3.3V/4095 counts) then
 *       applies `channel->settings.scale`/`offset`.
 */
telemetry_driver_result_t native_adc_voltage_read(telemetry_channel_t *channel, uint32_t now_ms);

/**
 * @brief Start (claim/configure) one GPIO input pin.
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid;
 *         TELEMETRY_DRIVER_RESULT_OK otherwise.
 * @note Hardware side effect: configures direction and pull resistors
 *       (up/down/disabled, mutually exclusive) on the selected pin.
 */
telemetry_driver_result_t native_gpio_input_start(telemetry_channel_t *channel);
/** @brief No-op; always returns TELEMETRY_DRIVER_RESULT_OK (see file-level vtable contract). */
telemetry_driver_result_t native_gpio_input_stop(telemetry_channel_t *channel);
/**
 * @brief Read the configured GPIO pin's level (optionally inverted) into @p channel.
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid
 *         or the channel's value type is not BOOLEAN; TELEMETRY_DRIVER_RESULT_OK
 *         otherwise, with `channel->runtime.value.boolean` updated.
 */
telemetry_driver_result_t native_gpio_input_read(telemetry_channel_t *channel, uint32_t now_ms);

#endif
