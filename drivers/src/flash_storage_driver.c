#include "drivers/flash_storage_driver.h"

bool flash_storage_driver_load(void *buffer, size_t size)
{
    (void)buffer;
    (void)size;
    return false;
}

bool flash_storage_driver_save(const void *buffer, size_t size)
{
    (void)buffer;
    (void)size;
    return false;
}
