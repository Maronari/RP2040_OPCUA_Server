/**
 * @file
 * @brief Implementation of the device configuration service: running/staged
 * `device_profile_config_t` state, field normalization, revision counters,
 * and the transactional apply/replace-and-save paths declared in
 * `services/config_service.h`.
 *
 * Responsibilities:
 * - Own the single PSRAM (or static fallback) block holding
 *   `g_running_config`/`g_staged_config`/`g_default_config` plus a scratch
 *   decode buffer for the persisted payload.
 * - Normalize every candidate configuration (text NUL-termination, channel
 *   defaulting/clamping, network/location defaults) before it can be staged
 *   as running.
 * - Load persisted configuration at boot via `config_store_service`/
 *   `config_compat`, including one-shot migration from the legacy read-only
 *   Flash importer.
 *
 * Ownership: all `device_profile_config_t` instances referenced here belong
 * to `config_service`; see `services/config_service.h` for the pointer
 * lifetime contract of `config_service_get()`/`config_service_get_staged()`.
 *
 * Concurrency: every read/write of `g_running_config`, `g_staged_config`,
 * `g_running_revision`, `g_staged_revision`, and `g_runtime_apply_callback`
 * is wrapped in `taskENTER_CRITICAL()`/`taskEXIT_CRITICAL()`. Apply-style
 * operations additionally take `g_transaction_active` via
 * `config_service_begin_transaction()`/`config_service_end_transaction()` so
 * only one such operation runs at a time; the runtime-apply callback and any
 * Flash write always happen outside the critical section.
 */
#include "services/config_service.h"

#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include <config_store_limits.h>
#include "services/alarm_service.h"
#include "services/config_compat.h"
#include "services/config_store_service.h"
#include "services/config_validate_service.h"
#include "services/history_service.h"
#include "pico/unique_id.h"

#if defined(RP2_BOARD_PSRAM_CS_PIN)
#include "psram_allocator.h"
#endif

_Static_assert(sizeof(device_profile_config_t) <= CONFIG_STORE_MAX_PAYLOAD_SIZE_BYTES,
               "device_profile_config_t must fit in one config storage slot");

/**
 * @brief Scratch decode buffer for the raw persisted payload read back from
 * `config_store_load_active()`, sized
 * to the current (largest) `device_profile_config_t` layout so any frozen
 * `config_compat` layout also fits.
 */
typedef union
{
    uint8_t payload[sizeof(device_profile_config_t)];
    device_profile_config_t config;
} config_service_persisted_scratch_t;

/**
 * @brief Single allocation holding all configuration-service runtime state.
 *
 * Owner: `config_service`, lifetime from `config_service_init()` until
 * reboot. Allocated from PSRAM on boards whose header defines
 * `RP2_BOARD_PSRAM_CS_PIN` (`g_config_state`), or a static fallback instance otherwise
 * (`g_config_state_fallback`) — kept out of the default `.bss`/stack budget
 * on the PSRAM target because `device_profile_config_t` (three copies, plus
 * scratch) is large. Synchronization: see file-level comment.
 */
typedef struct
{
    device_profile_config_t running_config;
    device_profile_config_t staged_config;
    device_profile_config_t default_config;
    config_service_persisted_scratch_t persisted;
} config_service_state_t;

static config_service_state_t *g_config_state;
#if !defined(RP2_BOARD_PSRAM_CS_PIN)
static config_service_state_t g_config_state_fallback;
#endif

#define g_running_config (g_config_state->running_config)
#define g_staged_config (g_config_state->staged_config)
#define g_default_config (g_config_state->default_config)
#define g_persisted_payload (g_config_state->persisted.payload)
static bool g_default_config_valid = false;
static uint32_t g_running_revision = 0u;
static uint32_t g_staged_revision = 0u;
static config_service_runtime_apply_fn g_runtime_apply_callback;
static config_service_restart_fn g_restart_callback;
static bool g_transaction_active;

/**
 * @brief Attempts to acquire the single-flight apply-transaction lock.
 * @return true if `g_transaction_active` was false and is now claimed by the
 * caller; false if another apply-style transaction is already in progress.
 * @note Synchronized with `taskENTER_CRITICAL()`/`taskEXIT_CRITICAL()`.
 */
static bool config_service_begin_transaction(void)
{
    bool acquired = false;
    taskENTER_CRITICAL();
    if (!g_transaction_active)
    {
        g_transaction_active = true;
        acquired = true;
    }
    taskEXIT_CRITICAL();
    return acquired;
}

/**
 * @brief Releases the single-flight apply-transaction lock.
 * @note Synchronized with `taskENTER_CRITICAL()`/`taskEXIT_CRITICAL()`.
 */
static void config_service_end_transaction(void)
{
    taskENTER_CRITICAL();
    g_transaction_active = false;
    taskEXIT_CRITICAL();
}

/**
 * @brief T7: finds every channel present (`in_use`) in @p previous_config
 * that is either absent from @p new_config or present there with a
 * different `driver_id`/`value_type`, and clears its history/alarm state.
 *
 * Per docs/releases/v0.5.0.md scope decision 7, deletion and modification
 * (same `channel_id`, different driver/value type) are treated identically
 * -- as delete+recreate -- and a channel whose only change is some other
 * field (e.g. sample period) is left untouched. Comparison is by
 * `channel_id` string, never array index: index does not identify a
 * channel across a reconfiguration (channels can be reordered).
 *
 * @param new_config The just-committed configuration.
 * @param previous_config The running configuration @p new_config replaced.
 */
static void config_service_clear_stale_channel_state(const device_profile_config_t *new_config,
                                                      const device_profile_config_t *previous_config)
{
    for (size_t prev_index = 0u; prev_index < PLATFORM_CHANNEL_LIMIT; ++prev_index)
    {
        const config_channel_definition_t *prev_channel = &previous_config->channels[prev_index];
        const config_channel_definition_t *matched = NULL;

        if (!prev_channel->in_use || prev_channel->id[0] == '\0')
        {
            continue;
        }

        for (size_t new_index = 0u; new_index < PLATFORM_CHANNEL_LIMIT; ++new_index)
        {
            const config_channel_definition_t *new_channel = &new_config->channels[new_index];

            if (new_channel->in_use &&
                strncmp(new_channel->id, prev_channel->id, sizeof(new_channel->id)) == 0)
            {
                matched = new_channel;
                break;
            }
        }

        if (matched == NULL || matched->driver_id != prev_channel->driver_id ||
            matched->value_type != prev_channel->value_type)
        {
            (void)history_service_clear_channel(prev_channel->id);
            (void)alarm_service_clear_channel(prev_channel->id);
        }
    }
}

