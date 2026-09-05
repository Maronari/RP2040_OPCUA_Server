/**
 * @file
 * @brief Real RP2350 implementation of spi_bus_vtbl_t
 *        (drivers/include/drivers/rp2350_spi_bus.h). Not compiled into any
 *        host test target -- only the real firmware and this file's own
 *        (nonexistent) host test would need real hardware_spi/hardware_gpio
 *        headers; drivers depending only on rp2350_spi_bus.h stay host
 *        testable without them.
 */
#include "drivers/rp2350_spi_bus.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"

static bool g_spi_peripheral_initialized[2];

static spi_inst_t *spi_instance_handle(uint8_t spi_instance)
{
    return spi_instance == 0u ? spi0 : spi1;
}

bool rp2350_spi_bus_init(rp2350_spi_bus_state_t *state, const rp2350_spi_bus_config_t *config)
{
    if (state == NULL || config == NULL || config->spi_instance > 1u)
    {
        return false;
    }
    state->config = *config;
    if (!g_spi_peripheral_initialized[config->spi_instance])
    {
        spi_inst_t *instance = spi_instance_handle(config->spi_instance);

        (void)spi_init(instance, config->baudrate_hz);
        spi_set_format(instance, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);
        gpio_set_function(config->sck_gpio, GPIO_FUNC_SPI);
        gpio_set_function(config->mosi_gpio, GPIO_FUNC_SPI);
        gpio_set_function(config->miso_gpio, GPIO_FUNC_SPI);
        g_spi_peripheral_initialized[config->spi_instance] = true;
    }
    gpio_init(config->cs_gpio);
    gpio_set_dir(config->cs_gpio, GPIO_OUT);
    gpio_put(config->cs_gpio, 1u); /* idle high (inactive) */
    return true;
}

static void rp2350_spi_bus_cs_assert(void *context)
{
    const rp2350_spi_bus_state_t *state = (const rp2350_spi_bus_state_t *)context;
    gpio_put(state->config.cs_gpio, 0u);
}

static void rp2350_spi_bus_cs_deassert(void *context)
{
    const rp2350_spi_bus_state_t *state = (const rp2350_spi_bus_state_t *)context;
    gpio_put(state->config.cs_gpio, 1u);
}

static bool rp2350_spi_bus_transfer(void *context, const uint8_t *tx, uint8_t *rx, size_t length)
{
    const rp2350_spi_bus_state_t *state = (const rp2350_spi_bus_state_t *)context;
    spi_inst_t *instance = spi_instance_handle(state->config.spi_instance);
    int transferred;

    if (tx != NULL && rx != NULL)
    {
        transferred = spi_write_read_blocking(instance, tx, rx, length);
    }
    else if (tx != NULL)
    {
        transferred = spi_write_blocking(instance, tx, length);
    }
    else if (rx != NULL)
    {
        transferred = spi_read_blocking(instance, 0x00u, rx, length);
    }
    else
    {
        return length == 0u;
    }
    return transferred >= 0 && (size_t)transferred == length;
}

const spi_bus_vtbl_t rp2350_spi_bus_vtbl = {
    .cs_assert = rp2350_spi_bus_cs_assert,
    .cs_deassert = rp2350_spi_bus_cs_deassert,
    .transfer = rp2350_spi_bus_transfer,
};
