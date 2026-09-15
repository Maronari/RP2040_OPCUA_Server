/**
 * @file
 * @brief Implementation of the industrial board bindings; see
 *        app/basic_profile/industrial_board_bindings.h for the contract.
 */
#include "industrial_board_bindings.h"

#include "drivers/industrial_board_pins.h"

#include <stddef.h>

/* FIRMWARE_DRIVERS exports these as PUBLIC compile definitions, so in a real
 * build they are always defined; they are defaulted to 0 here anyway, because
 * this file must never reference a driver that was not linked in. Spelled out
 * one by one rather than folded into a helper macro: `defined()` produced by a
 * macro expansion inside `#if` is undefined behavior. */
#ifndef FIRMWARE_ENABLE_NATIVE_ADS8688_DRIVER
#define FIRMWARE_ENABLE_NATIVE_ADS8688_DRIVER 0
#endif
#ifndef FIRMWARE_ENABLE_NATIVE_AD5422_DRIVER
#define FIRMWARE_ENABLE_NATIVE_AD5422_DRIVER 0
#endif
#ifndef FIRMWARE_ENABLE_NATIVE_MAX22190_DRIVER
#define FIRMWARE_ENABLE_NATIVE_MAX22190_DRIVER 0
#endif
#ifndef FIRMWARE_ENABLE_NATIVE_UIC_DRIVER
#define FIRMWARE_ENABLE_NATIVE_UIC_DRIVER 0
#endif

#if FIRMWARE_ENABLE_NATIVE_ADS8688_DRIVER || FIRMWARE_ENABLE_NATIVE_AD5422_DRIVER || \
    FIRMWARE_ENABLE_NATIVE_MAX22190_DRIVER
#define INDUSTRIAL_BOARD_HAS_SPI_DEVICE 1
#else
#define INDUSTRIAL_BOARD_HAS_SPI_DEVICE 0
#endif

#if INDUSTRIAL_BOARD_HAS_SPI_DEVICE
#include "drivers/rp2350_spi_bus.h"

/**
 * @brief Shared `BOARD_SPI` clock rate.
 *
 * The bus ceiling is the slowest device on it: `ADS8688` allows 17 MHz
 * (`fSCLK`), `AD5422` 30 MHz, `MAX22190` 10 MHz (`fSCLK`, `tSCLK` 100 ns) --
 * so 10 MHz. 8 MHz keeps a deliberate margin below that, because
 * `ADS8688`'s datasheet caps `SCLK` against conversion timing as well as by
 * `fSCLK`, and because no signal-integrity measurement can exist for a board
 * that has never been assembled. Derived from the datasheets, not measured:
 * bring-up establishes the real limit and may raise this
 * (`docs/releases/v0.5.1-hardware-bring-up-checklists.md`).
 */
#define INDUSTRIAL_BOARD_SPI_BAUD_HZ 8000000u

/**
 * @brief Build one chip-select binding on the shared industrial SPI bus.
 *
 * The clock mode is the device's own, and the SPI backend applies it per
 * transaction -- the three devices on `BOARD_SPI` do not agree on it:
 * - `ADS8688`: mode 1 (`CPOL=0`, `CPHA=1`) -- SDI latched on the falling
 *   SCLK edge (`docs/datasheets/ADS8688-register-notes.md`).
 * - `AD5422`: mode 0 -- "data is clocked in on the rising edge of SCLK",
 *   SDO updated on the falling edge (datasheet Rev. M, p.30).
 * - `MAX22190`: mode 0 -- the datasheet states `CPOL=0`, `CPHA=0`, and
 *   `HARDWARE_MVP.md` section 3.3.2 requires SCLK idle LOW.
 */
