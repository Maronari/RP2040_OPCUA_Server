/**
 * @file
 * @brief Config-store payload-format/schema-version constants and the
 * `config_store_record_info_t.flags` bit values, shared between
 * `config_store_service.c` and `config_store_limits.h` (payload size
 * bound).
 *
 * This is the *current* schema only — frozen prior-version binary layouts
 * for migration live in `services/config_compat.h`, not here.
 */
#ifndef SERVICES_CONFIG_WIRE_H
#define SERVICES_CONFIG_WIRE_H

#include <stdint.h>

/**
 * @brief `schema_version` value every currently-written config record and
 * JSON export/import document is stamped with.
 *
 * Unlike `CONFIG_STORE_SLOT_HEADER_VERSION` (the header's own struct-layout
 * version), this identifies the *payload's* shape and is the key
 * `config_compat_decode_versioned()`'s migration-function table
 * (`services/config_compat.h`) dispatches on. Bump this whenever the
 * `device_profile_config_t`/JSON schema changes in a way that table needs to
 * recognize and either migrate or reject; add the new value as a table entry
 * rather than repurposing this one, so a firmware image can still tell an
 * old record apart from the new shape. `config_export_service.c`'s JSON
 * `schemaVersion` field and `config_validate_service.c`'s import-time schema
 * check both source from this same constant rather than a separate literal.
 */
#define CONFIG_STORE_SCHEMA_VERSION_CURRENT 1u
/** @deprecated Prior name for `CONFIG_STORE_SCHEMA_VERSION_CURRENT`; kept as
 * an alias so call sites outside this release's schema-versioning work keep
 * compiling unchanged. New code should reference
 * `CONFIG_STORE_SCHEMA_VERSION_CURRENT` directly. */
#define CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY CONFIG_STORE_SCHEMA_VERSION_CURRENT

/**
 * @brief Encoding of the bytes stored in a config-store slot's payload.
 * @note Only `CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY` is ever written by
 * this codebase's `config_store_save_payload()` callers (not statically
 * confirmed to be produced anywhere for JSON/CBOR); the JSON/CBOR values
 * are reserved for a future payload encoding.
 */
typedef enum
{
    CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY = 0u,
    CONFIG_STORE_PAYLOAD_FORMAT_JSON = 1u,
    CONFIG_STORE_PAYLOAD_FORMAT_CBOR = 2u,
} config_store_payload_format_t;

/**
 * @brief Bit flags for `config_store_record_info_t::flags`
 * (`services/config_store_service.h`).
 * @note `CONFIG_STORE_SLOT_FLAG_PENDING_APPLY` is defined but not
 * statically confirmed to be set or read anywhere in this codebase —
 * reserved for a future two-phase apply protocol.
 */
enum
{
    CONFIG_STORE_SLOT_FLAG_VALID = (1u << 0),
    CONFIG_STORE_SLOT_FLAG_PENDING_APPLY = (1u << 1),
};

#endif