/**
 * @brief Commit-step extension point (v0.5.0 T4): invoked synchronously
 * immediately after a transaction's Flash commit succeeded and the RAM
 * running configuration has been updated, still inside the transaction
 * lock, before the transaction's caller regains control.
 *
 * Per docs/releases/v0.5.0.md's "Architecture and public-contract rules",
 * two actions run here, in this exact order:
 *   1. T7: an explicit per-channel history/alarm clear
 *      (`history_service_clear_channel()`/`alarm_service_clear_channel()`,
 *      via config_service_clear_stale_channel_state()) for every channel
 *      deleted or modified (same `channel_id`, different driver/value_type)
 *      between @p previous_config and @p new_config. Runs immediately above
 *      the restart callback below -- not after it -- so no live server ever
 *      serves the old namespace against already-cleared history/alarm
 *      state.
 *   2. T6 (this function): unconditionally invokes the registered restart
 *      callback (`config_service_set_restart_callback()`), passing both
 *      configs. `config_service` does not itself decide whether a rebuild
 *      is warranted -- see `config_service_restart_fn`'s doc comment --
 *      the callback (`opcua_model`'s `model_config_requires_server_reboot()`
 *      consumer) makes that call and, if the answer is yes, performs (or
 *      arms) a synchronous, controlled full OPC UA server restart.
 * Both must run synchronously and in this order before this function (and
 * therefore config_service_apply()/config_service_replace_and_save())
 * returns to its own caller.
 *
 * @param new_config The just-committed configuration, now `g_running_config`.
 * @param previous_config The running configuration @p new_config replaced.
 */
static void config_service_run_commit_extensions(const device_profile_config_t *new_config,
                                                  const device_profile_config_t *previous_config)
{
    config_service_restart_fn restart_callback;

    taskENTER_CRITICAL();
    restart_callback = g_restart_callback;
    taskEXIT_CRITICAL();

    config_service_clear_stale_channel_state(new_config, previous_config);

    if (restart_callback != NULL)
    {
        restart_callback(new_config, previous_config);
    }
}

static bool config_service_is_valid_timezone_offset_minutes(int16_t value)
{
    return value >= -720 && value <= 840;
}

/**
 * @brief Forces the last byte of a fixed-size text buffer to NUL.
 * @param text Buffer to terminate; no-op if NULL.
 * @param size Buffer capacity in bytes; no-op if 0.
 * @note Does not scan for or reject embedded NULs — only guarantees the
 * buffer is not read out-of-bounds by later string operations.
 */
static void config_service_force_terminated(char *text, size_t size)
{
    if (text == NULL || size == 0u)
    {
        return;
    }

    text[size - 1u] = '\0';
}

/**
 * @brief Overwrites `fw_revision`/`build_time`/`git_hash` in @p config with
 * the values from the compiled default profile.
 *
 * Firmware provenance belongs to the running image, not to the persisted
 * device configuration. A configuration slot may have been written by an
 * older firmware image, so this retains its other configurable fields but
 * always exposes the identity embedded in the current compiled profile.
 * Called from `config_service_normalize()`, i.e. on every normalize pass.
 * @param config Configuration to patch in place; no-op if NULL or if
 * `config_service_init()` was never given a valid `defaults` argument.
 */
static void config_service_apply_runtime_identity(device_profile_config_t *config)
{
    if (config == NULL || !g_default_config_valid)
    {
        return;
    }

    memcpy(config->fw_revision,
           g_default_config.fw_revision,
           sizeof(config->fw_revision));
    memcpy(config->build_time,
           g_default_config.build_time,
           sizeof(config->build_time));
    memcpy(config->git_hash,
           g_default_config.git_hash,
           sizeof(config->git_hash));
}

bool config_service_location_path_is_valid(const char *value)
{
    size_t depth = 0u;
    size_t segment_length = 0u;

    if (value == NULL)
    {
        return false;
    }
    if (value[0] == '\0')
    {
        return true;
    }

    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0';
         ++cursor)
    {
        if (*cursor < 0x20u || *cursor == 0x7fu)
        {
            return false;
        }
        if (*cursor == (unsigned char)'/')
        {
            if (segment_length == 0u)
            {
                return false;
            }
            depth++;
            if (depth >= CONFIG_SERVICE_LOCATION_MAX_DEPTH)
            {
                return false;
            }
            segment_length = 0u;
            continue;
        }

        segment_length++;
        if (segment_length > CONFIG_SERVICE_LOCATION_SEGMENT_MAX_LEN)
        {
            return false;
        }
    }

    return segment_length > 0u;
}

static bool config_service_ipv4_is_zero(const uint8_t address[4])
{
    return address == NULL ||
           (address[0] == 0u && address[1] == 0u && address[2] == 0u && address[3] == 0u);
}

/**
 * @brief Forces NUL-termination on every fixed-size text field of @p config.
 * @param config Configuration to patch in place; no-op if NULL.
 * @note Does not validate content, only bounds; called at the start of
 * `config_service_normalize()`.
 */
static void config_service_normalize_text_fields(device_profile_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    config_service_force_terminated(config->manufacturer, sizeof(config->manufacturer));
    config_service_force_terminated(config->model, sizeof(config->model));
    config_service_force_terminated(config->serial_number, sizeof(config->serial_number));
    config_service_force_terminated(config->hw_revision, sizeof(config->hw_revision));
    config_service_force_terminated(config->fw_revision, sizeof(config->fw_revision));
    config_service_force_terminated(config->build_time, sizeof(config->build_time));
    config_service_force_terminated(config->git_hash, sizeof(config->git_hash));
    config_service_force_terminated(config->device_name, sizeof(config->device_name));
    config_service_force_terminated(config->opcua_hostname, sizeof(config->opcua_hostname));
    config_service_force_terminated(config->ntp_server, sizeof(config->ntp_server));
    config_service_force_terminated(config->mqtt_broker_url, sizeof(config->mqtt_broker_url));
    config_service_force_terminated(config->mqtt_topic_root, sizeof(config->mqtt_topic_root));
    config_service_force_terminated(config->product_instance_uri, sizeof(config->product_instance_uri));
    config_service_force_terminated(config->asset_id, sizeof(config->asset_id));
    config_service_force_terminated(config->location, sizeof(config->location));
    config_service_force_terminated(config->asset_class, sizeof(config->asset_class));
    config_service_force_terminated(config->hierarchical_location_path,
                                    sizeof(config->hierarchical_location_path));
    config_service_force_terminated(config->operational_location_path,
                                    sizeof(config->operational_location_path));
}

