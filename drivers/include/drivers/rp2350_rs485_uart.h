/**
 * @file
 * @brief RP2350 hardware_uart/hardware_gpio-backed implementation of
 *        uart_bus_vtbl_t (drivers/include/drivers/uart_transport.h) for
 *        the `ISOW1432DFMR` isolated half-duplex RS-485 transceiver on
 *        `RP2350_INDUSTRIAL_MVP_V1` (`HARDWARE_MVP.md` section 9) --
 *        mirrors rp2350_spi_bus.h's/rp2350_i2c_bus.h's role for their
 *        buses. Pre-hardware: source-complete and host/mock-tested, but
 *        not hardware qualified -- see `ROADMAP.md` section 5.2.
 *
 * Board topology: `RS485_TX`/`RS485_RX` (this board's `GPIO36`/`GPIO37`)
 * are the UART data lines; `RS485_DE_RE` (`GPIO38`) is a single GPIO
 * driving **both** the `ISOW1432`'s `DE` (driver-enable) and `/RE`
 * (receiver-enable, active-low) pins tied together on this board (a
 * standard half-duplex wiring -- `DE`/`/RE` always move together: `1` =
 * transmit, `0` = receive). `HARDWARE_MVP.md`'s documented reset default
 * (`RS485_DE_RE -> 10k -> GND_SYS`) is `DE=0`/`/RE=0`: driver disabled,
 * receiver enabled -- this binding's `rp2350_rs485_uart_init()` explicitly
 * drives the same safe (receive) level rather than relying on the
 * external pull-down alone, matching every other board-safe-state
 * convention this release established (`gpio_output`/`IPS2050`/
 * `board_power_service`).
 *
 * **UART peripheral instance**: which RP2350 hardware UART instance
 * `GPIO36`/`GPIO37` route to via `GPIO_FUNC_UART` is a fixed silicon
 * pin-function assignment, not a board design choice --
 * `HARDWARE_MVP.md` documents the pin *names* (`RS485_TX`/`RS485_RX`) but
 * not which UART instance they belong to. Per the official RP2350
 * datasheet's GPIO Bank 0 function table (Table 3, QFN-80/RP2350B
 * variant, F2 column), `GPIO36`/`GPIO37` are `UART1 TX`/`UART1 RX` --
 * `rp2350_rs485_uart_config_t::uart_instance` should be `1` for this
 * board. Still supplied by the caller (board-init code) rather than
 * hardcoded here, matching every other board-binding config in this
 * codebase (`rp2350_spi_bus_config_t`/`rp2350_i2c_bus_config_t`).
 *
 * This header declares only types and prototypes; it does not include any
 * Pico SDK header, so a driver depending only on this header (not
 * rp2350_rs485_uart.c) stays host-testable.
 */
#ifndef DRIVERS_RP2350_RS485_UART_H
#define DRIVERS_RP2350_RS485_UART_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/uart_transport.h"

/** @brief Pin/peripheral/baud-rate binding for one RP2350 hardware UART used as a half-duplex RS-485 transport. */
typedef struct
{
    uint8_t uart_instance; /**< 0 or 1 (RP2350 hardware_uart instance index) -- see this header's file-level caveat. */
    uint8_t tx_gpio;
    uint8_t rx_gpio;
    uint8_t de_re_gpio;
    uint32_t baudrate_hz;
} rp2350_rs485_uart_config_t;

/** @brief Per-instance storage for one bound RP2350 RS-485 UART bus. Zero-initialize before use. */
typedef struct
{
    rp2350_rs485_uart_config_t config;
} rp2350_rs485_uart_state_t;

/**
 * @brief Bind @p state to @p config: initializes the hardware_uart
 *        peripheral (8N1), configures TX/RX pins for UART function, and
 *        drives the DE/RE GPIO to its safe receive level (0).
 * @return true on success; false for @p uart_instance > 1 or NULL
 *         arguments.
 */
bool rp2350_rs485_uart_init(rp2350_rs485_uart_state_t *state, const rp2350_rs485_uart_config_t *config);

/**
 * @brief uart_bus_vtbl_t populated with this file's write/read
 *        implementations, for use with an rp2350_rs485_uart_state_t as the
 *        uart_bus_t context.
 */
extern const uart_bus_vtbl_t rp2350_rs485_uart_vtbl;

#endif
