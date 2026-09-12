/**
 * @file
 * @brief Native (first-party, hardware-backed) channel driver operation
 *        table implementations: internal-temperature, ADC-voltage,
 *        GPIO-input, and GPIO-output channels.
 *
 * This header declares the concrete functions that
 * services/src/driver_catalog.c wires into telemetry_driver_vtbl_t
 * (services/include/services/telemetry_driver.h) instances -- one vtable
 * per driver, bound at compile time. `internal_temperature`/`adc_voltage`/
 * `gpio_input` implement exactly the vtable's `start`/`stop`/`read` slots
 * (read-only, non-writable hardware sources). `gpio_output`
 * (v0.5.1 T1, see docs/releases/v0.5.1.md) is the first native driver to
 * implement `write`/`configure`/`self_test`.
 *
 * Vtable contract (see telemetry_driver_vtbl_t):
 * - `start(channel)`: validate `channel->driver_context` (cast to
 *   native_channel_source_t) and claim/configure the hardware resource
 *   (GPIO direction+pulls, ADC input enablement). Must be idempotent enough
 *   to be safely retried on TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT.
 * - `stop(channel)`: release logical ownership. None of these four drivers
 *   actually deconfigures hardware on stop (they are all no-ops beyond
 *   returning TELEMETRY_DRIVER_RESULT_OK) since the underlying peripherals
 *   (ADC, GPIO bank) have no meaningful "off" state to restore here.
 * - `read(channel, now_ms)`: perform one hardware sample and write the
 *   converted value into `channel->runtime.value` (the union member
 *   matching `channel->metadata.value_type`). Runs synchronously/blocking
 *   on the calling task; never sleeps or yields.
 * - `write(channel, value)`: (gpio_output only) drive the channel's exact-
 *   type value out to hardware and mirror it into `channel->runtime.value`
 *   on success, so a subsequent `self_test` can compare commanded vs.
 *   observed state.
 * - `configure(channel, config)`: (gpio_output only) apply the driver's
 *   declared safe/idle output level outside the normal write path (see
 *   native_gpio_output_config_t).
 * - `self_test(channel)`: (gpio_output only) verify the pad's observable
 *   direction/level actually match the last commanded state.
 *
 * Execution context: per docs/code/hardware.md, `start`/`stop`/`configure`/
 * `self_test` are dispatched from sensor_manager.c, `read` from
 * channel_manager.c:545, and `write` from channel_manager.c's write path
 * (services/channel_manager.h `channel_manager_write_value()`) under a
 * per-channel driver-operation lease -- always from SensorTask, never from
 * an ISR or open62541 callback context.
 *
 * Concurrency: none of these drivers use locks. Hardware resource conflicts
 * (e.g. two channels claiming the same ADC input or GPIO pin) are caught
 * earlier, at configuration time, by services/src/driver_catalog.c's
 * resource-claim bookkeeping -- this header's functions assume the caller
 * has already ensured no two active channels contend for the same physical
 * resource.
 */
#ifndef DRIVERS_NATIVE_CHANNEL_DRIVER_H
#define DRIVERS_NATIVE_CHANNEL_DRIVER_H

#include <stdint.h>

#include "services/telemetry_driver.h"

/**
 * @brief `UIC` counter-mode derived-state scratch: the last-sampled edge
 *        count/timestamp baseline frequency/period modes diff against.
 *        Lives in `native_channel_source_t::scratch`, not a driver-private
 *        static -- see that union's doc comment.
 */
