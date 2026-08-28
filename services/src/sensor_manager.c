/**
 * @file
 * @brief Sensor channel activation/polling; see
 *        services/include/services/sensor_manager.h for the public API
 *        contract.
 *
 * Responsibilities:
 * - Validate a candidate `device_profile_config_t` (bounded strings, driver
 *   compatibility, resource-claim conflicts) before touching any driver.
 * - Drive channel driver `start`/`stop`/`configure`/`self_test` lifecycle
 *   calls through the double-buffered candidate/active bank scheme.
 * - Publish the resulting channel set to `channel_manager`.
 * - Poll channels each "SensorTask" iteration and forward eligible samples
 *   to `history_service`.
 *
 * Ownership:
 * `g_runtime_storage` (PSRAM-allocated) holds both configuration banks'
 * `channel_config_t` and driver-context arrays; `g_active_bank` selects
 * which bank is currently published/active. `g_initialized`,
 * `g_applied_config_revision`, `g_active_count` are all owned by this
 * module.
 *
 * Concurrency:
 * All state here is touched only from "SensorTask"
 * (sensor_manager_task()) and from synchronous calls into
 * sensor_manager_activate_config() (either from sensor_manager_init() at
 * boot, or as `config_service`'s runtime apply callback during a config
 * transaction, which itself runs on the caller's task without holding
 * `config_service`'s lock - see docs/code/state-ownership.md). No FreeRTOS
 * lock is taken in this file; correctness instead relies on
 * sensor_manager_activate_config() never running concurrently with itself.
 */
#include "services/sensor_manager.h"

#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "drivers/native_channel_driver.h"
#include "services/channel_config.h"
#include "services/channel_manager.h"
#include "services/config_service.h"
#include "services/device_channels.h"
#include "services/driver_catalog.h"
#include "services/history_service.h"

#include "psram_allocator.h"

#define SENSOR_MANAGER_DEFAULT_POLL_PERIOD_MS 1000u

/** @brief Per-channel driver-private context: bound source descriptor plus a pointer back to its catalog entry. */
typedef struct
{
    native_channel_source_t source;
    const driver_catalog_entry_t *driver_entry;
} sensor_manager_channel_context_t;

/**
 * @brief PSRAM-allocated double-buffered storage for both configuration
 *        banks (index 0/1), indexed by `g_active_bank`/candidate bank.
 *
 * The active registry borrows only the active bank's driver contexts. A
 * candidate is built in the other bank, so a failed activation cannot alter
 * active source bindings or metadata.
 */
typedef struct
{
    channel_config_t configs[2u][PLATFORM_CHANNEL_LIMIT];
    sensor_manager_channel_context_t contexts[2u][PLATFORM_CHANNEL_LIMIT];
} sensor_manager_runtime_storage_t;

static bool g_initialized;
static uint32_t g_applied_config_revision;
static size_t g_active_count;
/* The active registry borrows only the active bank's driver contexts.  A
 * candidate is built in the other bank, so a failed activation cannot alter
 * active source bindings or metadata. */
static sensor_manager_runtime_storage_t *g_runtime_storage;
#define g_runtime_channel_configs (g_runtime_storage->configs)
#define g_channel_contexts (g_runtime_storage->contexts)
static uint8_t g_active_bank;

/** @brief Checks that @p text is NUL-terminated within @p capacity bytes, and non-empty if @p required. */
static bool sensor_manager_text_is_bounded(const char *text, size_t capacity, bool required)
{
    return text != NULL && memchr(text, '\0', capacity) != NULL && (!required || text[0] != '\0');
}

