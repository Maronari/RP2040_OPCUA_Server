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

/*
 * Per-channel alarm condition instance, keyed by the channel's stable
 * config-assigned id (never registry index or memory address).
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

bool alarm_service_init(void);
void alarm_service_task(void *argument);

/* One bounded evaluation pass over every alarm-eligible channel.  Exposed
 * separately from alarm_service_task's infinite FreeRTOS loop so it can be
 * driven deterministically (with an explicit now_ms) from host tests. */
void alarm_service_evaluate_all(uint32_t now_ms);

size_t alarm_service_get_condition_count(void);
bool alarm_service_get_condition_by_index(size_t index, alarm_condition_status_t *out_status);
bool alarm_service_get_condition_by_channel_id(const char *channel_id, alarm_condition_status_t *out_status);

/* Preserved v0.4.2 public contract (wired to the
 * "Device.Maintenance.AcknowledgeAllAlarms" Method): acknowledges every
 * condition instance. */
void alarm_service_acknowledge_all(void);
/* v0.4.3 additive per-condition acknowledgement, backing the standard
 * per-condition OPC UA Acknowledge Method. */
bool alarm_service_acknowledge_channel(const char *channel_id);

/* Alarm eligibility rule (T5 scope decision): every numeric or Boolean
 * channel value type is eligible; String is not.  Exposed so the OPC UA
 * adapter builds exactly the same condition-instance set without
 * duplicating the rule. */
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

void alarm_service_get_status(alarm_service_status_t *out_status);

#endif
