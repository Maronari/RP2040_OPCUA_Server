/**
 * @file
 * @brief Native channel driver for a synthetic (no external hardware)
 *        benchmark/loopback source -- `ROADMAP.md`'s "synthetic benchmark
 *        source" / "loopback... source for host tests" generic driver
 *        (`ROADMAP.md:562-563`), and this project's minimal standalone
 *        example driver (`ROADMAP.md:590`, v0.5.1 T16).
 *
 * **Read this file first if you are adding a new native driver.** It is
 * deliberately the simplest complete driver in this codebase: no bus, no
 * GPIO, no datasheet, no hardware dependency of any kind -- just the bare
 * `start`/`stop`/`read` vtable slots (see `native_channel_driver.h`'s
 * file-level doc for the full contract every driver implements against)
 * and one caller-owned per-channel counter. See
 * `docs/code/adding-a-driver.md` (v0.5.1 T16) for the full walkthrough
 * this driver is the running example for.
 *
 * Behavior: each channel counts its own `read()` calls, starting from 0
 * at `start()`, wrapping modulo 2^32 like every other free-running
 * counter in this codebase (`UIC`'s pulse count, `IPS2050`'s DO_ENABLE_REQ
 * assert-once state). Useful for exercising the publish/subscribe/history
 * pipeline at a known, controllable rate independent of any real sensor's
 * timing -- a "synthetic benchmark source" -- and for host/integration
 * tests that need a channel with completely predictable values -- a
 * "loopback... source for host tests" -- without needing any mock bus or
 * stub header at all, since there is no hardware to stub.
 *
 * Vtable contract (see `telemetry_driver_vtbl_t`): implements `start`/
 * `stop`/`read` only -- no `write`/`configure`/`self_test`, matching
 * `internal_temperature`'s/`adc_voltage`'s/`gpio_input`'s minimal shape
 * (a read-only source with nothing to diagnose or configure).
 *
 * Ownership: `driver_context` carries only this channel's
 * `native_channel_source_t` (`source_index` = 0-7, one of eight
 * independent synthetic channels so several can be configured at once
 * without interfering; `source_flags` must be 0 -- no configuration
 * exists). Mutable counter state lives in native_channel_source_t, owned
 * by the sensor-manager context bank, and is reset by start(). No private
 * permanent SRAM pool is reserved for inactive synthetic drivers.
 *
 * Resource claims: none -- `driver_catalog_get_resource_claims()`
 * (`services/driver_catalog.c`) falls through to its default
 * `DRIVER_CATALOG_RESOURCE_NONE`/shared claim for this driver, correctly
 * representing that no physical resource is claimed at all; two channels
 * configured at the same `source_index` do not conflict (each still gets
 * its own independent counter via `driver_context`, they simply count the
 * same number of `read()` calls).
 *
 * Concurrency: the channel-manager operation lease serializes operations
 * on each instance. Separate contexts never share mutable counter state.
 */
#ifndef DRIVERS_SYNTHETIC_DRIVER_H
#define DRIVERS_SYNTHETIC_DRIVER_H

#include "drivers/native_channel_driver.h"

/**
 * @brief Validate a `synthetic` channel's source selector.
 * @return true only for source_index <= 7 (eight independent synthetic
 *         channels) and source_flags == 0 (no configuration exists).
 */
bool native_synthetic_source_is_valid(uint8_t source_index, uint8_t source_flags);

/** @brief Resets this channel's counter to 0. @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid; TELEMETRY_DRIVER_RESULT_OK otherwise. */
telemetry_driver_result_t native_synthetic_start(telemetry_channel_t *channel);
/** @brief No-op; always returns TELEMETRY_DRIVER_RESULT_OK (see file-level vtable contract). */
telemetry_driver_result_t native_synthetic_stop(telemetry_channel_t *channel);
/**
 * @brief Increments this channel's counter by 1 (wrapping modulo 2^32)
 *        and stores it as the channel's value.
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is
 *         invalid; TELEMETRY_DRIVER_RESULT_UNSUPPORTED if the channel's
 *         value type is neither FLOAT nor DOUBLE; TELEMETRY_DRIVER_RESULT_OK
 *         otherwise.
 */
telemetry_driver_result_t native_synthetic_read(telemetry_channel_t *channel, uint32_t now_ms);

#endif
