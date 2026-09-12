/**
 * @file
 * @brief Channel driver operation table (vtable) contract implemented by
 *        each native telemetry channel driver.
 *
 * This is a services-layer *contract*, not hardware code: it defines the
 * function-pointer shape that the `*_channel_driver.c` sources under
 * `drivers/src` implement and that `sensor_manager`/`channel_manager`
 * dispatch through (see docs/code/hardware.md, "Driver operation table").
 * Every operation in this table now has real implementations: `start`/
 * `stop`/`read` across the whole driver set, `write` in `gpio_output`/
 * `AD5422`/`IPS2050`, `configure` in `gpio_output`, and `self_test` in
 * `gpio_output`/`ADS8688`/`AD5422`/`MAX22190`/`IPS2050` (v0.5.1). Any entry
 * may still be NULL for a given driver; callers NULL-check before
 * dispatching.
 *
 * **`self_test` result convention** (`ROADMAP.md:584`, "supported and
 * passed" / "supported but hardware unavailable/not present" / "failed"):
 * `TELEMETRY_DRIVER_RESULT_OK` means the device answered and passed;
 * `TELEMETRY_DRIVER_RESULT_TIMEOUT` means the device did not respond at
 * all -- absent or unpowered hardware, **not** a board fault;
 * `TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE` means it responded and failed.
 * The distinction exists so that pre-production hardware is never reported
 * as a passing self-test, and so a caller can tell "the chip is not there"
 * from "the chip is broken". Callers that collapse the three into a single
 * pass/fail test discard that distinction -- see `sensor_manager.c`'s
 * activation path, which currently does.
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
    /** @brief Writes a value out to the channel's underlying source, for writable channels. Implemented by `gpio_output`, `AD5422` and `IPS2050`. */
    telemetry_driver_result_t (*write)(telemetry_channel_t *channel,
                                       const telemetry_channel_value_t *value);
    /** @brief Applies driver-specific configuration to the channel. Implemented by `gpio_output`; a NULL @p config means "re-apply the channel's own settings". */
    telemetry_driver_result_t (*configure)(telemetry_channel_t *channel, const void *config);
    /** @brief Runs a driver-specific self-test on the channel, per this header's three-way result convention. Implemented by `gpio_output`, `ADS8688`, `AD5422`, `MAX22190` and `IPS2050`. */
    telemetry_driver_result_t (*self_test)(telemetry_channel_t *channel);
} telemetry_driver_vtbl_t;

#endif
