#ifndef OPCUA_MODEL_INTERNAL_H
#define OPCUA_MODEL_INTERNAL_H

#include "opcua_model/opcua_model.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>
#include "ua_port_alloc.h"

#include "drivers/led_driver.h"
#include "nodesetinjector.h"
#include "open62541.h"
#include "pico/stdio.h"
#include "psram_allocator.h"
#include "services/alarm_service.h"
#include "services/benchmark_profile.h"
#include "services/channel_manager.h"
#include "services/config_service.h"
#include "services/config_export_service.h"
#include "services/config_import_service.h"
#include "services/config_presets.h"
#include "services/config_reset_service.h"
#include <services/config_validate_service.h>
#include "services/config_wire.h"
#include "services/device_channels.h"
#include "services/diag_service.h"
#include "services/opcua_resource_service.h"
#include "services/device_health_service.h"
#include "services/firmware_update_service.h"
#include "services/history_service.h"
#include "services/maintenance_service.h"
#include "services/network_manager.h"
#include "services/security_identity_import_service.h"
#include "services/security_certificate_group_service.h"
#include "services/security_service.h"
#include "services/security_state_service.h"
#include "services/security_trust_expiry_service.h"
#include "services/security_trust_import_service.h"
#include "services/sensor_manager.h"
#include "services/time_service.h"

#ifdef UA_ENABLE_ENCRYPTION_MBEDTLS
#include <mbedtls/sha1.h>
#include <mbedtls/x509_crt.h>
#include "test_certs.h"
#endif

#if defined(UA_ENABLE_PUBSUB) && defined(UA_ENABLE_MQTT)
/* open62541 v1.5.7's own public header only declares this for
 * POSIX/win32 (`(UA_ARCHITECTURE_POSIX && !UA_ARCHITECTURE_LWIP) ||
 * UA_ARCHITECTURE_WIN32`), even though the MQTT ConnectionManager
 * implementation itself is architecture-agnostic and still compiles in
 * for the lwip architecture (gated only by UA_ENABLE_MQTT). Restore the
 * declaration so this project's lwip build doesn't fall back to an
 * implicit int() prototype. */
UA_ConnectionManager *UA_ConnectionManager_new_MQTT(const UA_String eventSourceName);
#endif

#define MODEL_BOOTSTRAP_APPLICATION_URI "urn:open62541.server.application"
#define MODEL_CHANNEL_NAMESPACE_URI "urn:maronari:rp2-industrial-opcua:channel-model"
#define MODEL_CHANNEL_NODE_ID_MAX_LEN 128u

/**
 * @brief Compile-time driver-pack identity (ROADMAP.md 5.1), exposed
 *        read-only through `Device.Compatibility.FirmwareProfile`.
 *
 * Distinct from a runtime configuration preset (`services/config_presets.h`):
 * this names which fixed set of native drivers this firmware image was
 * built with, not which of those drivers a device's running configuration
 * actually uses. Set from the top-level `FIRMWARE_DRIVER_PACK` CMake cache
 * variable (see `CMakeLists.txt`/`opcua_model/CMakeLists.txt`, v0.5.1
 * T14) -- `"base"` (the generic reference drivers: internal temperature,
 * ADC voltage, GPIO input/output) or `"industrial"`
 * (`RP2350_INDUSTRIAL_MVP_V1`'s full v0.5.1 driver set).
 * `"environmental"`/`"motion"`/`"development"` are not yet defined -- see
 * `docs/releases/v0.5.1.md`'s still-open `ROADMAP.md:552` tracking.
 * `#ifndef`-guarded so a standalone build of this header (e.g. a host
 * test) that never passes the CMake definition still compiles.
 */
#ifndef MODEL_FIRMWARE_DRIVER_PACK_NAME
#define MODEL_FIRMWARE_DRIVER_PACK_NAME "industrial"
#endif

/**
 * @def fflush
 * @brief Redirects every `fflush(...)` call in the `opcua_model` sources to
 *        `stdio_flush()` so log output reaches the Pico SDK stdio backend
 *        immediately, regardless of which `FILE *` argument is passed.
 * @note Applies repo-wide within translation units that include this header;
 *       do not remove without confirming Pico SDK stdio buffering behavior.
 */
#define fflush(_stream) stdio_flush()

/**
 * @brief Discriminator for every fixed (non-channel, non-task) node the
 *        model exposes, stored in `model_node_context_t::kind` and switched
 *        on by the generic `model_read_value` / `model_write_value`
 *        DataSource callbacks (`model_history.c`, `model_configuration.c`)
 *        to decide which service call backs a given node.
 *
 * Grouped by subsystem in declaration order: DI identification block,
 * legacy single-channel process-temperature block, v0.4.2 generic per-channel
 * adapter nodes (`MODEL_NODE_CHANNEL_*`, context carries only a channel id),
 * diagnostics, network, time, security, configuration, and PubSub/messaging
 * mirror nodes. Values are not persisted or transmitted on the wire; only
 * uniqueness within this translation unit matters. Mirrors the `FIXED_CTX_*`
 * enum below one-for-one (excluding the channel/task kinds).
 */
typedef enum
{
    MODEL_NODE_IDENT_MANUFACTURER = 0,
    MODEL_NODE_IDENT_MODEL,
    MODEL_NODE_IDENT_SERIAL_NUMBER,
    MODEL_NODE_IDENT_HW_REVISION,
    MODEL_NODE_IDENT_FW_REVISION,
    MODEL_NODE_IDENT_BUILD_TIME,
    MODEL_NODE_IDENT_GIT_HASH,
    MODEL_NODE_IDENT_DEVICE_NAME,
    MODEL_NODE_IDENT_PRODUCT_INSTANCE_URI,
    MODEL_NODE_IDENT_ASSET_ID,
    MODEL_NODE_IDENT_REVISION_COUNTER,
    MODEL_NODE_IDENT_MAC_ADDRESS,
    MODEL_NODE_IDENT_MANUFACTURE_DATE,
    MODEL_NODE_IDENT_CALIBRATION_DATE,
    MODEL_NODE_IDENT_LOCATION,
    MODEL_NODE_IDENT_ASSET_CLASS,
    MODEL_NODE_DEVICE_HEALTH,
    MODEL_NODE_PROCESS_TEMPERATURE_VALUE,
    MODEL_NODE_PROCESS_TEMPERATURE_STATUS,
    MODEL_NODE_PROCESS_TEMPERATURE_SOURCE_TIMESTAMP,
    MODEL_NODE_PROCESS_TEMPERATURE_UNITS,
    MODEL_NODE_PROCESS_TEMPERATURE_UPDATE_PERIOD_MS,
    MODEL_NODE_PROCESS_TEMPERATURE_HISTORY_INTERVAL_MS,
    MODEL_NODE_PROCESS_TEMPERATURE_ALARM_STATE,
    /* v0.4.2 generic channel adapter nodes.  These contain no channel state;
     * their bounded contexts only hold a stable channel identifier. */
    MODEL_NODE_CHANNEL_VALUE,
    MODEL_NODE_CHANNEL_SAMPLE_PERIOD_MS,
    MODEL_NODE_CHANNEL_PUBLISH_PERIOD_MS,
    MODEL_NODE_DIAG_UPTIME_SEC,
    MODEL_NODE_DIAG_RESET_CAUSE,
    MODEL_NODE_DIAG_FREE_HEAP,
    MODEL_NODE_DIAG_MIN_EVER_FREE_HEAP,
    MODEL_NODE_DIAG_PSRAM_READY,
    MODEL_NODE_DIAG_PSRAM_TOTAL_BYTES,
    MODEL_NODE_DIAG_PSRAM_USED_BYTES,
    MODEL_NODE_DIAG_PSRAM_FREE_BYTES,
    MODEL_NODE_DIAG_PSRAM_LARGEST_FREE_BLOCK_BYTES,
    MODEL_NODE_DIAG_PSRAM_ALLOCATOR_MODE,
    MODEL_NODE_DIAG_CPU_LOAD_APPROX,
    MODEL_NODE_DIAG_LAST_FAULT_CODE,
    MODEL_NODE_DIAG_TASK_STACK_HIGH_WATER,
    /* v0.5.2 T2: one shared immutable node context backs the additive
     * resource diagnostics. The callback resolves the exact member from the
     * NodeId and reads the PSRAM-backed published snapshot; this avoids a
     * context object per counter in internal SRAM. */
    MODEL_NODE_DIAG_RESOURCE_SNAPSHOT,
    MODEL_NODE_NETWORK_MAC_ADDRESS,
    MODEL_NODE_NETWORK_IP_MODE,
    MODEL_NODE_NETWORK_IPV4,
    MODEL_NODE_NETWORK_MASK,
    MODEL_NODE_NETWORK_GATEWAY,
    MODEL_NODE_NETWORK_LINK_UP,
    MODEL_NODE_NETWORK_DHCP_STATE,
    MODEL_NODE_NETWORK_RECONNECT_COUNT,
    MODEL_NODE_TIME_SYNCED,
    MODEL_NODE_TIME_NTP_SERVER,
    MODEL_NODE_TIME_LAST_SYNC_EPOCH,
    MODEL_NODE_TIME_CLOCK_QUALITY,
    MODEL_NODE_TIME_TIMEZONE_OFFSET_MINUTES,
    MODEL_NODE_SECURITY_MODE,
    MODEL_NODE_SECURITY_COMMISSIONING_MODE,
    MODEL_NODE_SECURITY_TRUST_LIST_VERSION,
    MODEL_NODE_SECURITY_TRUSTED_CLIENT_COUNT,
    MODEL_NODE_SECURITY_LAST_AUTH_FAILURE,
    MODEL_NODE_SECURITY_FAILED_LOGIN_COUNT,
    MODEL_NODE_SECURITY_CERT_THUMBPRINT,
    MODEL_NODE_SECURITY_CERT_NOT_BEFORE,
    MODEL_NODE_SECURITY_CERT_NOT_AFTER,
    MODEL_NODE_SECURITY_DAYS_UNTIL_EXPIRY,
    MODEL_NODE_SECURITY_EXPIRY_STATE,
    MODEL_NODE_SECURITY_COMMISSIONING_STATE,
    MODEL_NODE_SECURITY_ACTIVE_IDENTITY_GENERATION,
    MODEL_NODE_SECURITY_ACTIVE_TRUST_GENERATION,
    MODEL_NODE_SECURITY_CURRENT_USER_ROLE,
    MODEL_NODE_SECURITY_LAST_EVENT_ID,
    MODEL_NODE_NETWORK_LAST_NETWORK_CHANGE,
    MODEL_NODE_DIAG_LAST_FAULT,
    MODEL_NODE_CONFIG_LAST_CONFIG_CHANGE,
    MODEL_NODE_CONFIG_SAMPLING_PERIOD_MS,
    MODEL_NODE_CONFIG_PUBLISH_PERIOD_MS,
    MODEL_NODE_CONFIG_TEMP_OFFSET,
    MODEL_NODE_CONFIG_TEMP_SCALE,
    MODEL_NODE_CONFIG_HIGH_ALARM,
    MODEL_NODE_CONFIG_HIGH_HIGH_ALARM,
    MODEL_NODE_CONFIG_REBOOT_REQUIRED,
    MODEL_NODE_CONFIG_COMMISSIONED,
    MODEL_NODE_MESSAGING_EVENT_SEQUENCE,
    MODEL_NODE_MESSAGING_EVENT_ID,
    MODEL_NODE_MESSAGING_EVENT_TYPE,
    MODEL_NODE_MESSAGING_EVENT_SOURCE,
    MODEL_NODE_MESSAGING_EVENT_MESSAGE,
    MODEL_NODE_MESSAGING_EVENT_SEVERITY,
    MODEL_NODE_MESSAGING_EVENT_TIME
} model_node_kind_t;

/**
 * @brief Per-node `void *nodeContext` payload open62541 passes back into
 *        `model_read_value` / `model_write_value` / diagnostic read
 *        callbacks for every variable node the model registers.
 *
 * Instances live in one of the fixed-size arrays inside
 * `opcua_model_context_t` (`fixed_node_contexts`, `task_node_contexts`,
 * `channel_node_contexts`, `generic_channel_node_contexts`) and therefore
 * share the static lifetime of the model context; open62541 only borrows a
 * pointer to them, it never owns or frees one. `channel_id_storage` exists
 * because node contexts outlive channel-registry snapshots, so the channel
 * key must be copied into model-owned bounded storage rather than pointing
 * back into a snapshot that may already be gone by the time a Read fires.
 */
typedef struct
{
    model_node_kind_t kind;
    const char *channel_id;
    /* Node contexts outlive registry snapshots. Keep their channel key in
     * model-owned bounded storage instead of retaining a snapshot pointer. */
    char channel_id_storage[TELEMETRY_CHANNEL_ID_MAX_LEN];
    const char *task_name;
} model_node_context_t;

/**
 * @brief Adapter-owned deferred-action state: a scheduled reboot, a pending
 *        controlled OPC UA server restart, and/or an in-progress
 *        identify-LED blink sequence, all driven by `model_core_task`'s poll
 *        loop rather than executed synchronously from the method callback
 *        (or, for `restart_pending`, the `config_service` commit-step
 *        callback) that requested them.
 *
 * Single instance embedded in `opcua_model_context_t` (`g_pending_actions`).
 * Written by `model_schedule_reboot` / `model_schedule_identify_blink` /
 * `model_config_service_restart_callback` (v0.5.0 T6) from a method-callback
 * or `config_service` commit-step context and consumed each iteration of
 * `model_core_task` (`OpcUaTask`); every writer and the sole reader run on
 * the same task (see `model_core_lifecycle.c`'s file-level concurrency
 * note for why: every current caller of `config_service_apply()`/
 * `config_service_replace_and_save()` is itself an OPC UA method callback,
 * i.e. already nested inside `OpcUaTask`'s own `UA_Server_run_iterate()`
 * call), so no locking is applied.
 *
 * @note `restart_pending` cannot be handled synchronously from the callback
 * that sets it the way its name might suggest: that callback runs nested
 * inside the very `UA_Server_run_iterate()` call whose `UA_Server` it would
 * need to tear down, so tearing down/rebuilding there would free memory the
 * outer call frame is still using. Deferring to the top-level loop (this
 * flag) is safe and still closes the race window `docs/releases/v0.5.0.md`
 * cares about: single-tasked execution means no further client request can
 * be served between the callback returning and the loop noticing this flag.
 */
typedef struct
{
    bool reboot_pending;
    TickType_t reboot_due_tick;
    bool restart_pending;
    bool identify_blink_active;
    TickType_t identify_next_tick;
    uint8_t identify_toggles_remaining;
    bool identify_led_on;
} model_pending_actions_t;

/**
 * @brief Per-session state open62541 stores as `session_context` for the
 *        lifetime of one client session, allocated by
 *        `model_activate_session_secure_wrapper` and released by
 *        `model_close_session_secure_wrapper`.
 *
 * `base_context` holds whatever the wrapped default `activateSession`
 * implementation returned (e.g. username/role state); this struct wraps it
 * with adapter-only bookkeeping. The rejected-maintenance-call rate limiter
 * fields are intentionally session-scoped and adapter-owned (never part of
 * `maintenance_service` state) so one misbehaving client cannot exhaust the
 * shared audit-event queue; `maintenance_reject_*` fields are mutated under
 * `taskENTER_CRITICAL`/`taskEXIT_CRITICAL` in `model_maintenance_rejected_audit_admit`
 * because they may be touched from more than one FreeRTOS task.
 */
typedef struct
{
    void *base_context;
    bool secure_channel;
    /* Fixed per-session rejected-call limiter. This belongs to the OPC UA
     * adapter session, never to maintenance service state. */
    bool maintenance_reject_limiter_initialized;
    TickType_t maintenance_reject_last_refill;
    uint8_t maintenance_reject_tokens;
    uint16_t maintenance_reject_suppressed;
    UA_StatusCode maintenance_reject_last_reason;
} model_session_context_t;

#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
/**
 * @brief Edge-detection state for `model_process_runtime_events` /
 *        `model_process_device_health_conditions` /
 *        `model_process_alarm_conditions`, tracking the previously observed
 *        network/sensor/certificate/trust snapshot so those functions only
 *        emit an event or flip a Condition when something actually changed.
 *
 * A caller-owned instance (not part of `opcua_model_context_t`) is
 * initialized once with `model_event_runtime_state_init` and then threaded
 * through every poll iteration of `model_core_task`; it is not shared with
 * any other task and needs no synchronization.
 */
typedef struct
{
    bool initialized;
    network_manager_status_t network_status;
    sensor_manager_snapshot_t sensor_snapshot;
    bool server_certificate_expiry_seen;
    uint32_t server_certificate_expiry_state;
    bool trust_expiry_seen;
    bool trust_expiry_commissioning_mode;
    security_trust_expiry_service_report_t trust_expiry_report;
#if defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
    bool security_condition_active;
    uint32_t security_condition_reason;
    bool security_condition_enabled;
    TickType_t security_condition_next_enable_poll;
    bool device_health_condition_active[5];
    uint32_t device_health_condition_reason[5];
    bool device_health_condition_enabled[5];
    TickType_t device_health_condition_next_enable_poll[5];
#endif
} model_event_runtime_state_t;

/** @brief Selects which `model_last_event_state_t` field an emitted event's
 *         id/message gets mirrored into by `model_store_last_event`. */
typedef enum
{
    MODEL_EVENT_LAST_KIND_GENERIC = 0,
    MODEL_EVENT_LAST_KIND_CONFIG = 1,
    MODEL_EVENT_LAST_KIND_NETWORK = 2,
    MODEL_EVENT_LAST_KIND_FAULT = 3
} model_event_last_kind_t;

/**
 * @brief Most-recent-event mirror surfaced through the
 *        `MODEL_NODE_SECURITY_LAST_EVENT_ID` / `MODEL_NODE_CONFIG_LAST_CONFIG_CHANGE`
 *        / `MODEL_NODE_DIAG_LAST_FAULT` / `MODEL_NODE_NETWORK_LAST_NETWORK_CHANGE`
 *        read-only variable nodes, since open62541 events themselves are
 *        fire-and-forget and have no queryable "last value".
 *
 * Owned by `opcua_model_context_t` (`g_last_event_state`); written only by
 * `model_store_last_event` from `OpcUaTask`/method-callback context.
 */
typedef struct
{
    char last_event_id[33];
    char last_config_change[160];
    char last_fault[160];
    char last_network_change[160];
} model_last_event_state_t;

#ifdef UA_ENABLE_PUBSUB
/**
 * @brief Latest published Events-mirror payload, surfaced read-only through
 *        the `Device.Messaging.LastPubSubEvent*` variable nodes since MQTT
 *        subscribers cannot browse/read the server's event log directly.
 *
 * Owned by `opcua_model_context_t` (`g_model_pubsub_event_mirror`), written
 * by `model_pubsub_update_event_mirror` from `OpcUaTask`/method-callback
 * context whenever an event is emitted.
 */
typedef struct
{
    uint32_t sequence;
    UA_DateTime time;
    UA_UInt32 severity;
    char event_id[33];
    char event_type[64];
    char source[64];
    char message[160];
} model_pubsub_event_mirror_t;

/**
 * @brief Adapter-owned PubSub/MQTT runtime state: whether MQTT publishing is
 *        enabled/configured, the resolved broker/topic/client-id strings,
 *        and the open62541 `UA_NodeId`s of the Connection/WriterGroups this
 *        model created, needed later to tear down or re-check state.
 *
 * Owned by `opcua_model_context_t` (`g_model_pubsub`); built by
 * `model_configure_pubsub_publishers` during server startup
 * (`model_core_task`) and read by `model_pubsub_trigger_event_publish` on
 * every poll iteration. Single-task ownership (`OpcUaTask`); not
 * synchronized. This is model/adapter runtime state, not service business
 * state — see `docs/code/modules/opcua_model.md` PubSub/MQTT section.
 */
typedef struct
{
    bool enabled;
    bool configured;
    bool use_json;
    char broker_url[CONFIG_SERVICE_TEXT_MAX_LEN];
    char topic_root[CONFIG_SERVICE_TEXT_MAX_LEN];
    char client_id[64];
    UA_NodeId connection_id;
    UA_NodeId telemetry_writer_group_ids[PLATFORM_CHANNEL_LIMIT];
    size_t telemetry_writer_group_count;
    UA_NodeId event_writer_group_id;
} model_pubsub_runtime_t;
#endif

/**
 * @brief One queued audit-log entry awaiting emission as an
 *        `MODEL_EVENT_TYPE_AUDIT*` open62541 event.
 *
 * Slots live in the fixed ring `opcua_model_context_t::pending_audit_events`
 * (`MODEL_AUDIT_QUEUE_CAPACITY` entries); `model_queue_pending_audit_event`
 * enqueues under `taskENTER_CRITICAL`/`taskEXIT_CRITICAL` (may be called
 * from `security_service`'s audit callback on a different task than
 * `OpcUaTask`), `model_pop_pending_audit_event` dequeues from
 * `model_core_task`. Oldest entry is overwritten if the queue is full.
 */
typedef struct
{
    char event[32];
    char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
    char subject[64];
    char details[160];
    bool success;
} model_pending_audit_event_t;

/** @brief `fields_context` payload for `model_write_config_event_fields`,
 *         naming which configuration section/key changed and who changed it. */
typedef struct
{
    const char *changed_section;
    const char *changed_key;
    const char *actor;
} model_config_event_fields_t;

/** @brief `fields_context` payload for `model_write_network_event_fields`,
 *         describing an IP-address/DHCP/link-state transition. */
typedef struct
{
    const char *old_ip;
    const char *new_ip;
    bool dhcp_enabled;
    bool link_up;
} model_network_event_fields_t;

/** @brief `fields_context` payload for `model_write_sensor_fault_event_fields`,
 *         identifying the faulting channel, its fault code/quality, and the
 *         last measured value. */
typedef struct
{
    const char *sensor_id;
    uint32_t fault_code;
    const char *quality;
    float measured_value;
} model_sensor_fault_event_fields_t;

/** @brief `fields_context` payload for `model_write_maintenance_event_fields`,
 *         naming the maintenance operation, its result code, and the actor. */
typedef struct
{
    const char *operation;
    uint32_t result_code;
    const char *actor;
} model_maintenance_event_fields_t;

