/**
 * @file
 * @brief Native channel driver for the `ADS8688` industrial 8-channel SPI
 *        ADC (`RP2350_INDUSTRIAL_MVP_V1`'s `AI0..AI3`, see `HARDWARE_MVP.md`
 *        and `docs/releases/v0.5.1.md` T2). Pre-hardware: source-complete
 *        and host/mock-tested, but not hardware qualified -- see
 *        `ROADMAP.md` section 5.2.
 *
 * Register/command reference: `docs/datasheets/ADS8688-register-notes.md`
 * (extracted from the official TI datasheet, SBAS582C).
 *
 * Bus binding: this driver depends only on `drivers/spi_transport.h`'s
 * generic `spi_bus_t`, never on `hardware_spi`/`hardware_gpio` directly,
 * so it stays host-testable. native_ads8688_bind_bus() supplies the bus --
 * a host test binds a fake one directly; the real firmware binds one backed
 * by `drivers/rp2350_spi_bus.h` (not this header's concern).
 *
 * Vtable contract (see telemetry_driver_vtbl_t): implements `start`/`stop`/
 * `read`/`self_test`. No `write` (read-only ADC input) and no `configure`
 * (the input-range mode lives in the channel's persisted `source_flags`,
 * validated by native_ads8688_source_is_valid() -- matching the v0.5.1 T1
 * finding that every real `configure()` call site passes `NULL` and
 * mode-like per-channel properties belong in `source_flags`, not a runtime
 * payload).
 *
 * Ownership: `driver_context` carries only this channel's
 * `native_channel_source_t` (source_index = ADS8688 mux input 0-7,
 * source_flags = `Range_CHn[3:0]`), matching every other native driver.
 * The SPI bus binding is process-wide (one physical `ADS8688` chip is in
 * scope for this release), held in this file's own static storage, not in
 * `driver_context` -- `sensor_manager_channel_context_t` has no room for
 * driver-specific extra fields.
 *
 * Concurrency: none; per `native_channel_driver.h`'s file-level doc, all
 * calls arrive serialized from SensorTask.
 */
#ifndef DRIVERS_ADS8688_DRIVER_H
#define DRIVERS_ADS8688_DRIVER_H

#include "drivers/native_channel_driver.h"
#include "drivers/spi_transport.h"

/**
 * @brief Binds the SPI bus this driver's channels use for every subsequent
 *        `start`/`read`/`self_test` call.
 *
 * Call once before activating any `ADS8688` channel. A host test binds a
 * fake `spi_bus_t` directly (no Pico SDK headers needed); the real firmware
 * binds one backed by `rp2350_spi_bus_init()`/`rp2350_spi_bus_vtbl`
 * (`drivers/rp2350_spi_bus.h`) using the industrial board's documented
 * pins (`HARDWARE_MVP.md`: `GPIO12`=MISO, `GPIO13`=`ADC_CS_N`, `GPIO14`=SCK,
 * `GPIO15`=MOSI -- not yet exercised on real hardware, see
 * `docs/releases/v0.5.1.md`).
 *
 * @param bus Bus to bind; the pointer is retained, not copied -- it must
 *        outlive every subsequent driver call. Pass NULL to unbind (every
 *        subsequent call then fails with TELEMETRY_DRIVER_RESULT_INVALID_STATE).
 */
void native_ads8688_bind_bus(const spi_bus_t *bus);

/**
 * @brief Validate an `ADS8688` channel's source selector.
 * @return true only for source_index <= 7 (mux inputs `AI0..AI7`) and
 *         source_flags one of the datasheet's five valid `Range_CHn[3:0]`
 *         codes (0x0, 0x1, 0x2, 0x5, 0x6 -- see
 *         docs/datasheets/ADS8688-register-notes.md).
 */
bool native_ads8688_source_is_valid(uint8_t source_index, uint8_t source_flags);

/**
 * @brief Start (claim/configure) one `ADS8688` mux input channel: binds the
 *        SPI bus if not already bound is the caller's responsibility (see
 *        native_ads8688_bind_bus()); writes this channel's `Range_CHn`
 *        program register and verifies the write-cycle echo matches.
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid;
 *         TELEMETRY_DRIVER_RESULT_INVALID_STATE if no bus is bound;
 *         TELEMETRY_DRIVER_RESULT_TIMEOUT if the transport transfer itself
 *         fails; TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE if the echoed
 *         byte does not match what was written; TELEMETRY_DRIVER_RESULT_OK
 *         otherwise.
 */
telemetry_driver_result_t native_ads8688_start(telemetry_channel_t *channel);
/** @brief No-op; always returns TELEMETRY_DRIVER_RESULT_OK (see file-level vtable contract). */
telemetry_driver_result_t native_ads8688_stop(telemetry_channel_t *channel);
/**
 * @brief Sample this channel's mux input and store the scaled voltage in
 *        @p channel.
 *
 * Issues `MAN_Ch_n` twice (the ADS8688 result pipeline is delayed by one
 * data frame -- see docs/datasheets/ADS8688-register-notes.md) to guarantee
 * a valid result for this channel regardless of which channel was
 * previously selected, then applies the range's full-scale voltage and
 * `channel->settings.scale`/`offset`.
 *
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid
 *         or the channel's value type is neither FLOAT nor DOUBLE;
 *         TELEMETRY_DRIVER_RESULT_INVALID_STATE if no bus is bound;
 *         TELEMETRY_DRIVER_RESULT_TIMEOUT if either transfer fails;
 *         TELEMETRY_DRIVER_RESULT_OK otherwise.
 */
telemetry_driver_result_t native_ads8688_read(telemetry_channel_t *channel, uint32_t now_ms);
/**
 * @brief Verify the chip actually echoes back distinct written values on
 *        this channel's `Range_CHn` register, distinguishing three
 *        outcomes per `ROADMAP.md`'s self-test requirement:
 *
 * - TELEMETRY_DRIVER_RESULT_OK: every write's echo matched exactly
 *   (supported and passed).
 * - TELEMETRY_DRIVER_RESULT_TIMEOUT: the echoes never changed between two
 *   different written values (a constant readback, e.g. all-zero or
 *   all-one, is what an absent/floating chip produces) -- supported but
 *   hardware unavailable/not present.
 * - TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE: the echoes changed but did
 *   not match what was written -- present but malfunctioning.
 *
 * Restores the channel's actual configured range on exit regardless of
 * outcome (best-effort on transport failure).
 */
telemetry_driver_result_t native_ads8688_self_test(telemetry_channel_t *channel);

#endif
