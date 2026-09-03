/**
 * @file
 * @brief Implementation of factory-reset scope parsing/execution declared
 * in `services/config_reset_service.h`.
 *
 * Ownership: no persistent state.
 * Concurrency: no internal synchronization; delegates entirely to the
 * synchronization of `config_service`/`config_store_service`/
 * `config_import_service`.
 */
#include "services/config_reset_service.h"

#include <string.h>

#include "services/config_import_service.h"
#include "services/config_service.h"
#include "services/config_store_service.h"

#define CONFIG_RESET_SCOPE_ALL_KEEP_IDENTITY_TEXT "all_keep_identity"

bool config_reset_service_parse_scope(const char *scope_text, config_reset_scope_t *out_scope)
{
    config_reset_scope_t scope = CONFIG_RESET_SCOPE_ALL_KEEP_IDENTITY;

    if (scope_text != NULL && scope_text[0] != '\0' &&
        strcmp(scope_text, CONFIG_RESET_SCOPE_ALL_KEEP_IDENTITY_TEXT) != 0)
    {
        scope = CONFIG_RESET_SCOPE_INVALID;
    }

    if (out_scope != NULL)
    {
        *out_scope = scope;
    }

    return scope != CONFIG_RESET_SCOPE_INVALID;
}

const char *config_reset_service_scope_to_string(config_reset_scope_t scope)
{
    switch (scope)
    {
    case CONFIG_RESET_SCOPE_ALL_KEEP_IDENTITY:
        return CONFIG_RESET_SCOPE_ALL_KEEP_IDENTITY_TEXT;
    case CONFIG_RESET_SCOPE_INVALID:
    default:
        return "invalid";
    }
}

bool config_reset_service_execute(config_reset_scope_t scope)
{
    if (scope != CONFIG_RESET_SCOPE_ALL_KEEP_IDENTITY)
    {
        return false;
    }

    if (!config_service_reset_to_defaults())
    {
        return false;
    }

    /* Order is deliberate and safety-critical, reversed from the pre-v0.5.0
     * sequence: the Flash store is wiped *before* defaults are committed.
     * config_service_apply() below unconditionally promotes whatever RUNNING
     * currently holds into LAST_KNOWN_GOOD as part of its own commit
     * (config_transaction_backend_commit()) -- if it ran first, against the
     * still-intact pre-reset RUNNING, LAST_KNOWN_GOOD would durably capture
     * the exact configuration this reset is meant to discard. A later
     * corrupted/erased RUNNING (power loss, bad sector, or nothing else ever
     * writing RUNNING again) would then auto-heal via
     * config_store_recover_fallback_to_last_known_good() on a future boot,
     * silently resurrecting it. Erasing every role first means RUNNING is not
     * CONFIG_TXN_COMMIT_COMMITTED when config_service_apply() commits
     * defaults, so config_transaction_backend_commit() skips the
     * promote-to-LAST_KNOWN_GOOD step entirely (see its own comment) and
     * LAST_KNOWN_GOOD stays empty -- there is nothing left for any later
     * failure to resurrect. */
    if (!config_store_reset())
    {
        return false;
    }

    if (!config_service_apply())
    {
        return false;
    }

    {
        config_import_service_result_t abort_result = config_import_service_abort();
        if (abort_result != CONFIG_IMPORT_RESULT_OK && abort_result != CONFIG_IMPORT_RESULT_NOT_ACTIVE)
        {
            return false;
        }
    }

    return true;
}