/**
 * @brief Normalizes and defaults a single channel slot in place.
 *
 * Behavior:
 * - `in_use == 0` zeroes the entire slot and returns.
 * - Unknown `driver_id` (not found in `driver_catalog`), or a driver-refused
 *   `source_index`/`source_flags` combination, leaves the slot otherwise
 *   untouched (aside from text termination) so downstream validation can
 *   reject it explicitly.
 * - `flags == 0` (a never-configured slot) adopts the driver's default
 *   kind/value-type/flags; a non-zero `flags` is left as-is so an explicit
 *   invalid/unsupported value set by a caller is never silently coerced —
 *   validation must reject it instead.
 * - Empty id/name/description/unit are filled with generated defaults
 *   (`channel.<n>`, `Channel<n>`, etc.) or the driver's default unit.
 * - Zero sample/publish/history periods default to 1000 ms (publish/history
 *   further default to the sample period).
 * - Non-analog sources force `scale = 1.0`, `offset = 0.0`, and clear
 *   `has_minimum`/`has_maximum`.
 * - `critical_threshold` is clamped up to at least `warning_threshold`;
 *   symmetrically, `low_low_threshold` is clamped down to at most
 *   `low_threshold`. `alarm_hysteresis` is clamped to `>= 0.0`.
 * - `settings.enabled == false` clears `TELEMETRY_CHANNEL_FLAG_ENABLED` in
 *   `flags`.
 *
 * @param channel Channel slot to normalize in place; no-op if NULL.
 * @param index Zero-based slot index, used only to generate default
 * id/name/description text.
 */
static void config_service_normalize_channel(config_channel_definition_t *channel, size_t index)
{
    const driver_catalog_entry_t *driver = NULL;
    const bool analog_source = channel != NULL &&
                               (driver = driver_catalog_get_by_id(channel->driver_id)) != NULL &&
                               driver->analog;

    if (channel == NULL)
    {
        return;
    }

    config_service_force_terminated(channel->id, sizeof(channel->id));
    config_service_force_terminated(channel->name, sizeof(channel->name));
    config_service_force_terminated(channel->description, sizeof(channel->description));
    config_service_force_terminated(channel->unit, sizeof(channel->unit));

    if (channel->in_use == 0u)
    {
        memset(channel, 0, sizeof(*channel));
        return;
    }

    if (driver == NULL)
    {
        return;
    }

    if (driver->validate_source != NULL && !driver->validate_source(channel->source_index, channel->source_flags))
    {
        return;
    }

    if (channel->flags == 0u)
    {
        channel->kind = (uint8_t)driver->default_kind;
        channel->value_type = (uint8_t)driver->default_value_type;
        channel->flags = driver->default_flags;
    }
    else
    {
        /* Preserve explicit invalid/unsupported values for validation.  They
         * must never be silently coerced to a driver's default type. */
    }

    if (channel->id[0] == '\0')
    {
        (void)snprintf(channel->id, sizeof(channel->id), "channel.%u", (unsigned)(index + 1u));
    }

    if (channel->name[0] == '\0')
    {
        (void)snprintf(channel->name, sizeof(channel->name), "Channel%u", (unsigned)(index + 1u));
    }

    if (channel->description[0] == '\0')
    {
        (void)snprintf(channel->description, sizeof(channel->description), "Configurable telemetry channel %u", (unsigned)(index + 1u));
    }

    if (channel->unit[0] == '\0' && driver->default_unit != NULL)
    {
        strncpy(channel->unit, driver->default_unit, sizeof(channel->unit));
        channel->unit[sizeof(channel->unit) - 1u] = '\0';
    }

    if (channel->settings.sample_period_ms == 0u)
    {
        channel->settings.sample_period_ms = 1000u;
    }

    if (channel->settings.publish_period_ms == 0u)
    {
        channel->settings.publish_period_ms = channel->settings.sample_period_ms;
    }

    if (channel->settings.history_interval_ms == 0u)
    {
        channel->settings.history_interval_ms = channel->settings.sample_period_ms;
    }

    if (channel->settings.scale == 0.0 && analog_source)
    {
        channel->settings.scale = 1.0;
    }

    if (!analog_source)
    {
        channel->settings.scale = 1.0;
        channel->settings.offset = 0.0;
        channel->has_minimum = 0u;
        channel->has_maximum = 0u;
    }

    if (channel->settings.critical_threshold < channel->settings.warning_threshold)
    {
        channel->settings.critical_threshold = channel->settings.warning_threshold;
    }

    /* Symmetric low-side clamp: low-low must be at or below low, mirroring
     * the existing critical/warning clamp above. */
    if (channel->settings.low_low_threshold > channel->settings.low_threshold)
    {
        channel->settings.low_low_threshold = channel->settings.low_threshold;
    }

    if (channel->settings.alarm_hysteresis < 0.0)
    {
        channel->settings.alarm_hysteresis = 0.0;
    }

    if (!channel->settings.enabled)
    {
        channel->flags &= ~TELEMETRY_CHANNEL_FLAG_ENABLED;
    }
}

/**
 * @brief Fills an empty `serial_number` from the RP2350's factory-programmed
 * unique board ID, hex-encoded.
 * @param config Configuration to patch in place; no-op if NULL or if
 * `serial_number` is already non-empty.
 * @note Reads hardware identity via `pico_get_unique_board_id()`.
 */
static void config_service_populate_serial_number(device_profile_config_t *config)
{
    pico_unique_board_id_t board_id;
    size_t offset = 0;

    if (config == NULL || config->serial_number[0] != '\0')
    {
        return;
    }

    pico_get_unique_board_id(&board_id);

    for (size_t index = 0; index < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; ++index)
    {
        if ((offset + 2u) >= sizeof(config->serial_number))
        {
            break;
        }

        offset += (size_t)snprintf(&config->serial_number[offset],
                                   sizeof(config->serial_number) - offset,
                                   "%02X",
                                   board_id.id[index]);
    }
}

