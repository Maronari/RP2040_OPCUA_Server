/**
 * @file
 * @brief RP2350 hardware_i2c/hardware_gpio-backed implementation of
 *        i2c_bus_vtbl_t (drivers/include/drivers/i2c_transport.h) -- the
 *        board-binding layer real I2C-based board services attach to,
 *        mirroring rp2350_spi_bus.h's role for SPI.
 *
 * This header declares only types and prototypes; it does not include any
 * Pico SDK header, so a driver depending only on this header (not
 * rp2350_i2c_bus.c) stays host-testable. Only rp2350_i2c_bus.c and real
 * firmware wiring code need the actual hardware_i2c/hardware_gpio APIs.
 */
#ifndef DRIVERS_RP2350_I2C_BUS_H
#define DRIVERS_RP2350_I2C_BUS_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/i2c_transport.h"

/** @brief Pin/peripheral/baud-rate binding for one RP2350 hardware I2C bus. */
typedef struct
{
    uint8_t i2c_instance; /**< 0 or 1 (RP2350 hardware_i2c instance index). */
    uint8_t sda_gpio;
    uint8_t scl_gpio;
    uint32_t baudrate_hz;
} rp2350_i2c_bus_config_t;

/** @brief Per-instance storage for one bound RP2350 I2C bus. Zero-initialize before use. */
typedef struct
{
    rp2350_i2c_bus_config_t config;
} rp2350_i2c_bus_state_t;

/**
 * @brief Bind @p state to @p config: initializes the underlying
 *        hardware_i2c peripheral if not already running (a second device
 *        on the same @p i2c_instance shares one initialization -- mirrors
 *        rp2350_spi_bus_init()'s/adc_voltage_channel_driver.c's existing
 *        lazy-shared-peripheral-init pattern) and configures the SDA/SCL
 *        pins for I2C function.
 * @return true on success; false for @p i2c_instance > 1 or NULL arguments.
 */
bool rp2350_i2c_bus_init(rp2350_i2c_bus_state_t *state, const rp2350_i2c_bus_config_t *config);

/**
 * @brief i2c_bus_vtbl_t populated with this file's write/read
 *        implementations, for use with an rp2350_i2c_bus_state_t as the
 *        i2c_bus_t context.
 */
extern const i2c_bus_vtbl_t rp2350_i2c_bus_vtbl;

#endif
