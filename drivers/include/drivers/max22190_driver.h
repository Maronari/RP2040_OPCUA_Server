/**
 * @file
 * @brief Native channel driver for the `MAX22190` industrial digital-input
 *        frontend (`RP2350_INDUSTRIAL_MVP_V1`'s `DI0..DI3`, see
 *        `HARDWARE_MVP.md` and `docs/releases/v0.5.1.md` Phase 1).
 *        Pre-hardware: source-complete and host/mock-tested, but not
 *        hardware qualified -- see `ROADMAP.md` section 5.2.
 *
 * Register/command reference: `docs/datasheets/MAX22190-register-notes.md`
 * (extracted from the official Analog Devices/Maxim Integrated datasheet).
 * The 5-bit CRC follows Analog Devices' "Guidelines to Implement CRC
 * Algorithm for the MAX22190 and MAX22192" (initial word `00111`,
 * polynomial `110101`) and is pinned by golden vectors computed with ADI's
 * reference code; see the register notes' CRC section. It is still not
 * confirmed against real silicon, so bring-up checks it first.
 *
 * Bus binding: depends only on `drivers/spi_transport.h`'s generic
 * `spi_bus_t`, never on `hardware_spi` directly.
 * `native_max22190_bind_bus()` supplies the bus; a host test binds a fake
 * one directly.
 *
 * One direct GPIO: `DI_READY_N` (`INDUSTRIAL_DI_READY_GPIO`, `GPIO24`),
 * the chip's open-drain active-low `/READY` output. It became a direct MCU
 * input in the 2026-09-10 `HARDWARE_MVP.md` revision (it previously reached
 * the MCU through the now-deleted `TCAL9539` expander), so this driver
 * reads it through `hardware/gpio.h` -- a host test shadows that header
 * with a stub, the same technique `ips2050_driver.c`'s test uses.
 *
 * Chip-level vs. per-channel startup: `DI0..DI3` are four logical views of
 * one physical chip. The `/READY` gate and `HARDWARE_MVP.md` section
 * 3.3.6's register initialization sequence therefore run **once**, latched
 * in file-static state, not once per channel; only the `FLTn` filter write
 * is genuinely per-channel. That write is itself a CRC-checked transaction,
 * so every channel still proves it can reach the chip.
 *
 * Board-fixed SPI mode: this driver only implements Mode 0 (24-bit frame,
 * CRC, no daisy chain) -- the mode is a hardware pin-strapping choice
 * (`M0`/`M1`, both tied to `GND_SYS` on this board per `HARDWARE_MVP.md`),
 * not software-selectable, so no other mode is implemented.
 *
 * Vtable contract (see telemetry_driver_vtbl_t): implements `start`/
 * `stop`/`read`/`self_test`. No `write` (read-only digital input, like
 * `gpio_input`/`ads8688`) and no `configure` (a channel's optional filter
 * override lives in `source_flags`, matching the v0.5.1 T1 finding).
 *
 * Ownership: `driver_context` carries only this channel's
 * `native_channel_source_t` (source_index = 0-3 for `DI0..DI3`;
 * source_flags = the channel's `FLTn` filter-register override byte, bits
 * 3-0 only). The SPI bus binding is process-wide file-static state, like
 * `ads8688_driver.c`'s/`ad5422_driver.c`'s.
 *
 * Concurrency: none; per `native_channel_driver.h`'s file-level doc, all
 * calls arrive serialized from SensorTask.
 */
#ifndef DRIVERS_MAX22190_DRIVER_H
#define DRIVERS_MAX22190_DRIVER_H

#include "drivers/native_channel_driver.h"
#include "drivers/spi_transport.h"

/**
 * @brief Binds the SPI bus this driver's channels use for every
 *        subsequent `start`/`read`/`self_test` call.
 *
 * Call once before activating any `MAX22190` channel. A host test binds a
 * fake `spi_bus_t` directly; the real firmware binds one backed by
 * `rp2350_spi_bus_init()`/`rp2350_spi_bus_vtbl` (`drivers/rp2350_spi_bus.h`)
 * -- not yet wired to any real board profile, see
 * `docs/releases/v0.5.1.md`.
 *
 * @param bus Bus to bind; the pointer is retained, not copied -- it must
 *        outlive every subsequent driver call. Pass NULL to unbind (every
 *        subsequent call then fails with TELEMETRY_DRIVER_RESULT_INVALID_STATE).
 */
