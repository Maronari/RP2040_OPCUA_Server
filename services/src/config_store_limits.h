#ifndef SERVICES_CONFIG_STORE_LIMITS_H
#define SERVICES_CONFIG_STORE_LIMITS_H

#include <stddef.h>

#include <ota_layout.h>
#include <services/config_wire.h>

#define CONFIG_STORE_MAX_PAYLOAD_SIZE_BYTES ((size_t)(OTA_LAYOUT_CONFIG_SLOT_SIZE_BYTES - sizeof(config_store_slot_header_t)))

#endif
