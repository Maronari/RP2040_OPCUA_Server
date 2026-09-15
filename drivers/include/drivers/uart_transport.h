/**
 * @file
 * @brief Minimal half-duplex UART/RS-485 transport contract, decoupling
 *        raw byte transmit/receive from the RP2350 hardware_uart API so
 *        host tests can inject a mock bus without shadowing Pico SDK
 *        headers.
 *
 * Per-bus, purpose-built interface (UART/RS-485 here), mirroring
 * drivers/spi_transport.h's and drivers/i2c_transport.h's shape and role.
 * A unified cross-bus (SPI/I2C/UART) transport was considered and
 * explicitly rejected as premature during v0.5.1 release planning -- see
 * docs/releases/v0.5.1.md scope decision 2.
 *
 * Scope: this is a **raw transport binding only** -- half-duplex
 * send/receive with the RS-485 driver-enable line correctly sequenced.
 * No protocol (Modbus or otherwise) is implemented on top of it; Modbus
 * support remains scheduled for `v0.7.0` per `ROADMAP.md`.
 */
#ifndef DRIVERS_UART_TRANSPORT_H
#define DRIVERS_UART_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Operation table for one half-duplex UART/RS-485 bus.
 */
typedef struct
{
    /**
     * @brief Transmit @p length bytes. On a real RS-485 binding, asserts
     *        the driver-enable line before sending and does not release it
     *        (return to receive mode) until the UART has finished shifting
     *        out the last bit -- not merely queued into the FIFO -- so the
     *        line is never released mid-transmission.
     * @return true on success; false on a transport-level failure.
     */
    bool (*write)(void *context, const uint8_t *data, size_t length);
    /**
     * @brief Non-blocking: copy up to @p length already-received bytes
     *        into @p data.
     * @return The number of bytes actually copied (0 if none are
     *         currently available) -- never blocks waiting for more.
     */
    size_t (*read)(void *context, uint8_t *data, size_t length);
} uart_bus_vtbl_t;

/** @brief One bound UART/RS-485 bus: an operation table plus its private context. */
typedef struct
{
    const uart_bus_vtbl_t *vtbl;
    void *context;
} uart_bus_t;

/** @brief NULL-safe wrapper around @c vtbl->write. @return false if @p bus, its vtbl, or write is NULL, or the write itself fails. */
bool uart_bus_write(const uart_bus_t *bus, const uint8_t *data, size_t length);
/** @brief NULL-safe wrapper around @c vtbl->read. @return 0 if @p bus, its vtbl, or read is NULL, or none are currently available. */
size_t uart_bus_read(const uart_bus_t *bus, uint8_t *data, size_t length);

#endif
