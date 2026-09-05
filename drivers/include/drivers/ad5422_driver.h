/**
 * @file
 * @brief Native channel driver for the two `AD5422` industrial analog
 *        output channels (`RP2350_INDUSTRIAL_MVP_V1`'s `AO0`/`AO1`, see
 *        `HARDWARE_MVP.md` section 3.2 and `docs/releases/v0.5.1.md`
 *        Phase 1). Pre-hardware: source-complete and host/mock-tested, but
 *        not hardware qualified -- see `ROADMAP.md` section 5.2.
 *
 * Register/command reference: `docs/datasheets/AD5422-register-notes.md`
 * (extracted from the official Analog Devices datasheet, AD5412/AD5422
 * Rev. M).
 *
 * Bus binding: like `ads8688_driver.c`, depends only on
 * `drivers/spi_transport.h`'s generic `spi_bus_t`, never on
 * `hardware_spi`/`hardware_gpio` directly. `native_ad5422_bind_bus()`
 * supplies the bus; a host test binds a fake one directly.
 *
 * Daisy-chain wiring: `AO0` and `AO1` share one `SCLK`/`LATCH`/`MOSI` (one
 * `spi_bus_t` for both channels, not one per channel) with their
 * `SDIN`/`SDO` chained (`HARDWARE_MVP.md:343-365`). Every transaction is a
 * 48-bit (6-byte), two-device frame: the word transmitted *first* reaches
 * the far device (`AO1`), the word transmitted *second* stays in the near
 * device (`AO0`). Targeting one channel while leaving the other untouched
 * is done by sending a real command for the target device's slot and
 * `NOP` (`0x000000`) for the other's -- no cross-channel state caching is
 * needed; see the register notes for why.
 *
 * Vtable contract (see telemetry_driver_vtbl_t): implements `start`/
 * `stop`/`write`/`self_test`. No `read` (write-only analog output, like
 * `gpio_output`) and no `configure` (the output range lives in the
 * channel's persisted `source_flags`, matching the v0.5.1 T1 finding that
 * every real `configure()` call site passes `NULL`). `stop()` is not a
 * no-op here: it disables the output (`OUTEN=0`), a real safe-state action
 * per `ROADMAP.md`'s "safe-state integration" requirement.
 *
 * Ownership: `driver_context` carries only this channel's
 * `native_channel_source_t` (source_index = 0 for `AO0`, 1 for `AO1`;
 * source_flags = `R[2:0]` output-range code). The SPI bus binding is
 * process-wide file-static state, like `ads8688_driver.c`'s.
 *
 * Hardware caveat not resolved here (see the register notes' wiring
 * section): `HARDWARE_MVP.md`'s current documented wiring does not show
 * either `AD5422`'s `SDO` returning to the shared controller `MISO` line,
 * so the readback path this driver's `self_test()` uses per the chip's own
 * documented capability may not be electrically available on the real
 * board as currently specified. This driver implements the datasheet's
 * documented protocol correctly and is host/mock-verified against it;
 * whether real-hardware self_test can ever report
 * TELEMETRY_DRIVER_RESULT_OK is a hardware-bring-up question, not a
 * firmware defect, and diagnosis may ultimately depend on the shared
 * `FAULT` pin (`BOARD_FAULT_N`) rather than SPI readback -- out of scope
 * for this driver, which only implements the SPI-visible contract.
 *
 * Concurrency: none; per `native_channel_driver.h`'s file-level doc, all
 * calls arrive serialized from SensorTask.
 */
#ifndef DRIVERS_AD5422_DRIVER_H
#define DRIVERS_AD5422_DRIVER_H

#include "drivers/native_channel_driver.h"
#include "drivers/spi_transport.h"

