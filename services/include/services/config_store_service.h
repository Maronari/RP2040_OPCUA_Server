#ifndef SERVICES_CONFIG_STORE_SERVICE_H
#define SERVICES_CONFIG_STORE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/config_wire.h"

typedef struct
{
    size_t slot_index;
    uint16_t payload_format;
    uint32_t schema_version;
    uint32_t generation;
    uint32_t payload_size;
    uint32_t payload_crc32;
    uint32_t flags;
} config_store_record_info_t;

bool config_store_get_active_info(config_store_record_info_t *out_info);
bool config_store_load_active(void *buffer,
                              size_t buffer_size,
                              size_t *out_payload_size,
                              config_store_record_info_t *out_info);
bool config_store_save_payload(const void *payload,
                               size_t payload_size,
                               uint16_t payload_format,
                               uint32_t schema_version,
                               config_store_record_info_t *out_info);
bool config_store_reset(void);

#endif