/* AMB NameNodeIdDataType (ns=<AMB>;i=3003).  The optional NodeId is null for
 * v0.3.2 because the bounded service stores names, not borrowed UA nodes. */
/**
 * @brief C mirror of the AMB companion-spec `NameNodeIdDataType`
 *        (`ns=<AMB>;i=3003`), registered as a custom `UA_DataType` via
 *        `model_register_amb_name_node_id_type` so it can be used as an
 *        array-valued property value (e.g. supplier/personnel lists).
 */
typedef struct
{
    UA_LocalizedText name;
    UA_NodeId node_id;
} model_amb_name_node_id_t;

/** @brief Selects which field of the active `MaintenanceRequired` AMB
 *         condition a `model_read_maintenance_value` DataSource read should
 *         return; carried in `model_maintenance_value_context_t`. */
typedef enum
{
    MODEL_MAINTENANCE_VALUE_CURRENT_STATE = 0,
    MODEL_MAINTENANCE_VALUE_CURRENT_STATE_ID,
    MODEL_MAINTENANCE_VALUE_PLANNED_DATE,
    MODEL_MAINTENANCE_VALUE_ESTIMATED_DOWNTIME,
    MODEL_MAINTENANCE_VALUE_METHOD,
    MODEL_MAINTENANCE_VALUE_SUPPLIER,
    MODEL_MAINTENANCE_VALUE_PERSONNEL,
    MODEL_MAINTENANCE_VALUE_PARTS_REPLACED,
    MODEL_MAINTENANCE_VALUE_PARTS_SERVICED,
    MODEL_MAINTENANCE_VALUE_CONFIGURATION_CHANGED,
    MODEL_MAINTENANCE_VALUE_MESSAGE,
    MODEL_MAINTENANCE_VALUE_SEVERITY,
    MODEL_MAINTENANCE_VALUE_QUALITY,
    MODEL_MAINTENANCE_VALUE_TIME,
    MODEL_MAINTENANCE_VALUE_RECEIVE_TIME,
    MODEL_MAINTENANCE_VALUE_ACTIVE_STATE,
    MODEL_MAINTENANCE_VALUE_RETAIN
} model_maintenance_value_kind_t;

/**
 * @brief `nodeContext` for every value-source variable node installed under
 *        the `MaintenanceRequired` AMB condition by
 *        `model_install_maintenance_value_source[_at_path]`; read by
 *        `model_read_maintenance_value`.
 */
typedef struct
{
    model_maintenance_value_kind_t kind;
} model_maintenance_value_context_t;

/** @brief `fields_context` payload for `model_write_firmware_event_fields`. */
typedef struct
{
    const char *version;
    const char *build_time;
    const char *git_hash;
} model_firmware_event_fields_t;

/** @brief Common actor/subject/details/result fields shared by every audit
 *         event; `fields_context` payload for `model_write_audit_event_fields`
 *         and embedded in the composite audit-emit structs below. */
typedef struct
{
    const char *actor;
    const char *subject;
    const char *details;
    bool result;
} model_audit_event_fields_t;

/** @brief `fields_context` payload for `model_write_audit_config_event_fields`. */
typedef struct
{
    const char *changed_section;
    const char *changed_key;
} model_audit_config_event_fields_t;

/** @brief `fields_context` payload for `model_write_audit_security_event_fields`. */
typedef struct
{
    const char *operation;
} model_audit_security_event_fields_t;

/** @brief Combined `fields_context` payload for
 *         `model_write_audit_config_emit_fields`, used when a single call to
 *         `model_emit_event_ex` must populate both the generic audit fields
 *         and the config-change-specific fields on one event. */
typedef struct
{
    model_audit_event_fields_t audit;
    model_audit_config_event_fields_t config;
} model_audit_config_emit_fields_t;

/** @brief Combined `fields_context` payload for
 *         `model_write_audit_security_emit_fields`, analogous to
 *         `model_audit_config_emit_fields_t` but for security operations. */
typedef struct
{
    model_audit_event_fields_t audit;
    model_audit_security_event_fields_t security;
} model_audit_security_emit_fields_t;

/**
 * @brief Callback type used by `model_emit_event_ex` to populate an event's
 *        `UA_KeyValueMap` fields from an opaque `fields_context`.
 *
 * @param event_fields Key-value map of the event being built; implementations
 *        add/overwrite entries in place.
 * @param fields_context Points at one of the `model_*_event_fields_t` /
 *        `model_*_emit_fields_t` structs above, matching the specific writer.
 * @return `UA_STATUSCODE_GOOD` on success, or the first field-write failure.
 */
typedef UA_StatusCode (*model_event_fields_writer_t)(UA_KeyValueMap *event_fields,
                                                     const void *fields_context);

/** @brief Certificate expiry classification bucket, driving both the
 *         `MODEL_NODE_SECURITY_EXPIRY_STATE` read-only node and whether a
 *         security-fault Condition/event should be raised or cleared. */
typedef enum
{
    MODEL_CERTIFICATE_EXPIRY_STATE_MISSING = 0,
    MODEL_CERTIFICATE_EXPIRY_STATE_UNKNOWN = 1,
    MODEL_CERTIFICATE_EXPIRY_STATE_VALID = 2,
    MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_MEDIUM = 3,
    MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_HIGH = 4,
    MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRED = 5,
} model_certificate_expiry_state_t;

/**
 * @brief Result of evaluating a remote (client) X.509 certificate's expiry
 *        against the current time, computed by
 *        `model_get_remote_certificate_expiry_info` from the certificate's
 *        `notAfter` field via `model_x509_time_to_epoch`.
 *
 * @note `time_check_performed` is false when the device has no valid time
 *       source yet (`time_service` unsynced); callers must not treat
 *       `state`/`days_until_expiry` as meaningful in that case.
 */
typedef struct
{
    model_certificate_expiry_state_t state;
    uint32_t days_until_expiry;
    uint32_t not_after_epoch_s;
    bool time_check_performed;
    char thumbprint[SECURITY_SERVICE_CERT_THUMBPRINT_LEN + 1u];
} model_remote_certificate_expiry_info_t;

#if defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
/** @brief Why `model_evaluate_security_condition` currently considers the
 *         `Device.Security.SecurityFaultCondition` active; drives its
 *         message text and severity. */
typedef enum
{
    MODEL_SECURITY_CONDITION_REASON_NONE = 0,
    MODEL_SECURITY_CONDITION_REASON_CERT_EXPIRED = 1,
    MODEL_SECURITY_CONDITION_REASON_CERT_EXPIRES_SOON_HIGH = 2,
    MODEL_SECURITY_CONDITION_REASON_CERT_EXPIRES_SOON_MEDIUM = 3,
    MODEL_SECURITY_CONDITION_REASON_TRUST_LIST_EMPTY = 4,
    MODEL_SECURITY_CONDITION_REASON_AUTH_FAILURES = 5,
    MODEL_SECURITY_CONDITION_REASON_COMMISSIONING = 6
} model_security_condition_reason_t;

#endif
#endif

/**
 * @name UA_AccessControl default-callback signatures
 * `model_core_task` saves open62541's own `UA_AccessControl` callback
 * implementations (installed by `UA_AccessControl_default`) into the
 * matching `g_default_*` context fields before replacing them with the
 * `model_*_wrapper` functions declared below. Each wrapper calls its saved
 * default first (or in place of it, for `activateSession`) and layers
 * role-based checks on top — see
 * `docs/code/modules/opcua_model.md#access-control`. These typedefs exist
 * only to give the saved function pointers a storable type; they mirror the
 * open62541 `UA_AccessControl` member signatures exactly.
 * @{
 */
/** @brief Signature of `UA_AccessControl.activateSession`. */
typedef UA_StatusCode (*model_activate_session_fn)(UA_Server *server,
                                                   UA_AccessControl *ac,
                                                   const UA_EndpointDescription *endpoint_description,
                                                   const UA_ByteString *secure_channel_remote_certificate,
                                                   const UA_NodeId *session_id,
                                                   const UA_ExtensionObject *user_identity_token,
                                                   void **session_context);

/** @brief Signature of `UA_AccessControl.closeSession`. */
typedef void (*model_close_session_fn)(UA_Server *server,
                                       UA_AccessControl *ac,
                                       const UA_NodeId *session_id,
                                       void *session_context);

/** @brief Signature of `UA_AccessControl.getUserExecutableOnObject`'s
 *         object-less sibling, `getUserExecutable` (method executability
 *         without an owning object node). */
typedef UA_Boolean (*model_get_user_executable_fn)(UA_Server *server,
                                                   UA_AccessControl *ac,
                                                   const UA_NodeId *session_id,
                                                   void *session_context,
                                                   const UA_NodeId *method_id,
                                                   void *method_context);

/** @brief Signature of `UA_AccessControl.getUserExecutableOnObject`. */
typedef UA_Boolean (*model_get_user_executable_on_object_fn)(UA_Server *server,
                                                             UA_AccessControl *ac,
                                                             const UA_NodeId *session_id,
                                                             void *session_context,
                                                             const UA_NodeId *method_id,
                                                             void *method_context,
                                                             const UA_NodeId *object_id,
                                                             void *object_context);

/** @brief Signature of `UA_AccessControl.getUserAccessLevel`. */
typedef UA_Byte (*model_get_user_access_level_fn)(UA_Server *server,
                                                  UA_AccessControl *ac,
                                                  const UA_NodeId *session_id,
                                                  void *session_context,
                                                  const UA_NodeId *node_id,
                                                  void *node_context);

/** @brief Signature of `UA_AccessControl.allowBrowseNode`. */
typedef UA_Boolean (*model_allow_browse_node_fn)(UA_Server *server,
                                                 UA_AccessControl *ac,
                                                 const UA_NodeId *session_id,
                                                 void *session_context,
                                                 const UA_NodeId *node_id,
                                                 void *node_context);
/** @} */

/**
 * @brief Index space for `opcua_model_context_t::fixed_node_contexts`,
 *        one entry per non-channel, non-task variable node. Declaration
 *        order matches `model_node_kind_t` field-for-field (excluding the
 *        channel/task kinds) and must stay in sync with it and with the
 *        initializer list in `opcua_model_context.c`; `FIXED_CTX_COUNT` is
 *        the sentinel array size.
 */
enum
{
    FIXED_CTX_IDENT_MANUFACTURER = 0,
    FIXED_CTX_IDENT_MODEL,
    FIXED_CTX_IDENT_SERIAL_NUMBER,
    FIXED_CTX_IDENT_HW_REVISION,
    FIXED_CTX_IDENT_FW_REVISION,
    FIXED_CTX_IDENT_BUILD_TIME,
    FIXED_CTX_IDENT_GIT_HASH,
    FIXED_CTX_IDENT_DEVICE_NAME,
    FIXED_CTX_IDENT_PRODUCT_INSTANCE_URI,
    FIXED_CTX_IDENT_ASSET_ID,
    FIXED_CTX_IDENT_REVISION_COUNTER,
    FIXED_CTX_IDENT_MAC_ADDRESS,
    FIXED_CTX_IDENT_MANUFACTURE_DATE,
    FIXED_CTX_IDENT_CALIBRATION_DATE,
    FIXED_CTX_IDENT_LOCATION,
    FIXED_CTX_IDENT_ASSET_CLASS,
    FIXED_CTX_DEVICE_HEALTH,
    FIXED_CTX_PROCESS_TEMPERATURE_VALUE,
    FIXED_CTX_PROCESS_TEMPERATURE_STATUS,
    FIXED_CTX_PROCESS_TEMPERATURE_SOURCE_TIMESTAMP,
    FIXED_CTX_PROCESS_TEMPERATURE_UNITS,
    FIXED_CTX_PROCESS_TEMPERATURE_UPDATE_PERIOD_MS,
    FIXED_CTX_PROCESS_TEMPERATURE_HISTORY_INTERVAL_MS,
    FIXED_CTX_PROCESS_TEMPERATURE_ALARM_STATE,
    FIXED_CTX_DIAG_UPTIME_SEC,
    FIXED_CTX_DIAG_RESET_CAUSE,
    FIXED_CTX_DIAG_FREE_HEAP,
    FIXED_CTX_DIAG_MIN_EVER_FREE_HEAP,
    FIXED_CTX_DIAG_PSRAM_READY,
    FIXED_CTX_DIAG_PSRAM_TOTAL_BYTES,
    FIXED_CTX_DIAG_PSRAM_USED_BYTES,
    FIXED_CTX_DIAG_PSRAM_FREE_BYTES,
    FIXED_CTX_DIAG_PSRAM_LARGEST_FREE_BLOCK_BYTES,
    FIXED_CTX_DIAG_PSRAM_ALLOCATOR_MODE,
    FIXED_CTX_DIAG_CPU_LOAD_APPROX,
    FIXED_CTX_DIAG_LAST_FAULT_CODE,
    FIXED_CTX_DIAG_RESOURCE_SNAPSHOT,
    FIXED_CTX_NETWORK_MAC_ADDRESS,
    FIXED_CTX_NETWORK_IP_MODE,
    FIXED_CTX_NETWORK_IPV4,
    FIXED_CTX_NETWORK_MASK,
    FIXED_CTX_NETWORK_GATEWAY,
    FIXED_CTX_NETWORK_LINK_UP,
    FIXED_CTX_NETWORK_DHCP_STATE,
    FIXED_CTX_NETWORK_RECONNECT_COUNT,
    FIXED_CTX_TIME_SYNCED,
    FIXED_CTX_TIME_NTP_SERVER,
    FIXED_CTX_TIME_LAST_SYNC_EPOCH,
    FIXED_CTX_TIME_CLOCK_QUALITY,
    FIXED_CTX_TIME_TIMEZONE_OFFSET_MINUTES,
    FIXED_CTX_SECURITY_MODE,
    FIXED_CTX_SECURITY_COMMISSIONING_MODE,
    FIXED_CTX_SECURITY_TRUST_LIST_VERSION,
    FIXED_CTX_SECURITY_TRUSTED_CLIENT_COUNT,
    FIXED_CTX_SECURITY_LAST_AUTH_FAILURE,
    FIXED_CTX_SECURITY_FAILED_LOGIN_COUNT,
    FIXED_CTX_SECURITY_CERT_THUMBPRINT,
    FIXED_CTX_SECURITY_CERT_NOT_BEFORE,
    FIXED_CTX_SECURITY_CERT_NOT_AFTER,
    FIXED_CTX_SECURITY_DAYS_UNTIL_EXPIRY,
    FIXED_CTX_SECURITY_EXPIRY_STATE,
    FIXED_CTX_SECURITY_COMMISSIONING_STATE,
    FIXED_CTX_SECURITY_ACTIVE_IDENTITY_GENERATION,
    FIXED_CTX_SECURITY_ACTIVE_TRUST_GENERATION,
    FIXED_CTX_SECURITY_CURRENT_USER_ROLE,
    FIXED_CTX_SECURITY_LAST_EVENT_ID,
    FIXED_CTX_NETWORK_LAST_NETWORK_CHANGE,
    FIXED_CTX_DIAG_LAST_FAULT,
    FIXED_CTX_CONFIG_LAST_CONFIG_CHANGE,
    FIXED_CTX_CONFIG_SAMPLING_PERIOD_MS,
    FIXED_CTX_CONFIG_PUBLISH_PERIOD_MS,
    FIXED_CTX_CONFIG_TEMP_OFFSET,
    FIXED_CTX_CONFIG_TEMP_SCALE,
    FIXED_CTX_CONFIG_HIGH_ALARM,
    FIXED_CTX_CONFIG_HIGH_HIGH_ALARM,
    FIXED_CTX_CONFIG_REBOOT_REQUIRED,
    FIXED_CTX_CONFIG_COMMISSIONED,
    FIXED_CTX_MESSAGING_EVENT_SEQUENCE,
    FIXED_CTX_MESSAGING_EVENT_ID,
    FIXED_CTX_MESSAGING_EVENT_TYPE,
    FIXED_CTX_MESSAGING_EVENT_SOURCE,
    FIXED_CTX_MESSAGING_EVENT_MESSAGE,
    FIXED_CTX_MESSAGING_EVENT_SEVERITY,
    FIXED_CTX_MESSAGING_EVENT_TIME,
    FIXED_CTX_COUNT
};

/* Secure OpenSecureChannel handshakes with X.509 client certificates are
   noticeably larger than plain SecurityPolicy#None traffic. 8 KiB is too
   small for UaExpert/Prosys, while 32 KiB per direction is too expensive for
   the current RP2350 RAM budget once security/events are enabled. 16 KiB
   keeps secure setup working and avoids per-connection heap spikes. */
#define OPCUA_SEND_BUFFER_SIZE 16384u
#define OPCUA_RECV_BUFFER_SIZE 16384u
#define OPCUA_MAX_METHODS_PER_CALL 1u
/*
 * Below: fixed NodeId string identifiers (Device.* object/folder paths,
 * Device.Maintenance.* / Device.Security.* method paths) and BrowseName-like
 * string literals for event/condition types, consumed as `UA_NODEID_STRING`
 * / `UA_QUALIFIEDNAME` arguments across `model_address_space.c`,
 * `model_events.c`, `model_maintenance.c`, `model_ota.c`, `model_security.c`.
 * Names are self-describing; not documented individually. Per
 * `docs/code/modules/opcua_model.md`, NodeId-path constants are split
 * between this header, inline literals in `model_address_space.c`, and
 * accessor functions in `model_channels.c` rather than centralized.
 */
#define MODEL_NODE_MAINTENANCE_CONFIGURATION "Device.Maintenance.Configuration"
#define MODEL_NODE_MAINTENANCE_FIRMWARE "Device.Maintenance.Firmware"
#define MODEL_NODE_MAINTENANCE_SECURITY "Device.Maintenance.Security"
#define MODEL_NODE_MAINTENANCE_DIAGNOSTICS "Device.Maintenance.Diagnostics"
#define MODEL_NODE_MAINTENANCE_SYSTEM "Device.Maintenance.System"
#define MODEL_METHOD_PLAN_MAINTENANCE "Device.Maintenance.PlanMaintenance"
#define MODEL_METHOD_RESCHEDULE_MAINTENANCE "Device.Maintenance.RescheduleMaintenance"
#define MODEL_METHOD_START_MAINTENANCE "Device.Maintenance.StartMaintenance"
#define MODEL_METHOD_FINISH_MAINTENANCE "Device.Maintenance.FinishMaintenance"
#define MODEL_METHOD_RESET_MAINTENANCE_JOURNAL "Device.Maintenance.ResetMaintenanceJournal"
#define MODEL_METHOD_EXPORT_MAINTENANCE_JOURNAL "Device.Maintenance.ExportMaintenanceJournal"
#define MODEL_NODE_MAINTENANCE_SUPPLIER "Device.DeviceHealthAlarms.MaintenanceRequired.MaintenanceSupplier"
#define MODEL_NODE_MAINTENANCE_PERSONNEL "Device.DeviceHealthAlarms.MaintenanceRequired.QualificationOfPersonnel"
#define MODEL_NODE_MAINTENANCE_REPLACED "Device.DeviceHealthAlarms.MaintenanceRequired.PartsOfAssetReplaced"
#define MODEL_NODE_MAINTENANCE_SERVICED "Device.DeviceHealthAlarms.MaintenanceRequired.PartsOfAssetServiced"
#define MODEL_METHOD_SET_DEVICE_IDENTITY "Device.Maintenance.SetDeviceIdentity"
#define MODEL_METHOD_SET_LOCATION_PATHS "Device.Maintenance.Configuration.SetLocationPaths"
#define MODEL_METHOD_SET_NETWORK_CONFIG "Device.Maintenance.SetNetworkConfig"
#define MODEL_METHOD_SET_TRUST_LIST "Device.Security.SetTrustList"
#define MODEL_METHOD_GET_CERTIFICATE_STATE "Device.Maintenance.Security.GetCertificateState"
#define MODEL_METHOD_EXPORT_CERTIFICATE_METADATA "Device.Maintenance.Security.ExportCertificateMetadata"
#define MODEL_METHOD_BEGIN_IDENTITY_ROTATION "Device.Maintenance.Security.BeginIdentityRotation"
#define MODEL_METHOD_WRITE_IDENTITY_ROTATION_CHUNK "Device.Maintenance.Security.WriteIdentityRotationChunk"
#define MODEL_METHOD_ABORT_IDENTITY_ROTATION "Device.Maintenance.Security.AbortIdentityRotation"
#define MODEL_METHOD_VALIDATE_STAGED_IDENTITY "Device.Maintenance.Security.ValidateStagedIdentity"
#define MODEL_METHOD_APPLY_STAGED_IDENTITY "Device.Maintenance.Security.ApplyStagedIdentity"
#define MODEL_METHOD_GET_TRUST_STATE "Device.Maintenance.Security.GetTrustState"
#define MODEL_METHOD_EXPORT_TRUST_LIST "Device.Maintenance.Security.ExportTrustList"
#define MODEL_METHOD_BEGIN_TRUST_LIST_IMPORT "Device.Maintenance.Security.BeginTrustListImport"
#define MODEL_METHOD_WRITE_TRUST_LIST_CHUNK "Device.Maintenance.Security.WriteTrustListChunk"
#define MODEL_METHOD_ABORT_TRUST_LIST_IMPORT "Device.Maintenance.Security.AbortTrustListImport"
#define MODEL_METHOD_VALIDATE_IMPORTED_TRUST_LIST "Device.Maintenance.Security.ValidateImportedTrustList"
#define MODEL_METHOD_APPLY_IMPORTED_TRUST_LIST "Device.Maintenance.Security.ApplyImportedTrustList"
#define MODEL_METHOD_ADD_USER "Device.Maintenance.AddUser"
#define MODEL_METHOD_CHANGE_PASSWORD "Device.Maintenance.ChangePassword"
#define MODEL_METHOD_SET_ROLE "Device.Maintenance.SetRole"
#define MODEL_METHOD_DISABLE_USER "Device.Maintenance.DisableUser"
#define MODEL_METHOD_DELETE_USER "Device.Maintenance.DeleteUser"
#define MODEL_METHOD_SET_MQTT_PUBSUB_ENABLED "Device.Maintenance.SetMqttPubSubEnabled"
#define MODEL_METHOD_SET_CHANNEL_CONFIG "Device.Maintenance.SetChannelConfig"
#define MODEL_METHOD_GET_CHANNEL_CONFIG "Device.Maintenance.GetChannelConfig"
#define MODEL_METHOD_CLEAR_CHANNEL_CONFIG "Device.Maintenance.ClearChannelConfig"
#define MODEL_METHOD_LIST_DRIVERS "Device.Maintenance.ListDrivers"
#define MODEL_METHOD_LIST_CONFIG_PRESETS "Device.Maintenance.ListConfigPresets"
/* Deliberately prefixed with MODEL_METHOD_APPLY_CONFIG's own NodeId text
 * ("Device.Maintenance.ApplyConfig") -- model_required_role_for_method()
 * (model_access.c) matches that as a string PREFIX, so this method inherits
 * ApplyConfig's Operator role automatically; no model_access.c change
 * needed. */
