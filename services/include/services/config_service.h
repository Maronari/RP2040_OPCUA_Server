/**
 * @file
 * @brief Public API for the device configuration service: running/staged
 * configuration state, staged-field mutators, and the stage-then-apply
 * transaction that activates a candidate configuration.
 *
 * Responsibilities:
 * - Own the in-memory running/staged/default copies of
 *   `device_profile_config_t` (backing storage lives in
 *   `services/src/config_service.c`).
 * - Normalize and validate staged edits before they can become the running
 *   configuration (validation delegated to `services/config_validate_service.h`).
 * - Persist accepted configuration through `services/config_store_service.h`.
 * - Invoke a runtime-owner-supplied callback so hardware/PubSub/OPC UA state
 *   can adopt a newly applied configuration synchronously.
 *
 * Ownership: `config_service` owns all `device_profile_config_t` storage
 * returned by `config_service_get()`/`config_service_get_staged()` — callers
 * never own or free these pointers, and the pointed-to contents can change
 * on a subsequent apply/reset (mutated in place under a critical section).
 *
 * Concurrency: internal state is protected with `taskENTER_CRITICAL()`/
 * `taskEXIT_CRITICAL()` (global critical section, not a mutex). Apply-style
 * operations (`config_service_apply()`, `config_service_replace_and_save()`)
 * additionally serialize through a single transaction flag so only one such
 * operation can be in flight at a time.
 */
#ifndef SERVICES_CONFIG_SERVICE_H
#define SERVICES_CONFIG_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/driver_catalog.h"
#include "services/platform_limits.h"
#include "services/service_types.h"
#include "services/telemetry_channel.h"

#define CONFIG_SERVICE_TEXT_MAX_LEN 64
#define CONFIG_SERVICE_URI_MAX_LEN 64
/* 40 Unicode code points require up to 160 bytes in UTF-8, plus NUL. */
#define CONFIG_SERVICE_ASSET_ID_MAX_LEN 161
#define CONFIG_SERVICE_LOCATION_MAX_LEN 48
#define CONFIG_SERVICE_LOCATION_PATH_MAX_LEN 192
#define CONFIG_SERVICE_LOCATION_MAX_DEPTH 8u
#define CONFIG_SERVICE_LOCATION_SEGMENT_MAX_LEN 48u
#define CONFIG_SERVICE_ASSET_CLASS_MAX_LEN 32
#define CONFIG_SERVICE_CHANNEL_ID_MAX_LEN 32
#define CONFIG_SERVICE_CHANNEL_NAME_MAX_LEN 32
#define CONFIG_SERVICE_CHANNEL_DESCRIPTION_MAX_LEN 64
#define CONFIG_SERVICE_CHANNEL_UNIT_MAX_LEN 16

/**
 * @brief One configurable telemetry channel slot within
 * `device_profile_config_t::channels`.
 *
 * Persisted verbatim as part of the binary configuration payload (see
 * `services/config_compat.h` for the frozen pre-v0.4.3 layout of
 * `settings`). `in_use == 0` marks the slot empty; `driver_id`/`kind`/
 * `value_type`/`source_index`/`source_flags` are validated against
 * `driver_catalog` before a candidate configuration can be applied.
 */
typedef struct
{
    uint8_t in_use;
    uint8_t kind;
    uint8_t value_type;
    uint8_t driver_id;
    uint8_t source_index;
    uint8_t source_flags;
    uint8_t has_minimum;
    uint8_t has_maximum;
    char id[CONFIG_SERVICE_CHANNEL_ID_MAX_LEN];
    char name[CONFIG_SERVICE_CHANNEL_NAME_MAX_LEN];
    char description[CONFIG_SERVICE_CHANNEL_DESCRIPTION_MAX_LEN];
    char unit[CONFIG_SERVICE_CHANNEL_UNIT_MAX_LEN];
    uint32_t flags;
    double minimum;
    double maximum;
    telemetry_channel_settings_t settings;
} config_channel_definition_t;

/**
 * @brief Input payload for `config_service_stage_set_identity()`: the subset
 * of device-identity fields an OPC UA/HTTP caller is allowed to stage in one
 * call. Field-for-field copied into the staged `device_profile_config_t`.
 */