/**
 * @brief Fills an empty `product_instance_uri` as `urn:rp2-opcua:<serial>`
 * (or `...:device` if no serial number is set).
 * @param config Configuration to patch in place; no-op if NULL or if
 * `product_instance_uri` is already non-empty.
 * @note Must run after `config_service_populate_serial_number()` in the same
 * normalize pass to pick up a freshly generated serial number.
 */
static void config_service_populate_product_instance_uri(device_profile_config_t *config)
{
    if (config == NULL || config->product_instance_uri[0] != '\0')
    {
        return;
    }

    (void)snprintf(config->product_instance_uri,
                   sizeof(config->product_instance_uri),
                   "urn:rp2-opcua:%s",
                   config->serial_number[0] != '\0' ? config->serial_number : "device");
    config->product_instance_uri[sizeof(config->product_instance_uri) - 1u] = '\0';
}

/**
 * @brief Full normalization pass applied to every configuration candidate
 * before it can be validated/activated/persisted.
 *
 * Order: NUL-terminate text fields -> clamp timezone/port/network-mode to
 * valid defaults -> default static netmask/DNS if zero -> mirror
 * device_name/opcua_hostname when only one is set -> default NTP/MQTT
 * broker/topic -> normalize every channel slot
 * (`config_service_normalize_channel()`) -> populate serial number and
 * product instance URI -> validate/mirror the two location-path fields
 * against the legacy `location` field -> reapply runtime firmware identity
 * (`config_service_apply_runtime_identity()`, always last so nothing above
 * can override it).
 *
 * @param config Configuration to normalize in place; no-op if NULL.
 * @note Pure in-memory transform — never touches Flash or hardware other
 * than reading the unique board ID via
 * `config_service_populate_serial_number()`.
 */
static void config_service_normalize(device_profile_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    config_service_normalize_text_fields(config);

    if (!config_service_is_valid_timezone_offset_minutes(config->timezone_offset_minutes))
    {
        config->timezone_offset_minutes = 0;
    }

    if (config->opcua_port == 0u)
    {
        config->opcua_port = 4840u;
    }

    if (config->network_mode != NETWORK_MODE_DHCP && config->network_mode != NETWORK_MODE_STATIC)
    {
        config->network_mode = NETWORK_MODE_DHCP;
    }

    if (config_service_ipv4_is_zero(config->static_netmask))
    {
        config->static_netmask[0] = 255u;
        config->static_netmask[1] = 255u;
        config->static_netmask[2] = 255u;
        config->static_netmask[3] = 0u;
    }

    if (config_service_ipv4_is_zero(config->static_dns))
    {
        config->static_dns[0] = 8u;
        config->static_dns[1] = 8u;
        config->static_dns[2] = 8u;
        config->static_dns[3] = 8u;
    }

    if (config->device_name[0] == '\0' && config->opcua_hostname[0] != '\0')
    {
        strncpy(config->device_name, config->opcua_hostname, sizeof(config->device_name));
        config->device_name[sizeof(config->device_name) - 1] = '\0';
    }

    if (config->opcua_hostname[0] == '\0' && config->device_name[0] != '\0')
    {
        strncpy(config->opcua_hostname, config->device_name, sizeof(config->opcua_hostname));
        config->opcua_hostname[sizeof(config->opcua_hostname) - 1] = '\0';
    }

    if (config->ntp_server[0] == '\0')
    {
        strncpy(config->ntp_server, "pool.ntp.org", sizeof(config->ntp_server));
        config->ntp_server[sizeof(config->ntp_server) - 1] = '\0';
    }

    if (config->mqtt_broker_url[0] == '\0')
    {
        strncpy(config->mqtt_broker_url, "opc.mqtt://127.0.0.1:1883", sizeof(config->mqtt_broker_url));
        config->mqtt_broker_url[sizeof(config->mqtt_broker_url) - 1] = '\0';
    }

    if (config->mqtt_topic_root[0] == '\0')
    {
        (void)snprintf(config->mqtt_topic_root,
                       sizeof(config->mqtt_topic_root),
                       "devices/%s",
                       config->opcua_hostname[0] != '\0' ? config->opcua_hostname : "device");
        config->mqtt_topic_root[sizeof(config->mqtt_topic_root) - 1] = '\0';
    }

    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        config_service_normalize_channel(&config->channels[index], index);
    }

    config_service_populate_serial_number(config);
    config_service_populate_product_instance_uri(config);

    if (!config_service_location_path_is_valid(config->hierarchical_location_path))
    {
        config->hierarchical_location_path[0] = '\0';
    }
    if (!config_service_location_path_is_valid(config->operational_location_path))
    {
        config->operational_location_path[0] = '\0';
    }
    if (config->hierarchical_location_path[0] == '\0' &&
        config->location[0] != '\0' &&
        config_service_location_path_is_valid(config->location))
    {
        strncpy(config->hierarchical_location_path,
                config->location,
                sizeof(config->hierarchical_location_path));
        config->hierarchical_location_path[sizeof(config->hierarchical_location_path) - 1u] = '\0';
    }
    if (config->location[0] == '\0' && config->hierarchical_location_path[0] != '\0')
    {
        strncpy(config->location, config->hierarchical_location_path, sizeof(config->location));
        config->location[sizeof(config->location) - 1u] = '\0';
    }

    config_service_apply_runtime_identity(config);
}

/* Loads the current-schema config-store slot pair (RUNNING, transparently
 * falling back to LAST_KNOWN_GOOD); @p defaults seeds running/staged/default
 * state for a device with no committed record yet (first boot) and remains
 * in place if the persisted record is missing or fails validation. There is
 * no on-Flash record older than the current config-store schema this
 * firmware still reads: no device this project ships was ever, or will ever
 * be, field-upgraded from a pre-transactional-config firmware image -- see
 * docs/known_limits.md. */