/**
 * @brief Validates and builds a candidate channel bank from @p config
 *        without touching any driver/hardware.
 *
 * Validates each in-use channel's string bounds, driver/source
 * compatibility (`driver_catalog_get_by_id`/`validate_source`), value-type
 * support, the v0.4.2 explicit-writable-flag rule (retaining v0.4.1's
 * opaque reserved-bit compatibility), and duplicate channel IDs. Collects
 * every channel's resource claims and rejects the whole candidate if any
 * two conflict (`driver_catalog_claims_are_compatible()`). This entire pass
 * is pure service validation: no peripheral is touched.
 *
 * @param config Source configuration to validate/prepare from.
 * @param candidate_bank Which of the two runtime-storage banks (0 or 1) to
 *        populate; the other bank (the active one) is left untouched.
 * @param out_count Receives the number of prepared channels on success.
 * @return true if the candidate is fully valid and resource-conflict-free;
 *         false on any validation failure (candidate bank contents are
 *         then not meaningful to the caller).
 */
static bool sensor_manager_prepare_candidate(const device_profile_config_t *config,
                                             uint8_t candidate_bank,
                                             size_t *out_count)
{
    driver_catalog_resource_claim_t claims[PLATFORM_CHANNEL_LIMIT * DRIVER_CATALOG_MAX_RESOURCE_CLAIMS_PER_CHANNEL];
    size_t prepared = 0u;
    size_t claim_count = 0u;

    if (config == NULL || out_count == NULL)
    {
        return false;
    }
    memset(g_runtime_channel_configs[candidate_bank], 0, sizeof(g_runtime_channel_configs[candidate_bank]));
    memset(g_channel_contexts[candidate_bank], 0, sizeof(g_channel_contexts[candidate_bank]));
    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        const config_channel_definition_t *stored = &config->channels[index];
        const driver_catalog_entry_t *entry;
        channel_config_t *runtime;
        driver_catalog_resource_claim_t channel_claims[DRIVER_CATALOG_MAX_RESOURCE_CLAIMS_PER_CHANNEL];
        size_t channel_claim_count = 0u;

        if (stored->in_use == 0u)
        {
            continue;
        }
        if (prepared >= PLATFORM_CHANNEL_LIMIT ||
            !sensor_manager_text_is_bounded(stored->id, CONFIG_SERVICE_CHANNEL_ID_MAX_LEN, true) ||
            !sensor_manager_text_is_bounded(stored->name, CONFIG_SERVICE_CHANNEL_NAME_MAX_LEN, true) ||
            !sensor_manager_text_is_bounded(stored->description, CONFIG_SERVICE_CHANNEL_DESCRIPTION_MAX_LEN, false) ||
            !sensor_manager_text_is_bounded(stored->unit, CONFIG_SERVICE_CHANNEL_UNIT_MAX_LEN, false) ||
            (entry = driver_catalog_get_by_id(stored->driver_id)) == NULL ||
            entry->validate_source == NULL || !entry->validate_source(stored->source_index, stored->source_flags) ||
            !driver_catalog_supports_value_type(entry, (telemetry_channel_value_type_t)stored->value_type) ||
            /* v0.4.1 persisted reserved flag bits without rejecting them.
             * Check an explicit v0.4.2 writable declaration here while
             * retaining that opaque-bit compatibility. */
            (((stored->flags & TELEMETRY_CHANNEL_FLAG_WRITABLE) != 0u) &&
             ((stored->flags & ~(TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_HISTORIZED |
                                 TELEMETRY_CHANNEL_FLAG_ALARM_ENABLED | TELEMETRY_CHANNEL_FLAG_WRITABLE)) == 0u) &&
             ((entry->capabilities & TELEMETRY_DRIVER_CAPABILITY_WRITE) == 0u || entry->driver == NULL ||
              entry->driver->write == NULL)))
        {
            return false;
        }
        for (size_t other = 0u; other < prepared; ++other)
        {
            if (strcmp(stored->id, g_runtime_channel_configs[candidate_bank][other].metadata.id) == 0)
            {
                return false;
            }
        }
        if (!driver_catalog_get_resource_claims(stored->driver_id, stored->source_index,
                                                stored->source_flags, channel_claims,
                                                DRIVER_CATALOG_MAX_RESOURCE_CLAIMS_PER_CHANNEL,
                                                &channel_claim_count) ||
            channel_claim_count > DRIVER_CATALOG_MAX_RESOURCE_CLAIMS_PER_CHANNEL ||
            claim_count > (sizeof(claims) / sizeof(claims[0])) - channel_claim_count)
        {
            return false;
        }
        memcpy(&claims[claim_count], channel_claims, channel_claim_count * sizeof(channel_claims[0]));
        claim_count += channel_claim_count;
        g_channel_contexts[candidate_bank][prepared].source.source_index = stored->source_index;
        g_channel_contexts[candidate_bank][prepared].source.source_flags = stored->source_flags;
        g_channel_contexts[candidate_bank][prepared].driver_entry = entry;
        runtime = &g_runtime_channel_configs[candidate_bank][prepared];
        runtime->metadata.id = stored->id;
        runtime->metadata.name = stored->name;
        runtime->metadata.description = stored->description;
        runtime->metadata.kind = (telemetry_channel_kind_t)stored->kind;
        runtime->metadata.value_type = (telemetry_channel_value_type_t)stored->value_type;
        runtime->metadata.unit = stored->unit;
        runtime->metadata.source = (telemetry_channel_source_descriptor_t){
            .driver_id = stored->driver_id,
            .source_index = stored->source_index,
            .source_flags = stored->source_flags,
            .reserved = 0u,
        };
        runtime->metadata.range.has_minimum = stored->has_minimum != 0u;
        runtime->metadata.range.has_maximum = stored->has_maximum != 0u;
        runtime->metadata.range.minimum = stored->minimum;
        runtime->metadata.range.maximum = stored->maximum;
        runtime->metadata.flags = stored->flags;
        runtime->settings = stored->settings;
        runtime->driver = entry->driver;
        runtime->driver_context = &g_channel_contexts[candidate_bank][prepared].source;
        ++prepared;
    }
    *out_count = prepared;
    return prepared != 0u && driver_catalog_claims_are_compatible(claims, claim_count);
}

