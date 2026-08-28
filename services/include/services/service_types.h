/**
 * @file
 * @brief Small shared enum vocabulary used across multiple services and the
 *        OPC UA adapter layer (quality, network mode, alarm state).
 *
 * These types have no owning `.c` file; they are pure type definitions
 * included wherever the concept is needed (channel manager, alarm service,
 * network manager, opcua_model).
 */
#ifndef SERVICES_SERVICE_TYPES_H
#define SERVICES_SERVICE_TYPES_H

/**
 * @brief OPC UA-style data quality tri-state for a channel sample/value.
 *
 * Mirrors the coarse-grained portion of the OPC UA StatusCode quality
 * model (Bad/Uncertain/Good) used by channel and history sample records.
 */
typedef enum
{
    SERVICE_QUALITY_BAD = 0,
    SERVICE_QUALITY_UNCERTAIN = 1,
    SERVICE_QUALITY_GOOD = 2
} service_quality_t;

/** @brief Selects whether `network_manager` acquires its address via DHCP or a static configuration. */
typedef enum
{
    NETWORK_MODE_DHCP = 0,
    NETWORK_MODE_STATIC = 1
} network_mode_t;

/**
 * @brief Legacy two-level (WARNING/CRITICAL) alarm state for the
 *        Temperature1 backward-compatibility surface.
 *
 * Frozen for compatibility; new per-channel alarm logic uses the richer
 * #alarm_condition_state_t instead. `temperature_alarm_state_t` and the
 * `TEMPERATURE_ALARM_STATE_*` macros below are legacy aliases retained so
 * existing callers compile unchanged.
 */
typedef enum
{
    TELEMETRY_ALARM_STATE_INVALID = 0,
    TELEMETRY_ALARM_STATE_NORMAL = 1,
    TELEMETRY_ALARM_STATE_WARNING = 2,
    TELEMETRY_ALARM_STATE_CRITICAL = 3
} telemetry_alarm_state_t;

/** @brief Legacy alias of #telemetry_alarm_state_t for the Temperature1 compatibility surface. */
typedef telemetry_alarm_state_t temperature_alarm_state_t;

#define TEMPERATURE_ALARM_STATE_INVALID TELEMETRY_ALARM_STATE_INVALID
#define TEMPERATURE_ALARM_STATE_NORMAL TELEMETRY_ALARM_STATE_NORMAL
#define TEMPERATURE_ALARM_STATE_HIGH TELEMETRY_ALARM_STATE_WARNING
#define TEMPERATURE_ALARM_STATE_HIGH_HIGH TELEMETRY_ALARM_STATE_CRITICAL

/**
 * @brief v0.4.3 per-channel alarm condition state.
 *
 * Unlike #telemetry_alarm_state_t (frozen above for the legacy Temperature1
 * two-level compatibility surface), this enum represents the full symmetric
 * high/high-high/low/low-low process state plus a discrete state for
 * Boolean channels. NORMAL is the only "clear" value so a plain
 * `(state != ALARM_CONDITION_STATE_NORMAL)` test matches "some numeric
 * alarm is active", mirroring how the legacy state used WARNING/CRITICAL
 * for the same purpose.
 */
typedef enum
{
    ALARM_CONDITION_STATE_NORMAL = 0,
    ALARM_CONDITION_STATE_LOW = 1,
    ALARM_CONDITION_STATE_HIGH = 2,
    ALARM_CONDITION_STATE_LOW_LOW = 3,
    ALARM_CONDITION_STATE_HIGH_HIGH = 4
} alarm_condition_state_t;

/**
 * @brief Distinct quality-fault sub-state, tracked independently of
 *        #alarm_condition_state_t.
 *
 * Kept separate so a stale/invalid channel quality never overwrites the
 * last good-quality process alarm state. BAD and UNCERTAIN are kept
 * separate (rather than collapsed into one "fault" flag) because
 * #service_quality_t already distinguishes them and operators benefit from
 * knowing whether a channel is definitely unavailable (BAD) versus merely
 * suspect (UNCERTAIN); the OPC UA layer maps each to a different condition
 * Severity/Quality StatusCode.
 */
typedef enum
{
    ALARM_QUALITY_FAULT_NONE = 0,
    ALARM_QUALITY_FAULT_UNCERTAIN = 1,
    ALARM_QUALITY_FAULT_BAD = 2
} alarm_quality_fault_t;

#endif
