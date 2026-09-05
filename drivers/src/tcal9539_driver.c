/**
 * @file
 * @brief Register-level implementation for the TCAL9539 I2C GPIO expander.
 *
 * See drivers/include/drivers/tcal9539_driver.h for the contract and the
 * standing I2C-address caveat.
 */
#include "drivers/tcal9539_driver.h"

static i2c_bus_t g_bus;
static uint8_t g_address;
static bool g_bound;

void tcal9539_bind_bus(const i2c_bus_t *bus, uint8_t address)
{
    if (bus != NULL)
    {
        g_bus = *bus;
        g_address = address;
        g_bound = true;
    }
    else
    {
        g_bound = false;
    }
}

bool tcal9539_read_register(uint8_t reg, uint8_t *value)
{
    if (!g_bound || value == NULL)
    {
        return false;
    }
    return i2c_bus_read_register(&g_bus, g_address, reg, value, 1u);
}

bool tcal9539_write_register(uint8_t reg, uint8_t value)
{
    if (!g_bound)
    {
        return false;
    }
    return i2c_bus_write_register(&g_bus, g_address, reg, &value, 1u);
}
