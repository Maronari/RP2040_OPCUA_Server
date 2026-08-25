/**
 * @file
 * @brief Internal-only header deriving the maximum config-store payload
 * size from the generated Flash layout, shared by `config_service.c`,
 * `config_store_service.c`, and `config_import_service.c` so they all bound
 * payloads identically without each duplicating the arithmetic.
 */
#ifndef SERVICES_CONFIG_STORE_LIMITS_H
#define SERVICES_CONFIG_STORE_LIMITS_H

#include <stddef.h>

#include <ota_layout.h>
#include <services/config_wire.h>

/** Largest payload (excluding `config_store_slot_header_t`) that fits in one
 * config-store Flash slot. */
#define CONFIG_STORE_MAX_PAYLOAD_SIZE_BYTES ((size_t)(OTA_LAYOUT_CONFIG_SLOT_SIZE_BYTES - sizeof(config_store_slot_header_t)))

#endif