typedef struct
{
    char device_name[CONFIG_SERVICE_TEXT_MAX_LEN];
    char opcua_hostname[CONFIG_SERVICE_TEXT_MAX_LEN];
    char product_instance_uri[CONFIG_SERVICE_URI_MAX_LEN];
    char serial_number[CONFIG_SERVICE_TEXT_MAX_LEN];
    char asset_id[CONFIG_SERVICE_ASSET_ID_MAX_LEN];
    char hw_revision[CONFIG_SERVICE_TEXT_MAX_LEN];
    char fw_revision[CONFIG_SERVICE_TEXT_MAX_LEN];
    int64_t manufacture_date_utc;
    int64_t calibration_date_utc;
    char location[CONFIG_SERVICE_LOCATION_MAX_LEN];
    char asset_class[CONFIG_SERVICE_ASSET_CLASS_MAX_LEN];
} device_identity_config_t;

/**
 * @brief Input payload for `config_service_stage_set_network()`: the subset
 * of network fields an OPC UA/HTTP caller is allowed to stage in one call.
 * `use_dhcp` maps to `device_profile_config_t::network_mode`.
 */
typedef struct
{
    bool use_dhcp;
    uint8_t mac[6];
    char opcua_hostname[CONFIG_SERVICE_TEXT_MAX_LEN];
    uint16_t opcua_port;
    char ntp_server[CONFIG_SERVICE_TEXT_MAX_LEN];
    uint8_t static_ip[4];
    uint8_t static_netmask[4];
    uint8_t static_gateway[4];
    uint8_t static_dns[4];
} device_network_config_t;

/**
 * @brief Full device configuration profile — the unit of persistence,
 * validation, export/import, and runtime-apply.
 *
 * `fw_revision`, `build_time`, and `git_hash` are runtime identity owned by
 * the compiled profile passed to `config_service_init()`: they remain
 * present in the persisted binary schema for wire compatibility, but a
 * persisted/imported value never overrides the currently running firmware
 * identity (`config_service_apply_runtime_identity()`,
 * `config_service.c`). `channels` is a fixed `PLATFORM_CHANNEL_LIMIT`-sized
 * array of `config_channel_definition_t`; unused slots have `in_use == 0`.
 */
typedef struct
{
    char manufacturer[CONFIG_SERVICE_TEXT_MAX_LEN];
    char model[CONFIG_SERVICE_TEXT_MAX_LEN];
    char serial_number[CONFIG_SERVICE_TEXT_MAX_LEN];
    char hw_revision[CONFIG_SERVICE_TEXT_MAX_LEN];
    char fw_revision[CONFIG_SERVICE_TEXT_MAX_LEN];
    char build_time[CONFIG_SERVICE_TEXT_MAX_LEN];
    char git_hash[CONFIG_SERVICE_TEXT_MAX_LEN];
    char device_name[CONFIG_SERVICE_TEXT_MAX_LEN];
    char opcua_hostname[CONFIG_SERVICE_TEXT_MAX_LEN];
    char ntp_server[CONFIG_SERVICE_TEXT_MAX_LEN];
    char mqtt_broker_url[CONFIG_SERVICE_TEXT_MAX_LEN];
    char mqtt_topic_root[CONFIG_SERVICE_TEXT_MAX_LEN];
    bool enable_heap_diagnostics;
    bool enable_task_diagnostics;
    bool enable_alarm_ack_method;
    bool enable_mqtt_pubsub;
    bool mqtt_pubsub_use_json;
    uint16_t opcua_port;
    int16_t timezone_offset_minutes;
    uint32_t watchdog_timeout_ms;
    uint8_t mac[6];
    network_mode_t network_mode;
    uint8_t static_ip[4];
    uint8_t static_netmask[4];
    uint8_t static_gateway[4];
    uint8_t static_dns[4];
    config_channel_definition_t channels[PLATFORM_CHANNEL_LIMIT];
    char product_instance_uri[CONFIG_SERVICE_URI_MAX_LEN];
    char asset_id[CONFIG_SERVICE_ASSET_ID_MAX_LEN];
    int64_t manufacture_date_utc;
    int64_t calibration_date_utc;
    char location[CONFIG_SERVICE_LOCATION_MAX_LEN];
    char asset_class[CONFIG_SERVICE_ASSET_CLASS_MAX_LEN];
    char hierarchical_location_path[CONFIG_SERVICE_LOCATION_PATH_MAX_LEN];
    char operational_location_path[CONFIG_SERVICE_LOCATION_PATH_MAX_LEN];
} device_profile_config_t;