static bool industrial_bind_spi_device(rp2350_spi_bus_state_t *state, uint8_t cs_gpio, uint8_t cpol, uint8_t cpha)
{
    const rp2350_spi_bus_config_t config = {
        .spi_instance = INDUSTRIAL_BOARD_SPI_INSTANCE,
        .sck_gpio = INDUSTRIAL_BOARD_SPI_SCK_GPIO,
        .mosi_gpio = INDUSTRIAL_BOARD_SPI_MOSI_GPIO,
        .miso_gpio = INDUSTRIAL_BOARD_SPI_MISO_GPIO,
        .cs_gpio = cs_gpio,
        .cpol = cpol,
        .cpha = cpha,
        .baudrate_hz = INDUSTRIAL_BOARD_SPI_BAUD_HZ,
    };

    /* The peripheral itself is initialized by whichever device gets here
     * first; later devices only configure their own select line. */
    return rp2350_spi_bus_init(state, &config);
}
#endif

#if FIRMWARE_ENABLE_NATIVE_ADS8688_DRIVER
#include "drivers/ads8688_driver.h"
static rp2350_spi_bus_state_t g_ads8688_bus_state;
static const spi_bus_t g_ads8688_bus = {
    .vtbl = &rp2350_spi_bus_vtbl,
    .context = &g_ads8688_bus_state,
};
#endif

#if FIRMWARE_ENABLE_NATIVE_AD5422_DRIVER
#include "drivers/ad5422_driver.h"
static rp2350_spi_bus_state_t g_ad5422_bus_state;
static const spi_bus_t g_ad5422_bus = {
    .vtbl = &rp2350_spi_bus_vtbl,
    .context = &g_ad5422_bus_state,
};
#endif

#if FIRMWARE_ENABLE_NATIVE_MAX22190_DRIVER
#include "drivers/max22190_driver.h"
static rp2350_spi_bus_state_t g_max22190_bus_state;
static const spi_bus_t g_max22190_bus = {
    .vtbl = &rp2350_spi_bus_vtbl,
    .context = &g_max22190_bus_state,
};
#endif

#if FIRMWARE_ENABLE_NATIVE_UIC_DRIVER
#include "drivers/rp2350_pio_edge_counter.h"
#include "drivers/uic_driver.h"
/** The real PIO backing keeps all its state file-static, so it needs no context. */
static const pio_edge_counter_t g_uic_counter = {
    .vtbl = &rp2350_pio_edge_counter_vtbl,
    .context = NULL,
};
#endif

bool industrial_board_bindings_apply(void)
{
    bool ok = true;

#if FIRMWARE_ENABLE_NATIVE_ADS8688_DRIVER
    if (industrial_bind_spi_device(&g_ads8688_bus_state, INDUSTRIAL_ADC_CS_GPIO, 0u, 1u))
    {
        native_ads8688_bind_bus(&g_ads8688_bus);
    }
    else
    {
        ok = false;
    }
#endif

#if FIRMWARE_ENABLE_NATIVE_AD5422_DRIVER
    /* AO_LATCH is this device's select line: idle high, and the deassert
     * edge closing each transfer is the latching LOW->HIGH edge both
     * daisy-chained DACs share. */
    if (industrial_bind_spi_device(&g_ad5422_bus_state, INDUSTRIAL_AO_LATCH_GPIO, 0u, 0u))
    {
        native_ad5422_bind_bus(&g_ad5422_bus);
    }
    else
    {
        ok = false;
    }
#endif

#if FIRMWARE_ENABLE_NATIVE_MAX22190_DRIVER
    if (industrial_bind_spi_device(&g_max22190_bus_state, INDUSTRIAL_DI_CS_GPIO, 0u, 0u))
    {
        native_max22190_bind_bus(&g_max22190_bus);
    }
    else
    {
        ok = false;
    }
#endif

#if FIRMWARE_ENABLE_NATIVE_UIC_DRIVER
    /* Digital-input mode needs no counter; the counter-based modes fail
     * start() without this. Binding cannot fail -- it only records the vtbl,
     * and the PIO state machine is claimed later, per channel, in start(). */
    native_uic_bind_counter(&g_uic_counter);
#endif

    return ok;
}
