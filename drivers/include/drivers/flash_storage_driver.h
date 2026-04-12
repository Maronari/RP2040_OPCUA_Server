#ifndef DRIVERS_FLASH_STORAGE_DRIVER_H
#define DRIVERS_FLASH_STORAGE_DRIVER_H

#include <stdbool.h>
#include <stddef.h>

bool flash_storage_driver_load(void *buffer, size_t size);
bool flash_storage_driver_load_partial(void *buffer, size_t buffer_size, size_t *out_payload_size);
bool flash_storage_driver_save(const void *buffer, size_t size);

#endif
