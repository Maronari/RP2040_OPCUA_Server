#ifndef SERVICES_CONFIG_RESET_SERVICE_H
#define SERVICES_CONFIG_RESET_SERVICE_H

#include <stdbool.h>

typedef enum
{
    CONFIG_RESET_SCOPE_INVALID = 0,
    CONFIG_RESET_SCOPE_ALL_KEEP_IDENTITY = 1,
} config_reset_scope_t;

bool config_reset_service_parse_scope(const char *scope_text, config_reset_scope_t *out_scope);
const char *config_reset_service_scope_to_string(config_reset_scope_t scope);
bool config_reset_service_execute(config_reset_scope_t scope);

#endif