/**
 * @brief Runtime-owner hook invoked synchronously to activate a validated
 * configuration candidate.
 *
 * Registered by the runtime owner (e.g. `sensor_manager_init`) once, after
 * it has initialized, via `config_service_set_runtime_apply_callback()`.
 * Called from within `config_service_apply()`/
 * `config_service_replace_and_save()` **without** the configuration critical
 * section held, so it may itself take time and touch hardware. It must
 * either fully activate the supplied immutable candidate or leave the
 * previous runtime state active — a partial activation is not permitted.
 *
 * @param candidate Immutable normalized/validated configuration to become
 * the new running configuration; must not be mutated or retained beyond the
 * call.
 * @param candidate_revision Revision number that will become
 * `g_running_revision` if this call returns true.
 * @return true if the candidate was fully activated; false to abort the
 * transaction (the previous running configuration remains active and, for
 * `config_service_apply()`, the staged configuration is left unmodified).
 */
typedef bool (*config_service_runtime_apply_fn)(const device_profile_config_t *candidate,
                                                uint32_t candidate_revision);

/**
 * @brief Initializes the configuration service: allocates state storage,
 * seeds it from @p defaults, and loads persisted configuration from Flash.
 *
 * As of v0.5.0 T4, first consults `config_store_get_recovery_state()` (the
 * `config_transaction_backend` boot-time classification T3 computes) and
 * reacts per the mapping documented next to that call site in
 * `config_service.c` — e.g. discarding a leftover uncommitted STAGED
 * record, or self-healing RUNNING from LAST_KNOWN_GOOD. Then, load order:
 * (1) the current-schema active config-store slot (`config_store_load_active()`
 * + `config_compat_decode_versioned()`); (2) on failure, a one-shot
 * read-only fallback via `flash_storage_driver_load_partial()` for
 * pre-config-store layouts, which — if it decodes and validates — is
 * immediately re-persisted through `config_store_save_payload()` so
 * subsequent boots use the normal path. If neither source yields a valid
 * configuration, @p defaults (if non-NULL) remains the running/staged
 * configuration and nothing is treated as an error.
 *
 * @param defaults Compiled-in default profile; also becomes the source of
 * truth for `fw_revision`/`build_time`/`git_hash` on every subsequent
 * normalize. May be NULL only in test/host builds that populate config
 * state another way.
 * @return true on success; false only if PSRAM-backed state allocation
 * fails (`WAVESHARE_CORE2350B_16MB_PSRAM8MB` builds).
 * @pre Must be called before any other `config_service_*` function.
 * @note Not safe to call concurrently with itself or other `config_service_*`
 * calls; intended to run once during single-threaded startup.
 * @note May perform a Flash write (migration re-persist) as a side effect.
 */
bool config_service_init(const device_profile_config_t *defaults);

/**
 * @brief Registers (or clears, with NULL) the runtime-apply callback invoked
 * by `config_service_apply()`/`config_service_replace_and_save()`.
 * @note Thread-safe (`taskENTER_CRITICAL`-protected pointer swap).
 */
void config_service_set_runtime_apply_callback(config_service_runtime_apply_fn callback);

/**
 * @brief Returns the live running configuration.
 * @return Pointer to service-owned storage; valid only until the next
 * `config_service_apply()`/`config_service_replace_and_save()`/
 * `config_service_init()` call. Callers needing a stable snapshot must copy
 * `*config_service_get()` under their own synchronization or rely on the
 * revision counters to detect staleness.
 * @note Not synchronized against concurrent mutation of the pointed-to
 * struct — this and `config_service_get_staged()` are the only two accessors
 * that return a live pointer rather than a copy.
 */
const device_profile_config_t *config_service_get(void);

/**
 * @brief Returns the live staged (not-yet-applied) configuration.
 * @return Pointer to service-owned storage; same lifetime/synchronization
 * caveats as `config_service_get()`.
 */
const device_profile_config_t *config_service_get_staged(void);

/**
 * @brief Stages the compiled-in default configuration for a subsequent
 * `config_service_apply()`.
 * @return true if defaults were staged; false if `config_service_init()`
 * was never given a valid `defaults` argument.
 * @post Staged revision is incremented; running configuration is unchanged
 * until `config_service_apply()` is called.
 */
bool config_service_reset_to_defaults(void);