#define MODEL_METHOD_APPLY_CONFIG_PRESET "Device.Maintenance.ApplyConfigPreset"
#define MODEL_METHOD_GET_CONFIG_STATE "Device.Maintenance.GetConfigState"
#define MODEL_METHOD_EXPORT_CONFIG "Device.Maintenance.ExportConfig"
#define MODEL_METHOD_BEGIN_CONFIG_IMPORT "Device.Maintenance.BeginConfigImport"
#define MODEL_METHOD_WRITE_CONFIG_CHUNK "Device.Maintenance.WriteConfigChunk"
#define MODEL_METHOD_ABORT_CONFIG_IMPORT "Device.Maintenance.AbortConfigImport"
#define MODEL_METHOD_VALIDATE_IMPORTED_CONFIG "Device.Maintenance.ValidateImportedConfig"
#define MODEL_METHOD_APPLY_IMPORTED_CONFIG "Device.Maintenance.ApplyImportedConfig"
#define MODEL_METHOD_GET_FIRMWARE_UPDATE_STATUS "Device.Maintenance.GetFirmwareUpdateStatus"
#define MODEL_METHOD_BEGIN_FIRMWARE_UPDATE "Device.Maintenance.BeginFirmwareUpdate"
#define MODEL_METHOD_WRITE_FIRMWARE_CHUNK "Device.Maintenance.WriteFirmwareChunk"
#define MODEL_METHOD_FINALIZE_FIRMWARE_UPDATE "Device.Maintenance.FinalizeFirmwareUpdate"
#define MODEL_METHOD_ABORT_FIRMWARE_UPDATE "Device.Maintenance.AbortFirmwareUpdate"
#define MODEL_METHOD_ENTER_COMMISSIONING "Device.Security.EnterCommissioningMode"
#define MODEL_METHOD_EXIT_COMMISSIONING "Device.Security.ExitCommissioningMode"
#define MODEL_EVENT_TYPE_DEVICE "Types.Events.DeviceEvent"
#define MODEL_EVENT_TYPE_CONFIG_CHANGED "Types.Events.ConfigChangedEvent"
#define MODEL_EVENT_TYPE_NETWORK "Types.Events.NetworkEvent"
#define MODEL_EVENT_TYPE_SENSOR_FAULT "Types.Events.SensorFaultEvent"
#define MODEL_EVENT_TYPE_MAINTENANCE "Types.Events.MaintenanceEvent"
#define MODEL_EVENT_TYPE_FIRMWARE "Types.Events.FirmwareEvent"
#define MODEL_EVENT_TYPE_AUDIT "Types.Events.AuditEvent"
#define MODEL_EVENT_TYPE_AUDIT_LOGIN_SUCCESS "Types.Events.AuditLoginSuccessEvent"
#define MODEL_EVENT_TYPE_AUDIT_LOGIN_FAILURE "Types.Events.AuditLoginFailureEvent"
#define MODEL_EVENT_TYPE_AUDIT_CONFIG_CHANGE "Types.Events.AuditConfigChangeEvent"
#define MODEL_EVENT_TYPE_AUDIT_SECURITY_CHANGE "Types.Events.AuditSecurityChangeEvent"
#define MODEL_CONDITION_TYPE_SECURITY_FAULT "Types.Conditions.SecurityFaultConditionType"
#define MODEL_CONDITION_SECURITY_FAULT "Device.Security.SecurityFaultCondition"
#define MODEL_CONDITION_DEVICE_HEALTH_FAILURE "Device.DeviceHealthAlarms.Failure"
#define MODEL_CONDITION_DEVICE_HEALTH_CHECK_FUNCTION "Device.DeviceHealthAlarms.CheckFunction"
#define MODEL_CONDITION_DEVICE_HEALTH_OFF_SPEC "Device.DeviceHealthAlarms.OffSpec"
#define MODEL_CONDITION_DEVICE_HEALTH_MAINTENANCE_REQUIRED "Device.DeviceHealthAlarms.MaintenanceRequired"
/* Tuning constants for certificate-expiry classification thresholds
 * (used by model_get_certificate_expiry_state), the bounded audit-event
 * queue, the security/health Condition re-enable poll period, the
 * per-session/global rejected-maintenance-call token-bucket limiter, and the
 * bounded maintenance-journal export continuation table (capacity, token
 * space, PSRAM payload buffer size, and continuation TTL). */
#define MODEL_CERTIFICATE_EXPIRY_WARN_DAYS 30u
#define MODEL_CERTIFICATE_EXPIRY_HIGH_WARN_DAYS 7u
#define MODEL_AUDIT_QUEUE_CAPACITY 8u
#define MODEL_CONDITION_ENABLE_POLL_MS 1000u
#define MODEL_MAINTENANCE_REJECT_SESSION_BURST 4u
#define MODEL_MAINTENANCE_REJECT_GLOBAL_BURST 16u
#define MODEL_MAINTENANCE_REJECT_SUPPRESSION_MAX UINT16_MAX
#define MODEL_MAINTENANCE_EXPORT_CONTINUATION_CAPACITY 4u
#define MODEL_MAINTENANCE_EXPORT_TOKEN_CAPACITY 16u
#define MODEL_MAINTENANCE_EXPORT_PAYLOAD_CAPACITY 24576u
#define MODEL_MAINTENANCE_EXPORT_EXPIRY_MS 30000u
/* HistoryReadRaw continuation points deliberately consume a fixed number of
 * adapter-owned slots.  A point is session-owned and expires rather than
 * becoming an unbounded server-side cursor. */
#define MODEL_HISTORY_CONTINUATION_CAPACITY 16u
#define MODEL_HISTORY_CONTINUATION_POINT_SIZE 12u
#define MODEL_HISTORY_CONTINUATION_EXPIRY_MS 30000u
#define MODEL_PUBSUB_TOPIC_MAX_LEN 96u
#define MODEL_PUBSUB_CLIENT_ID_MAX_LEN 64u
#define MODEL_PUBSUB_TELEMETRY_TOPIC_SUFFIX "telemetry"
#define MODEL_PUBSUB_EVENTS_TOPIC_SUFFIX "events"
#define MODEL_PUBSUB_CONNECTION_NAME "MQTT PubSub Connection"
#define MODEL_PUBSUB_TELEMETRY_PDS_PREFIX "TelemetryPDS"
#define MODEL_PUBSUB_TELEMETRY_WG_PREFIX "TelemetryWG"
#define MODEL_PUBSUB_TELEMETRY_DSW_PREFIX "TelemetryDSW"
#define MODEL_PUBSUB_EVENTS_PDS_NAME "EventsMirrorPDS"
#define MODEL_PUBSUB_EVENTS_WG_NAME "EventsMirrorWG"
#define MODEL_PUBSUB_EVENTS_DSW_NAME "EventsMirrorDSW"
#define MODEL_PUBSUB_MQTT_PROFILE_JSON "http://opcfoundation.org/UA-Profile/Transport/pubsub-mqtt-json"
#define MODEL_PUBSUB_MQTT_PROFILE_UADP "http://opcfoundation.org/UA-Profile/Transport/pubsub-mqtt-uadp"
#define MODEL_NODE_MESSAGING "Device.Messaging"
#define MODEL_NODE_MESSAGING_LAST_EVENT_SEQUENCE "Device.Messaging.LastPubSubEventSequence"
#define MODEL_NODE_MESSAGING_LAST_EVENT_ID "Device.Messaging.LastPubSubEventId"
#define MODEL_NODE_MESSAGING_LAST_EVENT_TYPE "Device.Messaging.LastPubSubEventType"
#define MODEL_NODE_MESSAGING_LAST_EVENT_SOURCE "Device.Messaging.LastPubSubEventSource"
#define MODEL_NODE_MESSAGING_LAST_EVENT_MESSAGE "Device.Messaging.LastPubSubEventMessage"
#define MODEL_NODE_MESSAGING_LAST_EVENT_SEVERITY "Device.Messaging.LastPubSubEventSeverity"
#define MODEL_NODE_MESSAGING_LAST_EVENT_TIME "Device.Messaging.LastPubSubEventTime"

#define MODEL_NODE_ASSET_ALIAS_PRODUCT_INSTANCE_URI "AssetAliases.ProductInstanceUri"
#define MODEL_NODE_ASSET_ALIAS_ASSET_ID "AssetAliases.AssetId"
#define MODEL_ASSET_ALIAS_UNASSIGNED "NoAssetIdAssigned"


/* Adapter-only state. Services retain all business, configuration, alarm,
 * security, history and OTA state; callbacks only translate to their APIs. */
/**
 * @brief One slot of the fixed maintenance-journal export continuation
 *        table (`opcua_model_context_t::maintenance_export_continuations`).
 *        Tracks a paged `ExportMaintenanceJournal` sequence: `token`
 *        identifies it to the client, `cursor` is the service-owned resume
 *        position, `expires_at` enforces `MODEL_MAINTENANCE_EXPORT_EXPIRY_MS`.
 * @note Session-scoped by convention (checked against the calling session by
 *       the export method), adapter-owned, never persisted to Flash.
 */
typedef struct { bool active; uint32_t token; TickType_t expires_at; maintenance_service_export_cursor_t cursor; } model_maintenance_export_continuation_t;
/**
 * @brief One slot of the fixed HistoryRead continuation-point table
 *        (`opcua_model_context_t::history_continuations`,
 *        `MODEL_HISTORY_CONTINUATION_CAPACITY` slots). Captures enough state
 *        to resume a paged `HistoryReadRaw` request: the resolved channel,
 *        the requested time range/direction, how many matches to skip on
 *        resume, and which session/generation it belongs to.
 * @note `session_context` is used only for ownership comparison, never
 *       dereferenced; entries expire after `MODEL_HISTORY_CONTINUATION_EXPIRY_MS`
 *       and are released early by `model_history_release_session_points` when
 *       a session closes or the client sets `releaseContinuationPoints`.
 */
typedef struct
{
    bool active;
    const void *session_context;
    uint32_t token;
    uint32_t generation;
    uint32_t skip_matches;
    uint32_t start_epoch_s;
    uint32_t end_epoch_s;
    bool descending;
    TickType_t expires_at;
    char channel_id[TELEMETRY_CHANNEL_ID_MAX_LEN];
    UA_UInt16 node_namespace_index;
    char node_identifier[MODEL_CHANNEL_NODE_ID_MAX_LEN];
} model_history_continuation_t;
_Static_assert(MAINTENANCE_SERVICE_EXPORT_PAGE_MAX == 4u,
               "v0.3.3 export page contract changed");
_Static_assert(MODEL_MAINTENANCE_EXPORT_CONTINUATION_CAPACITY == 4u,
               "v0.3.3 continuation contract changed");
_Static_assert(MODEL_MAINTENANCE_EXPORT_PAYLOAD_CAPACITY >=
                   MAINTENANCE_SERVICE_EXPORT_PAGE_MAX * 6u * 734u,
               "logical JSON buffer must cover fully escaped maximum records");
_Static_assert(MODEL_MAINTENANCE_EXPORT_EXPIRY_MS == 30000u,
               "v0.3.3 continuation expiry contract changed");
#define MODEL_CHANNEL_FIELD_CONTEXT_COUNT 7u
#define MODEL_GENERIC_CHANNEL_CONTEXT_COUNT 3u
#define MODEL_EVENT_FIELD_PATH_CAPACITY 64u
/**
 * @brief Staged-but-not-yet-published maintenance event, built by
 *        `model_prepare_maintenance_event` and handed to
 *        `model_publish_prepared_maintenance_event`. Splitting prepare/
 *        publish lets a caller build the event fields before committing a
 *        maintenance state transition, and only publish once the underlying
 *        service call has actually succeeded.
 * @note Caller-owned, stack-lifetime; `fields`/`source_name`/`message`
 *       reference open62541 types that must be cleared (`UA_clear`) if the
 *       event is abandoned before publishing.
 */
typedef struct
{
    UA_KeyValueMap fields;
    UA_DateTime event_time;
    UA_String source_name;
    UA_LocalizedText message;
    model_maintenance_event_fields_t maintenance_fields;
} model_prepared_maintenance_event_t;
/**
 * @brief The single OPC UA model adapter context: `UA_Server`/`UA_ServerConfig`
 *        handles, every adapter-owned runtime/continuation table, and every
 *        `model_node_context_t` the address-space builder hands out as node
 *        contexts.
 *
 * Ownership/lifetime: exactly one static-storage instance
 * (`g_model_context` in `opcua_model_context.c`), obtained only through
 * `model_context_get()`. Never heap-allocated, never copied, never freed.
 * Deliberately holds **no** service business state (config, alarm, history,
 * security, OTA state all remain in `services/`) — only adapter-side
 * bookkeeping the OPC UA layer itself needs (see
 * `docs/code/modules/opcua_model.md`).
 *
 * Concurrency: exclusively created/populated/read/written from `OpcUaTask`
 * (`model_core_task`) and from open62541 method/DataSource callbacks, which
 * only ever run synchronously inside `UA_Server_run_iterate` on that same
 * task — so most fields need no locking. The few fields that can be touched
 * from a different task (the audit-event ring, the maintenance-reject
 * limiter, both reachable from `security_service`'s audit callback) are
 * updated under `taskENTER_CRITICAL`/`taskEXIT_CRITICAL`; see
 * `model_queue_pending_audit_event` and
 * `model_maintenance_rejected_audit_admit`.
 *
 * Accessed almost everywhere in this codebase via the `g_model_*` /
 * `g_*` macros defined immediately below, not by dereferencing this struct
 * directly.
 */
typedef struct opcua_model_context {
    opcua_model_config_t config; bool initialized; bool maintenance_faulted;
    UA_Server *server;
    UA_ServerConfig *server_config;
    bool maintenance_reject_limiter_initialized; TickType_t maintenance_reject_last_refill;
    uint8_t maintenance_reject_tokens;
    model_maintenance_export_continuation_t maintenance_export_continuations[MODEL_MAINTENANCE_EXPORT_CONTINUATION_CAPACITY];
    uint32_t maintenance_export_next_token; char *maintenance_export_payload;
    UA_String maintenance_export_output_view;
    model_history_continuation_t history_continuations[MODEL_HISTORY_CONTINUATION_CAPACITY];
    uint32_t history_continuation_next_token;
    history_service_channel_sample_t *history_read_sample_buffer;
    /* v0.5.2 T2: published only by OpcUaTask. This bounded PSRAM allocation
     * is deliberately not service state; it is an adapter cache so Read
     * callbacks never place the complete 16-task snapshot on their stack. */
    diag_service_snapshot_t *resource_snapshot_cache;
    opcua_resource_service_snapshot_t opcua_resource_snapshot_cache;
    bool opcua_resource_snapshot_cache_available;
    uint64_t opcua_event_drops_last_raw;
    uint64_t opcua_event_drops_last_wrap_count;
    uint64_t opcua_rejected_requests_last_raw;
    uint64_t opcua_rejected_requests_last_wrap_count;
    bool resource_snapshot_cache_in_psram;
    bool resource_snapshot_cache_available;
    bool resource_snapshot_cache_stale;
    uint64_t resource_snapshot_cache_monotonic_ms;
    bool resource_task_model_incomplete;
    model_pending_actions_t pending_actions; device_profile_config_t address_space_config_snapshot;
    bool address_space_config_snapshot_valid; model_activate_session_fn default_activate_session;
    model_close_session_fn default_close_session; model_get_user_executable_fn default_get_user_executable;
    model_get_user_executable_on_object_fn default_get_user_executable_on_object;
    model_get_user_access_level_fn default_get_user_access_level; model_allow_browse_node_fn default_allow_browse_node;
    uint8_t history_backend_context; UA_UInt16 di_namespace_index; UA_UInt16 amb_namespace_index;
    UA_UInt16 channel_namespace_index;
    UA_DataTypeMember amb_name_node_id_members[2]; UA_DataType amb_name_node_id_type;
    UA_DataType amb_maintenance_method_type;
    UA_DataTypeArray amb_custom_types; bool amb_custom_types_registered; UA_UInt32 alias_last_change;
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    model_last_event_state_t last_event_state; model_pending_audit_event_t pending_audit_events[MODEL_AUDIT_QUEUE_CAPACITY];
    size_t pending_audit_event_head; size_t pending_audit_event_count;
#if defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
    bool alarm_conditions_available; bool maintenance_condition_available;
#endif
#endif
#ifdef UA_ENABLE_PUBSUB
    model_pubsub_runtime_t pubsub; model_pubsub_event_mirror_t pubsub_event_mirror;
#endif
    UA_Logger ua_logger; model_node_context_t fixed_node_contexts[FIXED_CTX_COUNT];
    model_node_context_t task_node_contexts[3];
    model_node_context_t channel_node_contexts[PLATFORM_CHANNEL_LIMIT][MODEL_CHANNEL_FIELD_CONTEXT_COUNT];
    model_node_context_t generic_channel_node_contexts[PLATFORM_CHANNEL_LIMIT][MODEL_GENERIC_CHANNEL_CONTEXT_COUNT];
} opcua_model_context_t;
/**
 * @brief Returns a pointer to the single static-lifetime `opcua_model_context_t`
 *        instance. Never NULL, never allocates.
 * @note This is the only accessor for the model context; every `g_model_*`
 *       macro below expands through it.
 */
opcua_model_context_t *model_context_get(void);
/**
 * @brief Records the live `UA_Server`/`UA_ServerConfig` handles in the model
 *        context after `UA_Server_newWithConfig` succeeds.
 * @note Called once per server lifetime from `model_core_task`.
 */
void model_context_set_server_handles(UA_Server *server, UA_ServerConfig *config);
/**
 * @brief Deletes @p server via `UA_Server_delete` and, if it was the
 *        context's currently-tracked server, clears the stored
 *        `server`/`server_config` handles first.
 * @param server May be NULL, in which case this is a no-op.
 * @note Called from `model_core_task` during a scheduled-reboot shutdown.
 */
void model_context_delete_server(UA_Server *server);
/**
 * @name Enum/state-to-string accessors
 * Each returns a static string literal (never NULL, never allocates) for
 * display in log messages and OPC UA string-valued diagnostic nodes; an
 * unrecognized/invalid input maps to an "Unknown"-style fallback rather than
 * asserting. Safe to call from any context these are reachable from
 * (`OpcUaTask` / method callbacks).
 * @{
 */
/** @brief `service_quality_t` (e.g. GOOD/BAD/UNCERTAIN family) to string. */
const char *model_quality_to_string(service_quality_t quality);
/** @brief `telemetry_alarm_state_t` to string, used for channel alarm text. */
const char *model_alarm_state_to_string(telemetry_alarm_state_t alarm_state);
/** @brief Current `UA_ServerConfig` security mode (None vs.
 *         Basic256Sha256/Sign&Encrypt) as configured by
 *         `model_configure_secure_server`, read back from the live context. */
const char *model_security_mode_to_string(void);
/** @brief `model_certificate_expiry_state_t` to string. */
const char *model_certificate_expiry_state_to_string(model_certificate_expiry_state_t state);
/** @brief Current commissioning-mode state (from `security_state_service`) as string. */
const char *model_security_commissioning_state_to_string(void);
/**
 * @brief Formats a human-readable actor name for @p session_context into
 *        @p buffer (username if authenticated, an anonymous/system label
 *        otherwise), truncating to fit.
 * @return @p buffer, for use inline in `printf`-style calls.
 */
const char *model_session_actor_name(void *session_context, char *buffer, size_t buffer_size);
/** @brief `security_user_role_t` of @p session_context as a string, for
 *         audit-log/event text. */
const char *model_session_role_to_string(const void *session_context);
/**
 * @brief Returns a pointer to the raw username `UA_ByteString` stored inside
 *        @p session_context, or NULL if the session has none (anonymous).
 * @note Borrowed pointer, valid only for the session's lifetime; caller must
 *       not free it.
 */
const UA_ByteString *model_session_get_username_bytes(const void *session_context);
/** @brief `UA_LogLevel` to string, used by `model_ua_logger_log`. */
const char *model_log_level_name(UA_LogLevel level);
/** @brief `maintenance_service_state_t` to string, used for maintenance
 *         Condition/event text and diagnostic node values. */
const char *model_maintenance_state_text(maintenance_service_state_t state);
#ifdef UA_ENABLE_PUBSUB
/** @brief open62541 `UA_PubSubComponentType` to string, for log messages
 *         from `model_pubsub_state_change_callback`. */
const char *model_pubsub_component_type_to_string(UA_PubSubComponentType type);
/** @brief open62541 `UA_PubSubState` to string, for log messages from
 *         `model_pubsub_state_change_callback`. */
const char *model_pubsub_state_to_string(UA_PubSubState state);
#endif
/** @} */
/**
 * @brief Public `opcua_model_init` implementation. See
 *        `opcua_model/opcua_model.h` for the full contract.
 */
bool model_core_init(const opcua_model_config_t *config);
/**
 * @brief Public `opcua_model_task` implementation (the `OpcUaTask` entry
 *        point). See `opcua_model/opcua_model.h` for the full contract.
 */
void model_core_task(void *argument);
/*
 * Field-accessor macros for the singleton model context (`model_context_get()`).
 * Each expands to a specific `opcua_model_context_t` field/array so call
 * sites can read/assign it as if it were a plain global (e.g.
 * `g_model_config`, `g_pending_actions`); purely mechanical aliases, not
 * documented individually. See `opcua_model_context_t` above for ownership
 * and synchronization of the underlying storage.
 */
