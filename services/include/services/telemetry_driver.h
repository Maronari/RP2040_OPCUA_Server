/**
 * @file
 * @brief Channel driver operation table (vtable) contract implemented by
 *        each native telemetry channel driver.
 *
 * This is a services-layer *contract*, not hardware code: it defines the
 * function-pointer shape that the `*_channel_driver.c` sources under
 * `drivers/src` implement and that `sensor_manager`/`channel_manager`
 * dispatch through (see docs/code/hardware.md, "Driver operation table").
 * Only `start`/`stop`/`read` currently have real implementations across the
 * shipped native drivers; `write`/`configure`/`self_test` are dispatched
 * past a NULL check but have no functioning implementation for the current
 * driver set.
 */
#ifndef SERVICES_TELEMETRY_DRIVER_H
#define SERVICES_TELEMETRY_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "services/telemetry_channel.h"

/** @brief Result/error code returned by every telemetry_driver_vtbl_t operation. */
typedef enum
{
    TELEMETRY_DRIVER_RESULT_OK = 0,
    TELEMETRY_DRIVER_RESULT_UNSUPPORTED,
    TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT,
    TELEMETRY_DRIVER_RESULT_INVALID_STATE,
    TELEMETRY_DRIVER_RESULT_TIMEOUT,
    TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE,
    TELEMETRY_DRIVER_RESULT_NO_MEMORY
} telemetry_driver_result_t;

/** @brief Bitmask a driver can report to declare which vtable operations it actually implements. */
typedef enum
{
    TELEMETRY_DRIVER_CAPABILITY_START = (1u << 0u),
    TELEMETRY_DRIVER_CAPABILITY_STOP = (1u << 1u),
    TELEMETRY_DRIVER_CAPABILITY_READ = (1u << 2u),
    TELEMETRY_DRIVER_CAPABILITY_WRITE = (1u << 3u),
    TELEMETRY_DRIVER_CAPABILITY_CONFIGURE = (1u << 4u),
    TELEMETRY_DRIVER_CAPABILITY_SELF_TEST = (1u << 5u)
} telemetry_driver_capability_t;

/**
 * @brief Driver operation table dispatched by `sensor_manager.c`
 *        (start/stop/configure/self_test) and `channel_manager.c` (read).
 *
 * Each function pointer may be NULL for a given driver instance; callers
 * check for NULL before dispatching, so an unimplemented operation is not a
 * crash but a silent no-op/unsupported result at the call site. Driver
 * implementations own any hardware access performed inside these calls
 * (ADC/GPIO/etc. per docs/code/hardware.md); this header only defines the
 * contract shape.
 */
typedef struct telemetry_driver_vtbl
{
    /** @brief Starts/initializes the channel's underlying source. Hardware side effects are driver-defined. */
    telemetry_driver_result_t (*start)(telemetry_channel_t *channel);
    /** @brief Stops/releases the channel's underlying source. Hardware side effects are driver-defined. */
    telemetry_driver_result_t (*stop)(telemetry_channel_t *channel);
    /** @brief Samples the channel's current value into @p channel's runtime state. @param now_ms Caller-supplied monotonic timestamp (ms) used for rate limiting/timestamping. */
    telemetry_driver_result_t (*read)(telemetry_channel_t *channel, uint32_t now_ms);
    /** @brief Writes a value out to the channel's underlying source, for writable channels. Not implemented by any shipped native driver. */
    telemetry_driver_result_t (*write)(telemetry_channel_t *channel,
                                       const telemetry_channel_value_t *value);
    /** @brief Applies driver-specific configuration to the channel. Not implemented by any shipped native driver. */
    telemetry_driver_result_t (*configure)(telemetry_channel_t *channel, const void *config);
    /** @brief Runs a driver-specific self-test on the channel. Not implemented by any shipped native driver. */
    telemetry_driver_result_t (*self_test)(telemetry_channel_t *channel);
} telemetry_driver_vtbl_t;

#endif