void native_max22190_bind_bus(const spi_bus_t *bus);

/**
 * @brief Validate a `MAX22190` channel's source selector.
 * @return true only for source_index <= 3 (`DI0..DI3`, the four physically
 *         wired inputs on this board) and source_flags with no bits set
 *         above bit 3 (the `FLTn` register's `FBP`/`DELAY[2:0]` fields
 *         occupy only the low 4 bits -- see
 *         docs/datasheets/MAX22190-register-notes.md).
 */
bool native_max22190_source_is_valid(uint8_t source_index, uint8_t source_flags);

/**
 * @brief Start one `MAX22190` channel.
 *
 * On the first channel started since the bus was bound, this also performs
 * the one-time chip startup, `HARDWARE_MVP.md` section 3.3.6's frozen
 * sequence: configure `DI_READY_N` (`GPIO24`) as an input with internal
 * pulls disabled (the board's external `10k -> +3V3_DIG` pull-up already
 * holds the line) and wait for it to read low within a bounded number of
 * polls; read `FAULT1` (a real, CRC-checked transaction); write
 * `INEN = 0x0F`, `FLT1..FLT4 = 0x04`, `CFG = 0x01`; read `WB` and `FAULT2`
 * to clear stale diagnostics; write `FAULT2EN = 0x3C`, `GPO = 0x80`,
 * `FAULT1EN = 0xF8`; clear `FAULT1.POR`; then read every one of those
 * registers back and confirm `POR` is clear. Later channels skip straight
 * past that. Every call then writes this channel's `FLTn` filter register
 * from `source_flags`.
 *
 * A failed chip startup is not latched, so a later `start()` retries it.
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid;
 *         TELEMETRY_DRIVER_RESULT_INVALID_STATE if no bus is bound;
 *         TELEMETRY_DRIVER_RESULT_TIMEOUT if `/READY` never asserts within
 *         the poll bound, or if this channel's `FLTn` write fails;
 *         TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE if any transaction of the
 *         chip startup sequence fails (transport or response CRC), a
 *         register does not read back as written, or `POR` stays set;
 *         TELEMETRY_DRIVER_RESULT_OK otherwise.
 */
telemetry_driver_result_t native_max22190_start(telemetry_channel_t *channel);
/** @brief No-op; always returns TELEMETRY_DRIVER_RESULT_OK -- read-only input device, nothing to disable per channel. */
telemetry_driver_result_t native_max22190_stop(telemetry_channel_t *channel);
/**
 * @brief Read this channel's live digital input state.
 *
 * Issues a `DI`-register read frame; the response's first byte is always
 * the chip's live `DI[7:0]` status regardless of which register was
 * addressed (a MAX22190 protocol property, not specific to this command --
 * see the register notes). Extracts bit `source_index` from that byte.
 *
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid
 *         or the channel's value type is not BOOLEAN;
 *         TELEMETRY_DRIVER_RESULT_INVALID_STATE if no bus is bound;
 *         TELEMETRY_DRIVER_RESULT_TIMEOUT if the transfer fails;
 *         TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE if the response's own
 *         CRC does not check out; TELEMETRY_DRIVER_RESULT_OK otherwise,
 *         with `channel->runtime.value.boolean` updated.
 */
telemetry_driver_result_t native_max22190_read(telemetry_channel_t *channel, uint32_t now_ms);
/**
 * @brief Verify the chip echoes back distinct written `FLTn` values,
 *        distinguishing three outcomes exactly like the other v0.5.1
 *        SPI drivers' self_test:
 *
 * - TELEMETRY_DRIVER_RESULT_OK: both readbacks matched exactly written
 *   values (supported and passed).
 * - TELEMETRY_DRIVER_RESULT_TIMEOUT: constant/non-tracking readback --
 *   supported but hardware unavailable/not present.
 * - TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE: readback changed but never
 *   matched, or a response's CRC never checks out -- present but
 *   malfunctioning.
 *
 * Restores the channel's actual configured filter setting on exit
 * regardless of probe outcome; failed restore transport/readback cannot
 * return OK. TIMEOUT also covers transport errors, not only absent chips.
 */
telemetry_driver_result_t native_max22190_self_test(telemetry_channel_t *channel);

#endif