#define g_model_config (model_context_get()->config)
#define g_model_initialized (model_context_get()->initialized)
#define g_model_maintenance_faulted (model_context_get()->maintenance_faulted)
#define g_model_maintenance_reject_limiter_initialized (model_context_get()->maintenance_reject_limiter_initialized)
#define g_model_maintenance_reject_last_refill (model_context_get()->maintenance_reject_last_refill)
#define g_model_maintenance_reject_tokens (model_context_get()->maintenance_reject_tokens)
#define g_model_maintenance_export_continuations (model_context_get()->maintenance_export_continuations)
#define g_model_maintenance_export_next_token (model_context_get()->maintenance_export_next_token)
#define g_model_maintenance_export_payload (model_context_get()->maintenance_export_payload)
#define g_model_maintenance_export_output_view (model_context_get()->maintenance_export_output_view)
#define g_model_history_continuations (model_context_get()->history_continuations)
#define g_model_history_continuation_next_token (model_context_get()->history_continuation_next_token)
#define g_model_history_read_sample_buffer (model_context_get()->history_read_sample_buffer)
#define g_model_resource_snapshot_cache (model_context_get()->resource_snapshot_cache)
#define g_model_opcua_resource_snapshot_cache (model_context_get()->opcua_resource_snapshot_cache)
#define g_model_opcua_resource_snapshot_cache_available (model_context_get()->opcua_resource_snapshot_cache_available)
#define g_model_resource_snapshot_cache_in_psram (model_context_get()->resource_snapshot_cache_in_psram)
#define g_model_resource_snapshot_cache_available (model_context_get()->resource_snapshot_cache_available)
#define g_model_resource_snapshot_cache_stale (model_context_get()->resource_snapshot_cache_stale)
#define g_model_resource_snapshot_cache_monotonic_ms (model_context_get()->resource_snapshot_cache_monotonic_ms)
#define g_model_resource_task_model_incomplete (model_context_get()->resource_task_model_incomplete)
#define g_pending_actions (model_context_get()->pending_actions)
#define g_model_address_space_config_snapshot (model_context_get()->address_space_config_snapshot)
#define g_model_address_space_config_snapshot_valid (model_context_get()->address_space_config_snapshot_valid)
#define g_default_activate_session (model_context_get()->default_activate_session)
#define g_default_close_session (model_context_get()->default_close_session)
#define g_default_get_user_executable (model_context_get()->default_get_user_executable)
#define g_default_get_user_executable_on_object (model_context_get()->default_get_user_executable_on_object)
#define g_default_get_user_access_level (model_context_get()->default_get_user_access_level)
#define g_default_allow_browse_node (model_context_get()->default_allow_browse_node)
#define g_history_backend_context (model_context_get()->history_backend_context)
#define g_model_di_namespace_index (model_context_get()->di_namespace_index)
#define g_model_amb_namespace_index (model_context_get()->amb_namespace_index)
#define g_model_channel_namespace_index (model_context_get()->channel_namespace_index)
#define g_generic_channel_node_contexts (model_context_get()->generic_channel_node_contexts)
#define g_model_amb_name_node_id_members (model_context_get()->amb_name_node_id_members)
#define g_model_amb_name_node_id_type (model_context_get()->amb_name_node_id_type)
#define g_model_amb_maintenance_method_type (model_context_get()->amb_maintenance_method_type)
#define g_model_amb_custom_types (model_context_get()->amb_custom_types)
#define g_model_amb_custom_types_registered (model_context_get()->amb_custom_types_registered)
#define g_model_alias_last_change (model_context_get()->alias_last_change)
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
#define g_last_event_state (model_context_get()->last_event_state)
#define g_pending_audit_events (model_context_get()->pending_audit_events)
#define g_pending_audit_event_head (model_context_get()->pending_audit_event_head)
#define g_pending_audit_event_count (model_context_get()->pending_audit_event_count)
#if defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
#define g_model_alarm_conditions_available (model_context_get()->alarm_conditions_available)
#define g_model_maintenance_condition_available (model_context_get()->maintenance_condition_available)
#endif
#endif
#ifdef UA_ENABLE_PUBSUB
#define g_model_pubsub (model_context_get()->pubsub)
#define g_model_pubsub_event_mirror (model_context_get()->pubsub_event_mirror)
#endif
#define g_model_ua_logger (model_context_get()->ua_logger)
#define g_fixed_node_contexts (model_context_get()->fixed_node_contexts)
#define g_task_node_contexts (model_context_get()->task_node_contexts)
#define g_channel_node_contexts (model_context_get()->channel_node_contexts)
/**
 * @brief `UA_Logger.log` implementation installed on the server config
 *        (`g_model_ua_logger`). Drops anything above `UA_LOGLEVEL_WARNING`
 *        (info/debug/trace) unconditionally: formatting/printing on every
 *        open62541 trace message can stall `OpcUaTask` behind a slow/absent
 *        USB CDC host for seconds, which would starve `UA_Server_run_iterate`.
 * @note Runs on `OpcUaTask` only (open62541 logs synchronously from the
 *       calling context); not reentrant-safe to call from another task.
 */
void model_ua_logger_log(void *context, UA_LogLevel level, UA_LogCategory category, const char *msg, va_list args);
/** @brief Prints @p stage plus current/minimum-ever FreeRTOS heap free bytes
 *         to stdout, for coarse allocation-pressure diagnostics. */
void log_opcua_heap(const char *stage);
/** @brief Prints a compact `ns=<i>;i=<id>` / `ns=<i>;s=<id>` form of
 *         @p node_id (or `"null"`) to stdout without a trailing newline. */
void model_log_node_id_compact(const UA_NodeId *node_id);
/**
 * @brief `UA_Server_setMonitoredItemRegisterCallback` hook, invoked by
 *        open62541 whenever a subscription monitored item is created or
 *        deleted for a node.
 * @note Updates one fixed-storage T3 occupancy counter; it must not perform
 *       blocking I/O or allocations. Runs on `OpcUaTask`.
 */
void model_monitored_item_register_callback(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *node_id, void *node_context, UA_UInt32 attribute_id, UA_Boolean removed);
/**
 * @brief Specialized open62541 lifecycle notification hook for secure
 *        channels, sessions and subscriptions.
 * @note Updates fixed-storage occupancy only. It must not call back into
 *       open62541 while its server mutex is held. Runs on `OpcUaTask`.
 */
void model_server_notification_callback(UA_Server *server, UA_ApplicationNotificationType type, const UA_KeyValueMap payload);
/** @brief True if @p session_context was activated over a secure
 *         (non-`SecurityPolicy#None`) SecureChannel. */
bool model_session_is_secure(const void *session_context);
/** @brief True if @p node_id is a namespace-1 string NodeId whose identifier
 *         starts with @p prefix; the primitive behind the
 *         `Device.Maintenance`/`Device.Identification`/`Device.Security`
 *         path-based checks below. */
bool model_nodeid_string_has_prefix(const UA_NodeId *node_id, const char *prefix);
/** @brief True if @p session_context has no stored username (anonymous login). */
bool model_session_is_anonymous(const void *session_context);
/** @brief True if @p node_id falls under `Device.Maintenance`,
 *         `Device.Identification`, or `Device.Security` — the subtrees hidden
 *         from anonymous browse/read via the access-control wrappers. */
bool model_node_is_anonymous_hidden(const UA_NodeId *node_id);
/** @brief True if @p method_id falls under `Device.Maintenance`. */
bool model_node_is_maintenance_method(const UA_NodeId *method_id);
/** @brief Clears the write/history-write/status-write/timestamp-write
 *         access-level bits, used to downgrade a node's advertised access
 *         level for anonymous sessions. */
UA_Byte model_strip_anonymous_write_access(UA_Byte access_level);
/**
 * @brief Copies the session's stored username into @p buffer as a
 *        NUL-terminated string.
 * @return false if @p session_context has no username, or if it does not
 *         fit in @p buffer_size (buffer is left untouched in that case).
 */
bool model_copy_session_username(const void *session_context, char *buffer, size_t buffer_size);
/**
 * @brief `UA_AccessControl.clear`-adjacent cleanup: frees the `UA_ByteString`
 *        username allocated for a session's `base_context` by the login path.
 * @note Called from `model_close_session_secure_wrapper`. @p base_context
 *       may be NULL (anonymous session).
 */
void model_free_session_username(void *base_context);
/**
 * @brief Resolves the session's role via `security_service_get_role` and
 *        compares it against @p required_role.
 * @return false (deny) if the session is anonymous or the role lookup fails,
 *         in addition to an actual role-too-low case.
 */
bool model_session_has_role(const void *session_context, security_user_role_t required_role);
/** @brief Resolves the session's `security_user_role_t` into @p out_role.
 *  @return false if the session has no username or the service lookup fails. */
bool model_session_get_role(const void *session_context, security_user_role_t *out_role);
/** @brief Emits a `security_service_audit_log` entry using the session's
 *         resolved actor name (username, or "anonymous"/"system"). */
void model_audit_session_event(void *session_context, const char *event, const char *subject, bool success, const char *details);
/**
 * @brief Token-bucket admission check for a rejected maintenance call,
 *        combining a per-session burst (`MODEL_MAINTENANCE_REJECT_SESSION_BURST`)
 *        and a global burst (`MODEL_MAINTENANCE_REJECT_GLOBAL_BURST`), both
 *        refilled at ~8 tokens/second. On denial, increments a saturating
 *        suppressed-count and records @p reason for later reporting.
 * @return true if the caller may audit-log this rejection now.
 * @param out_suppressed On admission, receives how many prior rejections were
 *        suppressed since the last logged one (0 if none).
 * @note Mutates shared per-session and global adapter state under
 *       `taskENTER_CRITICAL`/`taskEXIT_CRITICAL`.
 */
bool model_maintenance_rejected_audit_admit(void *session_context, UA_StatusCode reason, uint16_t *out_suppressed, UA_StatusCode *out_last_reason);
/** @brief Audits a rejected maintenance call if the rate limiter admits it
 *         (see `model_maintenance_rejected_audit_admit`); silently drops it
 *         otherwise. */
void model_audit_maintenance_rejection(void *session_context, const char *method, UA_StatusCode reason);
/** @brief Audits an accepted maintenance call unconditionally. */
void model_audit_maintenance_accept(void *session_context, const char *method);
/** @brief Audits a maintenance state transition, including the resulting
 *         revision/state text from @p snapshot. No-op if @p snapshot is NULL. */
void model_audit_maintenance_state_change(void *session_context, const char *method, const maintenance_service_snapshot_t *snapshot);
/**
 * @brief Enforces that the session is both over a secure channel (if
 *        @p required_role outranks Viewer) and holds at least @p required_role.
 * @return `UA_STATUSCODE_GOOD` if allowed; `UA_STATUSCODE_BADSECURITYMODEINSUFFICIENT`
 *         or `UA_STATUSCODE_BADUSERACCESSDENIED` otherwise, having already
 *         written a `false` boolean result into @p output via
 *         `model_set_boolean_method_output`.
 * @note Central role gate reused by every admin/operator/service-gated
 *       method callback and by the generic write dispatch's role recheck.
 */
UA_StatusCode model_require_session_role(void *session_context, security_user_role_t required_role, size_t output_size, UA_Variant *output);
/** @brief `model_require_session_role` specialized to `SECURITY_ROLE_ADMIN`. */
UA_StatusCode model_require_admin_session(void *session_context, size_t output_size, UA_Variant *output);
/**
 * @brief Central method-id-to-required-role policy table: maps a maintenance/
 *        security method NodeId to the minimum `security_user_role_t` allowed
 *        to call it (admin-only methods, `Device.Security.*`, maintenance
 *        workflow methods needing Service role, operator-level config
 *        methods, and a Viewer-level default).
 * @return `SECURITY_ROLE_ADMIN` if @p method_id is NULL.
 * @note This and `model_required_role_for_write_kind` are the single
 *       centralized access-policy mapping in the codebase; see
 *       `docs/code/modules/opcua_model.md#access-control`.
 */
security_user_role_t model_required_role_for_method(const UA_NodeId *method_id);
/** @brief Central write-node-kind-to-required-role policy table: most
 *         writable configuration/channel nodes require Operator, everything
 *         else defaults to Admin. */
security_user_role_t model_required_role_for_write_kind(model_node_kind_t kind);
/** @brief `model_require_session_role` using the role resolved by
 *         `model_required_role_for_method` for @p method_id. */
UA_StatusCode model_require_method_role(void *session_context, const UA_NodeId *method_id, size_t output_size, UA_Variant *output);
/** @brief True for the small fixed set of methods (AddUser, ChangePassword,
 *         SetRole, DisableUser, DeleteUser, ResetMaintenanceJournal) that are
 *         always Admin-only regardless of `model_required_role_for_method`'s
 *         generic prefix rules. */
bool model_is_admin_only_method(const UA_NodeId *method_id);
/**
 * @brief Copies a scalar `UA_String`-typed method-input @p value into
 *        @p buffer as a NUL-terminated C string.
 * @return false if @p value is not a scalar `UA_String`, or does not fit.
 */
bool model_copy_input_string(const UA_Variant *value, char *buffer, size_t buffer_size);
/** @brief Borrows the `UA_String *` inside a scalar `UA_String`-typed
 *         @p value without copying.
 *  @return false if @p value is not a scalar `UA_String` or has no data. */
bool model_get_input_ua_string(const UA_Variant *value, const UA_String **out_string);
/** @brief Borrows the `UA_ByteString *` inside a scalar `UA_ByteString`-typed
 *         @p value without copying.
 *  @return false if @p value is not a scalar `UA_ByteString`. */
bool model_get_input_ua_bytestring(const UA_Variant *value, const UA_ByteString **out_bytes);
/** @brief True if @p value is non-NULL and not an empty string; used to
 *         distinguish "field omitted" from "field explicitly cleared" in
 *         configuration method inputs. */
bool model_has_config_text(const char *value);
/** @brief Formats a 6-byte MAC address as `"aa:bb:cc:dd:ee:ff"` into @p buffer. */
void model_format_mac_string(const uint8_t mac[6], char *buffer, size_t buffer_size);
/** @brief Parses a dotted-decimal IPv4 string into @p out_ipv4.
 *  @return false if @p text is not a valid dotted-quad. */
bool model_parse_ipv4_string(const char *text, uint8_t out_ipv4[4]);
/** @brief Parses a colon-separated MAC address string into @p out_mac.
 *  @return false if @p text is not a valid MAC string. */
bool model_parse_mac_string(const char *text, uint8_t out_mac[6]);
/** @brief True if every DI-identification field required to leave
 *         commissioning mode is populated in @p config. */
bool model_identity_is_complete(const device_profile_config_t *config);
/** @brief True if @p config's static network fields (IPv4/mask/gateway, or
 *         DHCP enabled) are complete enough to leave commissioning mode. */
bool model_network_config_is_complete(const device_profile_config_t *config);
/** @brief True if the security trust list currently has at least one trust
 *         anchor installed (queried from `security_certificate_group_service`). */
bool model_has_trust_anchors(void);
/**
 * @brief True if the device has left commissioning mode: identity complete,
 *        network config complete, and at least one trust anchor present.
 * @note Central admission gate consulted before allowing secure-mode
 *       endpoints/behavior to activate.
 */
bool model_is_commissioned(void);
/**
 * @brief `UA_UsernamePasswordLogin`-style login callback installed on the
 *        server config, invoked by open62541 during `ActivateSession` for
 *        username/password authentication.
 * @return `UA_STATUSCODE_GOOD` on success after allocating and storing the
 *         username `UA_ByteString` into `*session_context`'s `base_context`
 *         (via `model_allocate_session_username`); `UA_STATUSCODE_BADUSERACCESSDENIED`
 *         (or similar) on invalid credentials, delegating credential
 *         verification to `security_service`.
 * @note Runs on `OpcUaTask` inside open62541's session-activation path.
 */
UA_StatusCode model_login_callback(const UA_String *user_name, const UA_ByteString *password, size_t username_password_login_size, const UA_UsernamePasswordLogin *username_password_login, void **session_context, void *login_context);
/**
 * @brief `UA_AccessControl.activateSession` wrapper installed by
 *        `model_core_task`, replacing open62541's default. Allocates a
 *        `model_session_context_t`, invokes the saved default
 *        (`g_default_activate_session`) to run standard token/identity
 *        validation, and wires the result into the new session context
 *        alongside secure-channel/role bookkeeping.
 * @return `UA_STATUSCODE_GOOD` on success; the default implementation's
 *         status code (or an allocation failure status) otherwise.
 * @note Runs on `OpcUaTask` inside open62541's session-activation path.
 */
UA_StatusCode model_activate_session_secure_wrapper(UA_Server *server, UA_AccessControl *ac, const UA_EndpointDescription *endpoint_description, const UA_ByteString *secure_channel_remote_certificate, const UA_NodeId *session_id, const UA_ExtensionObject *user_identity_token, void **session_context);
/**
 * @brief `UA_AccessControl.closeSession` wrapper: calls the saved default
 *        (`g_default_close_session`), then frees the `model_session_context_t`
 *        and its stored username.
 * @note Runs on `OpcUaTask`.
 */
void model_close_session_secure_wrapper(UA_Server *server, UA_AccessControl *ac, const UA_NodeId *session_id, void *session_context);
/** @brief `UA_AccessControl.getUserExecutable` wrapper: calls the saved
 *         default and additionally denies anonymous-hidden methods
 *         (`model_node_is_anonymous_hidden`). Runs on `OpcUaTask`. */
UA_Boolean model_get_user_executable_wrapper(UA_Server *server, UA_AccessControl *ac, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context);
/** @brief `UA_AccessControl.getUserExecutableOnObject` wrapper: same policy
 *         as `model_get_user_executable_wrapper` but for methods invoked
 *         with an owning object node. Runs on `OpcUaTask`. */
UA_Boolean model_get_user_executable_on_object_wrapper(UA_Server *server, UA_AccessControl *ac, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context);
/** @brief `UA_AccessControl.getUserAccessLevel` wrapper: calls the saved
 *         default, then strips write bits (`model_strip_anonymous_write_access`)
 *         for anonymous sessions on writable nodes. Runs on `OpcUaTask`. */
UA_Byte model_get_user_access_level_wrapper(UA_Server *server, UA_AccessControl *ac, const UA_NodeId *session_id, void *session_context, const UA_NodeId *node_id, void *node_context);
/** @brief `UA_AccessControl.allowBrowseNode` wrapper: calls the saved
 *         default and additionally hides anonymous-hidden subtrees
 *         (`model_node_is_anonymous_hidden`). Runs on `OpcUaTask`. */
UA_Boolean model_allow_browse_node_wrapper(UA_Server *server, UA_AccessControl *ac, const UA_NodeId *session_id, void *session_context, const UA_NodeId *node_id, void *node_context);
/** @brief Maps a `service_quality_t` to the corresponding OPC UA
 *         `UA_StatusCode` used as a `UA_DataValue.status` (e.g. Good/Bad/
 *         Uncertain family), for use as a value's read-quality code. */
UA_StatusCode model_quality_to_status(service_quality_t quality);
/** @brief Copies the live channel snapshot for @p channel_id from
 *         `channel_manager` into @p out_snapshot.
 *  @return false if no channel with that id is currently registered. */
bool model_copy_channel_by_id(const char *channel_id, telemetry_channel_snapshot_t *out_snapshot);
/** @brief Same as `model_copy_channel_by_id`, resolving the channel id from
 *         a `model_node_context_t` (its `channel_id`/`channel_id_storage`)
 *         instead of a raw string. */
bool model_copy_channel_from_context(const model_node_context_t *context,
                                     telemetry_channel_snapshot_t *out_snapshot);
/** @brief Copies the persisted channel *settings* (sample/publish period
 *         etc., distinct from the runtime value snapshot) for @p channel_id.
 *  @return false if no channel with that id is currently registered. */
bool model_copy_channel_settings(const char *channel_id, telemetry_channel_settings_t *out_settings);
/** @brief Maps a `telemetry_channel_value_type_t` to the matching open62541
 *         built-in `UA_NodeId` data type (e.g. Float, Int32, Boolean) used
 *         when creating a generic channel value variable node. */
UA_NodeId model_channel_value_type_to_data_type(telemetry_channel_value_type_t value_type);
/** @brief Builds the v0.4.2 generic channel NodeId string
 *         (`Device.Channels.<channel_id>.<field_name>`-style) for @p channel
 *         into @p buffer.
 *  @return false if the composed id would not fit in @p buffer_size. */
bool model_build_channel_node_id(const telemetry_channel_t *channel, const char *field_name, char *buffer, size_t buffer_size);
/** @brief Builds the legacy single-temperature-channel NodeId string for
 *         @p channel (back-compat path predating the generic channel model).
 *  @return false if the composed id would not fit in @p buffer_size. */
bool model_build_legacy_channel_node_id(const telemetry_channel_t *channel, const char *field_name, char *buffer, size_t buffer_size);
/** @brief True if a config-level change between @p lhs and @p rhs for one
 *         channel definition (driver, enabled state, node identity, etc.)
 *         requires tearing down and rebuilding that channel's address-space
 *         nodes rather than just updating values in place. */
bool model_channel_requires_server_rebuild(const config_channel_definition_t *lhs, const config_channel_definition_t *rhs);
/** @brief `memcmp`-based byte-array equality check treating two NULL
 *         pointers as equal and NULL-vs-non-NULL as different; building
 *         block for config-diff comparisons. */
bool model_byte_array_changed(const uint8_t *lhs, const uint8_t *rhs, size_t size);
/** @brief True if any field that requires a full `UA_Server` rebuild
 *         (port, hostname, PubSub settings, network identity, or any channel
 *         needing `model_channel_requires_server_rebuild`) differs between
 *         @p lhs and @p rhs. Two NULL configs compare unequal only if their
 *         pointers differ. The single source of truth consulted both by the
 *         read-only `MODEL_NODE_CONFIG_REBOOT_REQUIRED` diagnostic
 *         (`model_is_reboot_required`) and, as of v0.5.0 T6, by
 *         `model_address_space_snapshot_requires_server_restart` to decide
 *         whether a committed transaction must trigger an actual controlled
 *         restart. */
bool model_config_requires_server_reboot(const device_profile_config_t *lhs, const device_profile_config_t *rhs);
/**
 * @brief True if the address space was built from a config snapshot that no
 *        longer matches the service-owned RUNNING configuration in a field
 *        requiring a full server rebuild.
 * @note Compares `g_model_address_space_config_snapshot` against
 *       `config_service_get()` only. Staged-only data has not become the
 *       running model and therefore must not restart or mark the live server
 *       as stale. Returns false if the snapshot was never captured.
 */
