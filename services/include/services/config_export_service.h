/**
 * @file
 * @brief Public API for serializing configuration state to JSON: the
 * running/staged `device_profile_config_t` profiles (schema mirrored by
 * `config_validate_service`'s JSON parser) and a combined config/import
 * status snapshot for diagnostics/UI.
 *
 * Responsibilities: format the same public JSON schema that
 * `config_validate_service_validate_imported()` parses back, using a small
 * bounded-buffer streaming writer (`config_json_writer_t`,
 * `config_export_service.c`) rather than any dynamic allocation or
 * general-purpose JSON library.
 *
 * Ownership: no persistent state; every function writes into a
 * caller-supplied buffer.
 *
 * Concurrency: no internal synchronization; reads `config_service`/
 * `config_import_service`/`config_store_service` state through their own
 * public accessors, each of which is independently synchronized (or, for
 * `config_store_service`, not synchronized — see its header).
 */
#ifndef SERVICES_CONFIG_EXPORT_SERVICE_H
#define SERVICES_CONFIG_EXPORT_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Serializes the current running configuration
 * (`config_service_get()`) as JSON.
 * @param buffer Destination buffer; must not be NULL.
 * @param buffer_size Capacity of @p buffer.
 * @return true if the full JSON document fit in @p buffer; false on NULL
 * arguments or truncation. On truncation, @p buffer holds a partial,
 * NUL-terminated-up-to-the-truncation-point document (not valid JSON).
 */
bool config_export_service_write_running_json(char *buffer, size_t buffer_size);

/**
 * @brief Serializes the current staged (not-yet-applied) configuration
 * (`config_service_get_staged()`) as JSON.
 * @return Same contract as `config_export_service_write_running_json()`.
 */
bool config_export_service_write_staged_json(char *buffer, size_t buffer_size);

/**
 * @brief Serializes a combined configuration/import status snapshot as a
 * single-line JSON object: running/staged revisions, whether they differ,
 * persisted config-store slot metadata (if any), and the full
 * `config_import_service` session status.
 * @param buffer Destination buffer; must not be NULL.
 * @param buffer_size Capacity of @p buffer.
 * @param reboot_required Caller-supplied flag echoed into the
 * `"rebootRequired"` field (this module has no way to determine that
 * itself).
 * @return true if the JSON fit in @p buffer; false on NULL @p buffer,
 * zero @p buffer_size, or truncation (uses `snprintf()` internally, so a
 * truncated result is still NUL-terminated but incomplete JSON).
 */
bool config_export_service_write_state_json(char *buffer, size_t buffer_size, bool reboot_required);

#endif