/**
 * @brief Stops (in reverse order) the first @p count channel drivers in
 *        candidate bank @p bank, e.g. to unwind a partially-started candidate.
 *
 * @param bank Candidate bank index (0 or 1).
 * @param count Number of leading channels in that bank to stop, in reverse.
 * @note Calls each channel driver's `stop` vtable entry where present;
 *       touches hardware indirectly through those drivers.
 */
static void sensor_manager_stop_candidates(uint8_t bank, size_t count)
{
    while (count > 0u)
    {
        telemetry_channel_t channel = {0};
        channel_config_t *candidate = &g_runtime_channel_configs[bank][--count];

        channel.metadata = candidate->metadata;
        channel.settings = candidate->settings;
        channel.driver = candidate->driver;
        channel.driver_context = candidate->driver_context;
        if (channel.driver != NULL && channel.driver->stop != NULL)
        {
            (void)channel.driver->stop(&channel);
        }
    }
}

/**
 * @brief Starts, configures, and self-tests each enabled channel driver in
 *        candidate bank @p bank, in order.
 *
 * On the first failing channel (start/configure/self_test all treated as
 * fatal), rolls back by calling sensor_manager_stop_candidates() for every
 * channel started so far (including the failing one, since a failed start
 * may already have claimed or reconfigured shared hardware).
 *
 * @param bank Candidate bank index (0 or 1).
 * @param count Number of channels in that bank to attempt to start.
 * @return true if every enabled channel started/configured/self-tested
 *         successfully; false on first failure (candidate bank is left
 *         stopped).
 * @note Touches hardware indirectly through the channel drivers' `start`/
 *       `configure`/`self_test` vtable entries.
 */
