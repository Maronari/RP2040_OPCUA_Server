#ifndef SERVICES_ALARM_SERVICE_H
#define SERVICES_ALARM_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/platform_limits.h"
#include "services/service_types.h"
#include "services/telemetry_channel.h"

/* v0.4.3: one independent alarm condition instance per alarm-eligible
 * channel (T5).  Bounded by the same firmware-wide channel limit as the
 * channel registry itself -- there can never be more eligible channels than
 * registered channels. */
#define ALARM_SERVICE_MAX_CONDITIONS PLATFORM_CHANNEL_LIMIT

/**
 * @brief Per-channel alarm condition instance, keyed by the channel's stable
 *        config-assigned id (never registry index or memory address).
 *
 * Owned by alarm_service.c's g_conditions array (one slot per alarm-eligible
 * channel, bound for life once assigned -- see alarm_service_init()).
 * Written by alarm_service_task()'s evaluation pass and by any task calling
 * alarm_service_acknowledge_channel()/alarm_service_acknowledge_all()
 * (typically an OpcUaTask Method callback); every access to the live copy in
 * g_conditions goes through taskENTER_CRITICAL()/taskEXIT_CRITICAL() (see
 * alarm_service.c:301-385 for the evaluate-then-commit sequence). Callers
 * only ever see a copy returned by alarm_service_get_condition_by_index()/
 * alarm_service_get_condition_by_channel_id(), never a live pointer.
 *
 * Quality-fault design: quality_fault is tracked independently of
 * process_state/discrete_active and never overwrites them.  A channel that
 * was WARNING/HIGH and then goes stale keeps process_state == HIGH while
 * quality_fault switches to BAD/UNCERTAIN, so a reader can distinguish
 * "process out of range" (process_state/discrete_active) from "sensor
 * unavailable" (quality_fault) by reading both fields -- they are
 * simultaneously visible, never collapsed into one lossy state the way the
 * v0.4.2 singleton collapsed everything into TELEMETRY_ALARM_STATE_INVALID.
 */
typedef struct
{
    char channel_id[TELEMETRY_CHANNEL_ID_MAX_LEN];
    char channel_name[TELEMETRY_CHANNEL_NAME_MAX_LEN];
    bool in_use;      /* slot bound to an alarm-eligible channel */
    bool is_discrete; /* true for Boolean channels (discrete state alarm) */

    service_quality_t quality;           /* last observed channel quality, any value */
    alarm_quality_fault_t quality_fault; /* NONE unless quality != GOOD */

    /* Sticky process/discrete state, updated only from GOOD-quality
     * evaluations and left untouched while quality_fault != NONE. */
    alarm_condition_state_t process_state; /* numeric channels */
    bool discrete_active;                  /* Boolean channels */

    /* true whenever the condition needs operator attention for any reason:
     * process_state != NORMAL, discrete_active, or quality_fault != NONE. */
    bool active;
    bool acknowledged;
    /* Incremented under the condition lock by acknowledgement operations.
     * Evaluation uses it to merge a concurrent acknowledgement rather than
     * overwrite it with an older local snapshot. */
    uint32_t acknowledgement_revision;

    double warning_threshold;
    double critical_threshold;
    double low_threshold;
    double low_low_threshold;
    double last_value; /* last numeric measured value (double), diagnostic only */

    uint32_t evaluation_count;
    uint32_t trip_count; /* count of confirmed transitions into a non-normal/active state */

    /* Debounce bookkeeping (numeric candidate xor discrete candidate is
     * meaningful depending on is_discrete). */
    bool debounce_active;
    alarm_condition_state_t pending_state;
    bool pending_discrete_active;
    uint32_t debounce_deadline_ms;
} alarm_condition_status_t;

/**
 * @brief Allocate the per-channel condition array and bind one condition
 *        instance to every alarm-eligible channel present at startup.
 *
 * Eligibility is decided by alarm_service_value_type_is_eligible(). Call
 * once during platform init, after the channel registry is populated.
 *
 * @return true on success.
 */
bool alarm_service_init(void);

/**
 * @brief FreeRTOS task entry point that periodically evaluates every alarm
 *        condition.
 *
 * Calls alarm_service_evaluate_all() on a fixed period; never returns.
 *
 * @param argument Unused; required by the FreeRTOS task-entry signature.
 */
void alarm_service_task(void *argument);

/**
 * @brief Run one bounded evaluation pass over every alarm-eligible channel.
 *
 * For each channel reported by channel_manager_count()/
 * channel_manager_copy_snapshot_by_index(), classifies the value against its
 * configured thresholds, applies hysteresis/debounce, and commits the result
 * under a brief critical section (see alarm_service.c:301-385). Reads
 * channel snapshots directly from channel_manager -- never through
 * history_service. Exposed separately from alarm_service_task()'s infinite
 * FreeRTOS loop so it can be driven deterministically (with an explicit
 * @p now_ms) from host tests.
 *
 * @param now_ms Current time in milliseconds, used for debounce-deadline
 *        arithmetic; must be monotonically non-decreasing across calls for
 *        debounce timing to behave correctly.
 * @note Not safe to call concurrently with itself (single evaluator
 *       assumption); safe to call concurrently with acknowledge/read APIs.
 */
void alarm_service_evaluate_all(uint32_t now_ms);

/** @brief Number of bound alarm condition slots (channels observed since the
 *         last alarm_service_init()). Thread-safe (critical-section read). */
