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

    if (!config_store_reset())
    {
        return false;
    }

    if (!config_service_reset_to_defaults())
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