/**
 * @file
 * @brief Small Flash-backed runtime/commissioning snapshot for the security
 * subsystem, independent of `security_service`'s own on-Flash store.
 *
 * Responsibilities:
 * - Owns a single `security_state_store_t` record in its own reserved Flash
 *   region (`OTA_LAYOUT_SECURITY_STATE_ORIGIN`, distinct from
 *   `security_service`'s `OTA_LAYOUT_SECURITY_STORE_ORIGIN`).
 * - Tracks commissioning mode, trust-list version, commissioning
 *   entered/deadline timestamps, and identity/trust "generation" counters
 *   (active vs. pending) used by the import services to detect whether a
 *   newly-imported identity/trust list has actually taken effect.
 *
 * Ownership: `security_service` is the primary writer — it keeps this store
 * in sync after every mutation to its own store (see
 * `security_service_sync_state_store()` in `security_service.c:591`).
 * `security_identity_import_service` and `security_trust_import_service`
 * update the generation counters directly after applying an import.
 *
 * Concurrency: no internal locking; single-task-context use assumed, same as
 * `security_service`. All mutators read-modify-write the whole
 * `security_state_store_t` record and erase+reprogram the entire reserved
 * region on every change (no in-place partial writes).
 */
#ifndef SERVICES_SECURITY_STATE_SERVICE_H
#define SERVICES_SECURITY_STATE_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Point-in-time copy of the persisted security-state record.
 *
 * `active_*_generation`/`pending_*_generation` are monotonically increasing
 * counters bumped by the identity/trust import services; "pending" tracks
 * the generation about to be applied (and is only meaningfully distinct from
 * "active" while an import is being staged/applied).
 */
typedef struct
{
    bool commissioning_mode;
    uint32_t trust_list_version;
    uint32_t commissioning_entered_epoch_s;
    /** Reserved for a future commissioning-mode timeout; not currently set to a non-zero value by any writer. */
    uint32_t commissioning_deadline_epoch_s;
    uint32_t active_identity_generation;
    uint32_t active_trust_generation;
    uint32_t pending_identity_generation;
    uint32_t pending_trust_generation;
    /** Reserved for a future fault-code field; not currently written by any caller in this codebase. */
    uint32_t last_security_fault_code;
} security_state_service_snapshot_t;

/**
 * @brief Loads the persisted security-state store, or seeds it with defaults
 * if absent/corrupt (invalid magic/version/CRC32).
 *
 * @param default_commissioning_mode Initial commissioning-mode value used
 * only when seeding a fresh store.
 * @param default_trust_list_version Initial trust-list-version value used
 * only when seeding a fresh store.
 * @return true if a valid store is now in place (existing or freshly
 * seeded); false on Flash commit failure.
 * @note Performs Flash erase/program via `flash_operation_coordinator` when
 * seeding.
 */
bool security_state_service_init(bool default_commissioning_mode,
                                 uint32_t default_trust_list_version);
/**
 * @brief Copies the full current state snapshot.
 * @return false if the store has not been initialized (`security_state_service_init()`
 * not yet called or failed) or @p out_snapshot is NULL.
 */
bool security_state_service_get_snapshot(security_state_service_snapshot_t *out_snapshot);
/** @return false if the store is not initialized. */
bool security_state_service_get_commissioning_mode(bool *out_enabled);
/**
 * @brief Sets commissioning mode and, when enabling, records
 * `commissioning_entered_epoch_s` from the current system time.
 * @return true without writing if the value is unchanged; false on Flash
 * commit failure or if the store is not initialized.
 * @note Performs a full Flash erase+program of the reserved region.
 */
bool security_state_service_set_commissioning_mode(bool enabled);
/** @return false if the store is not initialized. */
bool security_state_service_get_trust_list_version(uint32_t *out_version);
/**
 * @brief Overwrites the trust-list-version counter.
 * @return true without writing if unchanged; false on Flash commit failure
 * or if the store is not initialized.
 */
bool security_state_service_set_trust_list_version(uint32_t version);
/**
 * @brief Overwrites the active/pending trust-generation counters (used by
 * `security_trust_import_service` to publish that an import has landed).
 * @return true without writing if unchanged; false on Flash commit failure
 * or if the store is not initialized.
 */
bool security_state_service_set_trust_generations(uint32_t active_generation,
                                                  uint32_t pending_generation);
/**
 * @brief Overwrites the active/pending identity-generation counters (used by
 * `security_identity_import_service` to publish that an import has landed).
 * @return true without writing if unchanged; false on Flash commit failure
 * or if the store is not initialized.
 */
bool security_state_service_set_identity_generations(uint32_t active_generation,
                                                     uint32_t pending_generation);

#endif
