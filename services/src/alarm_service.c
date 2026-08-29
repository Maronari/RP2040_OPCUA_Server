/**
 * @file
 * @brief Per-channel alarm/threshold evaluation service (v0.4.3).
 *
 * Responsibilities:
 * - Owns one alarm_condition_status_t per alarm-eligible channel (numeric or
 *   Boolean; String channels are never eligible, see
 *   alarm_service_value_type_is_eligible()).
 * - Evaluates every condition periodically from alarm_service_task(),
 *   applying symmetric high/high-high and low/low-low threshold
 *   classification with hysteresis-on-return-to-normal and debounce.
 * - Tracks channel quality independently of process/discrete state so a
 *   stale/bad-quality channel does not silently clear its last known alarm
 *   state.
 * - Serves per-condition and legacy singleton read/acknowledge APIs to the
 *   OPC UA adapter.
 *
 * Ownership: g_conditions (PSRAM-backed on device, static array on host) is
 * exclusively owned by this file; callers only ever receive owned copies.
 *
 * Concurrency: g_conditions/g_condition_count are protected by
 * taskENTER_CRITICAL()/taskEXIT_CRITICAL() around every access. Threshold
 * classification runs outside the critical section on a local snapshot;
 * only the final compare-and-commit is locked (see
 * alarm_service_evaluate_channel()). Concurrent acknowledgement is merged
 * via the acknowledgement_revision counter rather than overwritten.
 */

#include "services/alarm_service.h"

#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "services/channel_manager.h"
#include "services/device_channels.h"

#include "psram_allocator.h"

/* Bounded per-condition array.  Slots are bound to a channel id the first
 * time that channel is observed and never move afterward -- lookups are
 * always by channel id, never by registry index.  Allocated from PSRAM on
 * the reference target -- at PLATFORM_CHANNEL_LIMIT=16 this array alone is
 * over 2 KiB, and internal SRAM is the scarcer resource -- mirroring the
 * same PSRAM-backed, host-fallback pattern channel_manager.c already uses
 * for its own PLATFORM_CHANNEL_LIMIT-sized registry. */
static alarm_condition_status_t *g_conditions;
#if !defined(PICO_ON_DEVICE)
/* Host suites (and any non-PSRAM profile) get a fixed backing store;
 * production on the reference target never emits this BSS. */
static alarm_condition_status_t g_host_conditions[ALARM_SERVICE_MAX_CONDITIONS];
#endif
static size_t g_condition_count;

/* Legacy singleton shim storage (see alarm_service.h). */
static char g_legacy_channel_id[TELEMETRY_CHANNEL_ID_MAX_LEN];
static char g_legacy_channel_name[TELEMETRY_CHANNEL_NAME_MAX_LEN];

bool alarm_service_value_type_is_eligible(telemetry_channel_value_type_t value_type)
{
    switch (value_type)
    {
    case TELEMETRY_CHANNEL_VALUE_FLOAT:
    case TELEMETRY_CHANNEL_VALUE_DOUBLE:
    case TELEMETRY_CHANNEL_VALUE_INT32:
    case TELEMETRY_CHANNEL_VALUE_UINT32:
    case TELEMETRY_CHANNEL_VALUE_INT64:
    case TELEMETRY_CHANNEL_VALUE_UINT64:
    case TELEMETRY_CHANNEL_VALUE_BOOLEAN:
        return true;
    case TELEMETRY_CHANNEL_VALUE_STRING:
    default:
        return false;
    }
}

bool alarm_service_init(void)
{
    if (g_conditions == NULL)
    {
#if defined(PICO_ON_DEVICE)
        if (!psram_allocator_is_ready())
        {
            return false;
        }
        g_conditions = psram_calloc(ALARM_SERVICE_MAX_CONDITIONS, sizeof(*g_conditions));
        if (g_conditions == NULL)
        {
            return false;
        }
#else
        g_conditions = g_host_conditions;
#endif
    }

    taskENTER_CRITICAL();
    memset(g_conditions, 0, ALARM_SERVICE_MAX_CONDITIONS * sizeof(*g_conditions));
    g_condition_count = 0u;
    taskEXIT_CRITICAL();

    g_legacy_channel_id[0] = '\0';
    g_legacy_channel_name[0] = '\0';
    return true;
}