/**
 * @brief Normalizes, validates, persists, activates, and verifies the
 * currently staged configuration as the new running configuration.
 *
 * As of v0.5.0 T4, this is a full Flash-backed transaction, executed in the
 * frozen order: validate -> persist the candidate to the
 * `config_transaction_backend` STAGED role (`config_store_stage_payload()`)
 * -> stop the affected drivers, start/verify the candidate, and publish it
 * (one call into the registered runtime-apply callback) -> commit
 * (`config_store_commit_staged()`, promoting STAGED to RUNNING and RUNNING
 * to LAST_KNOWN_GOOD) or roll back (`config_store_discard_staged()`; the
 * runtime-apply callback's own internal rollback already restarts the
 * previous drivers on an activation/verify failure, and this function
 * additionally re-invokes it to restore the previous drivers if the Flash
 * commit itself fails after activation already succeeded). Unlike prior
 * releases, this call now performs synchronous Flash writes.
 *
 * @return true if every phase above succeeded. false leaves the running
 * configuration unchanged and, on a Flash-write failure, best-effort
 * restores the previously-active drivers; the staged configuration is
 * always left as whatever was attempted (never reverted by this call).
 * @pre No other apply-style transaction may already be in flight; returns
 * false immediately if `config_service_replace_and_save()`/
 * `config_service_apply()` is already executing.
 * @note Invokes the registered runtime-apply callback synchronously, which
 * may touch hardware and take real time (driver start/verify); this call
 * does not hold the configuration critical section while doing so.
 */
bool config_service_apply(void);

/**
 * @brief Validates and immediately mutates both running and staged timezone
 * offset in one atomic step (bypasses the stage/apply flow).
 * @param value Offset in minutes from UTC; valid range is -720..840.
 * @return true if @p value was in range and applied; false otherwise (no
 * state change).
 * @note Does not persist to Flash; does not invoke the runtime-apply
 * callback.
 */
bool config_service_set_timezone_offset_minutes(int16_t value);

/**
 * @brief Validates, persists, and immediately mutates both running and
 * staged asset ID in one step (bypasses the stage/apply flow).
 * @param value NUL-terminated asset ID string; must fit in
 * `CONFIG_SERVICE_ASSET_ID_MAX_LEN` including the terminator.
 * @return true on success; false if @p value is NULL/oversized or the
 * Flash write via `config_store_save_payload()` fails.
 * @note Performs a synchronous Flash write; on failure, in-memory state is
 * left unchanged (the write is attempted before the running/staged copies
 * are mutated).
 */
bool config_service_set_asset_id(const char *value);

/**
 * @brief Validates a `/`-separated hierarchical location path string.
 *
 * Rules: empty string is valid; otherwise each `/`-delimited segment must be
 * non-empty, at most `CONFIG_SERVICE_LOCATION_SEGMENT_MAX_LEN` characters,
 * contain no control characters (`< 0x20` or `0x7f`), and the path must have
 * fewer than `CONFIG_SERVICE_LOCATION_MAX_DEPTH` segments.
 * @return true if @p value is NULL-safe-checked and satisfies the above;
 * false otherwise (including @p value == NULL).
 */
bool config_service_location_path_is_valid(const char *value);

/**
 * @brief Stages both hierarchical and operational location paths together,
 * keeping the legacy `location` field mirrored to the hierarchical path.
 * @param hierarchical_path Validated via `config_service_location_path_is_valid()`.
 * @param operational_path Validated via `config_service_location_path_is_valid()`.
 * @return true if both paths validate and fit their buffers; false otherwise
 * (staged configuration unchanged).
 * @post On success, staged revision is incremented and the staged
 * configuration is re-normalized.
 */
bool config_service_stage_set_location_paths(const char *hierarchical_path,
                                             const char *operational_path);

/**
 * @brief Stages the MQTT PubSub enable flag.
 * @return Always true.
 * @post Staged revision is incremented.
 */
bool config_service_stage_set_mqtt_pubsub_enabled(bool enabled);

/**
 * @brief Stages a full identity field set from @p identity into the staged
 * configuration, then re-normalizes it.
 * @return false if @p identity is NULL; true otherwise.
 * @post On success, staged revision is incremented; `hierarchical_location_path`
 * is overwritten from `identity->location`.
 */
bool config_service_stage_set_identity(const device_identity_config_t *identity);

