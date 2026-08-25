/**
 * @file
 * @brief Public API for factory-reset scope parsing and execution: the
 * only currently supported scope resets configuration to compiled-in
 * defaults, erases both config-store Flash slots, and drops any pending
 * import session.
 *
 * Ownership: no persistent state; delegates entirely to `config_service`,
 * `config_store_service`, and `config_import_service`.
 */
#ifndef SERVICES_CONFIG_RESET_SERVICE_H
#define SERVICES_CONFIG_RESET_SERVICE_H

#include <stdbool.h>

/**
 * @brief Supported/requested factory-reset scopes.
 * @note Despite the name, `CONFIG_RESET_SCOPE_ALL_KEEP_IDENTITY` resets the
 * *staged/running configuration* to the compiled-in default profile
 * wholesale (`config_service_reset_to_defaults()`) — "keep identity" refers
 * to hardware-derived identity fields (serial number, product instance
 * URI) being empty in the compiled default and therefore regenerated
 * identically from the board's unique ID during normalization
 * (`config_service_populate_serial_number()`), not to any selective
 * field-level preservation of a caller-set identity. This is currently the
 * only non-invalid scope value.
 */
typedef enum
{
    CONFIG_RESET_SCOPE_INVALID = 0,
    CONFIG_RESET_SCOPE_ALL_KEEP_IDENTITY = 1,
} config_reset_scope_t;

/**
 * @brief Parses a caller-supplied scope string (currently only
 * `"all_keep_identity"`) into a `config_reset_scope_t`.
 * @param scope_text Input string; NULL or empty is treated as the default
 * `CONFIG_RESET_SCOPE_ALL_KEEP_IDENTITY` (i.e. an absent/empty scope
 * argument is not an error).
 * @param out_scope Filled with the parsed scope (or
 * `CONFIG_RESET_SCOPE_INVALID` on an unrecognized non-empty string);
 * optional (may be NULL).
 * @return true if @p scope_text was empty/NULL or matched a known scope;
 * false for an unrecognized non-empty string.
 */
bool config_reset_service_parse_scope(const char *scope_text, config_reset_scope_t *out_scope);

/**
 * @brief Maps a `config_reset_scope_t` to its wire string.
 * @return `"all_keep_identity"` for the known scope; `"invalid"` otherwise.
 */
const char *config_reset_service_scope_to_string(config_reset_scope_t scope);

/**
 * @brief Executes a factory reset: stages and applies the compiled default
 * configuration, erases both config-store Flash slots, then aborts any
 * pending config-import session.
 *
 * @param scope Must be `CONFIG_RESET_SCOPE_ALL_KEEP_IDENTITY`; any other
 * value (including `CONFIG_RESET_SCOPE_INVALID`) fails immediately.
 * @return true only if every step succeeds, in order: staging defaults
 * (`config_service_reset_to_defaults()`), applying them
 * (`config_service_apply()`), erasing both Flash slots
 * (`config_store_reset()`), and aborting any active import session
 * (`config_import_service_abort()`, where "no session active" also counts
 * as success). false if any step fails, in which case earlier steps are
 * **not** rolled back (e.g. defaults may already be running even if the
 * subsequent Flash erase fails) — see the implementation comment in
 * `config_reset_service.c` for why the Flash erase intentionally comes
 * after the running configuration is already switched to defaults.
 * @note Performs a Flash erase (`config_store_reset()`) and, transitively
 * through `config_service_apply()`, may invoke the registered
 * runtime-apply callback.
 */
bool config_reset_service_execute(config_reset_scope_t scope);

#endif
