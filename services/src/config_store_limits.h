/**
 * @file
 * @brief Internal-only header deriving the maximum config-store payload
 * size, shared by `config_service.c`, `config_store_service.c`, and
 * `config_import_service.c` so they all bound payloads identically without
 * each duplicating the arithmetic.
 *
 * As of v0.5.0 T3, `config_store_service.c` delegates every physical
 * write/read to `drivers/config_transaction_backend`'s staged/running/
 * last-known-good roles rather than owning two raw 64 KiB Flash slots
 * itself -- this bound now follows that backend's smaller (but still
 * >7x-margin, see that header) per-role capacity rather than the old
 * single-slot size, so every caller stays consistent with what a
 * `config_store_save_payload()` call can actually persist.
 */
#ifndef SERVICES_CONFIG_STORE_LIMITS_H
#define SERVICES_CONFIG_STORE_LIMITS_H

#include <stddef.h>

#include <drivers/config_transaction_backend.h>

/** Largest payload `config_store_save_payload()`/`config_import_service` can
 * actually persist -- see the file-level doc comment above. */
#define CONFIG_STORE_MAX_PAYLOAD_SIZE_BYTES ((size_t)CONFIG_TXN_MAX_PAYLOAD_SIZE_BYTES)

#endif
