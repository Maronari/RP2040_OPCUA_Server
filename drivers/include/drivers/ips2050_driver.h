/**
 * @file
 * @brief Native channel driver for the `IPS2050HTR` protected high-side
 *        digital-output frontend (`RP2350_INDUSTRIAL_MVP_V1`'s `DO0..DO1`,
 *        see `HARDWARE_MVP.md` section 3.4 and `docs/releases/v0.5.1.md`
 *        Phase 1). Pre-hardware: source-complete and host/mock-tested, but
 *        not hardware qualified -- see `ROADMAP.md` section 5.2.
 *
 * Board topology (`HARDWARE_MVP.md` section 3.4.4, "Required DO firmware
 * contract from the 2026-09-02 IPS2050H schematic"): a plain GPIO frontend,
 * not a bus/protocol device -- no SPI/I2C/UART transport is involved.
 * `GPIO20`/`GPIO21` drive the schematic's `DO1_CMD`/`DO2_CMD` (this driver's
 * `source_index` 0/1, a 0-based renumbering of the schematic's 1-based
 * `DO1`/`DO2`, matching `MAX22190`'s `IN1..IN4` -> `DI0..DI3` convention).
 * `GPIO22`/`GPIO23` read back `DO1_FAULT`/`DO2_FAULT`, electrically
 * active-low (`0` = fault) per the schematic's `TLP383` optoisolator
 * interface and external `10k -> +3V3_DIG` pull-ups (this driver disables
 * the RP2350's own internal pulls on those pins to avoid a duplicate,
 * per `HARDWARE_MVP.md`'s explicit "do not duplicate ... pull-up" note).
 *
 * Hardware safe-state interlock: the actual physical output at each channel
 * is `DO_SAFE_EN AND DOx_CMD`, where `DO_SAFE_EN = SYS_RESET_N AND
 * DO_ENABLE_REQ` is generated entirely in hardware from `GPIO10`
 * (`DO_ENABLE_REQ`) and the board's reset supervisor -- this driver never
 * computes that AND itself, it only ever drives `DOx_CMD` and asserts the
 * shared `DO_ENABLE_REQ` once (see native_ips2050_start()). `GPIO10`,
 * `GPIO20` and `GPIO21` all have `100k -> GND_SYS` pull-downs on the MCU
 * sheet, so an uninitialized/reset MCU already presents a hardware-safe
 * (outputs off) state before firmware ever runs.
 *
 * `DO_ENABLE_REQ` (`GPIO10`) is a single physical line shared by both
 * channels -- like `ads8688_driver.c`'s/`ad5422_driver.c`'s shared SPI bus,
 * it is asserted (once) by whichever channel starts first and is
 * deliberately never de-asserted by a later stop() of just one channel
 * (mirrors the existing shared-peripheral convention in
 * `adc_voltage_channel_driver.c`/`rp2350_spi_bus.c`) -- safe because each
 * channel's own `DOx_CMD` staying LOW after its stop() already removes that
 * channel's output regardless of `DO_ENABLE_REQ`.
 *
 * Vtable contract (see telemetry_driver_vtbl_t): implements `start`/
 * `stop`/`write`/`self_test`. No `read` (a commanded/write-only channel,
 * like `gpio_output`/`ad5422` -- the v0.5.1 T1 finding that a channel with
 * no `read` must be skipped by sensor_manager's verify gate applies here
 * too) and no `configure` (nothing to configure: no per-channel flags
 * exist on this board -- polarity and safe-state wiring are fixed in
 * hardware, unlike `gpio_output`'s invert/open-drain options).
 *
 * `self_test()` is a genuine hardware fault readback (the schematic's own
 * `DOx_FAULT` line), not merely a self-consistency check of driven state
 * like `gpio_output`'s -- but unlike the SPI drivers' three-way self_test
 * (OK/TIMEOUT/HARDWARE_FAILURE), a GPIO read cannot fail to communicate, so
 * this driver only ever reports OK or HARDWARE_FAILURE.
 *
 * Scope note: this driver exposes each channel's *commanded* state (via
 * `write()`'s echo into `channel->runtime.value`) and *logical fault*
 * state (via `self_test()`, and via `native_ips2050_read_fault()` for a
 * non-channel caller -- see `services/board_diagnostics_service.h`, v0.5.1
 * T11); the shared `DO_ENABLE_REQ`/safe-enable state itself is not exposed
 * anywhere -- per `ROADMAP.md`'s Phase 1 backlog item 9, board-level
 * diagnostics are driver/vtable-internal only, never per-channel OPC UA
 * nodes.
 *
 * Ownership: `driver_context` carries only this channel's
 * `native_channel_source_t` (source_index = 0-1 for `DO0`/`DO1`;
 * source_flags must be 0 -- no per-channel configuration exists on this
 * board). The shared `DO_ENABLE_REQ` assertion state is process-wide
 * file-static state, like the ADC/SPI peripheral-ready flags.
 *
 * Concurrency: none; per `native_channel_driver.h`'s file-level doc, all
 * calls arrive serialized from SensorTask.
 */
