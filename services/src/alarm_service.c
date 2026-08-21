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

/* Finds the slot already bound to channel_id, or binds and returns the next
 * free slot.  Only ever called from alarm_service_evaluate_all(), which runs
 * on a single task, so no lock is required for the search/bind step itself;
 * readers only ever observe slots through the short critical section in
 * alarm_service_get_condition_by_index(). */
static alarm_condition_status_t *alarm_service_find_or_bind_slot(const char *channel_id)
{
    size_t free_index = ALARM_SERVICE_MAX_CONDITIONS;

    if (channel_id == NULL || channel_id[0] == '\0' || g_conditions == NULL)
    {
        return NULL;
    }

    for (size_t index = 0u; index < g_condition_count; ++index)
    {
        if (g_conditions[index].in_use &&
            strncmp(g_conditions[index].channel_id, channel_id, TELEMETRY_CHANNEL_ID_MAX_LEN) == 0)
        {
            return &g_conditions[index];
        }
    }

    if (g_condition_count < ALARM_SERVICE_MAX_CONDITIONS)
    {
        free_index = g_condition_count;
        ++g_condition_count;
    }
    else
    {
        return NULL;
    }

    memset(&g_conditions[free_index], 0, sizeof(g_conditions[free_index]));
    (void)snprintf(g_conditions[free_index].channel_id, sizeof(g_conditions[free_index].channel_id),
                   "%s", channel_id);
    g_conditions[free_index].in_use = true;
    g_conditions[free_index].acknowledged = true;
    g_conditions[free_index].quality = SERVICE_QUALITY_UNCERTAIN;
    g_conditions[free_index].quality_fault = ALARM_QUALITY_FAULT_UNCERTAIN;
    g_conditions[free_index].process_state = ALARM_CONDITION_STATE_NORMAL;
    return &g_conditions[free_index];
}

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

/* Escalation and opposite-side crossing always take effect immediately;
 * hysteresis only delays the return toward NORMAL from whichever side the
 * condition is currently active on.  A single hysteresis band is applied
 * relative to the "entry" threshold on each side (warning_threshold /
 * low_threshold), shared by both severities on that side, matching common
 * two-level industrial alarm hysteresis conventions. */
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

static bool alarm_service_debounce_elapsed(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

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

static void alarm_service_evaluate_channel(const telemetry_channel_t *channel, uint32_t now_ms)
{
    alarm_condition_status_t *condition;
    alarm_condition_status_t local;
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

    condition = alarm_service_find_or_bind_slot(channel->metadata.id);
    if (condition == NULL)
    {
        return;
    }

    /* Work on a local copy so the critical section below stays a bounded
     * struct assignment rather than spanning the whole evaluation. */
    local = *condition;

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

    taskENTER_CRITICAL();
    *condition = local;
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
