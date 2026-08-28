/**
 * @file
 * @brief Public API for the two-slot Flash-backed configuration record
 * store: raw payload persistence, active-slot selection, and factory reset.
 *
 * Responsibilities:
 * - Persist an opaque, caller-supplied byte payload (the encoded
 *   `device_profile_config_t`) into one of `CONFIG_STORE_SLOT_COUNT` (2)
 *   fixed Flash slots, each guarded by a `config_store_slot_header_t`
 *   (`services/config_wire.h`) with magic/CRC/generation.
 * - Select the "active" record as whichever valid slot has the highest
 *   generation counter (`config_store_select_active_record`,
 *   `config_store_service.c`).
 * - Never interpret payload contents — schema decoding/migration is
 *   `services/config_compat.h`'s responsibility; validation is
 *   `services/config_validate_service.h`'s.
 *
 * Ownership: this module owns no long-lived state beyond the target-only
 * page-write scratch buffer (`g_config_store_page`); callers own all
 * payload buffers they pass in or receive.
 *
 * Concurrency: not internally synchronized — this module has no static
 * mutable state that a caller can race on, but Flash program/erase
 * operations always serialize through the single global
 * `flash_operation_coordinator` mutex ([persistence.md](../../../docs/code/persistence.md)).
 * Callers (`config_service`) are themselves responsible for not invoking
 * these functions concurrently from multiple tasks for the same logical
 * configuration.
 */
#ifndef SERVICES_CONFIG_STORE_SERVICE_H
#define SERVICES_CONFIG_STORE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/config_wire.h"

/**
 * @brief Metadata describing one config-store record, as read back from (or
 * about to be written to) a slot header.
 *
 * `payload_crc32` covers only the payload bytes (not the header). `flags`
 * currently only carries `CONFIG_STORE_SLOT_FLAG_VALID`/
 * `CONFIG_STORE_SLOT_FLAG_PENDING_APPLY` (see `services/config_wire.h`).
 */
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

/**
 * @brief Returns metadata for the currently active slot without reading its
 * payload.
 * @param out_info Filled with the active record's metadata on success; may
 * be NULL to just test whether an active record exists.
 * @return true if at least one slot has a valid header+CRC; false if both
 * slots are invalid/erased.
 */
bool config_store_get_active_info(config_store_record_info_t *out_info);

/**
 * @brief Reads the active slot's payload into @p buffer.
 * @param buffer Destination buffer; zero-filled then partially overwritten
 * with the payload bytes.
 * @param buffer_size Capacity of @p buffer; must be >= the active record's
 * `payload_size`, otherwise this call fails closed.
 * @param out_payload_size Set to the active record's payload size on
 * success; optional (may be NULL).
 * @param out_info Set to the active record's metadata on success; optional
 * (may be NULL).
 * @return true on success; false if @p buffer is NULL/zero-sized, no active
 * record exists, or @p buffer_size is too small.
 * @note Reads the slot header/payload via direct XIP-mapped pointer
 * dereference, not through a driver read call — see
 * [persistence.md](../../../docs/code/persistence.md#Персистентность-конфигурации)
 * for the layering caveat this implies (writes go through
 * `flash_operation_coordinator`; reads do not).
 */
bool config_store_load_active(void *buffer,
                              size_t buffer_size,
                              size_t *out_payload_size,
                              config_store_record_info_t *out_info);

/**
 * @brief Writes @p payload to the inactive slot with a new, incremented
 * generation counter, making it the new active record on success.
 *
 * @param payload Payload bytes to persist; must not be NULL.
 * @param payload_size Payload size in bytes; must be non-zero and
 * `<= CONFIG_STORE_MAX_PAYLOAD_SIZE_BYTES` (`config_store_limits.h`).
 * @param payload_format One of `config_store_payload_format_t`
 * (`services/config_wire.h`); stored verbatim, not interpreted here.
 * @param schema_version Caller-defined schema tag; stored verbatim.
 * @param out_info Filled with the newly written record's metadata on
 * success; optional (may be NULL).
 * @return true if the target slot was erased and fully programmed; false on
 * invalid arguments or any Flash erase/program failure (in which case the
 * previously active slot is left untouched, since writes always target the
 * *other* slot).
 * @note Performs a Flash sector erase followed by page-by-page programming
 * via `flash_operation_erase_sector()`/`flash_operation_program_page()`
 * (`drivers/flash_operation_coordinator.h`) — this is the module's only
 * write path and is not transactional beyond "old slot remains valid until
 * the new slot's header is fully written".
 */
bool config_store_save_payload(const void *payload,
                               size_t payload_size,
                               uint16_t payload_format,
                               uint32_t schema_version,
                               config_store_record_info_t *out_info);

/**
 * @brief Erases both config-store slots unconditionally.
 * @return true only if both slot erases succeed; a failure on the second
 * slot leaves the first already erased (no rollback).
 * @note Used by `config_reset_service_execute()` to drop persisted
 * configuration back to compiled-in defaults; destructive and irreversible.
 */
bool config_store_reset(void);

#endif
