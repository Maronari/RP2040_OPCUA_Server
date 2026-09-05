/**
 * @file
 * @brief RP2350 hardware_spi/hardware_gpio-backed implementation of
 *        spi_bus_vtbl_t (drivers/include/drivers/spi_transport.h) -- the
 *        board-binding layer real SPI-based industrial drivers attach to.
 *
 * This header declares only types and prototypes; it does not include any
 * Pico SDK header, so a driver depending only on this header (not
 * rp2350_spi_bus.c) stays host-testable. Only rp2350_spi_bus.c and real
 * firmware wiring code need the actual hardware_spi/hardware_gpio APIs.
 */
#ifndef DRIVERS_RP2350_SPI_BUS_H
#define DRIVERS_RP2350_SPI_BUS_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/spi_transport.h"

/** @brief Pin/peripheral/baud-rate binding for one RP2350 hardware SPI chip-select instance. */
typedef struct
{
    uint8_t spi_instance; /**< 0 or 1 (RP2350 hardware_spi instance index). */
    uint8_t sck_gpio;
    uint8_t mosi_gpio;
    uint8_t miso_gpio;
    uint8_t cs_gpio;
    uint32_t baudrate_hz;
} rp2350_spi_bus_config_t;

/** @brief Per-instance storage for one bound RP2350 SPI chip-select. Zero-initialize before use. */
typedef struct
{
    rp2350_spi_bus_config_t config;
} rp2350_spi_bus_state_t;

/**
 * @brief Bind @p state to @p config: initialize its chip-select GPIO (idle
 *        high) and initialize the underlying hardware_spi peripheral if not
 *        already running. A second device sharing the same @p spi_instance
 *        skips peripheral re-initialization and only configures its own CS
 *        pin -- mirrors the existing lazy-shared-peripheral-init pattern in
 *        drivers/src/adc_voltage_channel_driver.c.
 * @return true on success; false for @p spi_instance > 1 or NULL arguments.
 */
bool rp2350_spi_bus_init(rp2350_spi_bus_state_t *state, const rp2350_spi_bus_config_t *config);

/**
 * @brief spi_bus_vtbl_t populated with this file's cs_assert/cs_deassert/
 *        transfer implementations, for use with an rp2350_spi_bus_state_t
 *        as the spi_bus_t context.
 */
extern const spi_bus_vtbl_t rp2350_spi_bus_vtbl;

#endif
