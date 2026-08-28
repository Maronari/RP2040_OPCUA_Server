/**
 * @file
 * @brief Public API for Flash-backed configuration record persistence,
 * active-record selection, and factory reset.
 *
 * As of v0.5.0 T3, this is a thin wrapper over
 * `drivers/config_transaction_backend`'s staged/running/last-known-good
 * roles (see that header for the physical commit-page-classification
 * contract) rather than the pre-v0.5.0 two-slot generation-MRU scheme --
 * `config_store_service.c`'s file-level doc comment has the delegation
 * details. The public contract below is unchanged from every existing
 * caller's point of view: `config_store_save_payload()` still makes its
 * payload the new active record immediately (staging and committing it in
 * one call; a real staged-then-later-commit orchestration is v0.5.0 T4's
 * job), and reads still transparently return the best available record.
 *
 * Responsibilities:
 * - Persist an opaque, caller-supplied byte payload (the encoded
 *   `device_profile_config_t`) as the new RUNNING record, transparently
 *   promoting the previous RUNNING record to LAST_KNOWN_GOOD first.
 * - Select the "active" record for reads as RUNNING if usable, else
 *   LAST_KNOWN_GOOD as a transparent fallback (`config_store_select_read_role`,
 *   `config_store_service.c`).
 * - Never interpret payload contents — schema decoding/migration is
 *   `services/config_compat.h`'s responsibility; validation is
 *   `services/config_validate_service.h`'s.
 *
 * Ownership: this module owns one static `config_transaction_backend_t`
 * instance, lazily initialized and recovered on first use; callers own all
 * payload buffers they pass in or receive.
 *
 * Concurrency: not internally synchronized beyond what
 * `config_transaction_backend`/`flash_operation_coordinator` already
 * provide for the underlying Flash mutations. Callers (`config_service`)
 * are themselves responsible for not invoking these functions concurrently
 * from multiple tasks for the same logical configuration.
 */
#ifndef SERVICES_CONFIG_STORE_SERVICE_H
#define SERVICES_CONFIG_STORE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/config_wire.h"

/**
 * @brief Metadata describing one config-store record, as read back from (or
 * about to be written to) the underlying `config_transaction_backend` role.
 *
 * `slot_index` is the serving `config_txn_role_t` (`CONFIG_TXN_ROLE_RUNNING`
 * or `CONFIG_TXN_ROLE_LAST_KNOWN_GOOD`) rather than a physical Flash slot
 * number -- no caller was found to depend on its pre-v0.5.0 exact numeric
 * meaning beyond a diagnostic display. `payload_crc32` covers only the
 * payload bytes (not the header). `flags` currently only ever carries
 * `CONFIG_STORE_SLOT_FLAG_VALID` (see `services/config_wire.h`).
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
 * @brief Returns metadata for the currently active record without reading
 * its payload.
 * @param out_info Filled with the active record's metadata on success; may
 * be NULL to just test whether an active record exists.
 * @return true if RUNNING or (transparently) LAST_KNOWN_GOOD has a
 * committed record; false if neither does.
 */
bool config_store_get_active_info(config_store_record_info_t *out_info);

/**
 * @brief Reads the active record's payload into @p buffer.
 * @param buffer Destination buffer; overwritten with the payload bytes on
 * success.
 * @param buffer_size Capacity of @p buffer; must be >= the active record's
 * `payload_size`, otherwise this call fails closed.
 * @param out_payload_size Set to the active record's payload size on
 * success; optional (may be NULL).
 * @param out_info Set to the active record's metadata on success; optional
 * (may be NULL).
 * @return true on success; false if @p buffer is NULL/zero-sized, no active
 * record exists, or @p buffer_size is too small.
 * @note Serves RUNNING if it is currently committed; otherwise
 * transparently falls back to LAST_KNOWN_GOOD if that is committed. This is
 * a read-time convenience only -- it does not itself physically re-promote
 * LAST_KNOWN_GOOD into RUNNING (see
 * `config_transaction_backend_recover_fallback_to_last_known_good()` for
 * that).
 */
bool config_store_load_active(void *buffer,
                              size_t buffer_size,
                              size_t *out_payload_size,
                              config_store_record_info_t *out_info);

/**
 * @brief Stages @p payload and immediately commits it, making it the new
 * RUNNING record on success (and, as a side effect, promoting the previous
 * RUNNING record to LAST_KNOWN_GOOD).
 *
 * @param payload Payload bytes to persist; must not be NULL.
 * @param payload_size Payload size in bytes; must be non-zero and
 * `<= CONFIG_STORE_MAX_PAYLOAD_SIZE_BYTES` (`config_store_limits.h`).
 * @param payload_format One of `config_store_payload_format_t`
 * (`services/config_wire.h`); stored verbatim, not interpreted here.
 * @param schema_version Caller-defined schema tag; stored verbatim.
 * @param out_info Filled with the newly written record's metadata on
 * success; optional (may be NULL).
 * @return true if the stage and commit both landed and physically
 * classified as committed; false on invalid arguments or any Flash
 * failure, in which case the previously active record is left untouched
 * (see `config_transaction_backend_stage()`/`_commit()`'s crash-safety
 * contract for exactly what "untouched" guarantees at every failure
 * point).
 * @note Every physical Flash write routes through
 * `drivers/config_transaction_backend`, which itself routes through the
 * shared `flash_operation_coordinator` -- this module performs no direct
 * Flash I/O of its own.
 */
bool config_store_save_payload(const void *payload,
                               size_t payload_size,
                               uint16_t payload_format,
                               uint32_t schema_version,
                               config_store_record_info_t *out_info);

/**
 * @brief Erases all three `config_transaction_backend` roles unconditionally.
 * @return true only if every role's erase succeeds; a failure partway
 * through leaves earlier roles already erased (no rollback).
 * @note Used by `config_reset_service_execute()` to drop persisted
 * configuration back to compiled-in defaults; destructive and irreversible.
 */
bool config_store_reset(void);

#endif
