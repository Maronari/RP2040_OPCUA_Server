/**
 * @file
 * @brief Implementation of `services/config_store_service.h`, delegating
 * every physical Flash operation to `drivers/config_transaction_backend`'s
 * staged/running/last-known-good roles (v0.5.0 T3).
 *
 * Responsibilities:
 * - Own a single lazily-initialized `config_transaction_backend_t` (Flash-
 *   backed on target via `config_transaction_backend_init_flash()`,
 *   recovered once on first use).
 * - Present the pre-v0.5.0 "write always becomes immediately active"
 *   contract this module's callers (`config_service.c`'s
 *   `config_service_apply()`/`config_service_replace_and_save()`, not yet
 *   restructured into a true staged-then-later-commit flow -- that is
 *   v0.5.0 T4's job) still expect: `config_store_save_payload()` stages the
 *   new payload and immediately commits it, which also promotes the
 *   previous running record to last-known-good as a side effect --
 *   strictly more crash-safety than the old two-slot generation scheme
 *   ever had, not less.
 * - `config_store_load_active()`/`config_store_get_active_info()` read the
 *   RUNNING role, transparently falling back to LAST_KNOWN_GOOD if RUNNING
 *   is not currently usable (mirrors the old scheme's "MRU across two
 *   slots" spirit) -- this is a read-time convenience only; it does not
 *   itself decide to physically re-promote LAST_KNOWN_GOOD (that policy
 *   choice belongs to whatever eventually consumes
 *   `config_transaction_backend_recover()`'s outcome for real transaction
 *   orchestration, i.e. v0.5.0 T4).
 *
 * `config_store_record_info_t::slot_index` is repurposed from "which of the
 * two physical slots" (0/1) to "which config_txn_role_t served this read"
 * (CONFIG_TXN_ROLE_RUNNING or CONFIG_TXN_ROLE_LAST_KNOWN_GOOD) -- no caller
 * was found to depend on its old exact numeric meaning beyond a diagnostic
 * display (`config_export_service.c`'s `activeSlot` JSON field).
 *
 * Ownership: `g_config_store_backend` is this module's only state.
 *
 * Concurrency: unchanged from before -- no internal synchronization here;
 * `config_transaction_backend`'s own Flash mutations all route through the
 * shared `flash_operation_coordinator` mutex.
 */
#include "services/config_store_service.h"

#include <string.h>

#include "drivers/config_transaction_backend.h"

static config_transaction_backend_t g_config_store_backend;
static bool g_config_store_backend_ready;

/** @brief Lazily initialize and recover the backend on first use; safe to call from every entry point below. */
static config_transaction_backend_t *config_store_backend(void)
{
    if (!g_config_store_backend_ready)
    {
        config_transaction_backend_init_flash(&g_config_store_backend);
        (void)config_transaction_backend_recover(&g_config_store_backend);
        g_config_store_backend_ready = true;
    }
    return &g_config_store_backend;
}

/** @brief Role a read should serve from: RUNNING if usable, else LAST_KNOWN_GOOD as a transparent fallback, else neither (caller reports "no active record"). */
static bool config_store_select_read_role(config_transaction_backend_t *backend, config_txn_role_t *out_role)
{
    const config_transaction_record_info_t *running = config_transaction_backend_role_info(backend, CONFIG_TXN_ROLE_RUNNING);
    const config_transaction_record_info_t *last_known_good =
        config_transaction_backend_role_info(backend, CONFIG_TXN_ROLE_LAST_KNOWN_GOOD);

    if (running != NULL && running->commit_state == CONFIG_TXN_COMMIT_COMMITTED)
    {
        *out_role = CONFIG_TXN_ROLE_RUNNING;
        return true;
    }
    if (last_known_good != NULL && last_known_good->commit_state == CONFIG_TXN_COMMIT_COMMITTED)
    {
        *out_role = CONFIG_TXN_ROLE_LAST_KNOWN_GOOD;
        return true;
    }
    return false;
}

/** @brief Translates a config_transaction_record_info_t into the public config_store_record_info_t shape. */
static void config_store_translate_info(config_txn_role_t role, const config_transaction_record_info_t *txn_info,
                                        config_store_record_info_t *out_info)
{
    out_info->slot_index = (size_t)role;
    out_info->payload_format = txn_info->payload_format;
    out_info->schema_version = txn_info->schema_version;
    out_info->generation = txn_info->generation;
    out_info->payload_size = txn_info->payload_size;
    out_info->payload_crc32 = txn_info->payload_crc32;
    out_info->flags = CONFIG_STORE_SLOT_FLAG_VALID;
}