size_t alarm_service_get_condition_count(void);
/**
 * @brief Copy one alarm condition by its bind-order slot index.
 * @param index Slot index in [0, alarm_service_get_condition_count()).
 * @param out_status Receives an owned copy of the condition; never a live
 *        pointer into the internal array.
 * @return true if @p index named a bound slot; false otherwise.
 */
bool alarm_service_get_condition_by_index(size_t index, alarm_condition_status_t *out_status);
/**
 * @brief Copy one alarm condition by the channel's stable string id.
 * @param channel_id Channel id to look up; never registry index.
 * @param out_status Receives an owned copy of the condition.
 * @return true if @p channel_id was bound to a condition; false otherwise.
 */
bool alarm_service_get_condition_by_channel_id(const char *channel_id, alarm_condition_status_t *out_status);

/**
 * @brief Acknowledge every alarm condition instance.
 *
 * Preserved v0.4.2 public contract, wired to the
 * "Device.Maintenance.AcknowledgeAllAlarms" OPC UA Method. Increments each
 * condition's acknowledgement_revision so a concurrent evaluation merges
 * rather than overwrites the acknowledgement.
 */
void alarm_service_acknowledge_all(void);
/**
 * @brief Acknowledge the single alarm condition bound to @p channel_id.
 *
 * v0.4.3 additive per-condition acknowledgement, backing the standard
 * per-condition OPC UA Acknowledge Method. See alarm_service_acknowledge_all()
 * for the revision-merge behavior.
 *
 * @param channel_id Channel id to acknowledge.
 * @return true if @p channel_id named a bound condition; false otherwise.
 */
bool alarm_service_acknowledge_channel(const char *channel_id);

/**
 * @brief Free the alarm condition instance bound to @p channel_id, if any.
 *
 * v0.5.0 T7: called from `config_service`'s commit-step extension point for
 * every channel deleted or modified (same channel id, different driver/
 * value_type) by a just-committed transaction, so a reused channel id never
 * surfaces a stranger's stale process_state/acknowledged/quality_fault.
 * Zeroes the slot in place (including its `in_use` flag) rather than
 * shifting other slots, and the slot becomes reclaimable: the next
 * `alarm_service_find_or_bind_snapshot()` call scans for a freed slot below
 * `g_condition_count` before extending the allocation cursor, so capacity is
 * not permanently lost across repeated delete/modify cycles on reused ids.
 *
 * @param channel_id Channel id to clear; never registry index.
 * @return true if @p channel_id named a bound condition (now cleared); false
 *         if it was not found or the arguments were invalid.
 */
bool alarm_service_clear_channel(const char *channel_id);

/**
 * @brief Alarm eligibility rule (T5 scope decision): every numeric or
 *        Boolean channel value type is eligible; String is not.
 *
 * Exposed so the OPC UA adapter builds exactly the same condition-instance
 * set as alarm_service without duplicating the rule.
 *
 * @param value_type Channel value type to test.
 * @return true if a channel of this value type gets an alarm condition.
 */
bool alarm_service_value_type_is_eligible(telemetry_channel_value_type_t value_type);

/* ---- Legacy v0.4.2 singleton compatibility shim ----
 *
 * Preserved only so that opcua_model/src/model_history.c's existing
 * MODEL_NODE_PROCESS_TEMPERATURE_ALARM_STATE read callback keeps compiling
 * and returning a best-effort (lossy) projection until it is updated to call
 * alarm_service_get_condition_by_channel_id() directly -- see this release's
 * report for the exact required change.  No new v0.4.3 code should use this
 * shim; it exists purely for source compatibility with a file outside this
 * task's boundary.
 */
/**
 * @brief Lossy v0.4.2-shaped projection of one alarm condition (the legacy
 *        singleton temperature-channel alarm view).
 *
 * @p channel_id/@p channel_name point at static storage owned by
 * alarm_service.c (g_legacy_channel_id/g_legacy_channel_name) and are valid
 * only until the next alarm_service_get_status() call -- not safe to retain
 * across calls or across tasks. @p state collapses HIGH/LOW into WARNING and
 * HIGH_HIGH/LOW_LOW into CRITICAL, and any quality_fault into INVALID,
 * discarding the distinction the v0.4.3 alarm_condition_status_t preserves.
 */
typedef struct
{
    const char *channel_id;
    const char *channel_name;
    bool warning_active;
    bool critical_active;
    bool acknowledged;
    service_quality_t quality;
    float warning_threshold;
    float critical_threshold;
    telemetry_alarm_state_t state;
    uint32_t evaluation_count;
    uint32_t trip_count;
} alarm_service_status_t;

/**
 * @brief Fill @p out_status with the legacy singleton view of the
 *        DEVICE_CHANNEL_ID_TEMPERATURE_1 alarm condition.
 *
 * Looks up the condition via alarm_service_get_condition_by_channel_id() and
 * projects it into the lossy v0.4.2 shape (see alarm_service_status_t). If
 * no condition is bound yet, returns a default INVALID/acknowledged status
 * naming DEVICE_CHANNEL_ID_TEMPERATURE_1 without failing.
 *
 * @param out_status Must not be NULL; fully overwritten on every call.
 * @note Legacy compatibility shim only -- see the comment above
 *       alarm_service_status_t. No new v0.4.3 code should call this.
 */
void alarm_service_get_status(alarm_service_status_t *out_status);

#endif