bool config_service_init(const device_profile_config_t *defaults)
{
    bool persisted_requires_migration = false;
    bool loaded_persisted_record = false;
    config_transaction_recovery_state_t recovery_state;

#if defined(RP2_BOARD_PSRAM_CS_PIN)
    if (g_config_state == NULL)
    {
        if (!psram_allocator_is_ready())
        {
            return false;
        }
        g_config_state = (config_service_state_t *)psram_calloc(1u, sizeof(*g_config_state));
        if (g_config_state == NULL)
        {
            return false;
        }
    }
#else
    g_config_state = &g_config_state_fallback;
#endif
    memset(g_config_state, 0, sizeof(*g_config_state));
    memset(&g_running_config, 0, sizeof(g_running_config));
    memset(&g_staged_config, 0, sizeof(g_staged_config));
    memset(&g_default_config, 0, sizeof(g_default_config));
    g_default_config_valid = false;
    g_running_revision = 1u;
    g_staged_revision = 1u;
    g_runtime_apply_callback = NULL;
    g_restart_callback = NULL;
    g_transaction_active = false;

    if (defaults != NULL)
    {
        g_running_config = *defaults;
        g_staged_config = *defaults;
        g_default_config = *defaults;
        g_default_config_valid = true;
    }

    /*
     * v0.5.0 T4 reboot recovery: consult config_transaction_backend's (T3)
     * boot-time classification before reading anything, and react per this
     * frozen recovery-state-to-boot-decision mapping (T9's power-loss test
     * harness tests against this exact mapping):
     *
     *   CONFIG_TXN_RECOVERY_EMPTY / _OK / _DEGRADED:
     *       RUNNING (if any) is safe to use as-is; boot proceeds through the
     *       normal config_store_load_active() path below, unchanged. A
     *       leftover STAGED record -- a transaction that persisted its
     *       staged copy but was never committed, e.g. a power loss between
     *       config_store_stage_payload() and the driver-activation/verify
     *       work in config_service_apply() -- is explicitly discarded here
     *       as hygiene: boot on the existing RUNNING config exactly as if
     *       that transaction had never started.
     *   CONFIG_TXN_RECOVERY_FALLBACK_REQUIRED:
     *       RUNNING is not usable but LAST_KNOWN_GOOD is -- self-heal by
     *       promoting LAST_KNOWN_GOOD into RUNNING
     *       (config_store_recover_fallback_to_last_known_good()) so this and
     *       every later boot use the normal load_active() path below
     *       without relying on its transparent per-read LAST_KNOWN_GOOD
     *       fallback every single boot.
     *   CONFIG_TXN_RECOVERY_INDETERMINATE / _FATAL:
     *       Nothing usable and/or mutations are blocked backend-side; no
     *       Flash write is attempted here. Boot falls through
     *       config_store_load_active() (which will fail closed) to
     *       @p defaults, exactly as when no persisted record exists at all.
     *
     * A transaction that reached commit (STAGED promoted to RUNNING) but
     * whose driver activation/verify was interrupted by a power loss before
     * config_service_apply() returned cannot be observed here at all: that
     * in-flight state lives only in RAM for the duration of one live
     * transaction. Such a reboot always collapses to the CONFIG_TXN_RECOVERY_OK
     * case above (RUNNING already reflects the committed candidate) -- there
     * is no partial activation state on Flash to recover, and none is
     * needed.
     */
    recovery_state = config_store_get_recovery_state();
    (void)config_store_discard_staged();
    if (recovery_state == CONFIG_TXN_RECOVERY_FALLBACK_REQUIRED)
    {
        (void)config_store_recover_fallback_to_last_known_good(NULL);
    }

    size_t persisted_payload_size = 0u;
    config_store_record_info_t persisted_info = {0};
    memset(g_persisted_payload, 0, sizeof(g_persisted_payload));
    if (config_store_load_active(g_persisted_payload,
                                 sizeof(g_persisted_payload),
                                 &persisted_payload_size,
                                 &persisted_info) &&
        persisted_info.payload_format == CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY &&
        config_compat_decode_versioned(persisted_info.schema_version,
                                       g_persisted_payload,
                                       persisted_payload_size,
                                       &g_running_config,
                                       &persisted_requires_migration) &&
        config_validate_service_validate_candidate(&g_running_config, NULL, 0u) ==
            CONFIG_VALIDATE_RESULT_OK)
    {
        g_staged_config = g_running_config;
        g_running_revision = persisted_info.generation > 0u ? persisted_info.generation : 1u;
        g_staged_revision = g_running_revision;
        loaded_persisted_record = true;
    }

    config_service_normalize(&g_running_config);
    config_service_normalize(&g_staged_config);
    if (g_default_config_valid)
    {
        config_service_normalize(&g_default_config);
    }

    /*
     * Migration writes only to the alternate slot through the existing
     * CRC/commit path.  A failed write leaves the v0.4.0 source slot active
     * and the decoded known-good configuration remains usable for this boot.
     */
    if (persisted_requires_migration)
    {
        (void)config_store_save_payload(&g_running_config,
                                        sizeof(g_running_config),
                                        CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY,
                                        CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY,
                                        NULL);
    }
    else if (!loaded_persisted_record && g_default_config_valid)
    {
        /* No usable persisted record exists (a genuinely fresh device, or
         * one whose only record failed decode/validate) -- persist the
         * compiled defaults now running in RAM as RUNNING's first committed
         * record, so config_transaction_backend_commit()'s promote-RUNNING-
         * to-LAST_KNOWN_GOOD step (drivers/config_transaction_backend.c) has
         * something to promote as early as possible, ideally before the
         * very first real config_service_apply() transaction ever runs.
         * Necessarily best-effort here: config_service_init() cannot fail
         * the entire boot over one transient Flash write, so a failed write
         * leaves this boot running on the in-RAM defaults, exactly as
         * unpersisted as every prior release, until the guard below closes
         * the gap for real. This is only ever a head start, never the sole
         * guarantee -- config_service_ensure_running_persisted() (used by
         * config_service_apply()/config_service_replace_and_save()) checks
         * and retries this on every actual transaction attempt, so a
         * failure here does not silently persist as a gap the way it would
         * if this init()-time attempt were the only mechanism. */
        (void)config_store_save_payload(&g_running_config,
                                        sizeof(g_running_config),
                                        CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY,
                                        CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY,
                                        NULL);
    }
    return true;
}