bool model_address_space_snapshot_requires_server_restart(void);
/** @brief Read-only `Device.Configuration.RebootRequired` diagnostic. In
 *        addition to the RUNNING-only address-space comparison used to
 *        schedule a restart, it retains the existing running-vs-staged
 *        comparison so an uncommitted structural candidate remains visible. */
bool model_is_reboot_required(void);
/**
 * @brief Classifies the server's own device certificate expiry state
 *        relative to the current time, using
 *        `security_service_get_device_certificate_not_after` and
 *        `time_service_get_status`.
 * @param out_time_check_performed Set to false (and the returned state
 *        forced to UNKNOWN/MISSING) if there is no device identity yet or no
 *        usable time source — callers must check this before trusting
 *        `out_days_until_expiry`.
 */
model_certificate_expiry_state_t model_get_certificate_expiry_state(uint32_t *out_days_until_expiry, bool *out_time_check_performed);
/** @brief True if @p state warrants a Condition/event notice (anything but
 *         VALID/UNKNOWN/MISSING at their normal baseline). */
bool model_certificate_expiry_requires_attention(model_certificate_expiry_state_t state);
/** @brief Formats a human-readable certificate-expiry notice message and
 *         severity for @p state/@p days_until_expiry into @p message.
 *  @return false if @p message_size is too small for the formatted text. */
bool model_format_certificate_expiry_notice(model_certificate_expiry_state_t state, uint32_t days_until_expiry, char *message, size_t message_size, UA_UInt16 *out_severity);
/** @brief Combines `model_certificate_expiry_requires_attention` and
 *         `model_format_certificate_expiry_notice`: builds the notice only
 *         if the state actually requires attention.
 *  @return false if attention is not required, or formatting failed. */
bool model_build_certificate_expiry_notice(model_certificate_expiry_state_t state, uint32_t days_until_expiry, char *message, size_t message_size, UA_UInt16 *out_severity);
/** @brief True if @p report (from `security_trust_expiry_service`) contains
 *         a trust-list entry expiry state that warrants a notice, given
 *         whether the device is currently in @p commissioning_mode. */
bool model_trust_expiry_requires_attention(const security_trust_expiry_service_report_t *report, bool commissioning_mode);
/** @brief Formats a human-readable trust-list expiry notice from @p report
 *         into @p message, only when `model_trust_expiry_requires_attention`
 *         would return true.
 *  @return false if attention is not required, or formatting failed. */
bool model_build_trust_expiry_notice(const security_trust_expiry_service_report_t *report, bool commissioning_mode, char *message, size_t message_size, UA_UInt16 *out_severity);
/** @brief Field-wise comparison of two `security_trust_expiry_service_report_t`
 *         snapshots, used to suppress duplicate trust-expiry notices when
 *         nothing has changed since the last poll. */
bool model_trust_expiry_report_changed(const security_trust_expiry_service_report_t *lhs, const security_trust_expiry_service_report_t *rhs);
/**
 * @name Fixed NodeId accessors
 * Each returns a `UA_NODEID_STRING(1, ...)` value for a well-known, always-
 * present object/folder or Condition-type node. Cheap, allocation-free,
 * stateless; safe to call from any context. Node-id path constants they
 * wrap are split between `#define`s in this header and inline literals in
 * `model_channels.c`/`model_address_space.c` — see
 * `docs/code/modules/opcua_model.md#headline-finding-file-split-not-api-boundary`.
 * @{
 */
/** @brief `"Device"` root object NodeId. */
UA_NodeId model_device_node_id(void);
/** @brief `"Device.Network"` folder NodeId. */
UA_NodeId model_network_node_id(void);
/** @brief Legacy single-channel process-temperature NodeId
 *         (`DEVICE_CHANNEL_NODE_TEMPERATURE_1`). */
UA_NodeId model_temperature_node_id(void);
/** @brief `"Device.Maintenance"` folder NodeId. */
UA_NodeId model_maintenance_node_id(void);
/** @brief `"Device.Configuration"` folder NodeId. */
UA_NodeId model_configuration_node_id(void);
/** @brief `"Device.Security"` folder NodeId. */
UA_NodeId model_security_node_id(void);
/** @brief Event-type NodeId for @p node_id, or `MODEL_EVENT_TYPE_DEVICE` if
 *         @p node_id is NULL. */
UA_NodeId model_event_type_node_id(const char *node_id);
/** @brief Condition-type NodeId for @p node_id, or
 *         `MODEL_CONDITION_TYPE_SECURITY_FAULT` if @p node_id is NULL.
 *         Only declared when Alarms & Conditions is enabled. */
UA_NodeId model_condition_type_node_id(const char *node_id);
/** @brief `Device.Security.SecurityFaultCondition` instance NodeId. */
UA_NodeId model_security_condition_node_id(void);
/** @brief `"Device.DeviceHealthAlarms"` folder NodeId. */
UA_NodeId model_device_health_alarms_node_id(void);
/** @brief Instance NodeId of the DeviceHealth Condition matching
 *         @p state (Failure/CheckFunction/OffSpec/MaintenanceRequired). */
UA_NodeId model_device_health_condition_node_id(device_health_state_t state);
/** @} */
/** @brief Adds a `UA_ObjectTypeNode` with @p browse_name under
 *         @p parent_type_id, used to declare the small set of custom
 *         ObjectTypes this model defines beyond DI/AMB. */
UA_StatusCode model_add_object_type(UA_Server *server, const char *node_id, const char *browse_name, const UA_NodeId parent_type_id);
/** @brief `strncpy`-style safe string copy that always NUL-terminates
 *         @p buffer and truncates rather than overflowing. */
void model_copy_string_safe(char *buffer, size_t buffer_size, const char *text);
/** @brief Formats @p event_id's bytes as a lowercase hex string into
 *         @p buffer, truncating if it does not fit. */
void model_event_id_to_hex(const UA_ByteString *event_id, char *buffer, size_t buffer_size);
/** @brief Records an emitted event's id/message into the matching field of
 *         `g_last_event_state` selected by @p kind, backing the
 *         `MODEL_NODE_*_LAST_*` read-only variable nodes. */
void model_store_last_event(model_event_last_kind_t kind, const char *event_id_hex, const char *message);
/** @brief Clears every open62541 `UA_NodeId` and zeroes `g_model_pubsub` /
 *         `g_model_pubsub_event_mirror`, called before (re)configuring
 *         PubSub so stale Connection/WriterGroup ids are never reused. */
void model_pubsub_reset_runtime(void);
/** @brief True if the config requests MQTT PubSub and both broker URL and
 *         topic root are non-empty — the gate `model_configure_pubsub_publishers`
 *         checks before building anything. */
bool model_pubsub_requested(void);
/** @brief Formats @p node_id as `ns=<i>;i=<id>` / `ns=<i>;s=<id>` text into
 *         @p buffer (the non-printf counterpart of
 *         `model_log_node_id_compact`). */
void model_node_id_to_text(const UA_NodeId *node_id, char *buffer, size_t buffer_size);
/** @brief Copies the final `.`-delimited path segment of @p text into
 *         @p buffer, used to derive a short PubSub alias from a full NodeId
 *         path. */
void model_pubsub_extract_tail(const char *text, char *buffer, size_t buffer_size);
/** @brief Builds the MQTT client id used for the PubSub connection into
 *         @p buffer (derived from device identity).
 *  @return false if it does not fit in @p buffer_size. */
bool model_pubsub_build_client_id(char *buffer, size_t buffer_size);
/** @brief Joins @p root, @p suffix, and @p leaf into an MQTT topic string
 *         (`<root>/<suffix>/<leaf>`-style) into @p buffer.
 *  @return false if the composed topic does not fit in @p buffer_size. */
bool model_pubsub_build_topic(char *buffer, size_t buffer_size, const char *root, const char *suffix, const char *leaf);
/**
 * @brief `UA_PubSubStateChangeCallback` installed on every PubSub component
 *        this model creates; open62541 invokes it whenever a Connection/
 *        WriterGroup/DataSetWriter transitions state (e.g. into Error).
 * @note Currently only logs via `model_pubsub_component_type_to_string` /
 *       `model_pubsub_state_to_string`; does not itself retry or reconfigure.
 *       Runs on `OpcUaTask`.
 */
void model_pubsub_state_change_callback(UA_Server *server, const UA_NodeId id, UA_PubSubState state, UA_StatusCode status);
/**
 * @brief Registers the MQTT `UA_ConnectionManager` as an event source on
 *        @p config's event loop, required once before any MQTT PubSub
 *        Connection can be created.
 * @return `UA_STATUSCODE_BADINVALIDARGUMENT` if @p config or its event loop
 *         is NULL, `UA_STATUSCODE_BADOUTOFMEMORY` if the manager cannot be
 *         allocated, else the event loop's registration status.
 * @note Only declared when `UA_ENABLE_PUBSUB` and `UA_ENABLE_MQTT` are both
 *       enabled.
 */
UA_StatusCode model_pubsub_register_mqtt_manager(UA_ServerConfig *config);
/** @brief Updates `g_model_pubsub_event_mirror` with the fields of a just-
 *         emitted event, backing the `Device.Messaging.LastPubSubEvent*`
 *         read-only nodes. */
void model_pubsub_update_event_mirror(const UA_NodeId *event_type_id, const char *source_name, const char *message, UA_UInt16 severity, UA_DateTime event_time, const UA_ByteString *event_id);
/** @brief Forces the Events-mirror `DataSetWriter` to publish its current
 *         value immediately rather than waiting for its next periodic
 *         publishing interval, called right after an event is emitted so
 *         MQTT subscribers see it promptly. */
void model_pubsub_trigger_event_publish(UA_Server *server);
/** @brief Adds @p published_variable to PublishedDataSet @p pds_id under
 *         field name @p alias. Building block for the telemetry/event
 *         writer setup below. */
UA_StatusCode model_pubsub_add_variable_field(UA_Server *server, const UA_NodeId pds_id, const UA_NodeId published_variable, const char *alias);
/** @brief Creates the single MQTT PubSub `Connection` for this server
 *         (broker URL/client id from `g_model_pubsub`), storing its NodeId
 *         into `g_model_pubsub.connection_id`. */
UA_StatusCode model_pubsub_add_mqtt_connection(UA_Server *server);
/** @brief Creates one `WriterGroup` on the MQTT connection publishing to
 *         @p topic at @p publishing_interval, returning its NodeId via
 *         @p out_writer_group_id. */
UA_StatusCode model_pubsub_add_writer_group(UA_Server *server, const char *name, const char *topic, UA_Duration publishing_interval, UA_UInt16 writer_group_id, UA_NodeId *out_writer_group_id);
/** @brief Creates one `DataSetWriter` binding PublishedDataSet @p pds_id
 *         into WriterGroup @p writer_group_id. */
UA_StatusCode model_pubsub_add_data_set_writer(UA_Server *server, const UA_NodeId writer_group_id, const UA_NodeId pds_id, const char *name, const char *topic, UA_UInt16 writer_id);
/** @brief Builds the full PublishedDataSet/WriterGroup/DataSetWriter chain
 *         publishing @p channel's telemetry value, appending the new
 *         WriterGroup NodeId to `g_model_pubsub.telemetry_writer_group_ids`. */
UA_StatusCode model_pubsub_add_telemetry_writer_for_channel(UA_Server *server, size_t channel_index, const telemetry_channel_t *channel);
/** @brief Builds the PublishedDataSet/WriterGroup/DataSetWriter chain that
 *         mirrors the most recent event (`model_pubsub_event_mirror_t`
 *         fields) onto its own MQTT topic. */
UA_StatusCode model_pubsub_add_event_mirror_writer(UA_Server *server);
/**
 * @brief Top-level PubSub setup entry point, called once from
 *        `model_core_task` during server startup: resets runtime state,
 *        checks `model_pubsub_requested`, registers the MQTT manager,
 *        creates the connection, and adds a telemetry writer per eligible
 *        channel plus the event-mirror writer.
 * @return `UA_STATUSCODE_GOOD` if PubSub is not requested (no-op) or setup
 *         fully succeeded; the first failing step's status otherwise.
 * @note Runs on `OpcUaTask` during startup only, not on every poll.
 */
UA_StatusCode model_configure_pubsub_publishers(UA_Server *server, UA_ServerConfig *config);
/** @brief Enqueues one audit entry (see `model_pending_audit_event_t`). May
 *         run on a different task than `OpcUaTask` (e.g. via the
 *         `security_service` audit hook); synchronized internally. */
void model_queue_pending_audit_event(const char *event, const char *actor, const char *subject, bool success, const char *details);
/** @brief Dequeues the oldest pending audit entry into @p out_event.
 *  @return false if the queue is empty. Called from `OpcUaTask`
 *          (`model_process_pending_audit_events`) each poll iteration. */
bool model_pop_pending_audit_event(model_pending_audit_event_t *out_event);
/**
 * @brief `security_service` audit-log callback registered by the model so
 *        every service-level audit event also becomes a queued OPC UA
 *        audit event.
 * @note May be invoked by `security_service` from whichever task performed
 *       the audited action, not necessarily `OpcUaTask`; only queues (via
 *       `model_queue_pending_audit_event`), never touches open62541 state
 *       directly, so it is safe off the OPC UA task.
 */
void model_security_audit_callback(const char *event, const char *actor, const char *subject, bool success, const char *details, void *context);
/** @brief NULL-safe string inequality check (`NULL` treated as `""`). */
bool model_string_changed(const char *lhs, const char *rhs);
/** @brief Declares a custom Event ObjectType; thin wrapper over
 *         `model_add_object_type`. */
UA_StatusCode model_add_event_type(UA_Server *server, const char *node_id, const char *browse_name, const UA_NodeId parent_type_id);
/**
 * @brief Adds a read-only scalar property variable node under an event
 *        type, and — if namespace 0 was built with the Mandatory
 *        ModellingRule instantiated (not guaranteed under a MINIMAL NS0
 *        build) — links it with `HasModellingRule`.
 * @return `UA_STATUSCODE_GOOD` even when the ModellingRule reference is
 *         skipped because NS0 is MINIMAL; only the variable-node creation
 *         failure is propagated as an error.
 */
UA_StatusCode model_add_event_property(UA_Server *server, const UA_NodeId event_type_id, const char *node_id, const char *browse_name, UA_NodeId data_type);
/**
 * @brief Formats the open62541 `UA_SimpleAttributeOperand_print()`-style key
 *        (`"/1:<browse_name>"`) used to index a custom event field in a
 *        `UA_KeyValueMap`, since open62541 does not accept a plain
 *        `UA_QUALIFIEDNAME(1, browse_name)` for this lookup.
 * @return `UA_STATUSCODE_BADENCODINGLIMITSEXCEEDED` if @p browse_name does
 *         not fit within `MODEL_EVENT_FIELD_PATH_CAPACITY`.
 */
UA_StatusCode model_make_event_field_key(const char *browse_name, char field_key[MODEL_EVENT_FIELD_PATH_CAPACITY]);
/** @brief Sets a string-valued custom field on @p event_fields, keyed via
 *         `model_make_event_field_key`. NULL @p value is stored as `""`. */
UA_StatusCode model_set_event_string_field(UA_KeyValueMap *event_fields, const char *browse_name, const char *value);
/**
 * @brief Marks @p source_id as an event notifier and links it to
 *        @p parent_id via `HasNotifier`, required once per Condition source
 *        node before conditions on it can be refreshed/subscribed.
 * @return `UA_STATUSCODE_GOOD` even if the reference already existed
 *         (`UA_STATUSCODE_BADDUPLICATEREFERENCENOTALLOWED` is treated as
 *         success, so this is safe to call repeatedly/idempotently).
 * @note Only declared when Alarms & Conditions is enabled.
 */
UA_StatusCode model_enable_condition_source(UA_Server *server, const UA_NodeId parent_id, const UA_NodeId source_id);
/** @brief Declares a custom Condition ObjectType; thin wrapper over
 *         `model_add_object_type`. */
UA_StatusCode model_add_condition_type(UA_Server *server, const char *node_id, const char *browse_name, const UA_NodeId parent_type_id);
/** @brief Writes a single scalar Condition field (e.g. `Severity`,
 *         `Quality`, `Retain`) via `UA_Server_setConditionField`. */
UA_StatusCode model_condition_set_field_scalar(UA_Server *server, const UA_NodeId condition_id, const char *field_name, const void *value, const UA_DataType *type);
/** @brief Writes a Condition's `Message` field (a `UA_LocalizedText`) from a
 *         plain C string. */
UA_StatusCode model_condition_set_message(UA_Server *server, const UA_NodeId condition_id, const char *message);
/** @brief Sets a TwoStateVariable sub-field (e.g. `AckedState`,
 *         `ActiveState`) to @p state via `UA_Server_setConditionTwoStateVariableCallback`-
 *         adjacent field write. */
UA_StatusCode model_condition_set_two_state_id(UA_Server *server, const UA_NodeId condition_id, const char *field_name, UA_Boolean state);
/** @brief Reads a Condition's current `EnabledState` into @p out_enabled. */
UA_StatusCode model_condition_read_enabled(UA_Server *server, const UA_NodeId condition_id, bool *out_enabled);
/** @brief Adds one AMB `MaintenanceRequired` companion-spec property
 *         (e.g. PlannedDate, EstimatedDowntime) with an initial scalar
 *         @p value under Condition @p condition_id. */
UA_StatusCode model_add_amb_maintenance_property( UA_Server *server, const UA_NodeId condition_id, const char *node_id, const char *browse_name, UA_NodeId data_type, const void *value, const UA_DataType *value_type);
/** @brief Registers the custom `model_amb_name_node_id_t` (AMB
 *         `NameNodeIdDataType`) as a `UA_DataType` on @p server so it can be
 *         used as an array property value type. Idempotent via
 *         `g_model_amb_custom_types_registered`. */
UA_StatusCode model_register_amb_name_node_id_type(UA_Server *server);
/** @brief Populates @p value's `name` (from @p text) and null `node_id`
 *         fields for one AMB NameNodeId entry. */
void model_make_amb_name_node_id(model_amb_name_node_id_t *value, const char *text);
/** @brief Maps a `maintenance_service_state_t` to the AMB companion-spec
 *         numeric state-machine NodeId used for `CurrentState/Id`. */
UA_UInt32 model_maintenance_state_node_id( maintenance_service_state_t state);
/** @brief True if @p snapshot's maintenance state should present the
 *         `MaintenanceRequired` AMB Condition as active. */
bool model_maintenance_condition_is_active( const maintenance_service_snapshot_t *snapshot);
/** @brief Maps @p snapshot's maintenance state/urgency to a Condition
 *         severity value. */
UA_UInt16 model_maintenance_condition_severity( const maintenance_service_snapshot_t *snapshot);
/** @brief Sets @p value's status to @p status and clears its data, the
 *         standard error path for a DataSource read callback. */
UA_StatusCode model_maintenance_set_read_error(UA_DataValue *value, UA_StatusCode status);
/**
 * @brief `UA_DataSource.read` implementation for every value-source node
 *        installed under the `MaintenanceRequired` AMB condition (dispatches
 *        on `model_maintenance_value_context_t::kind`), reading the current
 *        `maintenance_service` snapshot on each call.
 * @note Runs on `OpcUaTask` inside open62541's Read service.
 */
UA_StatusCode model_read_maintenance_value( UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *node_id, void *node_context, UA_Boolean include_source_timestamp, const UA_NumericRange *range, UA_DataValue *value);
/** @brief Installs `model_read_maintenance_value` as the `UA_DataSource` for
 *         @p node_id, tagging its context with @p kind. */
UA_StatusCode model_install_maintenance_value_source( UA_Server *server, UA_NodeId node_id, model_maintenance_value_kind_t kind);
/** @brief Resolves @p path relative to @p condition_id and installs a
 *         maintenance value source there (see
 *         `model_install_maintenance_value_source`), used for AMB fields not
 *         addressable by a fixed NodeId. */
UA_StatusCode model_install_maintenance_value_source_at_path( UA_Server *server, const UA_NodeId condition_id, size_t path_size, UA_QualifiedName *path, model_maintenance_value_kind_t kind);
/** @brief Installs every value source (state, dates, parts, message,
 *         severity, quality, timestamps, ActiveState, Retain) for the
 *         `MaintenanceRequired` AMB condition in one call. */
UA_StatusCode model_install_maintenance_condition_value_sources( UA_Server *server, const UA_NodeId condition_id);
/** @brief Adds an AMB `NameNodeIdDataType`-valued property (scalar or, if
 *         @p is_array, array) under Condition @p condition_id, registering
 *         the custom type first if needed. */
UA_StatusCode model_add_amb_name_node_id_property( UA_Server *server, const UA_NodeId condition_id, const char *node_id, const char *browse_name, UA_Boolean is_array);
/** @brief Writes the AMB `CurrentState`/`CurrentState/Id` pair for the
 *         `MaintenanceRequired` condition from @p state_name/@p state_node_id. */
UA_StatusCode model_set_amb_maintenance_state( UA_Server *server, const UA_NodeId condition_id, const char *state_name, UA_UInt32 state_node_id);
/** @brief One-time setup of the `MaintenanceRequired` AMB condition's value
 *         sources and static AMB properties at address-space build time. */
UA_StatusCode model_initialize_amb_maintenance_condition( UA_Server *server, const UA_NodeId condition_id);
/** @brief Writes a Condition's `Time`/`ReceiveTime`/`Severity`/`Quality`/
 *         `Message` fields together; shared building block for the
 *         activate/deactivate/initialize helpers below. */
UA_StatusCode model_condition_write_common_fields(UA_Server *server, const UA_NodeId condition_id, UA_UInt16 severity, UA_StatusCode quality, const char *message);
/** @brief Initializes a freshly-created Condition to its baseline inactive/
 *         acked state (`Retain=false`, `AckedState=true`, `ActiveState=false`)
 *         with @p message, at address-space build time. */
UA_StatusCode model_condition_initialize_inactive(UA_Server *server, const UA_NodeId condition_id, const char *message);
/**
 * @brief First-activation transition: writes common fields, sets
 *        `Retain=true`, optionally resets `AckedState=false` (if
 *        @p reset_ack), and sets `ActiveState=true`.
 * @note Does **not** call `UA_Server_triggerConditionEvent` — used only when
 *       transitioning from inactive to active; see
 *       `model_condition_update_active` for the already-active case.
 */
