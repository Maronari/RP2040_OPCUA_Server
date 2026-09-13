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
 * `hardware_spi` directly. `native_ad5422_bind_bus()` supplies the bus; a
 * host test binds a fake one directly.
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
 * `stop`/`write`. No `read` (write-only analog output, like
 * `gpio_output`), no `configure` (the output range lives in the channel's
 * persisted `source_flags`, matching the v0.5.1 T1 finding that every real
 * `configure()` call site passes `NULL`), and no `self_test` -- see below.
 * `stop()` is not a no-op here: it disables the output (`OUTEN=0`), a real
 * safe-state action per `ROADMAP.md`'s "safe-state integration"
 * requirement.
 *
 * Ownership: `driver_context` carries only this channel's
 * `native_channel_source_t` (source_index = 0 for `AO0`, 1 for `AO1`;
 * source_flags = `R[2:0]` output-range code). The SPI bus binding is
 * process-wide file-static state, like `ads8688_driver.c`'s.
 *
 * Chain startup and `AO_ENABLE_REQ`: the first `start()` since the bus was
 * bound brings the whole chain up once -- software RESET of both DACs,
 * daisy-chain enable on both, then `AO_ENABLE_REQ` (`GPIO9`) raised to
 * release the hardware `CLEAR` -- and latches that in file-static state.
 * Until then `AO_ENABLE_REQ` stays low, so `CLEAR` holds both outputs.
 * That is the driver's only hardware access outside the SPI bus: it
 * includes `hardware/gpio.h` (for `AO_ENABLE_REQ`) and `hardware/timer.h`
 * (for the post-reset busy-wait), which a host test shadows with stubs.
 *
 * Why there is no `self_test`: the board gives the MCU no `SDO` path from
 * either DAC. `AO0`'s `SDO` feeds `AO1`'s `SDIN`, and `AO1`'s `SDO` ends
 * at a test point, deliberately not on `BOARD_SPI_MISO`
 * (`HARDWARE_MVP.md` section 3.2.2). Any SPI readback would sample a line
 * no DAC drives, so a readback self-test could never pass on real hardware
 * -- and the activation path would then treat both outputs as absent.
 * Runtime diagnosis for these devices is `BOARD_FAULT_N` (`GPIO19`, the
 * wired-OR of both DAC `FAULT` outputs and `MAX22190 /FAULT`), owned by
 * board diagnostics, not by this driver.
 *
 * Board range contract: only `0-10 V` (`R = 001`) and `4-20 mA`
 * (`R = 101`) are accepted -- `HARDWARE_MVP.md` section 3.2's frozen
 * targets. The other silicon ranges are rejected; the bipolar ones would
 * need a negative rail this board does not have (`AVSS` -> `AGND`).
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
 *        every subsequent `start`/`stop`/`write` call.
 *
 * Call once before activating either `AD5422` channel. A host test binds a
 * fake `spi_bus_t` directly; the real firmware binds one backed by
 * `rp2350_spi_bus_init()`/`rp2350_spi_bus_vtbl` (`drivers/rp2350_spi_bus.h`),
 * from `app/basic_profile/industrial_board_bindings.c`.
 *
 * @param bus Bus to bind; the pointer is retained, not copied -- it must
 *        outlive every subsequent driver call. Pass NULL to unbind (every
 *        subsequent call then fails with TELEMETRY_DRIVER_RESULT_INVALID_STATE).
 */
void native_ad5422_bind_bus(const spi_bus_t *bus);

/**
 * @brief Validate an `AD5422` channel's source selector.
 * @return true only for source_index <= 1 (`AO0`/`AO1`) and source_flags
 *         `0x01` (0-10 V) or `0x05` (4-20 mA), the two ranges
 *         `RP2350_INDUSTRIAL_MVP_V1` qualifies. Every other `R[2:0]` code,
 *         valid on the silicon or reserved, is rejected.
 */
bool native_ad5422_source_is_valid(uint8_t source_index, uint8_t source_flags);

/**
 * @brief Start (enable output) one `AD5422` channel, sending `NOP` for the
 *        other device's slot in every 48-bit frame: CONTROL with the output
 *        disabled (`DCEN=1`, `R[2:0]` from `source_flags`), DATA = zero
 *        scale, then CONTROL = `OUTEN | REXT | DCEN | R[2:0]`. `REXT` (the
 *        board's external 15.0 kOhm `RSET`) is set in the same write as
 *        `OUTEN`, as the datasheet recommends; `CLRSEL`, `OVRRNG` and slew
 *        stay 0. If the enabling write fails, the disabled word is sent
 *        again.
 *
 * On the first start since the bus was bound, this is preceded by chain
 * startup (`HARDWARE_MVP.md`'s AO firmware contract): drive
 * `AO_ENABLE_REQ` low; 24-bit RESET and 24-bit `CONTROL = DCEN` to `AO0`
 * (exactly 24 clocks, valid while it is still standalone); 48-bit RESET and
 * 48-bit `CONTROL = DCEN` to `AO1`; a busy-wait of at least 40 us after
 * each frame; then drive `AO_ENABLE_REQ` high. A failed chain startup
 * leaves `AO_ENABLE_REQ` low and is not latched.
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid;
 *         TELEMETRY_DRIVER_RESULT_INVALID_STATE if no bus is bound;
 *         TELEMETRY_DRIVER_RESULT_TIMEOUT if a transport transfer fails;
 *         TELEMETRY_DRIVER_RESULT_OK otherwise.
 */
telemetry_driver_result_t native_ad5422_start(telemetry_channel_t *channel);
/**
 * @brief Stop (disable output, safe-state) one `AD5422` channel: writes
 *        that device's control register with `OUTEN=0` and `REXT=0`
 *        (`DCEN=1` and `R[2:0]` preserved, so `AO1` stays reachable
 *        through `AO0`), sending `NOP` for the other device's slot.
 *        Not a no-op -- see the file-level doc. Before chain startup has
 *        succeeded there is nothing to disable (`CLEAR` still holds both
 *        outputs), so it returns TELEMETRY_DRIVER_RESULT_OK without a frame.
 *        `AO_ENABLE_REQ` is left high: it is shared with the other output.
 * @return Same result codes as native_ad5422_start().
 */
telemetry_driver_result_t native_ad5422_stop(telemetry_channel_t *channel);
/**
 * @brief Write @p value (engineering units matching the channel's
 *        configured range -- volts for 0-10 V, mA for 4-20 mA) to this
 *        channel's data register, converting via the inverse of the
 *        datasheet's scaling formula (docs/datasheets/AD5422-register-notes.md),
 *        clamped to the valid 16-bit code range.
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid,
 *         @p value is NULL, or the channel's value type is neither FLOAT
 *         nor DOUBLE; TELEMETRY_DRIVER_RESULT_INVALID_STATE if no bus is
 *         bound or chain startup has not yet succeeded (a 48-bit frame is
 *         not a valid write before it); TELEMETRY_DRIVER_RESULT_TIMEOUT if
 *         the transfer fails;
 *         TELEMETRY_DRIVER_RESULT_OK otherwise.
 */
telemetry_driver_result_t native_ad5422_write(telemetry_channel_t *channel,
                                              const telemetry_channel_value_t *value);

#endif