typedef struct
{
    bool have_baseline;
    uint32_t baseline_count;
    uint32_t baseline_time_ms;
} uic_derived_state_t;

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
    /** Mutable, service-owned per-instance scratch state (v0.5.1 T16/T19+:
     * lives here, in the already-existing per-channel allocation, rather
     * than a driver-private file-static array, so a compiled-but-unused
     * driver holds none of it -- ROADMAP.md:599). Meaning is entirely
     * driver-specific and not serialized; a driver resets its own member
     * in start() and must never read a member a different driver last
     * wrote -- the union itself does not track which one applies, only
     * each channel's own `driver_id` (looked up separately) does. Drivers
     * that need none of this leave it unused. */
    union
    {
        uint32_t synthetic_read_count;
        uic_derived_state_t uic_derived;
    } scratch;
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
 * @brief Raw diagnostic query: sample any ADC input (0-7, the full
 *        RP2350B/`RP2350_INDUSTRIAL_MVP_V1` range -- not limited to
 *        `native_adc_voltage_source_is_valid()`'s 0-3 channel range)
 *        without requiring a `telemetry_channel_t`, for a non-channel
 *        caller like `services/board_diagnostics_service.c` (v0.5.1,
 *        raw `ADC4..ADC6` rail monitoring). Shares the same lazy
 *        `adc_init()` state as `native_adc_voltage_start()`.
 * @param adc_index ADC channel 0-7.
 * @param out_volts Receives the raw pin voltage (3.3V/4095 counts,
 *        `channel->settings.scale`/`offset` are NOT applied -- there is
 *        no channel here to carry them, and no confirmed divider ratio
 *        exists for the industrial board's monitored rails; see
 *        `docs/releases/v0.5.1.md` T12).
 * @return false if @p adc_index > 7 or @p out_volts is NULL; true
 *         otherwise.
 */
bool native_adc_voltage_read_raw(uint8_t adc_index, double *out_volts);

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

/**
 * @brief Optional configuration payload for native_gpio_output_configure().
 *
 * Every current caller (sensor_manager.c's channel activation/restart
 * paths) invokes `configure()` with a NULL payload, which
 * native_gpio_output_configure() treats as "apply the released/safe
 * default" -- not an error. This struct exists for a future direct caller
 * that wants to override that default; none exists today.
 */
typedef struct
{
    bool safe_level;
} native_gpio_output_config_t;

/**
 * @brief Validate a GPIO-output channel's source selector.
 * @return true only for source_index < NUM_BANK0_GPIOS, source_flags
 *         containing no unknown bits (invert, open-drain).
 */
bool native_gpio_output_source_is_valid(uint8_t source_index, uint8_t source_flags);

/**
 * @brief Start (claim/configure) one GPIO output pin and drive it to its
 *        released/not-asserted default (`false`, pre-invert) via
 *        native_gpio_output_write().
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid;
 *         TELEMETRY_DRIVER_RESULT_OK otherwise.
 * @note Hardware side effect: configures direction/pulls per open-drain
 *       mode (see native_gpio_output_write()) and drives the pin.
 */
telemetry_driver_result_t native_gpio_output_start(telemetry_channel_t *channel);
/** @brief No-op; always returns TELEMETRY_DRIVER_RESULT_OK (see file-level vtable contract). */
telemetry_driver_result_t native_gpio_output_stop(telemetry_channel_t *channel);
/**
 * @brief Drive @p value out to the configured GPIO pin.
 *
 * Push-pull mode (default): `gpio_put()` the post-invert level directly.
 * Open-drain mode (source_flags NATIVE_GPIO_OUTPUT_SOURCE_OPEN_DRAIN):
 * asserted (post-invert `true`) actively drives the pin low
 * (`GPIO_OUT`, level 0); released (post-invert `false`) switches the pin
 * to high-impedance input (`GPIO_IN`), relying on the internal pull-up
 * enabled in native_gpio_output_start() (or external wiring) to read high.
 *
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid,
 *         @p value is NULL, or the channel's value type is not BOOLEAN;
 *         TELEMETRY_DRIVER_RESULT_OK otherwise, with `channel->runtime.value.boolean`
 *         updated to the pre-invert commanded value.
 */
telemetry_driver_result_t native_gpio_output_write(telemetry_channel_t *channel,
                                                    const telemetry_channel_value_t *value);
/**
 * @brief Apply this channel's safe/idle output level via
 *        native_gpio_output_write().
 *
 * @p config NULL (every current real call site) applies the released
 * (`false`) default; a non-NULL native_gpio_output_config_t overrides it.
 *
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid;
 *         otherwise the result of the underlying write.
 */
telemetry_driver_result_t native_gpio_output_configure(telemetry_channel_t *channel, const void *config);
/**
 * @brief Verify the pad's observable direction (open-drain) or
 *        direction+level (push-pull) match the last commanded state.
 *
 * Push-pull: compares `gpio_get_dir()`/`gpio_get()` against the last
 * commanded post-invert level. Open-drain: compares only `gpio_get_dir()`
 * against the expected direction (`GPIO_OUT` when asserted, `GPIO_IN` when
 * released) -- the released level depends on external wiring the reference
 * board does not provide, so it is not part of this check.
 *
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid;
 *         TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE if the observed pad state
 *         does not match the commanded state; TELEMETRY_DRIVER_RESULT_OK
 *         otherwise.
 */
telemetry_driver_result_t native_gpio_output_self_test(telemetry_channel_t *channel);

#endif