void config_service_set_runtime_apply_callback(config_service_runtime_apply_fn callback)
{
    taskENTER_CRITICAL();
    g_runtime_apply_callback = callback;
    taskEXIT_CRITICAL();
}

void config_service_set_restart_callback(config_service_restart_fn callback)
{
    taskENTER_CRITICAL();
    g_restart_callback = callback;
    taskEXIT_CRITICAL();
}

const device_profile_config_t *config_service_get(void)
{
    return &g_running_config;
}

const device_profile_config_t *config_service_get_staged(void)
{
    return &g_staged_config;
}

bool config_service_reset_to_defaults(void)
{
    if (!g_default_config_valid)
    {
        return false;
    }

    taskENTER_CRITICAL();
    g_staged_config = g_default_config;
    g_staged_revision++;
    taskEXIT_CRITICAL();
    return true;
}

/* Guards a gap in config_service_init()'s own first-boot persist (see its
 * doc comment): that persist is necessarily best-effort -- init() cannot
 * fail the entire boot over one transient Flash write -- so it can leave
 * Flash RUNNING uncommitted if that one write happened to fail. Without
 * this guard, the first real transaction after such a failure would commit
 * normally, but config_transaction_backend_commit()'s promote-RUNNING-to-
 * LAST_KNOWN_GOOD step would still have nothing to promote (RUNNING was
 * never committed), leaving LAST_KNOWN_GOOD empty exactly as if the
 * init()-time fix had never existed. Retried on every apply-style call
 * until it succeeds, rather than trusting a one-shot in-RAM flag that would
 * go stale after a single failed write and never retry; cheap on the common
 * path once RUNNING is committed (config_store_get_active_info() is a
 * metadata-only read, no Flash write).
 * @param current_running Snapshot of the configuration to persist as the
 * baseline if RUNNING is not already committed -- the caller's own
 * unmodified `previous_running`, never the candidate being applied. */
static bool config_service_ensure_running_persisted(const device_profile_config_t *current_running)
{
    if (config_store_get_active_info(NULL))
    {
        return true;
    }

    return config_store_save_payload(current_running,
                                     sizeof(*current_running),
                                     CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY,
                                     CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY,
                                     NULL);
}

bool config_service_apply(void)
{
    device_profile_config_t normalized_config;
    device_profile_config_t previous_running;
    config_service_runtime_apply_fn runtime_apply;
    uint32_t candidate_revision;
    uint32_t previous_revision;

    if (!config_service_begin_transaction())
    {
        return false;
    }

    taskENTER_CRITICAL();
    normalized_config = g_staged_config;
    runtime_apply = g_runtime_apply_callback;
    previous_running = g_running_config;
    previous_revision = g_running_revision;
    candidate_revision = g_running_revision + 1u;
    taskEXIT_CRITICAL();

    config_service_normalize(&normalized_config);

    if (config_validate_service_validate_candidate(&normalized_config, NULL, 0u) !=
        CONFIG_VALIDATE_RESULT_OK)
    {
        config_service_end_transaction();
        return false;
    }

    if (!config_service_ensure_running_persisted(&previous_running))
    {
        config_service_end_transaction();
        return false;
    }

    /* v0.5.0 T4 frozen order: persist staged -> stop/activate/verify (one
     * call into the registered runtime-apply callback) -> commit/rollback.
     * The old activate-before-persist order must not survive this release. */
    if (!config_store_stage_payload(&normalized_config,
                                    sizeof(normalized_config),
                                    CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY,
                                    CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY,
                                    NULL))
    {
        config_service_end_transaction();
        return false;
    }

    /* Stop affected drivers, start/verify the candidate, and publish the
     * channel-registry generation -- all inside the one runtime-apply call
     * (sensor_manager_activate_config() as of T4). Its own internal rollback
     * already restarts the previous drivers on any failure here, including
     * a failed verify, so nothing further is needed on this path beyond
     * discarding the now-orphaned staged Flash record. */
    if (runtime_apply != NULL && !runtime_apply(&normalized_config, candidate_revision))
    {
        (void)config_store_discard_staged();
        config_service_end_transaction();
        return false;
    }

    if (!config_store_commit_staged(NULL))
    {
        (void)config_store_discard_staged();
        if (runtime_apply != NULL)
        {
            /* Best-effort: the candidate is live but Flash could not be made
             * durable for it -- restore the previous drivers so running
             * hardware state matches what Flash will report on reboot. */
            (void)runtime_apply(&previous_running, previous_revision);
        }
        config_service_end_transaction();
        return false;
    }

    taskENTER_CRITICAL();
    g_running_config = normalized_config;
    g_running_revision = candidate_revision;
    taskEXIT_CRITICAL();

    config_service_run_commit_extensions(&normalized_config, &previous_running);

    config_service_end_transaction();
    return true;
}

