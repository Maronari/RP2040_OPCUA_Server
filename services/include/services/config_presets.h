#ifndef SERVICES_CONFIG_PRESETS_H
#define SERVICES_CONFIG_PRESETS_H

/**
 * @file
 * @brief Compile-time catalog of runtime configuration presets and the
 *        entry point that applies one through the existing `config_service`
 *        transaction pipeline.
 *
 * Responsibilities:
 * - Own `g_config_preset_catalog` (`config_presets.c`), a fixed array of
 *   named presets mirroring `driver_catalog.c`'s fixed-array-of-descriptors
 *   shape: each entry names a preset and supplies a function that populates
 *   a `device_profile_config_t::channels` array for it.
 * - `config_presets_build()` resolves a preset id against a base
 *   configuration (normally the currently running one) into a full
 *   candidate `device_profile_config_t`, replacing only `channels`.
 * - `config_presets_apply()` is the sole apply entry point: it builds the
 *   candidate and feeds it straight into `config_service_replace_and_save()`
 *   -- the same validate/persist/activate/verify/commit transaction every
 *   other candidate configuration goes through. No parallel apply path
 *   exists.
 *
 * A runtime configuration preset (ROADMAP.md's v0.5.0 scope) is distinct
 * from a compile-time driver pack (ROADMAP.md 5.1): a preset only chooses
 * among drivers already compiled into this firmware image and is applied
 * through this normal configuration transaction, never a firmware update.
 *
 * v0.5.0 T8 scope decision 4 (docs/releases/v0.5.0.md): the mechanism was
 * proven against one real preset ("sensor_node") first, then "remote_io" and
 * "dev_benchmark" were appended as two more `g_config_preset_catalog`
 * entries -- neither needed any change to this header or to
 * `config_presets_build()`/`config_presets_apply()`, confirming the catalog
 * really is append-only as designed.
 *
 * Ownership: this module holds no persistent/mutable state; the catalog is
 * a read-only compile-time array.
 *
 * Concurrency: `config_presets_build()` only reads its arguments and writes
 * @p out_config; `config_presets_apply()` inherits whatever synchronization
 * `config_service_get()`/`config_service_replace_and_save()` already
 * provide.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/config_service.h"

/**
 * @brief Persisted/wire identifier for one runtime configuration preset.
 *
 * Only appended to, never renumbered -- mirrors `config_channel_driver_id_t`'s
 * convention (`driver_catalog.h`). Any future preset is added the same way:
 * one more value here plus one more `g_config_preset_catalog[]` entry
 * (`config_presets.c`); nothing else in this header or in the apply
 * pipeline needs to change.
 */
typedef enum
{
    CONFIG_PRESET_ID_NONE = 0,
    CONFIG_PRESET_ID_SENSOR_NODE = 1,
    CONFIG_PRESET_ID_REMOTE_IO = 2,
    CONFIG_PRESET_ID_DEV_BENCHMARK = 3,
} config_preset_id_t;

/**
 * @brief One entry in the compile-time preset catalog (`config_presets.c`).
 */
typedef struct
{
    uint8_t preset_id;
    const char *name;
    const char *description;
    /** @brief Populates a `PLATFORM_CHANNEL_LIMIT`-sized channel array (already
     * zeroed by the caller) with this preset's channel layout. */
    void (*populate_channels)(config_channel_definition_t *channels);
} config_preset_catalog_entry_t;

/** @brief Number of presets compiled into the catalog. */
size_t config_presets_count(void);
/** @brief Look up a catalog entry by its position in the catalog array. @return NULL if @p index is out of range. */
const config_preset_catalog_entry_t *config_presets_get_by_index(size_t index);
/** @brief Look up a catalog entry by its `config_preset_id_t`. @return NULL if @p preset_id is not in the catalog. */
const config_preset_catalog_entry_t *config_presets_get_by_id(uint8_t preset_id);
/** @brief Look up a catalog entry by its stable name (e.g. "sensor_node"). @return NULL if @p name is NULL or not found. */
const config_preset_catalog_entry_t *config_presets_get_by_name(const char *name);

/**
 * @brief Builds a full candidate configuration for @p preset_id: a copy of
 * @p base with `device_profile_config_t::channels` replaced by the preset's
 * channel layout. Every other field (identity, network, MQTT, diagnostics,
 * ...) is carried over from @p base unchanged.
 * @return true on success; false if @p preset_id is unknown or @p base /
 * @p out_config is NULL.
 */
bool config_presets_build(uint8_t preset_id,
                          const device_profile_config_t *base,
                          device_profile_config_t *out_config);

/**
 * @brief Resolves @p preset_id, builds its candidate configuration onto the
 * currently running configuration (`config_service_get()`), and applies it
 * through `config_service_replace_and_save()` -- the existing validate ->
 * persist staged -> stop -> activate -> verify -> commit/rollback
 * transaction (see `services/config_service.h`).
 * @return true if the preset was resolved and the transaction committed;
 * false otherwise (running configuration unchanged).
 */
bool config_presets_apply(uint8_t preset_id);

#endif