/**
 * @brief Binds the shared SPI bus both `AO0` and `AO1` channels use for
 *        every subsequent `start`/`write`/`self_test` call.
 *
 * Call once before activating either `AD5422` channel. A host test binds a
 * fake `spi_bus_t` directly; the real firmware binds one backed by
 * `rp2350_spi_bus_init()`/`rp2350_spi_bus_vtbl` (`drivers/rp2350_spi_bus.h`)
 * -- not yet wired to any real board profile, see
 * `docs/releases/v0.5.1.md`.
 *
 * @param bus Bus to bind; the pointer is retained, not copied -- it must
 *        outlive every subsequent driver call. Pass NULL to unbind (every
 *        subsequent call then fails with TELEMETRY_DRIVER_RESULT_INVALID_STATE).
 */
void native_ad5422_bind_bus(const spi_bus_t *bus);

/**
 * @brief Validate an `AD5422` channel's source selector.
 * @return true only for source_index <= 1 (`AO0`/`AO1`) and source_flags
 *         one of the seven valid `R[2:0]` codes (all except `0x4`, which
 *         is reserved -- see docs/datasheets/AD5422-register-notes.md).
 */
bool native_ad5422_source_is_valid(uint8_t source_index, uint8_t source_flags);

/**
 * @brief Start (enable output) one `AD5422` channel: writes that device's
 *        control register (`OUTEN=1`, `DCEN=1`, `R[2:0]` from
 *        `source_flags`, slew disabled), sending `NOP` for the other
 *        device's slot in the same 48-bit frame.
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid;
 *         TELEMETRY_DRIVER_RESULT_INVALID_STATE if no bus is bound;
 *         TELEMETRY_DRIVER_RESULT_TIMEOUT if the transport transfer fails;
 *         TELEMETRY_DRIVER_RESULT_OK otherwise.
 */
telemetry_driver_result_t native_ad5422_start(telemetry_channel_t *channel);
/**
 * @brief Stop (disable output, safe-state) one `AD5422` channel: writes
 *        that device's control register with `OUTEN=0` (and `DCEN=1`,
 *        `R[2:0]` preserved), sending `NOP` for the other device's slot.
 *        Not a no-op -- see the file-level doc.
 * @return Same result codes as native_ad5422_start().
 */
telemetry_driver_result_t native_ad5422_stop(telemetry_channel_t *channel);
/**
 * @brief Write @p value (engineering units matching the channel's
 *        configured range -- volts for the four voltage ranges, mA for
 *        the three current ranges) to this channel's data register,
 *        converting via the inverse of the datasheet's scaling formula
 *        (docs/datasheets/AD5422-register-notes.md), clamped to the valid
 *        16-bit code range.
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid,
 *         @p value is NULL, or the channel's value type is neither FLOAT
 *         nor DOUBLE; TELEMETRY_DRIVER_RESULT_INVALID_STATE if no bus is
 *         bound; TELEMETRY_DRIVER_RESULT_TIMEOUT if the transfer fails;
 *         TELEMETRY_DRIVER_RESULT_OK otherwise.
 */
telemetry_driver_result_t native_ad5422_write(telemetry_channel_t *channel,
                                              const telemetry_channel_value_t *value);
/**
 * @brief Verify the chip echoes back distinct written data-register values
 *        via the datasheet's readback sequence, distinguishing three
 *        outcomes exactly like `native_ads8688_self_test()`:
 *
 * - TELEMETRY_DRIVER_RESULT_OK: both readbacks matched exactly written
 *   values (supported and passed).
 * - TELEMETRY_DRIVER_RESULT_TIMEOUT: constant/non-tracking readback --
 *   supported but hardware unavailable/not present (or, on the real
 *   board, possibly a structural MISO-wiring limitation -- see the
 *   file-level hardware caveat).
 * - TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE: readback changed but never
 *   matched -- present but malfunctioning.
 *
 * Restores the channel's actual commanded output value on exit regardless
 * of outcome (best-effort on transport failure). Does not change `OUTEN`.
 */
telemetry_driver_result_t native_ad5422_self_test(telemetry_channel_t *channel);

#endif
