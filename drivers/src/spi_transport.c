/**
 * @file
 * @brief Bus-agnostic helper for drivers/include/drivers/spi_transport.h.
 *        No hardware access; owns only the assert/transfer/deassert framing
 *        every SPI device transaction needs regardless of chip.
 */
#include "drivers/spi_transport.h"

bool spi_bus_transfer_framed(const spi_bus_t *bus, const uint8_t *tx, uint8_t *rx, size_t length)
{
    bool result;

    if (bus == NULL || bus->vtbl == NULL || bus->vtbl->cs_assert == NULL ||
        bus->vtbl->cs_deassert == NULL || bus->vtbl->transfer == NULL)
    {
        return false;
    }
    bus->vtbl->cs_assert(bus->context);
    result = bus->vtbl->transfer(bus->context, tx, rx, length);
    bus->vtbl->cs_deassert(bus->context);
    return result;
}