UA_StatusCode model_condition_activate(UA_Server *server, const UA_NodeId condition_id, UA_UInt16 severity, UA_StatusCode quality, const char *message, bool reset_ack);
/**
 * @brief Refresh transition for a Condition that is already active but
 *        whose severity/message/reason changed: same field writes as
 *        `model_condition_activate`, plus an explicit
 *        `UA_Server_triggerConditionEvent` so subscribers see a new event.
 */
UA_StatusCode model_condition_update_active(UA_Server *server, const UA_NodeId condition_id, const UA_NodeId source_id, UA_UInt16 severity, UA_StatusCode quality, const char *message, bool reset_ack);
/** @brief Deactivation transition: writes common fields with @p message,
 *         `Retain=false`, `AckedState=true`, `ActiveState=false`, then
 *         triggers a ConditionEvent for subscribers. */
UA_StatusCode model_condition_deactivate(UA_Server *server, const UA_NodeId condition_id, const UA_NodeId source_id, const char *message);
/**
 * @brief Reconciles the `MaintenanceRequired` Condition's state fields from
 *        @p snapshot without triggering a ConditionEvent.
 * @note Used for both live commits and startup/recovery replay; recovery
 *       never re-triggers the v0.3.2 workflow transition event, so silent
 *       field sync is intentional here (see in-source rationale comment
 *       above the implementation in `model_maintenance.c`).
 */
UA_StatusCode model_sync_maintenance_condition( UA_Server *server, const maintenance_service_snapshot_t *snapshot);
/** @brief Creates one Condition instance of @p condition_type_id at
 *         @p condition_node_id, sourced from @p source_id, and initializes
 *         it inactive with @p normal_message
 *         (`model_condition_initialize_inactive`). */
UA_StatusCode model_create_condition_instance(UA_Server *server, const char *condition_node_id, const char *browse_name, const UA_NodeId condition_type_id, const UA_NodeId source_id, const char *normal_message);
/** @brief Creates one DeviceHealth Condition instance (Failure/CheckFunction/
 *         OffSpec/MaintenanceRequired) for @p state. */
UA_StatusCode model_create_device_health_condition(UA_Server *server, device_health_state_t state, const char *browse_name, UA_UInt32 condition_type_id);
/**
 * @brief Builds the DeviceHealth and SecurityFault Condition instances (the
 *        older, non-per-channel Alarms & Conditions subsystem) at
 *        address-space build time.
 * @note Distinct from `model_build_channel_alarm_conditions` (v0.4.3
 *       per-channel alarms, `model_events.c`); this covers device-level
 *       health/security conditions only.
 */
UA_StatusCode model_build_alarm_conditions(UA_Server *server);
/**
 * @brief Evaluates whether `Device.Security.SecurityFaultCondition` should
 *        currently be active — certificate expiry, empty trust list, auth
 *        failures, or commissioning mode — and formats the corresponding
 *        message/severity/quality.
 * @return `MODEL_SECURITY_CONDITION_REASON_NONE` if nothing currently
 *         warrants the condition.
 */
model_security_condition_reason_t model_evaluate_security_condition(char *message, size_t message_size, UA_UInt16 *out_severity, UA_StatusCode *out_quality);
/**
 * @brief Shared edge-triggered Condition state machine: given a computed
 *        @p reason (0 = inactive) and the previous `*was_active`/`*was_reason`/
 *        `*was_enabled` state, calls `model_condition_activate`,
 *        `model_condition_update_active`, or `model_condition_deactivate` as
 *        needed and updates the tracking out-parameters. Also re-polls
 *        `EnabledState` at most every `MODEL_CONDITION_ENABLE_POLL_MS` via
 *        @p next_enable_poll, since a client may disable/re-enable a
 *        Condition at runtime.
 * @note `UA_STATUSCODE_BADCONDITIONALREADYDISABLED` from the underlying
 *       writes is treated as expected/non-fatal (condition disabled by a
 *       client), not an error.
 */
void model_sync_condition(UA_Server *server, const UA_NodeId condition_id, const UA_NodeId source_id, bool *was_active, uint32_t *was_reason, bool *was_enabled, TickType_t *next_enable_poll, uint32_t reason, UA_UInt16 severity, UA_StatusCode quality, const char *active_message, const char *normal_message);
/** @brief Per-poll-iteration reconciliation of the DeviceHealth Conditions
 *         against `device_health_service` state, via `model_sync_condition`
 *         for each of the 5 tracked states in @p state. */
void model_process_device_health_conditions(UA_Server *server, model_event_runtime_state_t *state);
/** @brief Per-poll-iteration reconciliation of the SecurityFault Condition
 *         via `model_evaluate_security_condition` + `model_sync_condition`. */
void model_process_alarm_conditions(UA_Server *server, model_event_runtime_state_t *state);
/* v0.4.3 per-channel alarm condition model (T6), defined in model_events.c. */
/**
 * @brief Creates one `ChannelAlarmConditionType` instance per channel
 *        eligible for alarms (per `alarm_service_value_type_is_eligible`),
 *        at address-space build time.
 * @note See `docs/code/modules/opcua_model.md#alarms--conditions-and-historyread`.
 */
UA_StatusCode model_build_channel_alarm_conditions(UA_Server *server);
/**
 * @brief Per-poll-iteration translation of `alarm_service`'s already-computed
 *        `alarm_condition_status_t` per channel into
 *        `ActiveState`/`Severity`/`Quality` on the matching Condition
 *        instance.
 * @note Pure translation — threshold evaluation happens in `alarm_service`,
 *       not here. Acknowledgement flows the opposite direction through a
 *       `UA_TwoStateVariableChangeCallback` into
 *       `alarm_service_acknowledge_channel` (not declared in this header).
 */
void model_process_channel_alarm_conditions(UA_Server *server);
/**
 * @brief One-time startup fixup: writes the `InputArguments`/`OutputArguments`
 *        Argument metadata for the shared namespace-0 Condition `Refresh`/
 *        `Refresh2`/`Acknowledge`/`Comment` Method nodes so UaExpert and
 *        similar clients render correct call dialogs.
 * @note Writes static namespace-0 node metadata once; not per-condition or
 *       per-channel state.
 */
UA_StatusCode model_fix_condition_method_arguments(UA_Server *server);
/**
 * @brief One-time startup fixup: this firmware's compiled-in namespace-0
 *        data does not give the fixed `Server` object a direct
 *        `HasComponent` reference to `ConditionType_ConditionRefresh`,
 *        which the OPC UA spec requires clients to call on; without it,
 *        `callWithMethodAndObject()`'s non-inherited browse-name lookup
 *        fails with `BadMethodInvalid`. Adds the missing reference once.
 * @return `UA_STATUSCODE_BADINVALIDARGUMENT` if @p server is NULL;
 *         `UA_STATUSCODE_GOOD` if the reference already existed.
 * @note Confirmed live during v0.4.3 hardware verification — see the
 *       in-source rationale comment above the implementation in
 *       `model_events.c`.
 */
UA_StatusCode model_fix_server_condition_refresh_reference(UA_Server *server);
/**
 * @name Typed event-field setters
 * Each sets one scalar-typed custom field on @p event_fields via
 * `model_make_event_field_key`, the typed counterparts of
 * `model_set_event_string_field`.
 * @{
 */
UA_StatusCode model_set_event_bool_field(UA_KeyValueMap *event_fields, const char *browse_name, UA_Boolean value);
UA_StatusCode model_set_event_uint32_field(UA_KeyValueMap *event_fields, const char *browse_name, UA_UInt32 value);
UA_StatusCode model_set_event_float_field(UA_KeyValueMap *event_fields, const char *browse_name, UA_Float value);
/** @} */
/**
 * @name Event field-writer implementations
 * Each is a `model_event_fields_writer_t` populating one event type's
 * custom fields from its matching `model_*_event_fields_t` /
 * `model_*_emit_fields_t` struct (cast from the opaque `fields_context`);
 * passed to `model_emit_event_ex`. `model_write_audit_config_emit_fields`
 * and `model_write_audit_security_emit_fields` compose the generic audit
 * fields with the config/security-specific ones for a single combined
 * event.
 * @{
 */
UA_StatusCode model_write_config_event_fields(UA_KeyValueMap *event_fields, const void *fields_context);
UA_StatusCode model_write_network_event_fields(UA_KeyValueMap *event_fields, const void *fields_context);
UA_StatusCode model_write_sensor_fault_event_fields(UA_KeyValueMap *event_fields, const void *fields_context);
UA_StatusCode model_write_maintenance_event_fields(UA_KeyValueMap *event_fields, const void *fields_context);
UA_StatusCode model_write_firmware_event_fields(UA_KeyValueMap *event_fields, const void *fields_context);
UA_StatusCode model_write_audit_event_fields(UA_KeyValueMap *event_fields, const void *fields_context);
UA_StatusCode model_write_audit_config_event_fields(UA_KeyValueMap *event_fields, const void *fields_context);
UA_StatusCode model_write_audit_security_event_fields(UA_KeyValueMap *event_fields, const void *fields_context);
UA_StatusCode model_write_audit_config_emit_fields(UA_KeyValueMap *event_fields, const void *fields_context);
UA_StatusCode model_write_audit_security_emit_fields(UA_KeyValueMap *event_fields, const void *fields_context);
/** @} */
/**
 * @brief Core event-emission primitive: builds `Time`/`ReceiveTime`/
 *        `SourceNode`/`SourceName`/`Message`/`Severity` plus any
 *        @p fields_writer-populated custom fields, calls
 *        `UA_Server_createEvent` (which creates and triggers the event node
 *        in one call in this open62541 version), mirrors the event id/
 *        message via `model_store_last_event(last_kind, ...)`, and (if
 *        PubSub is enabled) updates and force-publishes the Events mirror.
 * @param fields_writer May be NULL (no custom fields beyond the standard
 *        ones).
 * @return `UA_STATUSCODE_BADINVALIDARGUMENT` if @p server is NULL; the first
 *         failing `UA_KeyValueMap_setScalar`/`UA_Server_createEvent` status
 *         otherwise.
 * @note Runs on `OpcUaTask`. Every `model_emit_*` helper below is a thin
 *       wrapper over this.
 */
UA_StatusCode model_emit_event_ex(UA_Server *server, const UA_NodeId event_type_id, const UA_NodeId source_node_id, const char *source_name, const char *message, UA_UInt16 severity, model_event_fields_writer_t fields_writer, const void *fields_context, model_event_last_kind_t last_kind);
/** @brief `model_emit_event_ex` with no custom fields writer and
 *         `MODEL_EVENT_LAST_KIND_GENERIC`. */
UA_StatusCode model_emit_event(UA_Server *server, const UA_NodeId event_type_id, const UA_NodeId source_node_id, const char *source_name, const char *message, UA_UInt16 severity);
/** @brief Emits a `Types.Events.NetworkEvent` describing an IP/DHCP/link
 *         transition, sourced from `Device.Network`. */
void model_emit_network_runtime_event(UA_Server *server, const char *message, UA_UInt16 severity, const char *old_ip, const char *new_ip, bool dhcp_enabled, bool link_up);
/** @brief Emits a `Types.Events.SensorFaultEvent` for a channel fault,
 *         sourced from `Device`. */
void model_emit_sensor_runtime_event(UA_Server *server, const char *message, UA_UInt16 severity, uint32_t fault_code, const char *quality, float measured_value);
/** @brief Emits a `Types.Events.MaintenanceEvent` for a maintenance
 *         operation result, sourced from `Device.Maintenance`. */
UA_StatusCode model_emit_maintenance_event(UA_Server *server, const char *message, UA_UInt16 severity, const char *operation, uint32_t result_code, const char *actor);
/** @brief Days since the civil epoch for (@p year, @p month, @p day),
 *         Howard Hinnant's `days_from_civil` algorithm; building block for
 *         converting an mbedTLS X.509 time to a Unix epoch without a libc
 *         `mktime`/timezone-database dependency. */
int64_t model_x509_days_from_civil(int32_t year, uint32_t month, uint32_t day);
/** @brief Converts an mbedTLS `mbedtls_x509_time` (as found in a certificate's
 *         `notBefore`/`notAfter`) to a Unix epoch-seconds value via
 *         `model_x509_days_from_civil`.
 *  @return false if the time value is out of the representable range. */
bool model_x509_time_to_epoch(const mbedtls_x509_time *time, uint32_t *out_epoch_s);
/** @brief Computes a display thumbprint (SHA-1 digest, hex-formatted) of
 *         @p remote_certificate into @p buffer.
 *  @return false if hashing fails or the hex string does not fit. */
bool model_remote_certificate_thumbprint(const UA_ByteString *remote_certificate, char *buffer, size_t buffer_size);
/** @brief Parses @p remote_certificate's `notAfter` field and classifies its
 *         expiry state relative to the current time into @p out_info (see
 *         `model_remote_certificate_expiry_info_t`).
 *  @return false if the certificate cannot be parsed. */
bool model_get_remote_certificate_expiry_info(const UA_ByteString *remote_certificate, model_remote_certificate_expiry_info_t *out_info);
/** @brief If @p remote_certificate's expiry warrants attention, emits an
 *         audit/security event noting it for the connecting @p session_context.
 *         No-op otherwise. Called once per SecureChannel-open with a client
 *         certificate. */
void model_emit_client_certificate_expiry_notice(UA_Server *server, const UA_ByteString *remote_certificate, void *session_context);
/** @brief Emits a `Types.Events.ConfigChangedEvent`, sourced from
 *         `Device.Configuration`. */
void model_emit_config_changed_event(UA_Server *server, const char *message, UA_UInt16 severity, const char *changed_section, const char *changed_key, const char *actor);
/** @brief Emits a `Types.Events.FirmwareEvent`, sourced from `Device`. */
void model_emit_firmware_event(UA_Server *server, const char *message, UA_UInt16 severity);
/** @brief Drains and emits every queued audit event (see
 *         `model_pop_pending_audit_event`) as an
 *         `MODEL_EVENT_TYPE_AUDIT*` event, called once per `model_core_task`
 *         poll iteration. */
void model_process_pending_audit_events(UA_Server *server);
/** @brief Zero-initializes a caller-owned `model_event_runtime_state_t`,
 *         including its `#if`-gated Alarms & Conditions sub-fields. */
void model_event_runtime_state_init(model_event_runtime_state_t *state);
/** @brief Per-poll-iteration check of the server's own device-certificate
 *         expiry (`model_get_certificate_expiry_state`), emitting a notice
 *         event only on a state transition, tracked via @p state. */
void model_process_certificate_expiry_runtime(UA_Server *server, model_event_runtime_state_t *state, char *message, size_t message_size);
/** @brief Per-poll-iteration check of trust-list entry expiry
 *         (`security_trust_expiry_service`), emitting a notice event only on
 *         a change (`model_trust_expiry_report_changed`), tracked via
 *         @p state. */
void model_process_trust_expiry_runtime(UA_Server *server, model_event_runtime_state_t *state, char *message, size_t message_size);
/**
 * @brief Top-level per-poll-iteration event/condition reconciliation:
 *        network/sensor change detection, certificate/trust expiry checks,
 *        pending audit event drain, and (if Alarms & Conditions is enabled)
 *        DeviceHealth/SecurityFault/channel-alarm Condition sync.
 * @note Called once per `model_core_task` loop iteration on `OpcUaTask`.
 */
void model_process_runtime_events(UA_Server *server, model_event_runtime_state_t *state);
/** @brief Stamps @p value's source timestamp with the current time, only if
 *         @p include_source_timestamp is true (per the DataSource read
 *         contract's `timestampsToReturn` hint). */
void model_apply_now_source_timestamp(UA_Boolean include_source_timestamp, UA_DataValue *value);
/** @brief Stamps @p value's source timestamp from @p epoch_seconds, only if
 *         @p include_source_timestamp is true. */
void model_apply_epoch_source_timestamp(UA_Boolean include_source_timestamp, uint32_t epoch_seconds, UA_DataValue *value);
/** @brief Converts a microsecond-resolution Unix timestamp to a
 *         `UA_DateTime` (100ns ticks since 1601-01-01).
 *  @return false on overflow / out-of-range input. */
bool model_source_timestamp_to_datetime(uint64_t source_timestamp_unix_us,
                                        UA_DateTime *out_datetime);
/** @brief Stamps @p value's source timestamp from a channel sample's
 *         microsecond Unix timestamp (`model_source_timestamp_to_datetime`),
 *         only if @p include_source_timestamp is true; leaves the timestamp
 *         unset if the conversion fails. */
void model_apply_channel_source_timestamp(UA_Boolean include_source_timestamp,
                                          uint64_t source_timestamp_unix_us,
                                          UA_DataValue *value);
/**
 * @name Typed `UA_DataValue` setters
 * Each sets @p value's scalar data (of the named C/UA type), its `status`
 * code, and marks it as having a value only when @p status is Good-family;
 * the shared leaf primitive used by every generic-node DataSource read
 * callback to fill a `UA_DataValue` without repeating open62541
 * boilerplate. Never fail (always return @p status unchanged).
 * @{
 */
UA_StatusCode model_set_uint32_value(UA_DataValue *value, UA_UInt32 data, UA_StatusCode status);
UA_StatusCode model_set_int32_value(UA_DataValue *value, UA_Int32 data, UA_StatusCode status);
UA_StatusCode model_set_float_value(UA_DataValue *value, UA_Float data, UA_StatusCode status);
UA_StatusCode model_set_double_value(UA_DataValue *value, UA_Double data, UA_StatusCode status);
UA_StatusCode model_set_boolean_value(UA_DataValue *value, UA_Boolean data, UA_StatusCode status);
UA_StatusCode model_set_uint64_value(UA_DataValue *value, UA_UInt64 data, UA_StatusCode status);
UA_StatusCode model_set_int64_value(UA_DataValue *value, UA_Int64 data, UA_StatusCode status);
UA_StatusCode model_set_int16_value(UA_DataValue *value, UA_Int16 data, UA_StatusCode status);
/** @brief Same as the numeric setters, but copies @p data into a
 *         `UA_String` (allocating open62541-owned storage). */
UA_StatusCode model_set_string_value(UA_DataValue *value, const char *data, UA_StatusCode status);
/** @} */
/** @brief Maps a `maintenance_service_result_t` to the matching
 *         `UA_StatusCode` for a method-call return value. */
UA_StatusCode model_maintenance_result_status(maintenance_service_result_t result);
/** @brief Stages a `Types.Events.MaintenanceEvent` (fields + message) into
 *         @p event without publishing it yet, so it can be built before the
 *         underlying maintenance transaction is known to have committed. */
UA_StatusCode model_prepare_maintenance_event( model_prepared_maintenance_event_t *event, const char *message, const char *operation, const char *actor);
/** @brief Publishes an event previously staged by
 *         `model_prepare_maintenance_event` via `UA_Server_createEvent`,
 *         then clears its open62541 members. */
UA_StatusCode model_publish_prepared_maintenance_event( UA_Server *server, model_prepared_maintenance_event_t *event);
/**
 * @brief Availability gate checked before accepting a maintenance-workflow
 *        method call: fails if the adapter's own fault latch
 *        (`g_model_maintenance_faulted`) is set, or if
 *        `maintenance_service_get_event_publication_fault` reports the
 *        service itself cannot currently publish state.
 * @return `UA_STATUSCODE_BADRESOURCEUNAVAILABLE` if unavailable, else
 *         `UA_STATUSCODE_GOOD`.
 */
UA_StatusCode model_maintenance_mutations_available(void);
/**
 * @brief Shared commit path for every maintenance workflow method
 *        (Plan/Start/Reschedule/Finish): resolves @p transaction against
 *        `maintenance_service_get_prepared_snapshot`, commits it, syncs the
 *        AMB Condition (`model_sync_maintenance_condition`), audits the
 *        state change, and emits a maintenance event.
 * @return `UA_STATUSCODE_BADINVALIDARGUMENT` if @p server or @p transaction
 *         is NULL; otherwise the mapped `maintenance_service_result_t`
 *         status.
 */
UA_StatusCode model_commit_maintenance_transaction(UA_Server *server, const maintenance_service_transaction_t *transaction, const char *operation, void *session_context);
/** @brief Copies a scalar `UA_String` method-input @p value into @p out
 *         (bounded by @p capacity), used for free-text maintenance fields.
 *  @return false if not a scalar string or it does not fit. */
bool model_copy_maintenance_string(const UA_Variant *value, char *out, size_t capacity);
/** @brief Copies an array-of-strings method input @p value into the fixed
 *         @p out grid (`MAINTENANCE_SERVICE_MAX_PARTS` rows), used for the
 *         Parts Replaced/Serviced maintenance fields.
 *  @return false if @p value is not a string array, or any element does not
 *          fit / the array exceeds `MAINTENANCE_SERVICE_MAX_PARTS`. */
bool model_copy_maintenance_parts(const UA_Variant *value, char out[MAINTENANCE_SERVICE_MAX_PARTS][MAINTENANCE_SERVICE_PART_TEXT_CAPACITY], uint8_t *count);
/**
 * @name Maintenance workflow method callbacks
 * `UA_MethodCallback` implementations for the maintenance state machine
 * (`Device.Maintenance.PlanMaintenance` / `RescheduleMaintenance` /
 * `StartMaintenance` / `FinishMaintenance`). Each requires Service role
 * (`model_require_method_role`), parses its method-specific input arguments
 * into a `maintenance_service_transaction_t`, and delegates to
 * `model_commit_maintenance_transaction`. Run on `OpcUaTask` inside
 * open62541's Call service.
 * @{
 */
UA_StatusCode model_plan_maintenance(UA_Server *server, const UA_NodeId *sid, void *ctx, const UA_NodeId *mid, void *mc, const UA_NodeId *oid, void *oc, size_t n, const UA_Variant *in, size_t no, UA_Variant *out);
UA_StatusCode model_start_maintenance(UA_Server *server, const UA_NodeId *sid, void *ctx, const UA_NodeId *mid, void *mc, const UA_NodeId *oid, void *oc, size_t n, const UA_Variant *in, size_t no, UA_Variant *out);
UA_StatusCode model_reschedule_maintenance(UA_Server *server,const UA_NodeId *sid,void *ctx,const UA_NodeId *mid,void *mc,const UA_NodeId *oid,void *oc,size_t n,const UA_Variant *in,size_t no,UA_Variant *out);
UA_StatusCode model_finish_maintenance(UA_Server *server,const UA_NodeId *sid,void *ctx,const UA_NodeId *mid,void *mc,const UA_NodeId *oid,void *oc,size_t n,const UA_Variant *in,size_t no,UA_Variant *out);
/** @} */
/**
 * @brief `Device.Maintenance.ResetMaintenanceJournal` method callback
 *        (Admin-only, see `model_is_admin_only_method`): resets the
 *        maintenance journal via `maintenance_service`, taking the shared
 *        adapter critical section when applying the result. Runs on
 *        `OpcUaTask`.
 */