static bool sensor_manager_start_candidates(uint8_t bank, size_t count)
{
    for (size_t index = 0u; index < count; ++index)
    {
        telemetry_channel_t channel = {0};
        channel_config_t *candidate = &g_runtime_channel_configs[bank][index];

        channel.metadata = candidate->metadata;
        channel.settings = candidate->settings;
        channel.driver = candidate->driver;
        channel.driver_context = candidate->driver_context;
        if (!channel.settings.enabled)
        {
            continue;
        }
        if (channel.driver == NULL || channel.driver->start == NULL ||
            channel.driver->start(&channel) != TELEMETRY_DRIVER_RESULT_OK)
        {
            /* A failed start may already have claimed or reconfigured shared
             * hardware; include that channel in reverse cleanup. */
            sensor_manager_stop_candidates(bank, index + 1u);
            return false;
        }
        if (channel.driver->configure != NULL &&
            channel.driver->configure(&channel, NULL) != TELEMETRY_DRIVER_RESULT_OK)
        {
            sensor_manager_stop_candidates(bank, index + 1u);
            return false;
        }
        if (channel.driver->self_test != NULL &&
            channel.driver->self_test(&channel) != TELEMETRY_DRIVER_RESULT_OK)
        {
            sensor_manager_stop_candidates(bank, index + 1u);
            return false;
        }
    }
    return true;
}

/**
 * @brief Stops every currently-active channel's driver, via `channel_manager`'s
 *        acquire/release driver-operation lease pattern.
 *
 * Iterates all channel slots, acquiring each one's driver-operation lease
 * (`channel_manager_acquire_driver_operation()`) so the stop call runs
 * outside any channel_manager critical section, then releases the lease.
 * @note Touches hardware indirectly through each channel driver's `stop`
 *       vtable entry.
 */
static void sensor_manager_stop_active(void)
{
    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        channel_manager_driver_operation_t operation;

        if (!channel_manager_acquire_driver_operation(index, &operation))
        {
            continue;
        }
        if (operation.snapshot.channel.driver != NULL && operation.snapshot.channel.driver->stop != NULL)
        {
            (void)operation.snapshot.channel.driver->stop(&operation.snapshot.channel);
        }
        channel_manager_release_driver_operation(&operation);
    }
}

/**
 * @brief Reapplies start/configure/self_test to every enabled active
 *        channel, used to restore known-good hardware after a failed
 *        candidate activation.
 *
 * Candidate startup can reconfigure a peripheral before its later
 * lifecycle step fails. Reapplying the entire known-good configuration
 * covers that case; the active registry alone is not sufficient to restore
 * hardware settings. Failures for individual channels are accumulated but
 * do not stop the loop from attempting the remaining channels.
 *
 * @return true if every enabled active channel's start/configure/self_test
 *         succeeded; false if any failed (caller marks the manager
 *         uninitialized in that case).
 * @note Touches hardware indirectly through the channel drivers' `start`/
 *       `configure`/`self_test` vtable entries.
 */
static bool sensor_manager_restart_active(void)
{
    bool all_restored = true;

    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        channel_manager_driver_operation_t operation;

        if (!channel_manager_acquire_driver_operation(index, &operation))
        {
            continue;
        }
        if (operation.snapshot.channel.settings.enabled && operation.snapshot.channel.driver != NULL)
        {
            bool channel_restored = true;
            if (operation.snapshot.channel.driver->start != NULL &&
                operation.snapshot.channel.driver->start(&operation.snapshot.channel) != TELEMETRY_DRIVER_RESULT_OK)
            {
                channel_restored = false;
            }
            if (channel_restored && operation.snapshot.channel.driver->configure != NULL &&
                operation.snapshot.channel.driver->configure(&operation.snapshot.channel, NULL) !=
                    TELEMETRY_DRIVER_RESULT_OK)
            {
                channel_restored = false;
            }
            if (channel_restored && operation.snapshot.channel.driver->self_test != NULL &&
                operation.snapshot.channel.driver->self_test(&operation.snapshot.channel) !=
                    TELEMETRY_DRIVER_RESULT_OK)
            {
                channel_restored = false;
            }
            all_restored = all_restored && channel_restored;
        }
        channel_manager_release_driver_operation(&operation);
    }
    return all_restored;
}

/* Candidate startup can reconfigure a peripheral before its later lifecycle
 * step fails.  Reapply the entire known-good configuration in that case; the
 * active registry alone is not sufficient to restore hardware settings. */