/**
 * @brief Stages a full network field set from @p network into the staged
 * configuration, then re-normalizes it.
 * @return false if @p network is NULL or `network->opcua_port == 0`; true
 * otherwise.
 * @post On success, staged revision is incremented.
 */
bool config_service_stage_set_network(const device_network_config_t *network);

/**
 * @brief Copies one channel slot out of the staged configuration.
 * @param index Channel slot index; must be `< PLATFORM_CHANNEL_LIMIT`.
 * @param out_channel Destination for the copied slot; must not be NULL.
 * @return true on success; false if @p out_channel is NULL or @p index is
 * out of range.
 */
bool config_service_stage_copy_channel(size_t index, config_channel_definition_t *out_channel);

/**
 * @brief Replaces one channel slot in the staged configuration and
 * re-normalizes just that slot.
 * @param index Channel slot index; must be `< PLATFORM_CHANNEL_LIMIT`.
 * @param channel Replacement channel definition; must not be NULL.
 * @return true on success; false if @p channel is NULL or @p index is out
 * of range.
 * @post On success, staged revision is incremented. Normalization here does
 * not run full cross-channel validation (e.g. duplicate-ID or resource
 * conflicts) — that only happens in
 * `config_validate_service_validate_candidate()` at apply time.
 */
bool config_service_stage_set_channel(size_t index, const config_channel_definition_t *channel);

/**
 * @brief Clears one channel slot in the staged configuration (sets it to a
 * zeroed, unused definition).
 * @param index Channel slot index; must be `< PLATFORM_CHANNEL_LIMIT`.
 * @return Same as `config_service_stage_set_channel()`.
 */
bool config_service_stage_clear_channel(size_t index);

/**
 * @brief Validates, persists, and optionally activates a full replacement
 * configuration in one transaction (used by the config-import flow).
 *
 * As of v0.5.0 T4, sequence: normalize @p config -> validate -> persist to
 * the `config_transaction_backend` STAGED role
 * (`config_store_stage_payload()`) -> (if @p apply_running) invoke the
 * runtime-apply callback (stop/start/verify/publish) -> commit
 * (`config_store_commit_staged()`) or roll back
 * (`config_store_discard_staged()`, plus re-invoking the runtime-apply
 * callback with the previous configuration if activation had already
 * succeeded before the commit itself failed) -> update staged (+revision)
 * -> (if @p apply_running) update running (+revision) and run the
 * commit-step extension point. The old activate-before-persist order does
 * not survive this release.
 *
 * @param config Replacement configuration; must not be NULL. Copied and
 * normalized internally, not retained.
 * @param apply_running If true, also activates @p config as the running
 * configuration (invoking the runtime-apply callback); if false, only the
 * staged configuration and Flash are updated (no driver activation).
 * @return true on full success. false if: @p config is NULL; validation
 * fails; the candidate could not be staged to Flash; the runtime-apply
 * callback rejects the candidate (staged configuration is still updated
 * with the rejected-but-valid candidate for correction/retry, and the
 * staged Flash record is discarded); or the commit itself fails (if the
 * runtime was already switched over, the previous running configuration is
 * re-applied via the callback as a rollback attempt — the callback's own
 * success is not checked here, and the staged configuration is left
 * unchanged in this case).
 * @pre No other apply-style transaction may already be in flight.
 * @note Performs synchronous Flash writes and may invoke the runtime-apply
 * callback; neither happens while holding the configuration critical
 * section.
 */
bool config_service_replace_and_save(const device_profile_config_t *config, bool apply_running);

/**
 * @brief Returns the current running-configuration revision counter.
 * @return Monotonically increasing revision; starts at 1 after
 * `config_service_init()`, or the persisted generation number if a config
 * was loaded from Flash.
 */
uint32_t config_service_get_running_revision(void);

/**
 * @brief Returns the current staged-configuration revision counter.
 * @return Monotonically increasing revision, incremented by every
 * successful staged mutation.
 */
uint32_t config_service_get_staged_revision(void);

/**
 * @brief Normalizes the staged configuration and persists it to Flash
 * without activating it as the running configuration.
 * @return true if the Flash write via `config_store_save_payload()`
 * succeeded; false otherwise.
 * @post Staged revision is incremented and the staged configuration is
 * replaced with its normalized form regardless of whether the Flash write
 * succeeds.
 * @note Performs a synchronous Flash write.
 */
bool config_service_save(void);

#endif
