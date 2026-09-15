/**
 * @file
 * @brief Implementation of drivers/uart_transport.h's NULL-safe wrappers.
 */
#include "drivers/uart_transport.h"

bool uart_bus_write(const uart_bus_t *bus, const uint8_t *data, size_t length)
{
    if (bus == NULL || bus->vtbl == NULL || bus->vtbl->write == NULL)
    {
        return false;
    }
    return bus->vtbl->write(bus->context, data, length);
}

size_t uart_bus_read(const uart_bus_t *bus, uint8_t *data, size_t length)
{
    if (bus == NULL || bus->vtbl == NULL || bus->vtbl->read == NULL)
    {
        return 0u;
    }
    return bus->vtbl->read(bus->context, data, length);
}