bool sensor_manager_activate_config(const device_profile_config_t *config,
                                    uint32_t candidate_revision)
{
    const uint8_t candidate_bank = (uint8_t)(g_active_bank ^ 1u);
    size_t prepared = 0u;

    /* This entire pass is pure service validation: no peripheral is touched. */
    if (!sensor_manager_prepare_candidate(config, candidate_bank, &prepared))
    {
        return false;
    }
    /* Shared/exclusive claims may use the same peripheral as the old runtime.
     * Quiesce it before candidate lifecycle work, then restore it on every
     * failure path.  The registry itself remains published until replacement
     * is ready, so readers retain a coherent known-good snapshot. */
    sensor_manager_stop_active();
    if (!sensor_manager_start_candidates(candidate_bank, prepared))
    {
        if (!sensor_manager_restart_active())
        {
            g_initialized = false;
        }
        return false;
    }

    if (!channel_manager_reset())
    {
        sensor_manager_stop_candidates(candidate_bank, prepared);
        if (!sensor_manager_restart_active())
        {
            g_initialized = false;
        }
        return false;
    }
    if (channel_manager_register_from_config(g_runtime_channel_configs[candidate_bank], prepared) != prepared)
    {
        sensor_manager_stop_candidates(candidate_bank, prepared);
        /* Registration failure is deterministic (capacity/metadata).  Recover
         * the old catalog and hardware rather than leaving an empty runtime. */
        const bool registry_restored = channel_manager_reset() &&
            channel_manager_register_from_config(g_runtime_channel_configs[g_active_bank], g_active_count) ==
                g_active_count;
        if (!registry_restored || !sensor_manager_restart_active())
        {
            g_initialized = false;
        }
        return false;
    }
    g_active_bank = candidate_bank;
    g_active_count = prepared;
    g_initialized = true;
    g_applied_config_revision = candidate_revision;
    return true;
}

/** @brief Activates the currently-running config-service configuration/revision via sensor_manager_activate_config(). */
static bool sensor_manager_activate_running_config(void)
{
    return sensor_manager_activate_config(config_service_get(),
                                          config_service_get_running_revision());
}

/**
 * @brief Computes the "SensorTask" poll interval as the minimum
 *        `sample_period_ms` across enabled channels (0 excluded), or the
 *        1000 ms default if none set a period.
 */
static uint32_t sensor_manager_resolve_poll_period_ms(void)
{
    uint32_t period = SENSOR_MANAGER_DEFAULT_POLL_PERIOD_MS;

    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        telemetry_channel_snapshot_t snapshot;

        if (!channel_manager_copy_snapshot_by_index(index, &snapshot) || !snapshot.channel.settings.enabled)
        {
            continue;
        }
        if (snapshot.channel.settings.sample_period_ms != 0u &&
            snapshot.channel.settings.sample_period_ms < period)
        {
            period = snapshot.channel.settings.sample_period_ms;
        }
    }
    return period;
}

/**
 * @brief Forwards each eligible channel's current runtime value to
 *        `history_service_record_channel_sample()`.
 *
 * A channel is eligible only if `TELEMETRY_CHANNEL_FLAG_HISTORIZED` is set,
 * its value type is not `TELEMETRY_CHANNEL_VALUE_STRING` (v0.4.3: string
 * channels are never historized regardless of the flag, matching
 * `history_service`'s own rejection of string writes), quality is GOOD, and
 * the source timestamp is valid. Called once per "SensorTask" iteration
 * after polling.
 */