bool config_store_get_active_info(config_store_record_info_t *out_info)
{
    config_transaction_backend_t *backend = config_store_backend();
    config_txn_role_t role;
    const config_transaction_record_info_t *txn_info;

    if (!config_store_select_read_role(backend, &role))
    {
        return false;
    }
    if (out_info != NULL)
    {
        txn_info = config_transaction_backend_role_info(backend, role);
        config_store_translate_info(role, txn_info, out_info);
    }
    return true;
}

bool config_store_load_active(void *buffer, size_t buffer_size, size_t *out_payload_size, config_store_record_info_t *out_info)
{
    config_transaction_backend_t *backend = config_store_backend();
    config_txn_role_t role;
    config_transaction_record_info_t txn_info;

    if (buffer == NULL || buffer_size == 0u || !config_store_select_read_role(backend, &role))
    {
        return false;
    }
    if (config_transaction_backend_read(backend, role, buffer, buffer_size, out_payload_size, &txn_info) != CONFIG_TXN_BACKEND_OK)
    {
        return false;
    }
    if (out_info != NULL)
    {
        config_store_translate_info(role, &txn_info, out_info);
    }
    return true;
}

/* Stage-then-immediately-commit: preserves the pre-v0.5.0 "write always
 * becomes the new active record" contract every existing caller depends on.
 * Crash-safety is strictly better than the old two-slot scheme: a torn
 * stage never becomes visible (see config_transaction_backend_stage()),
 * and a torn commit is recoverable via the newly-populated LAST_KNOWN_GOOD
 * role -- this module does not yet expose a separate "stage now, commit
 * later" entry point; that orchestration is v0.5.0 T4's job. */
bool config_store_save_payload(const void *payload, size_t payload_size, uint16_t payload_format,
                               uint32_t schema_version, config_store_record_info_t *out_info)
{
    config_transaction_backend_t *backend = config_store_backend();
    config_transaction_record_info_t running_info;

    if (config_transaction_backend_stage(backend, payload, payload_size, payload_format, schema_version, NULL) !=
        CONFIG_TXN_BACKEND_OK)
    {
        return false;
    }
    if (config_transaction_backend_commit(backend, &running_info) != CONFIG_TXN_BACKEND_OK)
    {
        return false;
    }
    if (out_info != NULL)
    {
        config_store_translate_info(CONFIG_TXN_ROLE_RUNNING, &running_info, out_info);
    }
    return true;
}

bool config_store_reset(void)
{
    config_transaction_backend_t *backend = config_store_backend();
    return config_transaction_backend_reset_all(backend) == CONFIG_TXN_BACKEND_OK;
}

bool config_store_stage_payload(const void *payload, size_t payload_size, uint16_t payload_format,
                                uint32_t schema_version, config_store_record_info_t *out_info)
{
    config_transaction_backend_t *backend = config_store_backend();
    config_transaction_record_info_t staged_info;

    if (config_transaction_backend_stage(backend, payload, payload_size, payload_format, schema_version,
                                         &staged_info) != CONFIG_TXN_BACKEND_OK)
    {
        return false;
    }
    if (out_info != NULL)
    {
        config_store_translate_info(CONFIG_TXN_ROLE_STAGED, &staged_info, out_info);
    }
    return true;
}

bool config_store_commit_staged(config_store_record_info_t *out_info)
{
    config_transaction_backend_t *backend = config_store_backend();
    config_transaction_record_info_t running_info;

    if (config_transaction_backend_commit(backend, &running_info) != CONFIG_TXN_BACKEND_OK)
    {
        return false;
    }
    if (out_info != NULL)
    {
        config_store_translate_info(CONFIG_TXN_ROLE_RUNNING, &running_info, out_info);
    }
    return true;
}

bool config_store_discard_staged(void)
{
    config_transaction_backend_t *backend = config_store_backend();
    return config_transaction_backend_discard_staged(backend) == CONFIG_TXN_BACKEND_OK;
}

config_transaction_recovery_state_t config_store_get_recovery_state(void)
{
    return config_store_backend()->recovery;
}

bool config_store_recover_fallback_to_last_known_good(config_store_record_info_t *out_info)
{
    config_transaction_backend_t *backend = config_store_backend();
    config_transaction_record_info_t running_info;

    if (config_transaction_backend_recover_fallback_to_last_known_good(backend, &running_info) !=
        CONFIG_TXN_BACKEND_OK)
    {
        return false;
    }
    if (out_info != NULL)
    {
        config_store_translate_info(CONFIG_TXN_ROLE_RUNNING, &running_info, out_info);
    }
    return true;
}

void config_store_service_init_with_storage(const config_transaction_storage_t *storage)
{
    config_transaction_backend_init(&g_config_store_backend, storage);
    (void)config_transaction_backend_recover(&g_config_store_backend);
    g_config_store_backend_ready = true;
}
