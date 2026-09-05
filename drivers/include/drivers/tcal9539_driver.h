/**
 * @file
 * @brief Low-level register access for the `TCAL9539` internal I2C GPIO
 *        expander (`RP2350_INDUSTRIAL_MVP_V1`, see `HARDWARE_MVP.md` and
 *        `docs/datasheets/TCAL9539-register-notes.md`).
 *
 * This is **not** a `native_channel_driver.h`-shaped telemetry channel
 * driver -- the expander itself has no single "channel" meaning; its 16
 * pins serve several unrelated board functions (`SENSOR_PWR_EN0..3`,
 * `TCA4307` `EN`/`READY`, `MAX22190` `/READY`). This file only implements
 * the datasheet's register read/write protocol; pin-level meaning is
 * `services/board_power_service.c`'s responsibility (kept as an internal
 * implementation detail behind that service's own public API).
 *
 * **I2C address caveat**: the address bound via native_tcal9539_bind_bus()
 * is a placeholder (`0x74`, `A1=A0=L`) -- `HARDWARE_MVP.md` does not
 * document this board's actual `A1`/`A0` strap. See the register notes'
 * explicit caveat; this is the first thing to confirm during hardware
 * bring-up (a wrong address fails loudly -- every transaction NACKs).
 */
#ifndef DRIVERS_TCAL9539_DRIVER_H
#define DRIVERS_TCAL9539_DRIVER_H

#include "drivers/i2c_transport.h"

#define TCAL9539_REG_INPUT_PORT0 0x00u
#define TCAL9539_REG_INPUT_PORT1 0x01u
#define TCAL9539_REG_OUTPUT_PORT0 0x02u
#define TCAL9539_REG_OUTPUT_PORT1 0x03u
#define TCAL9539_REG_POLARITY_INVERSION0 0x04u
#define TCAL9539_REG_POLARITY_INVERSION1 0x05u
#define TCAL9539_REG_CONFIG_PORT0 0x06u
#define TCAL9539_REG_CONFIG_PORT1 0x07u

/**
 * @brief Binds the I2C bus and 7-bit device address used by every
 *        subsequent tcal9539_read_register()/tcal9539_write_register()
 *        call.
 * @param bus Bus to bind; the pointer is retained, not copied -- it must
 *        outlive every subsequent call. Pass NULL to unbind (every
 *        subsequent call then fails).
 * @param address 7-bit I2C device address (see this header's file-level
 *        caveat -- not confirmed against real hardware).
 */
void tcal9539_bind_bus(const i2c_bus_t *bus, uint8_t address);
/** @brief Read one register. @return false if unbound or the transport reports failure. */
bool tcal9539_read_register(uint8_t reg, uint8_t *value);
/** @brief Write one register. @return false if unbound or the transport reports failure. */
bool tcal9539_write_register(uint8_t reg, uint8_t value);

#endif