/**
 * @brief Find the condition slot bound to @p channel_id, or bind the next
 *        free slot to it, and copy the (possibly freshly-bound) slot out
 *        while holding the condition lock.
 *
 * Bind-once semantics: once a slot is bound to a channel id it is never
 * reassigned, so callers can rely on a stable @p out_index across calls.
 * The alarm task is the only evaluator, but acknowledgements originate from
 * an OPC UA task, so bind/snapshot/commit must not race them -- hence the
 * critical section around the whole search-or-bind sequence.
 *
 * @param channel_id Channel id to find or bind.
 * @param out_index Receives the bound slot index.
 * @param out_snapshot Receives an owned copy of the (found or freshly-bound)
 *        condition.
 * @return true on success; false on bad arguments or if every slot is
 *         already bound to a different channel (registry exhausted).
 * @note Slot allocation reclaims freed slots (see alarm_service_clear_channel()):
 *       it first scans [0, g_condition_count) for an `in_use == false` hole
 *       left by a clear, and only extends the g_condition_count cursor when
 *       none exists. Without this, a clear would permanently shrink capacity
 *       by one slot per call -- see docs/releases/v0.5.0.md T7.
 */
static bool alarm_service_find_or_bind_snapshot(const char *channel_id,
                                                size_t *out_index,
                                                alarm_condition_status_t *out_snapshot)
{
    size_t free_index = ALARM_SERVICE_MAX_CONDITIONS;

    if (channel_id == NULL || channel_id[0] == '\0' || g_conditions == NULL || out_index == NULL ||
        out_snapshot == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();
    for (size_t index = 0u; index < g_condition_count; ++index)
    {
        if (g_conditions[index].in_use &&
            strncmp(g_conditions[index].channel_id, channel_id, TELEMETRY_CHANNEL_ID_MAX_LEN) == 0)
        {
            *out_index = index;
            *out_snapshot = g_conditions[index];
            taskEXIT_CRITICAL();
            return true;
        }
    }

    for (size_t index = 0u; index < g_condition_count; ++index)
    {
        if (!g_conditions[index].in_use)
        {
            free_index = index;
            break;
        }
    }

    if (free_index == ALARM_SERVICE_MAX_CONDITIONS)
    {
        if (g_condition_count < ALARM_SERVICE_MAX_CONDITIONS)
        {
            free_index = g_condition_count;
            ++g_condition_count;
        }
        else
        {
            taskEXIT_CRITICAL();
            return false;
        }
    }

    memset(&g_conditions[free_index], 0, sizeof(g_conditions[free_index]));
    (void)snprintf(g_conditions[free_index].channel_id, sizeof(g_conditions[free_index].channel_id),
                   "%s", channel_id);
    g_conditions[free_index].in_use = true;
    g_conditions[free_index].acknowledged = true;
    g_conditions[free_index].quality = SERVICE_QUALITY_UNCERTAIN;
    g_conditions[free_index].quality_fault = ALARM_QUALITY_FAULT_UNCERTAIN;
    g_conditions[free_index].process_state = ALARM_CONDITION_STATE_NORMAL;
    *out_index = free_index;
    *out_snapshot = g_conditions[free_index];
    taskEXIT_CRITICAL();
    return true;
}

/**
 * @brief Extract a channel's runtime value as a double for numeric threshold
 *        comparison.
 *
 * @param channel Channel whose runtime.value union to read; value_type
 *        selects the active union member.
 * @param out_numeric Set to false (and 0.0 returned) for a non-numeric type
 *        (e.g. Boolean or String), true otherwise. Never NULL.
 * @return The extracted value, or 0.0 if not numeric.
 */
static double alarm_service_extract_numeric(const telemetry_channel_t *channel, bool *out_numeric)
{
    double value = 0.0;

    *out_numeric = true;
    switch (channel->metadata.value_type)
    {
    case TELEMETRY_CHANNEL_VALUE_DOUBLE:
        value = channel->runtime.value.f64;
        break;
    case TELEMETRY_CHANNEL_VALUE_FLOAT:
        value = channel->runtime.value.f32;
        break;
    case TELEMETRY_CHANNEL_VALUE_INT32:
        value = channel->runtime.value.i32;
        break;
    case TELEMETRY_CHANNEL_VALUE_UINT32:
        value = channel->runtime.value.u32;
        break;
    case TELEMETRY_CHANNEL_VALUE_INT64:
        value = (double)channel->runtime.value.i64;
        break;
    case TELEMETRY_CHANNEL_VALUE_UINT64:
        value = (double)channel->runtime.value.u64;
        break;
    default:
        *out_numeric = false;
        break;
    }
    return value;
}

/**
 * @brief Classify one numeric measurement into an alarm_condition_state_t
 *        using symmetric high/high-high and low/low-low thresholds with
 *        one-sided hysteresis on the return to NORMAL.
 *
 * Escalation and opposite-side crossing always take effect immediately;
 * hysteresis only delays the return toward NORMAL from whichever side the
 * condition is currently active on. A single hysteresis band is applied
 * relative to the "entry" threshold on each side (@p high / @p low), shared
 * by both severities on that side, matching common two-level industrial
 * alarm hysteresis conventions.
 *
 * @param current Condition's process_state before this evaluation; used
 *        only to decide which side's hysteresis band applies.
 * @param value Measured value to classify.
 * @param low_low Low-low (critical low) threshold.
 * @param low Low (warning low) threshold.
 * @param high High (warning high) threshold.
 * @param high_high High-high (critical high) threshold.
 * @param hysteresis Band width applied only when leaving a non-NORMAL state.
 * @return The candidate state; the caller (alarm_service_evaluate_numeric())
 *         still applies debounce before committing it.
 * @note The lower side (low/low_low) is only active when ordered
 *       (low_low <= low <= high); this lets the v0.4.2->v0.4.3 config
 *       migration represent "no lower thresholds configured" without a
 *       separate enable flag -- see the comment inside the function body.
 */
static alarm_condition_state_t alarm_service_classify_numeric(alarm_condition_state_t current,
                                                               double value,
                                                               double low_low,
                                                               double low,
                                                               double high,
                                                               double high_high,
                                                               double hysteresis)
{
    /* The migration decoder represents absent pre-v0.4.3 lower thresholds
     * with finite values above the high entry threshold.  Require an ordered
     * lower side before evaluating it: native zero remains a valid lower
     * threshold whenever 0 <= high, while a high-only legacy configuration
     * cannot create alarms for ordinary negative measurements. */
    const bool lower_side_enabled = low_low <= low && low <= high;

    if (value >= high_high)
    {
        return ALARM_CONDITION_STATE_HIGH_HIGH;
    }
    if (lower_side_enabled && value <= low_low)
    {
        return ALARM_CONDITION_STATE_LOW_LOW;
    }
    if (value >= high)
    {
        return ALARM_CONDITION_STATE_HIGH;
    }
    if (lower_side_enabled && value <= low)
    {
        return ALARM_CONDITION_STATE_LOW;
    }

    switch (current)
    {
    case ALARM_CONDITION_STATE_HIGH:
    case ALARM_CONDITION_STATE_HIGH_HIGH:
        return (value < high - hysteresis) ? ALARM_CONDITION_STATE_NORMAL : ALARM_CONDITION_STATE_HIGH;
    case ALARM_CONDITION_STATE_LOW:
    case ALARM_CONDITION_STATE_LOW_LOW:
        return (!lower_side_enabled || value > low + hysteresis) ?
                   ALARM_CONDITION_STATE_NORMAL : ALARM_CONDITION_STATE_LOW;
    case ALARM_CONDITION_STATE_NORMAL:
    default:
        return ALARM_CONDITION_STATE_NORMAL;
    }
}

/**
 * @brief Whether a debounce deadline (in FreeRTOS tick-ms terms) has
 *        elapsed.
 *
 * Uses signed-difference wraparound-safe comparison so it stays correct
 * across a tick-count rollover, rather than a plain @p now_ms >= @p
 * deadline_ms comparison.
 */
static bool alarm_service_debounce_elapsed(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

/**
 * @brief Apply debounce to a numeric classification result and commit the
 *        process_state transition once the debounce deadline elapses.
 *
 * Classifies @p value via alarm_service_classify_numeric(), then: if the
 * candidate equals the current process_state, cancels any pending debounce;
 * if it differs and doesn't match the currently pending candidate, (re)starts
 * a debounce window of channel->settings.alarm_debounce_ms; once the window
 * elapses, commits the candidate as the new process_state and, for any
 * transition into a non-NORMAL state, increments trip_count and clears
 * acknowledged.
 *
 * @param condition Local (not-yet-committed) condition snapshot to mutate;
 *        not the live registry entry.
 * @param channel Channel providing thresholds/hysteresis/debounce settings.
 * @param value Extracted numeric value (see alarm_service_extract_numeric()).
 * @param now_ms Current time in milliseconds for debounce-deadline math.
 */
static void alarm_service_evaluate_numeric(alarm_condition_status_t *condition,
                                           const telemetry_channel_t *channel,
                                           double value,
                                           uint32_t now_ms)
{
    const alarm_condition_state_t candidate =
        alarm_service_classify_numeric(condition->process_state,
                                       value,
                                       channel->settings.low_low_threshold,
                                       channel->settings.low_threshold,
                                       channel->settings.warning_threshold,
                                       channel->settings.critical_threshold,
                                       channel->settings.alarm_hysteresis);

    condition->last_value = value;

    if (candidate == condition->process_state)
    {
        condition->debounce_active = false;
        return;
    }

    if (!condition->debounce_active || condition->pending_state != candidate)
    {
        condition->debounce_active = true;
        condition->pending_state = candidate;
        condition->debounce_deadline_ms = now_ms + channel->settings.alarm_debounce_ms;
    }

    if (alarm_service_debounce_elapsed(now_ms, condition->debounce_deadline_ms))
    {
        const alarm_condition_state_t previous = condition->process_state;

        condition->process_state = candidate;
        condition->debounce_active = false;
        if (candidate != ALARM_CONDITION_STATE_NORMAL && candidate != previous)
        {
            condition->trip_count++;
            condition->acknowledged = false;
        }
    }
}

/**
 * @brief Apply debounce to a Boolean channel's discrete alarm state and
 *        commit the transition once the debounce deadline elapses.
 *
 * Same debounce shape as alarm_service_evaluate_numeric() but for
 * discrete_active instead of process_state: a transition to active
 * (false -> true) after debounce increments trip_count and clears
 * acknowledged; a transition to inactive does not.
 *
 * @param condition Local (not-yet-committed) condition snapshot to mutate.
 * @param channel Channel providing alarm_debounce_ms.
 * @param value Current Boolean channel value.
 * @param now_ms Current time in milliseconds for debounce-deadline math.
 */
static void alarm_service_evaluate_discrete(alarm_condition_status_t *condition,
                                            const telemetry_channel_t *channel,
                                            bool value,
                                            uint32_t now_ms)
{
    condition->last_value = value ? 1.0 : 0.0;

    if (value == condition->discrete_active)
    {
        condition->debounce_active = false;
        return;
    }

    if (!condition->debounce_active || condition->pending_discrete_active != value)
    {
        condition->debounce_active = true;
        condition->pending_discrete_active = value;
        condition->debounce_deadline_ms = now_ms + channel->settings.alarm_debounce_ms;
    }

    if (alarm_service_debounce_elapsed(now_ms, condition->debounce_deadline_ms))
    {
        const bool was_active = condition->discrete_active;

        condition->discrete_active = value;
        condition->debounce_active = false;
        if (!was_active && value)
        {
            condition->trip_count++;
            condition->acknowledged = false;
        }
    }
}

/**
 * @brief Evaluate one channel's alarm condition and commit the result.
 *
 * Called from alarm_service_evaluate_all() for every alarm-eligible channel
 * snapshot. Finds/binds the condition slot (see
 * alarm_service_find_or_bind_snapshot()), evaluates outside any lock into a
 * local copy: numeric channels via alarm_service_evaluate_numeric(), Boolean
 * channels via alarm_service_evaluate_discrete(). If the channel is disabled
 * or its quality is not GOOD, process_state/discrete_active are left
 * untouched (sticky) and quality_fault is set to BAD or UNCERTAIN instead;
 * any in-flight debounce is cancelled since it was computed from
 * now-untrusted data. Recomputes the aggregate `active` flag, then commits
 * under the condition lock -- but only if a concurrent acknowledgement
 * changed acknowledgement_revision since the snapshot was taken, in which
 * case the newer acknowledged/acknowledgement_revision win over this
 * evaluation's stale local copy (see alarm_service.h's
 * acknowledgement_revision field doc).
 *
 * @param channel Channel snapshot to evaluate; must have a non-empty
 *        metadata.id (silently ignored otherwise).
 * @param now_ms Current time in milliseconds, forwarded to the debounce
 *        helpers.
 */
static void alarm_service_evaluate_channel(const telemetry_channel_t *channel, uint32_t now_ms)
{
    alarm_condition_status_t local;
    size_t condition_index;
    const bool is_discrete = channel->metadata.value_type == TELEMETRY_CHANNEL_VALUE_BOOLEAN;
    bool numeric = false;
    const double numeric_value = is_discrete ? 0.0 : alarm_service_extract_numeric(channel, &numeric);
    const bool enabled = (channel->metadata.flags & TELEMETRY_CHANNEL_FLAG_ENABLED) != 0u;
    const bool good = enabled && channel->runtime.quality == SERVICE_QUALITY_GOOD &&
                      (is_discrete || numeric);

    if (channel->metadata.id == NULL || channel->metadata.id[0] == '\0')
    {
        return;
    }

    if (!alarm_service_find_or_bind_snapshot(channel->metadata.id, &condition_index, &local))
    {
        return;
    }

    (void)snprintf(local.channel_name, sizeof(local.channel_name), "%s",
                   channel->metadata.name != NULL ? channel->metadata.name : "");
    local.is_discrete = is_discrete;
    local.warning_threshold = channel->settings.warning_threshold;
    local.critical_threshold = channel->settings.critical_threshold;
    local.low_threshold = channel->settings.low_threshold;
    local.low_low_threshold = channel->settings.low_low_threshold;
    local.quality = channel->runtime.quality;
    local.evaluation_count++;

    if (good)
    {
        local.quality_fault = ALARM_QUALITY_FAULT_NONE;
        if (is_discrete)
        {
            alarm_service_evaluate_discrete(&local, channel, channel->runtime.value.boolean, now_ms);
        }
        else
        {
            alarm_service_evaluate_numeric(&local, channel, numeric_value, now_ms);
        }
    }
    else
    {
        /* Sticky fault: process_state/discrete_active are left untouched so
         * the last good-quality alarm state stays visible.  Any in-flight
         * debounce is cancelled -- it was computed from data that is now
         * considered untrustworthy, so a fresh candidate is required once
         * quality returns to GOOD. */
        local.debounce_active = false;
        local.quality_fault = channel->runtime.quality == SERVICE_QUALITY_BAD
                                  ? ALARM_QUALITY_FAULT_BAD
                                  : ALARM_QUALITY_FAULT_UNCERTAIN;
    }

    local.active = local.quality_fault != ALARM_QUALITY_FAULT_NONE ||
                   (local.is_discrete ? local.discrete_active
                                       : local.process_state != ALARM_CONDITION_STATE_NORMAL);

#if defined(ALARM_SERVICE_TEST_HOOKS)
    /* Host-only deterministic interleaving hook. It runs outside the
     * condition lock, modelling an OPC UA Acknowledge between snapshot and
     * commit. */
    extern void alarm_service_test_before_commit(const char *channel_id);
    alarm_service_test_before_commit(channel->metadata.id);
#endif

    taskENTER_CRITICAL();
    if (condition_index < g_condition_count && g_conditions[condition_index].in_use &&
        strncmp(g_conditions[condition_index].channel_id, channel->metadata.id,
                TELEMETRY_CHANNEL_ID_MAX_LEN) == 0)
    {
        /* An acknowledgement newer than this evaluation snapshot wins. A
         * new trip still clears acknowledgement when no concurrent operator
         * action occurred. */
        if (g_conditions[condition_index].acknowledgement_revision != local.acknowledgement_revision)
        {
            local.acknowledged = g_conditions[condition_index].acknowledged;
            local.acknowledgement_revision = g_conditions[condition_index].acknowledgement_revision;
        }
        g_conditions[condition_index] = local;
    }
    taskEXIT_CRITICAL();
}

void alarm_service_evaluate_all(uint32_t now_ms)
{
    const size_t channel_count = channel_manager_count();

    for (size_t index = 0u; index < channel_count; ++index)
    {
        telemetry_channel_snapshot_t snapshot = {0};

        if (!channel_manager_copy_snapshot_by_index(index, &snapshot))
        {
            continue;
        }
        if (!alarm_service_value_type_is_eligible(snapshot.channel.metadata.value_type))
        {
            continue;
        }

        alarm_service_evaluate_channel(&snapshot.channel, now_ms);
    }
}

void alarm_service_task(void *argument)
{
    TickType_t last_wake_time;

    (void)argument;
    last_wake_time = xTaskGetTickCount();

    while (1)
    {
        alarm_service_evaluate_all((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(500));
    }
}

size_t alarm_service_get_condition_count(void)
{
    size_t count;

    taskENTER_CRITICAL();
    count = g_condition_count;
    taskEXIT_CRITICAL();
    return count;
}

bool alarm_service_get_condition_by_index(size_t index, alarm_condition_status_t *out_status)
{
    bool found = false;

    if (out_status == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();
    if (index < g_condition_count)
    {
        *out_status = g_conditions[index];
        found = true;
    }
    taskEXIT_CRITICAL();
    return found;
}

bool alarm_service_get_condition_by_channel_id(const char *channel_id, alarm_condition_status_t *out_status)
{
    bool found = false;

    if (out_status == NULL || channel_id == NULL || channel_id[0] == '\0')
    {
        return false;
    }

    taskENTER_CRITICAL();
    for (size_t index = 0u; index < g_condition_count; ++index)
    {
        if (g_conditions[index].in_use &&
            strncmp(g_conditions[index].channel_id, channel_id, TELEMETRY_CHANNEL_ID_MAX_LEN) == 0)
        {
            *out_status = g_conditions[index];
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL();
    return found;
}

void alarm_service_acknowledge_all(void)
{
    taskENTER_CRITICAL();
    for (size_t index = 0u; index < g_condition_count; ++index)
    {
        g_conditions[index].acknowledged = true;
        g_conditions[index].acknowledgement_revision++;
    }
    taskEXIT_CRITICAL();
}

bool alarm_service_acknowledge_channel(const char *channel_id)
{
    bool found = false;

    if (channel_id == NULL || channel_id[0] == '\0')
    {
        return false;
    }

    taskENTER_CRITICAL();
    for (size_t index = 0u; index < g_condition_count; ++index)
    {
        if (g_conditions[index].in_use &&
            strncmp(g_conditions[index].channel_id, channel_id, TELEMETRY_CHANNEL_ID_MAX_LEN) == 0)
        {
            g_conditions[index].acknowledged = true;
            g_conditions[index].acknowledgement_revision++;
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL();
    return found;
}

bool alarm_service_clear_channel(const char *channel_id)
{
    bool found = false;

    if (channel_id == NULL || channel_id[0] == '\0' || g_conditions == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();
    for (size_t index = 0u; index < g_condition_count; ++index)
    {
        if (g_conditions[index].in_use &&
            strncmp(g_conditions[index].channel_id, channel_id, TELEMETRY_CHANNEL_ID_MAX_LEN) == 0)
        {
            /* Zeroing clears in_use, making this slot reclaimable by
             * alarm_service_find_or_bind_snapshot()'s free-slot scan. */
            memset(&g_conditions[index], 0, sizeof(g_conditions[index]));
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL();
    return found;
}

/* ---- Legacy v0.4.2 singleton compatibility shim ---- */

void alarm_service_get_status(alarm_service_status_t *out_status)
{
    alarm_condition_status_t condition = {0};

    if (out_status == NULL)
    {
        return;
    }

    memset(out_status, 0, sizeof(*out_status));
    out_status->quality = SERVICE_QUALITY_UNCERTAIN;
    out_status->state = TELEMETRY_ALARM_STATE_INVALID;
    out_status->acknowledged = true;

    if (!alarm_service_get_condition_by_channel_id(DEVICE_CHANNEL_ID_TEMPERATURE_1, &condition))
    {
        (void)snprintf(g_legacy_channel_id, sizeof(g_legacy_channel_id), "%s", DEVICE_CHANNEL_ID_TEMPERATURE_1);
        (void)snprintf(g_legacy_channel_name, sizeof(g_legacy_channel_name), "%s", DEVICE_CHANNEL_NAME_TEMPERATURE_1);
        out_status->channel_id = g_legacy_channel_id;
        out_status->channel_name = g_legacy_channel_name;
        return;
    }

    (void)snprintf(g_legacy_channel_id, sizeof(g_legacy_channel_id), "%s", condition.channel_id);
    (void)snprintf(g_legacy_channel_name, sizeof(g_legacy_channel_name), "%s", condition.channel_name);
    out_status->channel_id = g_legacy_channel_id;
    out_status->channel_name = g_legacy_channel_name;
    out_status->acknowledged = condition.acknowledged;
    out_status->quality = condition.quality;
    out_status->warning_threshold = (float)condition.warning_threshold;
    out_status->critical_threshold = (float)condition.critical_threshold;
    out_status->evaluation_count = condition.evaluation_count;
    out_status->trip_count = condition.trip_count;

    if (condition.quality_fault != ALARM_QUALITY_FAULT_NONE)
    {
        /* The legacy 4-value enum has no distinct fault state; collapse to
         * INVALID exactly as v0.4.2 did.  This lossy projection is why
         * model_history.c's callback should move to the per-channel API. */
        out_status->state = TELEMETRY_ALARM_STATE_INVALID;
        return;
    }

    switch (condition.process_state)
    {
    case ALARM_CONDITION_STATE_NORMAL:
        out_status->state = TELEMETRY_ALARM_STATE_NORMAL;
        break;
    case ALARM_CONDITION_STATE_HIGH:
    case ALARM_CONDITION_STATE_LOW:
        out_status->state = TELEMETRY_ALARM_STATE_WARNING;
        out_status->warning_active = true;
        break;
    case ALARM_CONDITION_STATE_HIGH_HIGH:
    case ALARM_CONDITION_STATE_LOW_LOW:
        out_status->state = TELEMETRY_ALARM_STATE_CRITICAL;
        out_status->warning_active = true;
        out_status->critical_active = true;
        break;
    default:
        out_status->state = TELEMETRY_ALARM_STATE_INVALID;
        break;
    }
}