#ifndef DRIVERS_IPS2050_DRIVER_H
#define DRIVERS_IPS2050_DRIVER_H

#include "drivers/native_channel_driver.h"

/**
 * @brief Validate an `IPS2050` channel's source selector.
 * @return true only for source_index <= 1 (`DO0`/`DO1`, the board's two
 *         protected high-side channels) and source_flags == 0 (no
 *         per-channel configuration exists on this board).
 */
bool native_ips2050_source_is_valid(uint8_t source_index, uint8_t source_flags);

/**
 * @brief Start one `IPS2050` channel: initializes this channel's `DOx_CMD`
 *        GPIO as an output driven LOW (safe/off) and its `DOx_FAULT` GPIO
 *        as an input with internal pulls disabled (the board already
 *        provides an external pull-up); on the first `IPS2050` channel to
 *        start (of either index), also asserts the shared `DO_ENABLE_REQ`
 *        GPIO -- see this header's file-level doc for why that assertion
 *        is never undone by a later per-channel stop().
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid;
 *         TELEMETRY_DRIVER_RESULT_OK otherwise.
 */
telemetry_driver_result_t native_ips2050_start(telemetry_channel_t *channel);
/**
 * @brief Stop one `IPS2050` channel: drives this channel's `DOx_CMD` GPIO
 *        LOW (a real safe-state action, like ad5422_driver.c's stop() --
 *        not a no-op). Does not touch the shared `DO_ENABLE_REQ` line; see
 *        this header's file-level doc.
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid;
 *         TELEMETRY_DRIVER_RESULT_OK otherwise.
 */
telemetry_driver_result_t native_ips2050_stop(telemetry_channel_t *channel);
/**
 * @brief Command one `IPS2050` channel's output on/off by driving its
 *        `DOx_CMD` GPIO.
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid,
 *         @p value is NULL, or the channel's value type is not BOOLEAN;
 *         TELEMETRY_DRIVER_RESULT_OK otherwise, with
 *         `channel->runtime.value.boolean` updated to the commanded state.
 */
telemetry_driver_result_t native_ips2050_write(telemetry_channel_t *channel, const telemetry_channel_value_t *value);
/**
 * @brief Read this channel's `DOx_FAULT` GPIO (a real hardware fault
 *        readback, electrically active-low).
 * @return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT if the source is invalid;
 *         TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE if the fault line is
 *         currently asserted; TELEMETRY_DRIVER_RESULT_OK otherwise.
 */
telemetry_driver_result_t native_ips2050_self_test(telemetry_channel_t *channel);
/**
 * @brief Raw diagnostic query: this channel's `DOx_FAULT` GPIO state,
 *        without requiring a `telemetry_channel_t` -- for a non-channel
 *        caller like `services/board_diagnostics_service.c` (v0.5.1 T11).
 *        Reads the same physical pin as `native_ips2050_self_test()`, just
 *        through a plainer signature.
 * @return false if @p source_index > 1 or @p out_fault_active is NULL;
 *         true otherwise, with @p *out_fault_active set (electrically
 *         active-low; this function already inverts it).
 */
bool native_ips2050_read_fault(uint8_t source_index, bool *out_fault_active);

#endif