config_service_rollback_result_t config_service_rollback_running_to_last_known_good(void)
{
    device_profile_config_t recovered_config;
    config_store_record_info_t info = {0};
    size_t payload_size = 0u;
    bool unused_requires_migration = false;
    uint32_t recovered_revision;
    config_service_runtime_apply_fn runtime_apply;

    if (!config_store_recover_fallback_to_last_known_good(NULL))
    {
        /* Nothing usable in LAST_KNOWN_GOOD -- Flash RUNNING is untouched,
         * so there is nothing a reboot would recover into that isn't
         * already true right now. */
        return CONFIG_SERVICE_ROLLBACK_UNAVAILABLE;
    }

    /* From here on, Flash RUNNING already holds LAST_KNOWN_GOOD's bytes --
     * any failure below leaves Flash ahead of this service's own RAM state
     * and of whatever runtime_apply last activated. A caller that gets
     * CONFIG_SERVICE_ROLLBACK_FLASH_CHANGED_INCOMPLETE back should force a
     * reboot rather than continue: a fresh boot re-derives both this
     * service's RAM state and the runtime-apply layer's own state
     * (channel_manager's registry, drivers) from Flash RUNNING from
     * scratch, which reconciles the split this function cannot undo
     * in-place (there is no "put the new candidate back" path once its own
     * activation already succeeded and Flash has moved on). */
    memset(g_persisted_payload, 0, sizeof(g_persisted_payload));
    if (!config_store_load_active(g_persisted_payload,
                                  sizeof(g_persisted_payload),
                                  &payload_size,
                                  &info) ||
        info.payload_format != CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY ||
        !config_compat_decode_versioned(info.schema_version,
                                        g_persisted_payload,
                                        payload_size,
                                        &recovered_config,
                                        &unused_requires_migration) ||
        config_validate_service_validate_candidate(&recovered_config, NULL, 0u) !=
            CONFIG_VALIDATE_RESULT_OK)
    {
        return CONFIG_SERVICE_ROLLBACK_FLASH_CHANGED_INCOMPLETE;
    }

    config_service_normalize(&recovered_config);
    recovered_revision = info.generation > 0u ? info.generation : (g_running_revision + 1u);

    taskENTER_CRITICAL();
    runtime_apply = g_runtime_apply_callback;
    taskEXIT_CRITICAL();

    /* Flash RUNNING and g_running_config were already committed to the *new*
     * (now-failed) candidate by the config_service_apply() that triggered the
     * restart this is recovering from -- so runtime_apply's own state
     * (channel_manager's live registry, active drivers) is still on that new
     * candidate too, and must be brought back to the recovered configuration
     * the same way config_service_apply() would activate any other
     * candidate. Skipping this and only touching config_service's own state
     * would rebuild the address space from a mixture: recovered device-level
     * fields plus still-new channel/driver runtime state. */
    if (runtime_apply != NULL && !runtime_apply(&recovered_config, recovered_revision))
    {
        return CONFIG_SERVICE_ROLLBACK_FLASH_CHANGED_INCOMPLETE;
    }

    taskENTER_CRITICAL();
    g_running_config = recovered_config;
    g_staged_config = recovered_config;
    g_running_revision = recovered_revision;
    g_staged_revision = g_running_revision;
    taskEXIT_CRITICAL();

    return CONFIG_SERVICE_ROLLBACK_OK;
}

