/**
 * @file
 * @brief Minimal I2C bus transport contract, decoupling I2C-based board
 *        service logic from the RP2350 hardware_i2c API so host tests can
 *        inject a mock bus without shadowing Pico SDK headers.
 *
 * Per-bus, purpose-built interface (I2C here), mirroring
 * drivers/spi_transport.h's shape and role. A unified cross-bus (SPI/I2C/
 * UART) transport was considered and explicitly rejected as premature
 * during v0.5.1 release planning -- see docs/releases/v0.5.1.md scope
 * decision 2; each bus gets its own minimal interface designed against its
 * first real consumer -- here the `TCAL9539` I2C GPIO expander, whose
 * driver was removed with the 2026-09-10 `HARDWARE_MVP.md` change that
 * deleted every I2C device from `RP2350_INDUSTRIAL_MVP_V1`. This contract
 * and its `rp2350_i2c_bus.h` binding are deliberately retained as driver-
 * SDK surface for the next I2C device; they have no in-tree consumer today.
 */
#ifndef DRIVERS_I2C_TRANSPORT_H
#define DRIVERS_I2C_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Operation table for one physical I2C bus.
 *
 * Devices are distinguished by the 7-bit @p addr parameter passed to each
 * call, not by binding -- unlike `spi_bus_t` (one chip-select per bound
 * instance), a single `i2c_bus_t` addresses every device on that physical
 * bus. Two devices on genuinely separate physical I2C buses (e.g. this
 * board's internal bus vs. an isolated external one) get their own
 * `i2c_bus_t` with distinct `context`.
 */
typedef struct
{
    /**
     * @brief Write @p length bytes to device @p addr, then STOP.
     * @return true on success; false on a transport-level failure (NACK,
     *         bus error, timeout, arbitration loss) -- distinct from a
     *         device-level protocol mismatch, which the caller detects
     *         from subsequent read data.
     */
    bool (*write)(void *context, uint8_t addr, const uint8_t *data, size_t length);
    /**
     * @brief Read @p length bytes from device @p addr (a bare read with a
     *        repeated START if immediately preceded by a @c write call on
     *        the same bus, per I2C convention for addressing a device's
     *        internal register pointer -- the vtbl implementation, not
     *        this contract, owns whether/how it issues a repeated START).
     * @return true on success; false on a transport-level failure.
     */
    bool (*read)(void *context, uint8_t addr, uint8_t *data, size_t length);
} i2c_bus_vtbl_t;

/** @brief One bound I2C bus: an operation table plus its private context. */
typedef struct
{
    const i2c_bus_vtbl_t *vtbl;
    void *context;
} i2c_bus_t;

/**
 * @brief Convenience wrapper: write a device's 8-bit register pointer, then
 *        read back @p length bytes (the common "set pointer, read" register
 *        access pattern used by auto-incrementing register files).
 * @return false if @p bus, its vtbl, or a required vtbl entry is NULL, or
 *         if either the pointer write or the read itself fails.
 */
bool i2c_bus_read_register(const i2c_bus_t *bus, uint8_t addr, uint8_t reg, uint8_t *data, size_t length);
/**
 * @brief Convenience wrapper: write a device's 8-bit register pointer
 *        immediately followed by @p length bytes of data, as one
 *        transaction (register pointer + data concatenated, the usual
 *        register-write framing).
 * @return false if @p bus, its vtbl, or `write` is NULL, @p length exceeds
 *         the internal framing buffer, or the write itself fails.
 */
bool i2c_bus_write_register(const i2c_bus_t *bus, uint8_t addr, uint8_t reg, const uint8_t *data, size_t length);

#endif
