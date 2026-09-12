/**
 * @file
 * @brief Native channel driver for the `RP2350_INDUSTRIAL_MVP_V1` universal
 *        protected input channels `UIC0..UIC3` (`HARDWARE_MVP.md` section
 *        3.5, `GPIO40..GPIO43`), supporting digital-input, pulse-count,
 *        frequency and period modes. Pre-hardware: source-complete and
 *        host/mock-tested, but not hardware qualified -- see `ROADMAP.md`
 *        section 5.2.
 *
 * **Scope decision, not a silent gap**: `ROADMAP.md`/`HARDWARE_MVP.md`
 * section 3.5 also lists a quadrature-encoder mode ("using channel
 * pairs"). That mode is **deliberately not implemented in this release**.
 * A correct RP2350 PIO quadrature decoder needs a materially more complex
 * state-machine program (a 2-bit-state transition table, several times the
 * instruction count of the single edge-counting loop this driver's other
 * modes use) than can be confidently hand-written and left unverified in a
 * pre-hardware, no-PIO-simulator environment -- unlike this driver's other
 * modes, a wrong quadrature decoder would not fail loudly (it would just
 * silently miscount or reverse direction under specific transition
 * sequences). Deferred to a tracked follow-up
 * (`docs/releases/v0.5.1.md` T6) rather than shipped unverified; the
 * `pio_edge_counter_t` abstraction this driver's other modes are built on
 * does not block adding it later.
 *
 * Board topology: `UIC0..UIC3` sit behind a `TLV9034PWR` comparator
 * frontend (`HARDWARE_MVP.md` section 3.5) that already produces a clean
 * digital signal on `GPIO40..GPIO43` -- this driver reads/counts that
 * digital signal directly. The UIC block is **input-only digital**: the
 * comparator path does not preserve analog amplitude, and
 * `GPIO40..GPIO43`'s ADC capability is explicitly not part of the UIC
 * hardware contract -- there is no analog or bidirectional UIC mode to
 * implement.
 *
 * **The frontend inverts** (`HARDWARE_MVP.md` section 3.5.1): an asserted
 * field input appears as a LOW level at the MCU, so every UIC channel is
 * electrically active-low. Digital-input mode inverts on read, so a
 * channel's value is the field state rather than the pin state. The
 * counter-based modes need no correction: the PIO counts one pin
 * low-to-high transition per field cycle either way (the field's falling
 * edge), which leaves frequency and period unchanged.
 *
 * Per-channel mode selection lives in `source_flags` (bits 0-2, matching
 * the v0.5.1 T1 finding that a channel with no runtime `configure()`
 * payload should carry its static configuration in `source_flags`):
 *
 * - `0` = digital input (direct `hardware/gpio.h` read, inverted for the
 *   active-low frontend, no PIO/counter involved at all -- value type
 *   `BOOLEAN`, `true` = field input asserted);
 * - `1` = pulse count (cumulative edge count since `start()`, value type
 *   `DOUBLE`);
 * - `2` = frequency in Hz, computed from the edge-count delta between
 *   successive `read()` calls and the caller-supplied `now_ms` delta
 *   (value type `DOUBLE`; reports `0.0` on the first read after `start()`,
 *   when no prior sample exists to diff against);
 * - `3` = period in seconds (`1/frequency`, using the same delta window;
 *   `0.0` whenever the computed frequency is `0.0`, including the first
 *   read after `start()`).
 *
 * Modes `1`-`3` share one underlying `pio_edge_counter_t` reading per
 * `read()` call -- `native_uic_bind_counter()` supplies the backing (a
 * host test binds a fake one; the real firmware binds one backed by
 * `rp2350_pio_edge_counter.[ch]`, not yet wired to any real board profile,
 * see `docs/releases/v0.5.1.md`).
 *
 * Vtable contract (see telemetry_driver_vtbl_t): implements `start`/
 * `stop`/`read` only. No `write`/`configure` (nothing to command or
 * runtime-reconfigure -- mode is fixed at config-apply time via
 * `source_flags`) and no `self_test` (no diagnostic signal exists to probe
 * for any of these modes, matching `gpio_input`'s convention of no
 * `self_test` either).
 *
 * Ownership: `driver_context` carries this channel's `native_channel_source_t`
 * (source_index = 0-3 for `UIC0..UIC3`; source_flags\[2:0\] = mode,
 * remaining bits must be 0). Per-channel counter-mode derived state (the
 * last-sampled edge count and timestamp, needed to compute frequency/
 * period deltas) lives in that same struct's `scratch.uic_derived` member
 * (v0.5.1 T19+ static-allocation audit correction) -- not a driver-private
 * file-static array, so a compiled-but-unconfigured `UIC` driver holds
 * none of it (`ROADMAP.md:602`).
 *
 * Concurrency: none; per `native_channel_driver.h`'s file-level doc, all
 * calls arrive serialized from SensorTask.
 */
#ifndef DRIVERS_UIC_DRIVER_H
#define DRIVERS_UIC_DRIVER_H

#include "drivers/native_channel_driver.h"
#include "drivers/pio_edge_counter.h"

/**
 * @brief Binds the edge-counter backing used by this driver's pulse-count/
 *        frequency/period modes (digital-input mode never touches it).
 * @param counter Backing to bind; the pointer is retained, not copied -- it
 *        must outlive every subsequent driver call using a counter-based
 *        mode. Pass NULL to unbind (every subsequent counter-mode call then
 *        fails with TELEMETRY_DRIVER_RESULT_INVALID_STATE).
 */
void native_uic_bind_counter(const pio_edge_counter_t *counter);

/**
 * @brief Validate a `UIC` channel's source selector.
 * @return true only for source_index <= 3 (`UIC0..UIC3`) and
 *         source_flags[2:0] <= 3 (a defined mode) with no bits set above
 *         bit 2.
 */
bool native_uic_source_is_valid(uint8_t source_index, uint8_t source_flags);

/**
 * @brief Start one `UIC` channel: digital-input mode initializes the GPIO
 *        as an input with internal pulls disabled (the comparator frontend
 *        already drives a clean digital level); counter-based modes start
 *        the bound `pio_edge_counter_t` for this channel's GPIO and reset
 *        this channel's derived frequency/period sampling state.
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid;
 *         TELEMETRY_DRIVER_RESULT_INVALID_STATE if a counter-based mode is
 *         selected and no counter backing is bound;
 *         TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE if the backing's own
 *         start() fails (e.g. no free PIO state machine on real hardware);
 *         TELEMETRY_DRIVER_RESULT_OK otherwise.
 */
telemetry_driver_result_t native_uic_start(telemetry_channel_t *channel);
/** @brief Stops a counter-based mode's backing (no-op for digital-input mode, matching gpio_input's no-op stop()). */
telemetry_driver_result_t native_uic_stop(telemetry_channel_t *channel);
/**
 * @brief Sample this channel per its configured mode -- see this header's
 *        file-level doc for the per-mode value/units.
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid
 *         or the channel's value type does not match its mode
 *         (BOOLEAN for digital input, DOUBLE for the counter-based modes);
 *         TELEMETRY_DRIVER_RESULT_INVALID_STATE if a counter-based mode is
 *         selected and no counter backing is bound;
 *         TELEMETRY_DRIVER_RESULT_OK otherwise.
 */
telemetry_driver_result_t native_uic_read(telemetry_channel_t *channel, uint32_t now_ms);

#endif