static void sensor_manager_record_channel_history(void)
{
    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        telemetry_channel_snapshot_t snapshot;
        const telemetry_channel_t *channel;
        history_service_value_t value;

        if (!channel_manager_copy_snapshot_by_index(index, &snapshot))
        {
            continue;
        }
        channel = &snapshot.channel;
        /* Every historizing-enabled channel of any supported type except
         * STRING is recorded (v0.4.3): string channels are never historized
         * regardless of the flag, matching how history_service itself
         * rejects TELEMETRY_CHANNEL_VALUE_STRING writes. */
        if ((channel->metadata.flags & TELEMETRY_CHANNEL_FLAG_HISTORIZED) == 0u ||
            channel->metadata.value_type == TELEMETRY_CHANNEL_VALUE_STRING ||
            channel->runtime.quality != SERVICE_QUALITY_GOOD ||
            channel->runtime.source_timestamp_unix_us == TELEMETRY_CHANNEL_SOURCE_TIMESTAMP_INVALID)
        {
            continue;
        }
        memcpy(&value, &channel->runtime.value, sizeof(value));
        (void)history_service_record_channel_sample(channel->metadata.id, channel->metadata.value_type, &value,
                                                    channel->runtime.quality,
                                                    channel->runtime.source_timestamp_unix_us,
                                                    channel->settings.history_interval_ms);
    }
}

bool sensor_manager_init(void)
{
    g_initialized = false;
    g_applied_config_revision = 0u;
    g_active_count = 0u;
    g_active_bank = 0u;
    if (!channel_manager_is_ready() || !psram_allocator_is_ready())
    {
        return false;
    }
    if (g_runtime_storage == NULL)
    {
        g_runtime_storage = psram_calloc(1u, sizeof(*g_runtime_storage));
        if (g_runtime_storage == NULL)
        {
            return false;
        }
    }
    config_service_set_runtime_apply_callback(sensor_manager_activate_config);
    return sensor_manager_activate_running_config();
}

void sensor_manager_task(void *argument)
{
    TickType_t last_wake_time = xTaskGetTickCount();

    (void)argument;
    while (true)
    {
        if (config_service_get_running_revision() != g_applied_config_revision)
        {
            (void)sensor_manager_activate_running_config();
        }
        if (g_initialized)
        {
            (void)channel_manager_poll_all((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
            sensor_manager_record_channel_history();
        }
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(sensor_manager_resolve_poll_period_ms()));
    }
}

void sensor_manager_get_snapshot(sensor_manager_snapshot_t *out_snapshot)
{
    telemetry_channel_snapshot_t channel_snapshot;
    const telemetry_channel_t *channel;

    if (out_snapshot == NULL)
    {
        return;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->quality = SERVICE_QUALITY_BAD;
    out_snapshot->source_timestamp_unix_us = TELEMETRY_CHANNEL_SOURCE_TIMESTAMP_INVALID;
    if (!g_initialized)
    {
        return;
    }
    /* v0.4.0 temperature compatibility only: this is a projection from the
     * channel registry, not a second runtime sample or a selected primary. */
    if (!channel_manager_copy_snapshot_by_id(DEVICE_CHANNEL_ID_TEMPERATURE_1, &channel_snapshot))
    {
        return;
    }
    channel = &channel_snapshot.channel;
    out_snapshot->enabled = channel->settings.enabled;
    out_snapshot->quality = channel->runtime.quality;
    out_snapshot->source_timestamp_unix_us = channel->runtime.source_timestamp_unix_us;
    if (channel->runtime.source_timestamp_unix_us != TELEMETRY_CHANNEL_SOURCE_TIMESTAMP_INVALID)
    {
        out_snapshot->source_timestamp_epoch_s =
            (uint32_t)(channel->runtime.source_timestamp_unix_us / 1000000u);
    }
    out_snapshot->sample_count = channel->runtime.sample_count;
    out_snapshot->error_count = channel->runtime.error_count;
    out_snapshot->last_update_ms = channel->runtime.last_update_ms;
    if (channel->metadata.value_type == TELEMETRY_CHANNEL_VALUE_FLOAT)
    {
        out_snapshot->temperature_c = channel->runtime.value.f32;
    }
    else if (channel->metadata.value_type == TELEMETRY_CHANNEL_VALUE_DOUBLE)
    {
        out_snapshot->temperature_c = (float)channel->runtime.value.f64;
    }
}