UA_StatusCode model_reset_maintenance_journal( UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/**
 * @brief Appends a `vsnprintf`-formatted fragment to the shared PSRAM
 *        export payload buffer (`g_model_maintenance_export_payload`) at
 *        `*offset`, advancing it on success.
 * @return false if the buffer is exhausted (`MODEL_MAINTENANCE_EXPORT_PAYLOAD_CAPACITY`)
 *         or the formatted text would overflow it; the buffer is left
 *         truncated at the previous offset in that case.
 */
bool model_export_append(size_t *offset, const char *format, ...);
/** @brief Appends @p text as a quoted, backslash/control-character-escaped
 *         JSON string via repeated `model_export_append` calls.
 *  @return false if any underlying append fails (buffer exhausted). */
bool model_export_append_json_string(size_t *offset, const char *text);
/** @brief Appends @p parts (up to @p count entries) as a JSON string array
 *         via `model_export_append_json_string`.
 *  @return false if any underlying append fails. */
bool model_export_append_parts( size_t *offset, const char parts[MAINTENANCE_SERVICE_MAX_PARTS][MAINTENANCE_SERVICE_PART_TEXT_CAPACITY], uint8_t count);
/** @brief Parses an 8-hex-digit continuation-token method input into
 *         @p out_token.
 *  @return false if @p input is not a scalar 8-character hex string. */
bool model_parse_maintenance_export_token(const UA_Variant *input, uint32_t *out_token);
/**
 * @brief Shared failure path for `ExportMaintenanceJournal`: marks
 *        @p continuation inactive (if non-NULL, under
 *        `taskENTER_CRITICAL`/`taskEXIT_CRITICAL`) and audits the rejection.
 * @return @p status unchanged, for direct use as the callback's return value.
 */
UA_StatusCode model_export_maintenance_failure( void *session_context, model_maintenance_export_continuation_t *continuation, UA_StatusCode status);
/**
 * @brief `Device.Maintenance.ExportMaintenanceJournal` method callback:
 *        paginated JSON export of the maintenance journal using the fixed
 *        `model_maintenance_export_continuation_t` table
 *        (`MODEL_MAINTENANCE_EXPORT_CONTINUATION_CAPACITY` slots,
 *        `MODEL_MAINTENANCE_EXPORT_EXPIRY_MS` TTL). First call with no
 *        token starts a new continuation; subsequent calls resume via
 *        `model_parse_maintenance_export_token`.
 * @return `UA_STATUSCODE_BADCONTINUATIONPOINTINVALID` for an unknown/expired
 *         token; otherwise the mapped `maintenance_service_result_t` status.
 * @note Runs on `OpcUaTask`; mutates the continuation table under
 *       `taskENTER_CRITICAL`/`taskEXIT_CRITICAL`.
 */
UA_StatusCode model_export_maintenance_journal( UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `UA_DataValue` setter for a `UA_LocalizedText` built from @p data. */
UA_StatusCode model_set_localized_text_value(UA_DataValue *value, const char *data, UA_StatusCode status);
/** @brief `UA_DataValue` setter for a scalar `UA_DateTime`. */
UA_StatusCode model_set_datetime_value(UA_DataValue *value, UA_DateTime data, UA_StatusCode status);
/** @brief Sets a zero/empty default scalar value on @p attributes matching
 *         @p data_type, used when creating a variable node before its first
 *         real value is available. */
void model_set_default_variant_value(UA_VariableAttributes *attributes, UA_NodeId data_type);
/** @brief `UA_DataValue` setter for a generic channel's current runtime
 *         value, dispatching on `channel->value_type` to the matching typed
 *         setter above. */
UA_StatusCode model_set_channel_runtime_value(UA_DataValue *value, const telemetry_channel_t *channel, UA_StatusCode status);
/** @brief Converts a `UA_DateTime` (100ns ticks since 1601-01-01) to Unix
 *         epoch seconds, saturating/clamping rather than under/overflowing
 *         for out-of-range input. */
uint32_t model_datetime_to_epoch_seconds(UA_DateTime value);
/** @brief Converts one `history_service_channel_sample_t` into a
 *         `UA_DataValue` for a `HistoryReadRaw` response entry, honoring
 *         @p timestamps_to_return (source/server/both/neither). */
void model_fill_history_data_value(const history_service_channel_sample_t *sample, UA_TimestampsToReturn timestamps_to_return, UA_DataValue *value);
/** @brief `UA_HistoryDatabase.clear` implementation; releases any resources
 *         the history database plugin struct itself owns (not the ring
 *         buffer, which `history_service` owns). Called once at server
 *         shutdown. */
void model_history_database_clear(UA_HistoryDatabase *history_database);
/** @brief Releases every active `model_history_continuation_t` slot owned
 *         by @p session_context, called from `model_close_session_secure_wrapper`
 *         so continuation points do not outlive their session. */
void model_history_release_session_points(const void *session_context);
/**
 * @brief `UA_HistoryDatabase.readRaw` implementation, installed as
 *        `config->historyDatabase.readRaw`. Resolves each requested NodeId
 *        to a channel id, validates/allocates a
 *        `model_history_continuation_t` continuation point (bounded table,
 *        `MODEL_HISTORY_CONTINUATION_CAPACITY` slots), clamps
 *        `numValuesPerNode` to `HISTORY_SERVICE_MAX_RETURN_COUNT`, and calls
 *        `history_service_read_channel_samples()` to fill the response —
 *        never touches the channel history ring buffer directly.
 * @note Runs on `OpcUaTask` inside open62541's HistoryRead service. Uses
 *       bounded critical sections per fragment rather than one lock across
 *       the whole scan (v0.4.3 constraint); see
 *       `docs/code/modules/opcua_model.md#alarms--conditions-and-historyread`.
 */
void model_history_read_raw(UA_Server *server, void *history_database_context, const UA_NodeId *session_id, void *session_context, const UA_RequestHeader *request_header, const UA_ReadRawModifiedDetails *history_read_details, UA_TimestampsToReturn timestamps_to_return, UA_Boolean release_continuation_points, size_t nodes_to_read_size, const UA_HistoryReadValueId *nodes_to_read, UA_HistoryReadResponse *response, UA_HistoryData *const *const history_data);
/**
 * @brief Generic `UA_DataSource.read` installed on every variable node this
 *        model creates (except the maintenance-value-source nodes, which use
 *        `model_read_maintenance_value`); dispatches on
 *        `model_node_context_t.kind` to the matching service snapshot.
 * @note Runs on `OpcUaTask` inside open62541's Read service. Never mutates
 *       service state — read-only translation, per
 *       `docs/code/modules/opcua_model.md#generic-readwrite-dispatch`.
 */
UA_StatusCode model_read_value(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *node_id, void *node_context, UA_Boolean include_source_timestamp, const UA_NumericRange *range, UA_DataValue *value);
/** True only for v0.5.2 additive resource-diagnostics NodeIds. Legacy
 * diagnostics deliberately return false so their established role behavior
 * is untouched. */
bool model_node_is_t2_resource_diagnostic(const UA_NodeId *node_id);
/** Refreshes the adapter cache and reconciles dynamic named-task subtrees.
 * Must be called only from OpcUaTask after diag_service_sample(). */
void model_refresh_resource_diagnostics(UA_Server *server);
/**
 * @brief Generic `UA_DataSource.write` installed on every writable variable
 *        node; dispatches on `model_node_context_t.kind`, re-checks the
 *        caller's role via `model_required_role_for_write_kind` (defense in
 *        depth beyond `getUserAccessLevel`), then delegates the actual
 *        mutation to the owning service (`channel_manager_write_value`,
 *        `config_service_set_asset_id`, etc.) — never writes service state
 *        fields directly.
 * @return `UA_STATUSCODE_BADUSERACCESSDENIED` if the role check fails; the
 *         mapped service-call status otherwise.
 * @note Runs on `OpcUaTask` inside open62541's Write service.
 */
UA_StatusCode model_write_value(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *node_id, void *node_context, const UA_NumericRange *range, const UA_DataValue *value);
/** @brief Writes a scalar `UA_Boolean` @p result into @p output if
 *         @p output_size permits (the standard single-output-argument
 *         pattern for boolean-result method callbacks). */
void model_set_boolean_method_output(size_t output_size, UA_Variant *output, UA_Boolean result);
/** @brief Arms `g_pending_actions.reboot_pending` for `model_core_task` to
 *         act on after @p delay_ms (minimum 1 tick); does not reboot
 *         synchronously so the method call can return its result first. */
void model_schedule_reboot(uint32_t delay_ms);
/** @brief Arms `g_pending_actions.identify_blink_active` for
 *         `model_core_task` to drive @p blink_count LED toggles at
 *         @p period_ms; `blink_count == 0` cancels an in-progress sequence. */
void model_schedule_identify_blink(uint8_t blink_count, uint32_t period_ms);
/**
 * @name Maintenance/configuration/security method callbacks
 * `UA_MethodCallback` implementations registered under
 * `Device.Maintenance`/`Device.Security` by the matching
 * `model_add_*_method_node` in `model_address_space.c`. Each: enforces its
 * required role via `model_require_method_role`/`model_require_admin_session`,
 * validates/copies its `input` `UA_Variant` array (matching the
 * `UA_Argument` list the registering function supplied), delegates the
 * actual mutation to exactly one `*_service_*` call, maps the result to a
 * `UA_StatusCode`, and audits the outcome
 * (`model_audit_maintenance_accept`/`model_audit_maintenance_rejection`).
 * All run on `OpcUaTask` inside open62541's Call service; none touch
 * hardware directly. Per-function briefs below note only what is
 * non-obvious from the name.
 * @{
 */
/** @brief `Device.Maintenance.AcknowledgeAllAlarms`: acknowledges every
 *         active channel/device alarm via `alarm_service`/`device_health_service`. */
UA_StatusCode model_acknowledge_alarms(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.Reboot`: schedules a deferred reboot via
 *         `model_schedule_reboot`. */
UA_StatusCode model_request_reboot(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.FactoryReset`: resets configuration to
 *         defaults via `config_reset_service`, then schedules a reboot. */
UA_StatusCode model_factory_reset(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.SyncTimeNow`: triggers an immediate NTP
 *         resync via `time_service`. */
UA_StatusCode model_sync_time_now(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.BlinkIdentify`: schedules the identify-LED
 *         blink sequence via `model_schedule_identify_blink`. */
UA_StatusCode model_blink_identify(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.ApplyConfig`: promotes the staged
 *         configuration to running via `config_service`, without persisting
 *         to Flash; may require a subsequent reboot
 *         (`model_config_requires_server_reboot`). */
UA_StatusCode model_apply_config(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.SaveConfig`: persists the running
 *         configuration to Flash via `config_service`/`config_store_service`. */
UA_StatusCode model_save_config(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.SetMqttPubSubEnabled`: toggles MQTT PubSub in
 *         `config_service`; the change only takes effect for the running
 *         server after a reboot (PubSub is configured once at startup). */
UA_StatusCode model_set_mqtt_pubsub_enabled(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.SetDeviceIdentity`: writes DI identification
 *         fields (asset id, location, etc.) via `config_service`. */
UA_StatusCode model_set_device_identity(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.Configuration.SetLocationPaths`: writes AMB
 *         location-tree paths via `config_service`, then rebuilds the AMB
 *         location subtree (`model_refresh_amb_locations`). */
UA_StatusCode model_set_location_paths(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.SetNetworkConfig`: writes static/DHCP network
 *         settings via `config_service`; typically requires a reboot to
 *         take effect. */
UA_StatusCode model_set_network_config(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @} */
/** @brief Formats one channel definition (@p slot_index, @p channel) as a
 *         JSON object into @p buffer, used by `GetChannelConfig`/`ExportConfig`. */
void model_format_channel_config_json(size_t slot_index, const config_channel_definition_t *channel, char *buffer, size_t buffer_size);
/** @brief Formats the compiled-in driver catalog (`driver_catalog`) as a
 *         JSON array into @p buffer, used by `ListDrivers`. */
void model_format_driver_catalog_json(char *buffer, size_t buffer_size);
/** @brief Formats the compile-time preset catalog (`config_presets`) as a
 *         JSON array into @p buffer, used by `ListConfigPresets`. */
void model_format_config_preset_catalog_json(char *buffer, size_t buffer_size);
/** @brief `Device.Maintenance.ListConfigPresets`: returns the compile-time
 *         runtime-configuration-preset catalog as JSON text
 *         (`model_format_config_preset_catalog_json`). */
UA_StatusCode model_list_config_presets(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.ApplyConfigPreset`: resolves the named preset
 *         via `config_presets_get_by_name()` and applies it through
 *         `config_presets_apply()` -- the existing `config_service`
 *         transaction pipeline; a channel-changing preset always requires
 *         and triggers a controlled restart (`model_config_requires_server_reboot`,
 *         T6), which already rebuilds the full address space, so this
 *         callback does not separately call `model_refresh_amb_locations`. */
UA_StatusCode model_apply_config_preset(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.GetChannelConfig`: returns one channel's
 *         configuration as JSON text
 *         (`model_format_channel_config_json`). */
UA_StatusCode model_get_channel_config(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.ListDrivers`: returns the compiled-in driver
 *         catalog as JSON text (`model_format_driver_catalog_json`). */
UA_StatusCode model_list_drivers(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.GetConfigState`: returns whether a reboot is
 *         pending/config is commissioned as JSON text. */
UA_StatusCode model_get_config_state(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.ExportConfig`: returns the full running
 *         configuration as JSON text via `config_export_service`. */
UA_StatusCode model_export_config(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief Re-points the live `UA_ServerConfig` secure-channel PKI store at
 *         whatever `security_certificate_group_service` currently manages
 *         (e.g. after a trust-list change), if that service is the active
 *         manager for it.
 *  @return `UA_STATUSCODE_GOOD` if @p server is NULL (no-op);
 *          `UA_STATUSCODE_BADINTERNALERROR` if the server has no config. */
UA_StatusCode model_refresh_runtime_trust_backends(UA_Server *server);
/** @brief Post-startup identity bookkeeping: reads the current device
 *        certificate/key and `security_state_service` snapshot, and advances
 *        the active/pending identity-generation counters if a rotation
 *        completed while the server was down. Called once during
 *        `model_core_task` startup. */
void model_finalize_identity_generation_after_startup(void);
/** @brief Formats an identity-import operation report as JSON into
 *         @p buffer, used by the identity-rotation validate/apply method
 *         outputs. */
void model_write_identity_import_report_json(char *buffer, size_t buffer_size, const security_identity_import_service_report_t *report);
/** @brief Formats the device certificate's metadata (thumbprint, validity
 *         window, subject) as JSON into @p buffer. */
void model_write_certificate_metadata_json(char *buffer, size_t buffer_size);
/** @brief Formats the overall certificate/identity state (expiry
 *         classification, generation counters) as JSON into @p buffer. */
void model_write_certificate_state_json(char *buffer, size_t buffer_size);
/** @brief `Device.Maintenance.Security.GetCertificateState`: returns
 *         `model_write_certificate_state_json` as the method's string
 *         output. */
UA_StatusCode model_get_certificate_state(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.Security.ExportCertificateMetadata`: returns
 *         `model_write_certificate_metadata_json` as the method's string
 *         output. */
UA_StatusCode model_export_certificate_metadata(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/**
 * @name Identity-rotation chunked-upload method callbacks
 * `UA_MethodCallback`s implementing the staged device-identity rotation
 * workflow: `Begin` allocates a `security_identity_import_service` staging
 * slot, `Write*Chunk` appends one chunk of certificate/key bytes,
 * `Abort` discards the staged upload, `ValidateStaged` checks the staged
 * material (parseable cert/key, key match) without committing, and
 * `ApplyStaged` atomically swaps it in as the new active device identity
 * (bumping the identity generation counter). All are Admin-only
 * (`Device.Security` prefix, see `model_required_role_for_method`), run on
 * `OpcUaTask`, and delegate parsing/staging/commit to
 * `security_identity_import_service`.
 * @{
 */
UA_StatusCode model_begin_identity_rotation(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_write_identity_rotation_chunk(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_abort_identity_rotation(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_validate_staged_identity(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_apply_staged_identity(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @} */
/** @brief Formats a trust-list-import operation report as JSON into
 *         @p buffer. */
void model_write_trust_import_report_json(char *buffer, size_t buffer_size, const security_trust_import_service_report_t *report);
/** @brief Appends `"<field_name>": <object_json>` into a JSON object being
 *         built in @p buffer (helper for composing nested JSON reports). */
void model_append_json_object_field(char *buffer, size_t buffer_size, const char *field_name, const char *object_json);
/** @brief Formats the current trust-list state (version, entry count,
 *         expiry summary) as JSON into @p buffer. */
void model_write_trust_state_json(char *buffer, size_t buffer_size);
/** @brief `Device.Maintenance.Security.GetTrustState`: returns
 *         `model_write_trust_state_json` as the method's string output. */
UA_StatusCode model_get_trust_state(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.Security.ExportTrustList`: returns the current
 *         trust list (certificates/CRLs) as method output via
 *         `security_certificate_group_service`. */
UA_StatusCode model_export_trust_list(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/**
 * @name Trust-list chunked-import method callbacks
 * Same Begin/Write/Abort/Validate/Apply staged-upload pattern as the
 * identity-rotation family above, but for the trust list (trusted CAs/
 * certificates/CRLs), delegating to `security_trust_import_service`. Apply
 * calls `model_refresh_runtime_trust_backends` afterward so the live
 * `UA_ServerConfig` picks up the new trust list without a reboot.
 * Admin-only, run on `OpcUaTask`.
 * @{
 */
UA_StatusCode model_begin_trust_list_import(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_write_trust_list_chunk(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_abort_trust_list_import(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_validate_imported_trust_list(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_apply_imported_trust_list(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @} */
/**
 * @name Full-configuration chunked-import method callbacks
 * Same Begin/Write/Abort/Validate/Apply staged-upload pattern for a
 * complete device configuration document (superset of individual channel/
 * network/identity settings), delegating to `config_import_service` and
 * `config_validate_service`. Apply may set `g_pending_actions.reboot_pending`
 * if the imported configuration requires a rebuild
 * (`model_config_requires_server_reboot`). Admin-only, run on `OpcUaTask`.
 * @{
 */
UA_StatusCode model_begin_config_import(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_write_config_chunk(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_abort_config_import(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_validate_imported_config(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_apply_imported_config(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @} */
/** @brief Formats the current firmware-update state machine status
 *         (`firmware_update_service`/OTA) as JSON into @p buffer. */
void model_format_firmware_update_status_json(char *buffer, size_t buffer_size);
/** @brief `Device.Maintenance.GetFirmwareUpdateStatus`: returns
 *         `model_format_firmware_update_status_json` as the method's string
 *         output. */
UA_StatusCode model_get_firmware_update_status(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/**
 * @name Firmware-update (OTA) chunked-upload method callbacks
 * Begin/Write/Finalize/Abort staged-upload pattern for a new firmware image,
 * delegating to `firmware_update_service` (which owns the OTA state machine
 * and Flash bank writes via `ota/`/`drivers/flash_operation_coordinator`).
 * `Finalize` validates the completed image (CRC/size) and marks it ready for
 * the bootloader to apply on next boot; it does not itself trigger a reboot.
 * Admin-only, run on `OpcUaTask`; the actual Flash writes happen
 * synchronously within the service call, not on a separate task.
 * @{
 */
UA_StatusCode model_begin_firmware_update(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_write_firmware_chunk(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_finalize_firmware_update(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_abort_firmware_update(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @} */
/** @brief `Device.Maintenance.ClearChannelConfig`: removes one channel's
 *         configuration slot via `config_service`, requiring a reboot to
 *         take effect on the running address space. */
UA_StatusCode model_clear_channel_config(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.SetChannelConfig`: writes one channel's driver/
 *         settings configuration via `config_service`, validated against
 *         `driver_catalog`. */
UA_StatusCode model_set_channel_config(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.SelfTest`: runs a device self-test via
 *         `diag_service`, returning a pass/fail-style result. */
UA_StatusCode model_self_test(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.ClearDiagnostics`: resets sticky diagnostic
 *         counters (fault code, min-ever-free-heap, etc.) via `diag_service`. */
UA_StatusCode model_clear_diagnostics(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Security.SetTrustList`: one-shot (non-chunked) trust-list
 *         replacement for small trust lists, via
 *         `security_certificate_group_service`. */
UA_StatusCode model_set_trust_list(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Security.EnterCommissioningMode`: relaxes strict identity/
 *         trust requirements via `security_state_service`, gated by
 *         `model_is_commissioned`/admin role. */
UA_StatusCode model_enter_commissioning_mode(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Security.ExitCommissioningMode`: re-enforces strict mode
 *         via `security_state_service`, typically requiring identity/
 *         network/trust completeness first. */
UA_StatusCode model_exit_commissioning_mode(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/**
 * @name User-management method callbacks
 * Admin-only (`model_is_admin_only_method`) `UA_MethodCallback`s delegating
 * directly to `security_service`'s user table. Run on `OpcUaTask`.
 * @{
 */
/** @brief `Device.Maintenance.AddUser`: creates a new user/role entry. */
UA_StatusCode model_add_user(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.ChangePassword`: sets a user's password hash. */
UA_StatusCode model_change_password(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.SetRole`: changes a user's `security_user_role_t`. */
UA_StatusCode model_set_role(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.DisableUser`: disables a user account without deleting it. */
UA_StatusCode model_disable_user(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief `Device.Maintenance.DeleteUser`: removes a user account entirely. */
UA_StatusCode model_delete_user(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @} */
/**
 * @name Low-level address-space node builders
 * Thin wrappers over `UA_Server_add{Object,Variable,...}Node` shared across
 * every subsystem's node-building code. Allocation-free beyond what
 * open62541 itself allocates for the node; all take a plain
 * `UA_NODEID_STRING(1, node_id)` unless noted. Not documented individually
 * beyond a one-line brief since each is a direct, obvious wrapper.
 * @{
 */
/** @brief Adds an Object node with an explicit reference/type-definition
 *         (the general case; most callers use `model_add_folder` instead). */
UA_StatusCode model_add_object(UA_Server *server, const char *node_id, const char *browse_name, const char *display_name, UA_NodeId parent_node_id, UA_NodeId reference_type_id, UA_NodeId type_definition_id);
/** @brief Adds a `FolderType` Object node under @p parent_node_id. */
UA_StatusCode model_add_folder(UA_Server *server, const char *node_id, const char *display_name, UA_NodeId parent_node_id, UA_NodeId reference_type_id);
/** @brief Adds an AMB-namespace folder node (browse name/description in the
 *         AMB namespace rather than namespace 1). */
UA_StatusCode model_add_amb_folder(UA_Server *server, const char *node_id, const char *browse_name, const char *description, UA_NodeId parent_node_id);
/** @brief Adds a read-only variable node with a fixed initial @p value,
 *         organized (not componentized) under @p parent_node_id — used for
 *         static AMB capability/requirement values that never change at
 *         runtime. */
UA_StatusCode model_add_static_organized_variable(UA_Server *server, const char *node_id, const char *browse_name, const char *description, UA_NodeId parent_node_id, const void *value, const UA_DataType *data_type);
/** @} */
/** @brief Builds the fixed AMB `Capabilities`/`Requirements` static variable
 *         nodes under @p device_id via repeated
 *         `model_add_static_organized_variable` calls. */
UA_StatusCode model_build_amb_capabilities_and_requirements(UA_Server *server, UA_NodeId device_id);
/** @brief Formats one compiled driver-catalog entry's compatibility
 *         descriptor (id, name, supported value types, capability
 *         bitmask, and a representative resource-claim shape via
 *         `driver_catalog_get_resource_claims()`) as a JSON object into
 *         @p buffer, used by `model_build_compatibility_info` to populate
 *         each `Device.Compatibility.SupportedDrivers.<name>` node. */
void model_format_driver_compatibility_json(const driver_catalog_entry_t *driver, char *buffer, size_t buffer_size);
/** @brief Builds the `Device.Compatibility` subtree: firmware profile
 *         (compile-time driver-pack identity), hardware revision,
 *         configuration-schema version, and one
 *         `Device.Compatibility.SupportedDrivers.<name>` static variable
 *         node per compiled driver (`driver_catalog_count()`/
 *         `driver_catalog_get_by_index()`), each holding that driver's
 *         capabilities/resource descriptor
 *         (`model_format_driver_compatibility_json`). Read-only,
 *         compile-time data -- built once, not gated on
 *         `g_model_config.enable_alarm_methods` like the
 *         `Device.Maintenance.*` methods are. */
UA_StatusCode model_build_compatibility_info(UA_Server *server, UA_NodeId device_id, const device_profile_config_t *device_config);
/** @brief Adds the AMB `NodeVersion` property to a location node, required
 *         so clients can detect when a location subtree has changed. */
UA_StatusCode model_add_location_node_version(UA_Server *server, const char *node_id, UA_NodeId parent_id);
/** @brief Bumps a location node's `NodeVersion` property value, signaling
 *         subtree change to clients monitoring it. */
UA_StatusCode model_write_location_node_version(UA_Server *server, const char *node_id);
/**
 * @brief Deletes every location node under @p node_id_prefix, deepest level
 *        first (up to `CONFIG_SERVICE_LOCATION_MAX_DEPTH`), used before
 *        rebuilding the AMB location tree from a new configuration.
 */
UA_StatusCode model_delete_location_tree(UA_Server *server, const char *node_id_prefix);
/** @brief Creates every intermediate location node along @p path
 *         (`/`-delimited) under @p device_id, linking each level via
 *         @p contains_reference_id from @p entry_point_id. */
UA_StatusCode model_build_location_path(UA_Server *server, const char *path, const char *node_id_prefix, UA_UInt32 entry_point_id, UA_UInt32 contains_reference_id, UA_NodeId device_id);
/**
 * @brief Tears down (`model_delete_location_tree`) and rebuilds the AMB
 *        location subtree from @p config's location paths, used by
 *        `SetLocationPaths` to apply a change without a full server
 *        restart.
 * @return `UA_STATUSCODE_BADINVALIDARGUMENT` if @p server/@p config is NULL
 *         or the AMB namespace was not resolved.
 */
UA_StatusCode model_refresh_amb_locations(UA_Server *server, const device_profile_config_t *config);
/** @brief Initial build of the AMB location subtree at address-space build
 *         time (calls `model_build_location_path` per configured location). */
UA_StatusCode model_build_amb_locations(UA_Server *server, const device_profile_config_t *config);
/** @brief Adds one DI `FunctionalGroupType` folder (e.g. Identification,
 *         Configuration) under @p parent_node_id. */
UA_StatusCode model_add_di_functional_group(UA_Server *server, const char *node_id, const char *browse_name, UA_NodeId parent_node_id);
/** @brief Adds one DI identification property variable node, optionally
 *         @p writable, tagging its `model_node_context_t *context` for the
 *         generic read/write dispatch. */
UA_StatusCode model_add_di_identification_property( UA_Server *server, const char *node_id, const char *browse_name, UA_NodeId parent_node_id, UA_NodeId data_type, model_node_context_t *context, UA_Boolean writable);
/**
 * @brief Matches one bracketed character class (e.g. `[abc]`/`[!abc]`) at
 *        the start of @p pattern against @p value, the primitive behind
 *        `model_alias_pattern_matches`'s glob engine.
 * @param consumed Set to the number of pattern bytes the class consumed
 *        (including the brackets).
 * @param valid Set to false if the class syntax itself is malformed.
 * @return true if @p value matches the class.
 */
bool model_alias_match_class(const UA_Byte *pattern, size_t pattern_length, UA_Byte value, size_t *consumed, bool *valid);
/**
 * @brief Glob-style asset-alias pattern matcher (`*`, `?`, and `[...]`
 *        character classes) implemented as a dynamic-programming boolean
 *        reachability scan over @p value (two alternating bitmap rows,
 *        bounded by `CONFIG_SERVICE_ASSET_ID_MAX_LEN`), used by
 *        `FindAssetAlias` to resolve a client-supplied pattern to a
 *        configured asset id/alias.
 * @param valid Set to false if @p pattern contains invalid glob syntax.
 * @return true if @p value matches @p pattern.
 */
bool model_alias_pattern_matches(const UA_String *pattern, const char *value, bool *valid);
/** @brief Syntax-only validation of an asset-alias glob pattern (balanced
 *         `[...]` classes, no dangling escapes), without matching any value. */
bool model_alias_pattern_is_valid(const UA_String *pattern);
/**
 * @brief Callback for the standard namespace-0 `AliasesType.FindAlias`
 *        method and the AMB companion-spec `FindAlias` methods on the
 *        Assets/ProductInstanceUri/AssetId alias folders (registered four
 *        times onto different Method NodeIds by `model_build_asset_discovery`):
 *        resolves a glob pattern against configured asset aliases via
 *        `model_alias_pattern_matches`.
 */
UA_StatusCode model_find_asset_alias(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
/** @brief Bumps the asset-alias folder's `NodeVersion` property, signaling
 *         to clients that the alias set changed. */
UA_StatusCode model_write_asset_alias_node_version(UA_Server *server, UA_UInt32 node_id);
/** @brief Updates `g_model_alias_last_change` and writes it to the matching
 *         diagnostic node after an alias-affecting change. */
UA_StatusCode model_write_alias_last_change(UA_Server *server);
/** @brief Updates the `AssetAliases.AssetId` alias target to @p asset_id
 *         (or `MODEL_ASSET_ALIAS_UNASSIGNED` if empty) after a
 *         `SetDeviceIdentity` call changes the asset id. */
UA_StatusCode model_update_asset_id_alias(UA_Server *server, const char *asset_id);
/** @brief Adds one static asset-alias variable node (@p alias_name ->
 *         current value) under @p device_id. */
UA_StatusCode model_add_asset_alias(UA_Server *server, const char *node_id, const char *alias_name, UA_UInt32 category_id, UA_NodeId device_id);
/** @brief Builds the `AssetAliases` folder and its ProductInstanceUri/
 *         AssetId alias nodes at address-space build time from @p config. */
UA_StatusCode model_build_asset_discovery(UA_Server *server, UA_NodeId device_id, const device_profile_config_t *config);
/** @brief Builds the DeviceHealth diagnostic variable nodes (current health
 *         state and related fields) under @p device_id. */
UA_StatusCode model_build_device_health_nodes(UA_Server *server, UA_NodeId device_id);
/** @brief Adds a read-only variable node with explicit @p access_level and
 *         @p historizing flag, tagging @p context for generic read dispatch
 *         (the general case behind `model_add_readonly_variable`). */
UA_StatusCode model_add_readonly_variable_ex(UA_Server *server, const char *node_id, const char *display_name, UA_NodeId parent_node_id, UA_NodeId data_type, model_node_context_t *context, UA_Byte access_level, UA_Boolean historizing);
/** @brief `model_add_readonly_variable_ex` with default read-only access
 *         level and historizing disabled — the common case for most
 *         diagnostic/status nodes. */
UA_StatusCode model_add_readonly_variable(UA_Server *server, const char *node_id, const char *display_name, UA_NodeId parent_node_id, UA_NodeId data_type, model_node_context_t *context);
/** @brief Adds a variable node with read+write access, tagging @p context
 *         for generic read/write dispatch — used for configuration/settable
 *         nodes. */
UA_StatusCode model_add_writable_variable(UA_Server *server, const char *node_id, const char *display_name, UA_NodeId parent_node_id, UA_NodeId data_type, model_node_context_t *context);
/**
 * @name Method Argument builders
 * Each fills one `UA_Argument` (name/description/data type/scalar rank) for
 * the named UA type, used when assembling a method node's `inputArguments`
 * array before registration.
 * @{
 */
void model_fill_string_argument(UA_Argument *argument, const char *name, const char *description);
void model_fill_bytestring_argument(UA_Argument *argument, const char *name, const char *description);
void model_fill_datetime_argument(UA_Argument *argument, const char *name, const char *description);
void model_fill_uint32_argument(UA_Argument *argument, const char *name, const char *description);
void model_fill_double_argument(UA_Argument *argument, const char *name, const char *description);
void model_fill_boolean_argument(UA_Argument *argument, const char *name, const char *description);
/** @} */
/** @brief Writes a scalar `UA_String` built from @p result into @p output
 *         (the string-result counterpart of `model_set_boolean_method_output`). */
void model_set_string_method_output(size_t output_size, UA_Variant *output, const char *result);
/** @brief Writes the maintenance-export payload view
 *         (`g_model_maintenance_export_output_view`, length-limited to
 *         @p payload_length) into @p output as a `UA_String` result. */
UA_StatusCode model_set_maintenance_export_method_output(size_t output_size,
                                                         UA_Variant *output,
                                                         size_t payload_length);
/** @brief Writes a `UA_ByteString` @p result into @p output (the
 *         byte-string-result counterpart of `model_set_boolean_method_output`). */
void model_set_bytestring_method_output(size_t output_size, UA_Variant *output, const UA_ByteString *result);
/**
 * @name Method-node registration helpers
 * Each registers one `UA_MethodCallback` as a Method node under
 * @p maintenance_folder_id with @p input_arguments and a single output
 * argument of the named result type, used by the ~90
 * `model_add_*_method_node` functions below. Not the callbacks themselves —
 * see the method-callback families documented earlier in this header for
 * what each registered method actually does.
 * @{
 */
UA_StatusCode model_add_method_node_with_boolean_result(UA_Server *server, UA_NodeId maintenance_folder_id, const char *node_id, const char *method_name, const char *description, UA_MethodCallback callback, size_t input_size, UA_Argument *input_arguments);
UA_StatusCode model_add_method_node_with_string_result(UA_Server *server, UA_NodeId maintenance_folder_id, const char *node_id, const char *method_name, const char *description, UA_MethodCallback callback, size_t input_size, UA_Argument *input_arguments);
UA_StatusCode model_add_method_node_with_bytestring_result(UA_Server *server, UA_NodeId maintenance_folder_id, const char *node_id, const char *method_name, const char *description, UA_MethodCallback callback, size_t input_size, UA_Argument *input_arguments);
/** @} */
/** @brief `model_add_method_node_with_boolean_result` with no input
 *         arguments — the common case for parameterless Boolean-result
 *         maintenance methods. */
UA_StatusCode model_add_boolean_method_node(UA_Server *server, UA_NodeId maintenance_folder_id, const char *node_id, const char *method_name, const char *description, UA_MethodCallback callback);
/**
 * @name Method-node registration functions (address-space build time)
 * Each registers exactly one maintenance/security/user-management method
 * node under @p maintenance_folder_id / @p security_folder_id, wiring its
 * `UA_Argument` input list and the matching `model_*` callback documented
 * earlier in this header (name correspondence is direct: `model_add_X_method_node`
 * registers `model_X`). Called once each, in sequence, from
 * `model_add_maintenance_workflow_methods` / `model_build_address_space`
 * during server startup on `OpcUaTask`; not documented individually beyond
 * this shared contract. `model_add_task_stack_high_water_node`,
 * `model_add_channel_nodes`, and `model_add_legacy_temperature_nodes` are
 * the non-method exceptions in this block — see their own briefs below.
 * @{
 */
UA_StatusCode model_add_alarm_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_reboot_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_factory_reset_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_sync_time_now_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_blink_identify_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_apply_config_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_save_config_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_set_mqtt_pubsub_enabled_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_set_device_identity_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_set_location_paths_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
/** @brief Registers every maintenance workflow method
 *         (Plan/Start/Reschedule/Finish + ResetMaintenanceJournal +
 *         ExportMaintenanceJournal) in one call, rather than one function
 *         per method like its siblings. */
UA_StatusCode model_add_maintenance_workflow_methods(UA_Server *server, UA_NodeId maintenance_id);
UA_StatusCode model_add_set_network_config_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_set_channel_config_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_get_channel_config_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_clear_channel_config_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_list_drivers_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_list_config_presets_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_apply_config_preset_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_get_config_state_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_export_config_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_begin_config_import_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_write_config_chunk_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_abort_config_import_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_validate_imported_config_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_apply_imported_config_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_get_trust_state_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_export_trust_list_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_begin_trust_list_import_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_write_trust_list_chunk_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_abort_trust_list_import_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_validate_imported_trust_list_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_apply_imported_trust_list_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_get_certificate_state_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_export_certificate_metadata_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_begin_identity_rotation_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_write_identity_rotation_chunk_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_abort_identity_rotation_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_validate_staged_identity_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_apply_staged_identity_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_get_firmware_update_status_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_begin_firmware_update_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_write_firmware_chunk_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_finalize_firmware_update_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_abort_firmware_update_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_self_test_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_clear_diagnostics_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_set_trust_list_method_node(UA_Server *server, UA_NodeId security_folder_id);
UA_StatusCode model_add_enter_commissioning_method_node(UA_Server *server, UA_NodeId security_folder_id);
UA_StatusCode model_add_exit_commissioning_method_node(UA_Server *server, UA_NodeId security_folder_id);
UA_StatusCode model_add_add_user_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_change_password_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_set_role_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_disable_user_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_delete_user_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
/** @} */
/** @brief Adds one per-task stack-high-water-mark diagnostic node
 *         (`Device.Diagnostics.Task.*`) for @p registered_task_name,
 *         tagging @p task_context so the generic read dispatch can look up
 *         `uxTaskGetStackHighWaterMark` for that FreeRTOS task by name. */
UA_StatusCode model_add_task_stack_high_water_node(UA_Server *server, UA_NodeId task_folder_id, const char *task_object_id, const char *task_display_name, const char *registered_task_name, model_node_context_t *task_context);
/**
 * @brief Builds the full v0.4.2 generic channel node set (Value,
 *        Description, ChannelKind, DriverId, SourceIndex/Flags,
 *        SamplePeriodMs, PublishPeriodMs, Historizing, Writable,
 *        EngineeringUnits, EURange — 13 fields) for one channel under
 *        @p channels_folder_id, using a fixed stack array for every NodeId
 *        string (no heap allocation).
 * @note On partial failure, only the nodes already created for *this*
 *       channel are left for the caller's rollback
 *       (`model_rollback_channel_model`, not declared in this header) to
 *       clean up — see `docs/code/modules/opcua_model.md#channel-model-generation`.
 */
UA_StatusCode model_add_channel_nodes(UA_Server *server, UA_NodeId channels_folder_id, size_t channel_index, const telemetry_channel_t *channel);
/** @brief Builds the legacy single-temperature-channel node set (back-compat
 *         NodeIds predating the generic channel model) alongside the
 *         generic nodes, for channels that need both. */
UA_StatusCode model_add_legacy_temperature_nodes(UA_Server *server, UA_NodeId channels_folder_id, size_t channel_index, const telemetry_channel_t *channel);
/**
 * @brief Top-level address-space build orchestrator, called once from
 *        `model_core_task` after `UA_Server_newWithConfig` succeeds: builds
 *        the `Device` object tree (Identification, Channels, Diagnostics,
 *        Network, Time, Security, Configuration, Maintenance, Messaging),
 *        every channel's nodes, DI/AMB structures, alarm conditions, and
 *        every method node.
 * @note Captures `g_model_address_space_config_snapshot` from the config
 *       used to build, for later reboot-required comparisons
 *       (`model_is_reboot_required`).
 */
UA_StatusCode model_build_address_space(UA_Server *server);
/** @brief Redeclared here (identical to `opcua_model/opcua_model.h`) so
 *         internal `.c` files including only this header still see the
 *         public entry point's prototype. See the public header for the
 *         full contract. */
bool opcua_model_init(const opcua_model_config_t *config);
/** @brief True if @p certificate's bytes start with the PEM
 *         `"-----BEGIN CERTIFICATE-----"` header (vs. raw DER). */
bool model_certificate_view_is_pem(const security_service_blob_view_t *certificate);
/** @brief Clears any previously-set certificate/private-key/trust-list
 *         `UA_ByteString`s on @p config before (re)configuring security,
 *         avoiding a stale identity surviving a reconfigure. */
void model_clear_runtime_security_config(UA_ServerConfig *config);
/** @brief Frees and clears @p config's `serverUrls` array before rebuilding it. */
void model_clear_server_urls(UA_ServerConfig *config);
/** @brief Sets @p config's single server URL from @p hostname/@p port
 *         (`opc.tcp://<hostname>:<port>`). */
UA_StatusCode model_configure_server_url(UA_ServerConfig *config, const char *hostname, uint16_t port);
/** @brief Sets @p config's ApplicationUri, using
 *         `MODEL_BOOTSTRAP_APPLICATION_URI` or a configured product
 *         instance URI. */
UA_StatusCode model_configure_application_uri(UA_ServerConfig *config);
/**
 * @brief If the device has no identity yet, or has an identity but it is
 *        still in bootstrap PEM form (`model_certificate_view_is_pem`),
 *        seeds a fixed-in-firmware bootstrap self-signed certificate/key via
 *        `security_service_set_device_identity` so the server always has
 *        *some* usable identity to start with, pending a real
 *        identity-rotation.
 * @note This is the only path that installs `g_bootstrap_device_certificate`;
 *       it is explicitly not a production-quality identity.
 */
UA_StatusCode model_seed_bootstrap_identity_if_needed(void);
/**
 * @brief Installs the custom `UA_AccessControl` plugin (login callback,
 *        wrapper functions) onto @p config, replacing whatever
 *        `UA_AccessControl_default` set up, after first saving the
 *        defaults into `g_default_*`.
 * @return `UA_STATUSCODE_BADINVALIDARGUMENT` if @p config is NULL.
 */
UA_StatusCode model_configure_secure_access_control(UA_ServerConfig *config);
/** @brief Filters @p config's endpoint list down to only
 *         `SecurityPolicy#None` and Basic256Sha256/Sign&Encrypt endpoints,
 *         freeing the discarded `UA_EndpointDescription` entries. */
UA_StatusCode model_keep_only_none_and_signandencrypt_endpoints(UA_ServerConfig *config);
/** @brief Filters @p config's endpoint list down to only
 *         Basic256Sha256/Sign&Encrypt endpoints (strict mode, no
 *         `SecurityPolicy#None`), freeing the discarded entries. */
UA_StatusCode model_keep_only_signandencrypt_endpoints(UA_ServerConfig *config);
/** @brief Restricts every insecure (`SecurityPolicy#None`) endpoint in
 *         @p config to anonymous-only authentication, preventing
 *         username/password credentials from being sent in the clear. */
UA_StatusCode model_limit_insecure_endpoints_to_anonymous(UA_ServerConfig *config);
/**
 * @brief Top-level security configuration orchestrator, called once during
 *        `model_core_task` startup: loads the device certificate/key/trust
 *        list from `security_service`, installs them on @p config, and
 *        selects the endpoint filtering policy
 *        (`model_keep_only_none_and_signandencrypt_endpoints` /
 *        `model_keep_only_signandencrypt_endpoints` /
 *        `model_limit_insecure_endpoints_to_anonymous`) based on
 *        commissioning mode and trust-list presence.
 * @note Falls back toward `SecurityPolicy#None` if strict-mode
 *       prerequisites are not met (no trust anchors yet), which is why
 *       commissioning mode exists as an explicit, audited state — see
 *       `docs/code/modules/opcua_model.md#access-control`.
 */
UA_StatusCode model_configure_secure_server(UA_ServerConfig *config);
/**
 * @brief Points open62541's global malloc/calloc/realloc/free singletons at
 *        the PSRAM allocator (`ua_port_malloc` family) so every open62541
 *        internal allocation comes from PSRAM instead of the FreeRTOS heap.
 * @note Only takes effect when built with `UA_ENABLE_MALLOC_SINGLETON`;
 *       otherwise a no-op. Must be called before `UA_Server_newWithConfig`.
 *       Global process-wide effect, not scoped to one `UA_Server` instance.
 */
void model_bind_open62541_allocators(void);
/** @brief Redeclared here (identical to `opcua_model/opcua_model.h`) so
 *         internal `.c` files including only this header still see the
 *         `OpcUaTask` entry point's prototype. See the public header for
 *         the full contract. */
void opcua_model_task(void *argument);

#endif
