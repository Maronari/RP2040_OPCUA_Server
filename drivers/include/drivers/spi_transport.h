/**
 * @file
 * @brief Minimal SPI bus transport contract, decoupling SPI-based
 *        industrial channel drivers' register/command logic from the
 *        RP2350 hardware_spi/hardware_gpio API so host tests can inject a
 *        mock bus without shadowing Pico SDK headers.
 *
 * Per-bus, purpose-built interface (SPI here). A unified cross-bus
 * (SPI/I2C/UART) transport was considered and explicitly rejected as
 * premature during v0.5.1 release planning -- see docs/releases/v0.5.1.md
 * scope decision 2; each bus gets its own minimal interface designed
 * against its first real consumer.
 */
#ifndef DRIVERS_SPI_TRANSPORT_H
#define DRIVERS_SPI_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Operation table for one SPI device's chip-select and full-duplex
 *        byte transfer.
 *
 * One instance binds to one physical chip-select line. Two devices sharing
 * the same underlying SPI peripheral (clock/MOSI/MISO) each get their own
 * `spi_bus_t` with distinct `context` (and therefore distinct
 * `cs_assert`/`cs_deassert` targets); the peripheral-level configuration
 * (baud rate, mode) is a board-binding concern, not part of this contract.
 */
typedef struct
{
    /** @brief Drives the chip-select line active (low). */
    void (*cs_assert)(void *context);
    /** @brief Drives the chip-select line inactive (high). */
    void (*cs_deassert)(void *context);
    /**
     * @brief Full-duplex transfer of @p length bytes, MSB-first per byte.
     * @param tx Bytes to transmit; NULL sends zero/don't-care bytes.
     * @param rx Destination for received bytes; NULL discards them.
     * @return true on success; false on a transport-level failure (bus
     *         error, timeout) -- distinct from a chip-level protocol
     *         mismatch, which the caller detects from the received data.
     */
    bool (*transfer)(void *context, const uint8_t *tx, uint8_t *rx, size_t length);
    /** Optional paired hooks. Hardware backends sharing a peripheral must
     * acquire its lock with bounded waiting before any CS edge. Both NULL
     * is valid only for a transport serialized by its caller (e.g. a mock). */
    bool (*acquire)(void *context);
    void (*release)(void *context);
} spi_bus_vtbl_t;

/** @brief One bound SPI device: an operation table plus its private context. */
typedef struct
{
    const spi_bus_vtbl_t *vtbl;
    void *context;
} spi_bus_t;

/**
 * @brief Convenience wrapper: assert CS, transfer, deassert CS.
 * @return false if @p bus, its vtbl, or any required vtbl entry is NULL, or
 *         if the transfer itself reports failure; CS is still deasserted on
 *         a transfer failure (never left asserted).
 */
bool spi_bus_transfer_framed(const spi_bus_t *bus, const uint8_t *tx, uint8_t *rx, size_t length);

#endif
