/**
 * @file
 * @brief Board-level diagnostics collection for `RP2350_INDUSTRIAL_MVP_V1`
 *        (`HARDWARE_MVP.md`): `PWR_IN_FAULT_N`, `PWR_3V3_PGOOD_N`,
 *        `PWR_5V_PGOOD_N`, `BOARD_FAULT_N` (four direct MCU GPIOs), plus
 *        `IPS2050` DO faults and `TCAL9539`-mediated sensor-power/I2C-
 *        isolation state (re-queried from their owning drivers/services,
 *        not re-implemented here). Pre-hardware: source-complete and
 *        host/mock-tested, but not hardware qualified -- see `ROADMAP.md`
 *        section 5.2.
 *
 * Scope, per `ROADMAP.md`'s Phase 1 backlog item 9: **driver/vtable-
 * internal only -- no OPC UA nodes.** This module exists to give a future
 * consumer (e.g. the status-LED logic `HARDWARE_MVP.md`'s LED table
 * describes -- `FAULT`/`SENS`/`PWR` LEDs derived from exactly these
 * signals -- not implemented by this release) one place to collect the
 * board's hardware health, not to publish it to OPC UA. Adding OPC UA
 * exposure for any of this is a distinct, not-yet-made decision (see
 * `ROADMAP.md`'s separately tracked `SupportedDrivers`/`DriverCapabilities`
 * model item) -- do not wire this into `opcua_model/` without that
 * decision being made first.
 *
 * Ownership: no persistent state of its own -- every
 * `board_diagnostics_collect()` call re-reads the four direct GPIOs fresh
 * and re-queries `ips2050_driver.h`/`board_power_service.h` directly; nothing
 * is cached between calls.
 *
 * Concurrency: none; intended to be called only from a single
 * service/maintenance task context, matching this codebase's existing
 * single-writer convention for board-level state.
 */
#ifndef SERVICES_BOARD_DIAGNOSTICS_SERVICE_H
#define SERVICES_BOARD_DIAGNOSTICS_SERVICE_H

#include <stdbool.h>

/**
 * @brief One collected snapshot of board-level diagnostic state. Every
 *        field defaults to `false` ("no fault"/"not ready") if its
 *        underlying read fails, so a single failing source never leaves a
 *        field in an indeterminate state -- matching `ROADMAP.md`'s "one
 *        missing/failing peripheral must not block unrelated channels"
 *        requirement applied to diagnostics collection itself.
 */
typedef struct
{
    bool power_input_fault;   /**< PWR_IN_FAULT_N (GPIO11), active-low, already inverted. */
    bool rail_3v3_fault;      /**< !PWR_3V3_PGOOD_N (GPIO34), active-low, already inverted. */
    bool rail_5v_fault;       /**< !PWR_5V_PGOOD_N (GPIO35), active-low, already inverted. */
    bool board_fault;         /**< BOARD_FAULT_N (GPIO19) -- wired-OR of both AD5422 FAULT outputs and MAX22190 /FAULT; active-low, already inverted. */
    bool do_fault[2];         /**< IPS2050 DO0/DO1 fault, via native_ips2050_read_fault(). */
    bool sensor_power_fault[4]; /**< TPS2553 sensor-power ports I2C0/I2C1/SPI/UART, via board_power_get_sensor_fault(). */
    bool i2c_isolation_ready[2]; /**< TCA4307 external I2C buses 0/1, via board_power_get_i2c_ready(). */
} board_diagnostics_snapshot_t;

/**
 * @brief Initializes the four direct-GPIO diagnostic inputs
 *        (`PWR_IN_FAULT_N`/`PWR_3V3_PGOOD_N`/`PWR_5V_PGOOD_N`/
 *        `BOARD_FAULT_N`) as inputs with internal pulls disabled -- each
 *        already has its own external `10k -> +3V3_DIG` pull-up
 *        (`HARDWARE_MVP.md`), so this driver does not duplicate it,
 *        matching that document's explicit convention.
 */
void board_diagnostics_service_init(void);

/**
 * @brief Collects a fresh snapshot of every diagnostic signal this module
 *        knows about. Best-effort: a failing underlying read (e.g.
 *        `board_power_service` not bound) leaves that field `false`
 *        rather than aborting the whole collection.
 * @return false only if @p out_snapshot is NULL; true otherwise.
 */
bool board_diagnostics_collect(board_diagnostics_snapshot_t *out_snapshot);

#endif
