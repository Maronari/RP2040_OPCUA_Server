/**
 * @file
 * @brief Backward-compatibility decoder for prior on-Flash
 * `device_profile_config_t` binary layouts, used by `config_service_init()`
 * and `config_validate_service` to accept configuration records written by
 * older firmware without a forced factory reset.
 *
 * Responsibilities: recognize a persisted binary payload's size as one of
 * the current layout, the frozen v0.4.1/v0.4.2 layout (pre-alarm-tuning
 * `telemetry_channel_settings_t`), or the frozen four-channel v0.4.0 layout
 * (including its oldest short-`asset_id` variant), and expand it in place
 * into the current `device_profile_config_t` shape.
 *
 * This header intentionally exposes only the decode entry point and the two
 * frozen-size accessors used by callers that need to distinguish "this was
 * a migration" from "this was already current" without decoding twice.
 *
 * `config_compat_decode_versioned()` adds a `schema_version`-keyed migration
 * table on top of `config_compat_decode_binary()`: every record ever written
 * by this codebase (current and every frozen prior layout above) was stamped
 * with the same `CONFIG_STORE_SCHEMA_VERSION_CURRENT` value, since the field
 * had no real discriminating meaning before this table existed, so
 * `config_compat_decode_binary()`'s payload-size dispatch remains that
 * version's decode function unchanged — it is not superseded, only wrapped.
 * The table's job is the *next* schema bump: a future `schema_version` gets
 * its own table entry (a new migration function, or a rejection if no
 * migration is offered), and any value with no entry is refused outright
 * rather than blindly decoded.
 */
#ifndef SERVICES_CONFIG_COMPAT_H
#define SERVICES_CONFIG_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/config_service.h"

/** Channel slot count in every frozen v0.4.0 binary layout (the platform
 * channel limit grew after v0.4.0; current builds use `PLATFORM_CHANNEL_LIMIT`). */
#define CONFIG_COMPAT_V040_CHANNEL_LIMIT 4u

/**
 * @brief Decodes @p payload into @p out_config, auto-detecting its layout
 * by size: the current native layout, the frozen v0.4.1/v0.4.2 layout, or a
 * frozen v0.4.0 layout (either of its two on-Flash sizes).
 *
 * @param payload Raw persisted bytes; must not be NULL.
 * @param payload_size Size of @p payload in bytes; determines which layout
 * is assumed (exact match against one of the four known sizes; no other
 * size is accepted).
 * @param out_config Destination; must not be NULL. Fully overwritten on
 * success (unused/added fields, including the v0.4.3 lower-alarm-tuning
 * fields for a migrated record, are zero-initialized before being filled or
 * set to the migration sentinel — see `config_compat_expand_channel()`).
 * @param out_migrated Set to true if a non-native (older) layout was
 * decoded, i.e. the caller should re-persist @p out_config to upgrade the
 * stored record; set to false for a native-layout payload. Optional (may be
 * NULL).
 * @return true if @p payload_size matched a known layout and decoded
 * successfully; false for an unrecognized size or (for the oldest v0.4.0
 * variant) a channel array with an in-use slot beyond
 * `CONFIG_COMPAT_V040_CHANNEL_LIMIT`/`PLATFORM_CHANNEL_LIMIT`'s overlap that
 * can't be safely dropped.
 * @note If @p out_config aliases @p payload exactly (same address) and the
 * layout is already native, no copy is performed (fast path for in-place
 * buffers already typed as `device_profile_config_t`).
 */
bool config_compat_decode_binary(const uint8_t *payload,
                                 size_t payload_size,
                                 device_profile_config_t *out_config,
                                 bool *out_migrated);

/** @brief Returns the exact byte size of the frozen v0.4.0 on-Flash profile layout. */
size_t config_compat_v040_payload_size(void);
/** @brief Returns the exact byte size of the frozen v0.4.1/v0.4.2 on-Flash profile layout. */
size_t config_compat_v042_payload_size(void);

/**
 * @brief Signature shared by every entry in `config_compat_decode_versioned()`'s
 * migration table: decode/migrate @p payload into @p out_config, exactly
 * like `config_compat_decode_binary()`.
 */
typedef bool (*config_compat_migrate_fn)(const uint8_t *payload,
                                         size_t payload_size,
                                         device_profile_config_t *out_config,
                                         bool *out_migrated);

/**
 * @brief Returns true if @p schema_version has a decode entry in the
 * migration table (i.e. is not an unknown/future schema this firmware would
 * have to reject). Shared by every caller that needs to accept-or-reject a
 * declared schema version without necessarily decoding yet -- the Flash
 * boot path, JSON export/import, and `config_compat_decode_versioned()`
 * itself all agree on this one answer.
 */
bool config_compat_schema_version_is_supported(uint32_t schema_version);

/**
 * @brief Decodes @p payload according to @p schema_version, dispatching
 * through the migration-function table keyed on schema version instead of
 * assuming the current format.
 *
 * `CONFIG_STORE_SCHEMA_VERSION_CURRENT` dispatches to
 * `config_compat_decode_binary()`, which keeps deciding the concrete layout
 * (current, or a frozen pre-mechanism size) by @p payload_size exactly as it
 * does today -- that payload-size fallback is preserved unconditionally for
 * records predating this table, not replaced by it.
 *
 * @param schema_version Declared schema version to dispatch on (e.g. from a
 * Flash slot header or a JSON document's `schemaVersion` field).
 * @param payload,payload_size,out_config,out_migrated Forwarded to the
 * matched migration function; semantics match `config_compat_decode_binary()`.
 * @return false if @p schema_version has no table entry (unknown/future
 * schema, rejected without attempting to decode) or if the matched
 * function's own decode fails; true on successful decode.
 */
bool config_compat_decode_versioned(uint32_t schema_version,
                                    const uint8_t *payload,
                                    size_t payload_size,
                                    device_profile_config_t *out_config,
                                    bool *out_migrated);

#endif
