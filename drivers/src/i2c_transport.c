/**
 * @file
 * @brief Implementation of drivers/i2c_transport.h's convenience wrappers.
 */
#include "drivers/i2c_transport.h"

#include <string.h>

/** @brief Upper bound on a single i2c_bus_write_register() payload -- generous for every known consumer (TCAL9539 registers are 1 byte). */
#define I2C_BUS_WRITE_REGISTER_MAX_LENGTH 8u

bool i2c_bus_read_register(const i2c_bus_t *bus, uint8_t addr, uint8_t reg, uint8_t *data, size_t length)
{
    if (bus == NULL || bus->vtbl == NULL || bus->vtbl->write == NULL || bus->vtbl->read == NULL)
    {
        return false;
    }
    if (!bus->vtbl->write(bus->context, addr, &reg, 1u))
    {
        return false;
    }
    return bus->vtbl->read(bus->context, addr, data, length);
}

bool i2c_bus_write_register(const i2c_bus_t *bus, uint8_t addr, uint8_t reg, const uint8_t *data, size_t length)
{
    uint8_t frame[1u + I2C_BUS_WRITE_REGISTER_MAX_LENGTH];

    if (bus == NULL || bus->vtbl == NULL || bus->vtbl->write == NULL || length > I2C_BUS_WRITE_REGISTER_MAX_LENGTH)
    {
        return false;
    }
    frame[0] = reg;
    if (length > 0u)
    {
        memcpy(&frame[1], data, length);
    }
    return bus->vtbl->write(bus->context, addr, frame, 1u + length);
}