bool config_service_set_timezone_offset_minutes(int16_t value)
{
    if (!config_service_is_valid_timezone_offset_minutes(value))
    {
        return false;
    }

    taskENTER_CRITICAL();
    g_running_config.timezone_offset_minutes = value;
    g_staged_config.timezone_offset_minutes = value;
    g_running_revision++;
    g_staged_revision++;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_set_asset_id(const char *value)
{
    device_profile_config_t persisted_config;
    size_t value_length;

    if (value == NULL)
    {
        return false;
    }

    value_length = strlen(value);
    if (value_length >= CONFIG_SERVICE_ASSET_ID_MAX_LEN)
    {
        return false;
    }

    taskENTER_CRITICAL();
    persisted_config = g_running_config;
    taskEXIT_CRITICAL();
    memset(persisted_config.asset_id, 0, sizeof(persisted_config.asset_id));
    memcpy(persisted_config.asset_id, value, value_length + 1u);

    if (!config_store_save_payload(&persisted_config,
                                   sizeof(persisted_config),
                                   CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY,
                                   CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY,
                                   NULL))
    {
        return false;
    }

    taskENTER_CRITICAL();
    memset(g_running_config.asset_id, 0, sizeof(g_running_config.asset_id));
    memset(g_staged_config.asset_id, 0, sizeof(g_staged_config.asset_id));
    memcpy(g_running_config.asset_id, value, value_length + 1u);
    memcpy(g_staged_config.asset_id, value, value_length + 1u);
    g_running_revision++;
    g_staged_revision++;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_stage_set_mqtt_pubsub_enabled(bool enabled)
{
    taskENTER_CRITICAL();
    g_staged_config.enable_mqtt_pubsub = enabled;
    g_staged_revision++;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_stage_set_location_paths(const char *hierarchical_path,
                                             const char *operational_path)
{
    device_profile_config_t updated_config;
    size_t hierarchical_length;
    size_t operational_length;

    if (hierarchical_path == NULL || operational_path == NULL ||
        !config_service_location_path_is_valid(hierarchical_path) ||
        !config_service_location_path_is_valid(operational_path))
    {
        return false;
    }

    hierarchical_length = strlen(hierarchical_path);
    operational_length = strlen(operational_path);
    if (hierarchical_length >= CONFIG_SERVICE_LOCATION_PATH_MAX_LEN ||
        operational_length >= CONFIG_SERVICE_LOCATION_PATH_MAX_LEN)
    {
        return false;
    }

    taskENTER_CRITICAL();
    updated_config = g_staged_config;
    taskEXIT_CRITICAL();

    memset(updated_config.hierarchical_location_path,
           0,
           sizeof(updated_config.hierarchical_location_path));
    memset(updated_config.operational_location_path,
           0,
           sizeof(updated_config.operational_location_path));
    memcpy(updated_config.hierarchical_location_path,
           hierarchical_path,
           hierarchical_length + 1u);
    memcpy(updated_config.operational_location_path,
           operational_path,
           operational_length + 1u);
    memset(updated_config.location, 0, sizeof(updated_config.location));
    strncpy(updated_config.location,
            hierarchical_path,
            sizeof(updated_config.location) - 1u);
    config_service_normalize(&updated_config);

    taskENTER_CRITICAL();
    g_staged_config = updated_config;
    g_staged_revision++;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_stage_set_identity(const device_identity_config_t *identity)
{
    device_profile_config_t updated_config;

    if (identity == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();
    updated_config = g_staged_config;
    taskEXIT_CRITICAL();

    memcpy(updated_config.device_name, identity->device_name, sizeof(updated_config.device_name));
    memcpy(updated_config.opcua_hostname, identity->opcua_hostname, sizeof(updated_config.opcua_hostname));
    memcpy(updated_config.product_instance_uri, identity->product_instance_uri, sizeof(updated_config.product_instance_uri));
    memcpy(updated_config.serial_number, identity->serial_number, sizeof(updated_config.serial_number));
    memcpy(updated_config.asset_id, identity->asset_id, sizeof(updated_config.asset_id));
    memcpy(updated_config.hw_revision, identity->hw_revision, sizeof(updated_config.hw_revision));
    memcpy(updated_config.fw_revision, identity->fw_revision, sizeof(updated_config.fw_revision));
    updated_config.manufacture_date_utc = identity->manufacture_date_utc;
    updated_config.calibration_date_utc = identity->calibration_date_utc;
    memcpy(updated_config.location, identity->location, sizeof(updated_config.location));
    memcpy(updated_config.asset_class, identity->asset_class, sizeof(updated_config.asset_class));
    memset(updated_config.hierarchical_location_path,
           0,
           sizeof(updated_config.hierarchical_location_path));
    strncpy(updated_config.hierarchical_location_path,
            identity->location,
            sizeof(updated_config.hierarchical_location_path) - 1u);
    config_service_normalize(&updated_config);

    taskENTER_CRITICAL();
    g_staged_config = updated_config;
    g_staged_revision++;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_stage_set_network(const device_network_config_t *network)
{
    device_profile_config_t updated_config;

    if (network == NULL || network->opcua_port == 0u)
    {
        return false;
    }

    taskENTER_CRITICAL();
    updated_config = g_staged_config;
    taskEXIT_CRITICAL();

    updated_config.network_mode = network->use_dhcp ? NETWORK_MODE_DHCP : NETWORK_MODE_STATIC;
    memcpy(updated_config.mac, network->mac, sizeof(updated_config.mac));
    memcpy(updated_config.opcua_hostname, network->opcua_hostname, sizeof(updated_config.opcua_hostname));
    updated_config.opcua_port = network->opcua_port;
    memcpy(updated_config.ntp_server, network->ntp_server, sizeof(updated_config.ntp_server));
    memcpy(updated_config.static_ip, network->static_ip, sizeof(updated_config.static_ip));
    memcpy(updated_config.static_netmask, network->static_netmask, sizeof(updated_config.static_netmask));
    memcpy(updated_config.static_gateway, network->static_gateway, sizeof(updated_config.static_gateway));
    memcpy(updated_config.static_dns, network->static_dns, sizeof(updated_config.static_dns));
    config_service_normalize(&updated_config);

    taskENTER_CRITICAL();
    g_staged_config = updated_config;
    g_staged_revision++;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_stage_copy_channel(size_t index, config_channel_definition_t *out_channel)
{
    if (out_channel == NULL || index >= PLATFORM_CHANNEL_LIMIT)
    {
        return false;
    }

    taskENTER_CRITICAL();
    *out_channel = g_staged_config.channels[index];
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_stage_set_channel(size_t index, const config_channel_definition_t *channel)
{
    device_profile_config_t updated_config;

    if (channel == NULL || index >= PLATFORM_CHANNEL_LIMIT)
    {
        return false;
    }

    taskENTER_CRITICAL();
    updated_config = g_staged_config;
    taskEXIT_CRITICAL();

    updated_config.channels[index] = *channel;
    config_service_normalize_channel(&updated_config.channels[index], index);

    taskENTER_CRITICAL();
    g_staged_config = updated_config;
    g_staged_revision++;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_stage_clear_channel(size_t index)
{
    config_channel_definition_t cleared_channel = {0};
    return config_service_stage_set_channel(index, &cleared_channel);
}

bool config_service_replace_and_save(const device_profile_config_t *config, bool apply_running)
{
    device_profile_config_t normalized_config;
    device_profile_config_t previous_running;
    config_service_runtime_apply_fn runtime_apply;
    uint32_t candidate_revision = 0u;
    uint32_t previous_revision = 0u;

    if (config == NULL)
    {
        return false;
    }

    if (!config_service_begin_transaction())
    {
        return false;
    }

    normalized_config = *config;
    config_service_normalize(&normalized_config);

    if (config_validate_service_validate_candidate(&normalized_config, NULL, 0u) !=
        CONFIG_VALIDATE_RESULT_OK)
    {
        config_service_end_transaction();
        return false;
    }

    taskENTER_CRITICAL();
    previous_running = g_running_config;
    previous_revision = g_running_revision;
    runtime_apply = g_runtime_apply_callback;
    candidate_revision = g_running_revision + 1u;
    taskEXIT_CRITICAL();

    if (!config_service_ensure_running_persisted(&previous_running))
    {
        config_service_end_transaction();
        return false;
    }

    /* v0.5.0 T4 frozen order: persist staged -> stop/activate/verify ->
     * commit/rollback. This function's old activate-before-persist order
     * must not survive this release. */
    if (!config_store_stage_payload(&normalized_config,
                                    sizeof(normalized_config),
                                    CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY,
                                    CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY,
                                    NULL))
    {
        config_service_end_transaction();
        return false;
    }

    if (apply_running && runtime_apply != NULL &&
        !runtime_apply(&normalized_config, candidate_revision))
    {
        (void)config_store_discard_staged();
        /* Keep a valid but non-running candidate staged for correction/retry. */
        taskENTER_CRITICAL();
        g_staged_config = normalized_config;
        g_staged_revision++;
        taskEXIT_CRITICAL();
        config_service_end_transaction();
        return false;
    }

    if (!config_store_commit_staged(NULL))
    {
        (void)config_store_discard_staged();
        if (apply_running && runtime_apply != NULL)
        {
            (void)runtime_apply(&previous_running, previous_revision);
        }
        config_service_end_transaction();
        return false;
    }

    taskENTER_CRITICAL();
    g_staged_config = normalized_config;
    g_staged_revision++;
    taskEXIT_CRITICAL();

    if (apply_running)
    {
        taskENTER_CRITICAL();
        g_running_config = normalized_config;
        g_running_revision = candidate_revision;
        taskEXIT_CRITICAL();

        config_service_run_commit_extensions(&normalized_config, &previous_running);
    }
    config_service_end_transaction();
    return true;
}

uint32_t config_service_get_running_revision(void)
{
    uint32_t revision = 0u;

    taskENTER_CRITICAL();
    revision = g_running_revision;
    taskEXIT_CRITICAL();
    return revision;
}

uint32_t config_service_get_staged_revision(void)
{
    uint32_t revision = 0u;

    taskENTER_CRITICAL();
    revision = g_staged_revision;
    taskEXIT_CRITICAL();
    return revision;
}

bool config_service_save(void)
{
    device_profile_config_t normalized_config;

    taskENTER_CRITICAL();
    normalized_config = g_staged_config;
    taskEXIT_CRITICAL();

    config_service_normalize(&normalized_config);

    taskENTER_CRITICAL();
    g_staged_config = normalized_config;
    g_staged_revision++;
    taskEXIT_CRITICAL();

    return config_store_save_payload(&normalized_config,
                                     sizeof(normalized_config),
                                     CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY,
                                     CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY,
                                     NULL);
}
