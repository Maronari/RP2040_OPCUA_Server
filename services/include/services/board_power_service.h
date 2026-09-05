/**
 * @file
 * @brief Board-power-service block for `RP2350_INDUSTRIAL_MVP_V1`
 *        (`HARDWARE_MVP.md`): sensor-power enable/fault control (4x
 *        `TPS2553`) and external-I2C isolation/recovery control (2x
 *        `TCA4307`), both wired entirely through the `TCAL9539` internal
 *        I2C GPIO expander's pins -- see
 *        `docs/datasheets/TCAL9539-register-notes.md` for the exact pin
 *        mapping this file interprets. Pre-hardware: source-complete and
 *        host/mock-tested, but not hardware qualified -- see `ROADMAP.md`
 *        section 5.2.
 *
 * Layering: this is a `services/` board-support module, **not** a
 * `native_channel_driver.h`-shaped telemetry channel driver -- sensor
 * power/I2C isolation control has no natural per-channel telemetry shape
 * and is not registered in `services/driver_catalog.h`. `MAX22190`'s
 * `/READY` line also happens to be wired through this same expander
 * (`P14`/`DI_READY_N`) purely as a board-diagnostic convenience -- this
 * service exposes it (`board_power_get_di_ready()`) but the `MAX22190`
 * channel driver itself (`drivers/max22190_driver.h`) does not depend on
 * this service in any way.
 *
 * Neither `TCA4307` nor `TPS2553` is a separately I2C-addressable device
 * on this board -- both are controlled/read purely through `TCAL9539`
 * GPIO pins, so no dedicated driver file exists for either; this service
 * is their only firmware representation.
 *
 * Bounded sensor-power-cycle sequencing: `board_power_start_sensor_power_cycle()`
 * disables a port immediately and schedules a single, fixed-duration
 * (`BOARD_POWER_CYCLE_OFF_MS`) re-enable, advanced by
 * `board_power_poll()` -- a one-shot sequence, not a retry loop; per
 * `ROADMAP.md`'s "one missing/failing peripheral must not block unrelated
 * channels" requirement, a port that faults again after re-enabling is
 * left enabled (its fault is observable via
 * `board_power_get_sensor_fault()`) rather than this service retrying
 * indefinitely on its own.
 *
 * Ownership: all state is process-wide file-static (`TCAL9539` output-
 * register shadow, per-port power-cycle timers) -- there is exactly one
 * `TCAL9539` in scope for this board.
 *
 * Concurrency: none; intended to be called only from a single service/
 * maintenance task context, matching this codebase's existing
 * single-writer convention for board-level state.
 */
#ifndef SERVICES_BOARD_POWER_SERVICE_H
#define SERVICES_BOARD_POWER_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/i2c_transport.h"

/** @brief The four board sensor-power ports, matching HARDWARE_MVP.md's SENSOR_PWR0..3 wiring (I2C0/I2C1/SPI/UART connectors). */
typedef enum
{
    BOARD_POWER_SENSOR_PORT_I2C0 = 0,
    BOARD_POWER_SENSOR_PORT_I2C1 = 1,
    BOARD_POWER_SENSOR_PORT_SPI = 2,
    BOARD_POWER_SENSOR_PORT_UART = 3
} board_power_sensor_port_t;

/** @brief The two external I2C isolation/recovery buses, each behind its own TCA4307. */
typedef enum
{
    BOARD_POWER_I2C_ISOLATION_BUS0 = 0,
    BOARD_POWER_I2C_ISOLATION_BUS1 = 1
} board_power_i2c_isolation_bus_t;

/** @brief Fixed off-duration (milliseconds) of a bounded sensor-power-cycle sequence; a first-cut policy, tunable at hardware bring-up. */
#define BOARD_POWER_CYCLE_OFF_MS 200u

/**
 * @brief Binds the I2C bus/address used by every subsequent call in this
 *        file (forwards to tcal9539_bind_bus() -- the TCAL9539 driver is
 *        an internal implementation detail of this service, not exposed
 *        separately).
 * @param bus Bus to bind; the pointer is retained, not copied.
 * @param address 7-bit TCAL9539 I2C address (see
 *        docs/datasheets/TCAL9539-register-notes.md's address caveat --
 *        not confirmed against this board's actual A1/A0 strap).
 */
void board_power_service_bind_bus(const i2c_bus_t *bus, uint8_t address);

/**
 * @brief Initializes the TCAL9539 into this board's known-safe pin
 *        configuration: every sensor-power enable and I2C isolation-enable
 *        pin driven to its disabled/safe level *before* the corresponding
 *        Configuration register bits are flipped to output (so no pin is
 *        ever transiently driven active), then Configuration registers
 *        set for this board's fixed input/output pin assignment.
 * @return false if no bus is bound or any register write fails; true
 *         otherwise. Idempotent -- safe to call again (e.g. after a
 *         detected I2C fault) to re-establish the known-safe state.
 */
bool board_power_service_init(void);

/**
 * @brief Enable or disable one sensor-power port's TPS2553 switch.
 * @return false if @p port is out of range or the register write fails.
 */
bool board_power_set_sensor_enable(board_power_sensor_port_t port, bool enable);
/**
 * @brief Read one sensor-power port's TPS2553 fault state.
 * @param out_fault_active Receives true if a fault is currently asserted
 *        (the underlying signal is electrically active-low; this function
 *        already inverts it).
 * @return false if @p port is out of range, @p out_fault_active is NULL,
 *         or the register read fails.
 */
bool board_power_get_sensor_fault(board_power_sensor_port_t port, bool *out_fault_active);

/**
 * @brief Begin a bounded power-cycle sequence for one sensor-power port:
 *        disables it immediately, then board_power_poll() re-enables it
 *        after BOARD_POWER_CYCLE_OFF_MS.
 * @return false if @p port is out of range or the initial disable write
 *         fails; true once the sequence is scheduled.
 */
bool board_power_start_sensor_power_cycle(board_power_sensor_port_t port, uint32_t now_ms);
/**
 * @brief Advance any in-progress power-cycle sequences; re-enables a port
 *        whose scheduled off-window has elapsed. Cheap to call frequently
 *        (a no-op when no sequence is in progress); intended to be polled
 *        periodically from a maintenance/service task.
 */
void board_power_poll(uint32_t now_ms);

/**
 * @brief Enable or disable one external I2C bus's TCA4307 isolation
 *        buffer (connects/disconnects the external segment).
 * @return false if @p bus is out of range or the register write fails.
 */
bool board_power_set_i2c_isolation_enable(board_power_i2c_isolation_bus_t bus, bool enable);
/**
 * @brief Read one external I2C bus's TCA4307 READY state (active-high,
 *        per this signal's undecorated name unlike the "_N"-suffixed
 *        active-low signals elsewhere on this board).
 * @return false if @p bus is out of range, @p out_ready is NULL, or the
 *         register read fails.
 */
bool board_power_get_i2c_ready(board_power_i2c_isolation_bus_t bus, bool *out_ready);

/**
 * @brief Read the MAX22190 `/READY` state via TCAL9539 P14
 *        (`DI_READY_N`, active-low; this function already inverts it) --
 *        a board-diagnostic convenience only, not a dependency of the
 *        MAX22190 channel driver itself (see this header's file-level
 *        doc).
 * @return false if @p out_ready is NULL or the register read fails.
 */
bool board_power_get_di_ready(bool *out_ready);

#endif
