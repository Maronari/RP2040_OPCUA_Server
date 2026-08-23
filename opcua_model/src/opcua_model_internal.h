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
#include "services/channel_manager.h"
#include "services/config_service.h"
#include "services/config_export_service.h"
#include "services/config_import_service.h"
#include "services/config_reset_service.h"
#include <services/config_validate_service.h>
#include "services/device_channels.h"
#include "services/diag_service.h"
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
UA_ConnectionManager *UA_ConnectionManager_new_MQTT(const UA_String eventSourceName);
#endif

#define MODEL_BOOTSTRAP_APPLICATION_URI "urn:open62541.server.application"
#define MODEL_CHANNEL_NAMESPACE_URI "urn:maronari:rp2-industrial-opcua:channel-model"
#define MODEL_CHANNEL_NODE_ID_MAX_LEN 128u

#define fflush(_stream) stdio_flush()

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

typedef struct
{
    model_node_kind_t kind;
    const char *channel_id;
    /* Node contexts outlive registry snapshots. Keep their channel key in
     * model-owned bounded storage instead of retaining a snapshot pointer. */
    char channel_id_storage[TELEMETRY_CHANNEL_ID_MAX_LEN];
    const char *task_name;
} model_node_context_t;

typedef struct
{
    bool reboot_pending;
    TickType_t reboot_due_tick;
    bool identify_blink_active;
    TickType_t identify_next_tick;
    uint8_t identify_toggles_remaining;
    bool identify_led_on;
} model_pending_actions_t;

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

typedef enum
{
    MODEL_EVENT_LAST_KIND_GENERIC = 0,
    MODEL_EVENT_LAST_KIND_CONFIG = 1,
    MODEL_EVENT_LAST_KIND_NETWORK = 2,
    MODEL_EVENT_LAST_KIND_FAULT = 3
} model_event_last_kind_t;

typedef struct
{
    char last_event_id[33];
    char last_config_change[160];
    char last_fault[160];
    char last_network_change[160];
} model_last_event_state_t;

#ifdef UA_ENABLE_PUBSUB
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

typedef struct
{
    char event[32];
    char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
    char subject[64];
    char details[160];
    bool success;
} model_pending_audit_event_t;

typedef struct
{
    const char *changed_section;
    const char *changed_key;
    const char *actor;
} model_config_event_fields_t;

typedef struct
{
    const char *old_ip;
    const char *new_ip;
    bool dhcp_enabled;
    bool link_up;
} model_network_event_fields_t;

typedef struct
{
    const char *sensor_id;
    uint32_t fault_code;
    const char *quality;
    float measured_value;
} model_sensor_fault_event_fields_t;

typedef struct
{
    const char *operation;
    uint32_t result_code;
    const char *actor;
} model_maintenance_event_fields_t;

/* AMB NameNodeIdDataType (ns=<AMB>;i=3003).  The optional NodeId is null for
 * v0.3.2 because the bounded service stores names, not borrowed UA nodes. */
typedef struct
{
    UA_LocalizedText name;
    UA_NodeId node_id;
} model_amb_name_node_id_t;

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

typedef struct
{
    model_maintenance_value_kind_t kind;
} model_maintenance_value_context_t;

typedef struct
{
    const char *version;
    const char *build_time;
    const char *git_hash;
} model_firmware_event_fields_t;

typedef struct
{
    const char *actor;
    const char *subject;
    const char *details;
    bool result;
} model_audit_event_fields_t;

typedef struct
{
    const char *changed_section;
    const char *changed_key;
} model_audit_config_event_fields_t;

typedef struct
{
    const char *operation;
} model_audit_security_event_fields_t;

typedef struct
{
    model_audit_event_fields_t audit;
    model_audit_config_event_fields_t config;
} model_audit_config_emit_fields_t;

typedef struct
{
    model_audit_event_fields_t audit;
    model_audit_security_event_fields_t security;
} model_audit_security_emit_fields_t;

typedef UA_StatusCode (*model_event_fields_writer_t)(UA_KeyValueMap *event_fields,
                                                     const void *fields_context);

typedef enum
{
    MODEL_CERTIFICATE_EXPIRY_STATE_MISSING = 0,
    MODEL_CERTIFICATE_EXPIRY_STATE_UNKNOWN = 1,
    MODEL_CERTIFICATE_EXPIRY_STATE_VALID = 2,
    MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_MEDIUM = 3,
    MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_HIGH = 4,
    MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRED = 5,
} model_certificate_expiry_state_t;

typedef struct
{
    model_certificate_expiry_state_t state;
    uint32_t days_until_expiry;
    uint32_t not_after_epoch_s;
    bool time_check_performed;
    char thumbprint[SECURITY_SERVICE_CERT_THUMBPRINT_LEN + 1u];
} model_remote_certificate_expiry_info_t;

#if defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
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

typedef UA_StatusCode (*model_activate_session_fn)(UA_Server *server,
                                                   UA_AccessControl *ac,
                                                   const UA_EndpointDescription *endpoint_description,
                                                   const UA_ByteString *secure_channel_remote_certificate,
                                                   const UA_NodeId *session_id,
                                                   const UA_ExtensionObject *user_identity_token,
                                                   void **session_context);

typedef void (*model_close_session_fn)(UA_Server *server,
                                       UA_AccessControl *ac,
                                       const UA_NodeId *session_id,
                                       void *session_context);

typedef UA_Boolean (*model_get_user_executable_fn)(UA_Server *server,
                                                   UA_AccessControl *ac,
                                                   const UA_NodeId *session_id,
                                                   void *session_context,
                                                   const UA_NodeId *method_id,
                                                   void *method_context);

typedef UA_Boolean (*model_get_user_executable_on_object_fn)(UA_Server *server,
                                                             UA_AccessControl *ac,
                                                             const UA_NodeId *session_id,
                                                             void *session_context,
                                                             const UA_NodeId *method_id,
                                                             void *method_context,
                                                             const UA_NodeId *object_id,
                                                             void *object_context);

typedef UA_Byte (*model_get_user_access_level_fn)(UA_Server *server,
                                                  UA_AccessControl *ac,
                                                  const UA_NodeId *session_id,
                                                  void *session_context,
                                                  const UA_NodeId *node_id,
                                                  void *node_context);

typedef UA_Boolean (*model_allow_browse_node_fn)(UA_Server *server,
                                                 UA_AccessControl *ac,
                                                 const UA_NodeId *session_id,
                                                 void *session_context,
                                                 const UA_NodeId *node_id,
                                                 void *node_context);

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
typedef struct { bool active; uint32_t token; TickType_t expires_at; maintenance_service_export_cursor_t cursor; } model_maintenance_export_continuation_t;
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
typedef struct
{
    UA_KeyValueMap fields;
    UA_DateTime event_time;
    UA_String source_name;
    UA_LocalizedText message;
    model_maintenance_event_fields_t maintenance_fields;
} model_prepared_maintenance_event_t;
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
opcua_model_context_t *model_context_get(void);
void model_context_set_server_handles(UA_Server *server, UA_ServerConfig *config);
void model_context_delete_server(UA_Server *server);
const char *model_quality_to_string(service_quality_t quality);
const char *model_alarm_state_to_string(telemetry_alarm_state_t alarm_state);
const char *model_security_mode_to_string(void);
const char *model_certificate_expiry_state_to_string(model_certificate_expiry_state_t state);
const char *model_security_commissioning_state_to_string(void);
const char *model_session_actor_name(void *session_context, char *buffer, size_t buffer_size);
const char *model_session_role_to_string(const void *session_context);
const UA_ByteString *model_session_get_username_bytes(const void *session_context);
const char *model_log_level_name(UA_LogLevel level);
const char *model_maintenance_state_text(maintenance_service_state_t state);
#ifdef UA_ENABLE_PUBSUB
const char *model_pubsub_component_type_to_string(UA_PubSubComponentType type);
const char *model_pubsub_state_to_string(UA_PubSubState state);
#endif
bool model_core_init(const opcua_model_config_t *config);
void model_core_task(void *argument);
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
void model_ua_logger_log(void *context, UA_LogLevel level, UA_LogCategory category, const char *msg, va_list args);
void log_opcua_heap(const char *stage);
void model_log_node_id_compact(const UA_NodeId *node_id);
void model_monitored_item_register_callback(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *node_id, void *node_context, UA_UInt32 attribute_id, UA_Boolean removed);
void model_server_notification_callback(UA_Server *server, UA_ApplicationNotificationType type, const UA_KeyValueMap payload);
bool model_session_is_secure(const void *session_context);
bool model_nodeid_string_has_prefix(const UA_NodeId *node_id, const char *prefix);
bool model_session_is_anonymous(const void *session_context);
bool model_node_is_anonymous_hidden(const UA_NodeId *node_id);
bool model_node_is_maintenance_method(const UA_NodeId *method_id);
UA_Byte model_strip_anonymous_write_access(UA_Byte access_level);
bool model_copy_session_username(const void *session_context, char *buffer, size_t buffer_size);
void model_free_session_username(void *base_context);
bool model_session_has_role(const void *session_context, security_user_role_t required_role);
bool model_session_get_role(const void *session_context, security_user_role_t *out_role);
void model_audit_session_event(void *session_context, const char *event, const char *subject, bool success, const char *details);
bool model_maintenance_rejected_audit_admit(void *session_context, UA_StatusCode reason, uint16_t *out_suppressed, UA_StatusCode *out_last_reason);
void model_audit_maintenance_rejection(void *session_context, const char *method, UA_StatusCode reason);
void model_audit_maintenance_accept(void *session_context, const char *method);
void model_audit_maintenance_state_change(void *session_context, const char *method, const maintenance_service_snapshot_t *snapshot);
UA_StatusCode model_require_session_role(void *session_context, security_user_role_t required_role, size_t output_size, UA_Variant *output);
UA_StatusCode model_require_admin_session(void *session_context, size_t output_size, UA_Variant *output);
security_user_role_t model_required_role_for_method(const UA_NodeId *method_id);
security_user_role_t model_required_role_for_write_kind(model_node_kind_t kind);
UA_StatusCode model_require_method_role(void *session_context, const UA_NodeId *method_id, size_t output_size, UA_Variant *output);
bool model_is_admin_only_method(const UA_NodeId *method_id);
bool model_copy_input_string(const UA_Variant *value, char *buffer, size_t buffer_size);
bool model_get_input_ua_string(const UA_Variant *value, const UA_String **out_string);
bool model_get_input_ua_bytestring(const UA_Variant *value, const UA_ByteString **out_bytes);
bool model_has_config_text(const char *value);
void model_format_mac_string(const uint8_t mac[6], char *buffer, size_t buffer_size);
bool model_parse_ipv4_string(const char *text, uint8_t out_ipv4[4]);
bool model_parse_mac_string(const char *text, uint8_t out_mac[6]);
bool model_identity_is_complete(const device_profile_config_t *config);
bool model_network_config_is_complete(const device_profile_config_t *config);
bool model_has_trust_anchors(void);
bool model_is_commissioned(void);
UA_StatusCode model_login_callback(const UA_String *user_name, const UA_ByteString *password, size_t username_password_login_size, const UA_UsernamePasswordLogin *username_password_login, void **session_context, void *login_context);
UA_StatusCode model_activate_session_secure_wrapper(UA_Server *server, UA_AccessControl *ac, const UA_EndpointDescription *endpoint_description, const UA_ByteString *secure_channel_remote_certificate, const UA_NodeId *session_id, const UA_ExtensionObject *user_identity_token, void **session_context);
void model_close_session_secure_wrapper(UA_Server *server, UA_AccessControl *ac, const UA_NodeId *session_id, void *session_context);
UA_Boolean model_get_user_executable_wrapper(UA_Server *server, UA_AccessControl *ac, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context);
UA_Boolean model_get_user_executable_on_object_wrapper(UA_Server *server, UA_AccessControl *ac, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context);
UA_Byte model_get_user_access_level_wrapper(UA_Server *server, UA_AccessControl *ac, const UA_NodeId *session_id, void *session_context, const UA_NodeId *node_id, void *node_context);
UA_Boolean model_allow_browse_node_wrapper(UA_Server *server, UA_AccessControl *ac, const UA_NodeId *session_id, void *session_context, const UA_NodeId *node_id, void *node_context);
UA_StatusCode model_quality_to_status(service_quality_t quality);
bool model_copy_channel_by_id(const char *channel_id, telemetry_channel_snapshot_t *out_snapshot);
bool model_copy_channel_from_context(const model_node_context_t *context,
                                     telemetry_channel_snapshot_t *out_snapshot);
bool model_copy_channel_settings(const char *channel_id, telemetry_channel_settings_t *out_settings);
UA_NodeId model_channel_value_type_to_data_type(telemetry_channel_value_type_t value_type);
bool model_build_channel_node_id(const telemetry_channel_t *channel, const char *field_name, char *buffer, size_t buffer_size);
bool model_build_legacy_channel_node_id(const telemetry_channel_t *channel, const char *field_name, char *buffer, size_t buffer_size);
bool model_channel_requires_server_rebuild(const config_channel_definition_t *lhs, const config_channel_definition_t *rhs);
bool model_byte_array_changed(const uint8_t *lhs, const uint8_t *rhs, size_t size);
bool model_config_requires_server_reboot(const device_profile_config_t *lhs, const device_profile_config_t *rhs);
bool model_is_reboot_required(void);
model_certificate_expiry_state_t model_get_certificate_expiry_state(uint32_t *out_days_until_expiry, bool *out_time_check_performed);
bool model_certificate_expiry_requires_attention(model_certificate_expiry_state_t state);
bool model_format_certificate_expiry_notice(model_certificate_expiry_state_t state, uint32_t days_until_expiry, char *message, size_t message_size, UA_UInt16 *out_severity);
bool model_build_certificate_expiry_notice(model_certificate_expiry_state_t state, uint32_t days_until_expiry, char *message, size_t message_size, UA_UInt16 *out_severity);
bool model_trust_expiry_requires_attention(const security_trust_expiry_service_report_t *report, bool commissioning_mode);
bool model_build_trust_expiry_notice(const security_trust_expiry_service_report_t *report, bool commissioning_mode, char *message, size_t message_size, UA_UInt16 *out_severity);
bool model_trust_expiry_report_changed(const security_trust_expiry_service_report_t *lhs, const security_trust_expiry_service_report_t *rhs);
UA_NodeId model_device_node_id(void);
UA_NodeId model_network_node_id(void);
UA_NodeId model_temperature_node_id(void);
UA_NodeId model_maintenance_node_id(void);
UA_NodeId model_configuration_node_id(void);
UA_NodeId model_security_node_id(void);
UA_NodeId model_event_type_node_id(const char *node_id);
UA_NodeId model_condition_type_node_id(const char *node_id);
UA_NodeId model_security_condition_node_id(void);
UA_NodeId model_device_health_alarms_node_id(void);
UA_NodeId model_device_health_condition_node_id(device_health_state_t state);
UA_StatusCode model_add_object_type(UA_Server *server, const char *node_id, const char *browse_name, const UA_NodeId parent_type_id);
void model_copy_string_safe(char *buffer, size_t buffer_size, const char *text);
void model_event_id_to_hex(const UA_ByteString *event_id, char *buffer, size_t buffer_size);
void model_store_last_event(model_event_last_kind_t kind, const char *event_id_hex, const char *message);
void model_pubsub_reset_runtime(void);
bool model_pubsub_requested(void);
void model_node_id_to_text(const UA_NodeId *node_id, char *buffer, size_t buffer_size);
void model_pubsub_extract_tail(const char *text, char *buffer, size_t buffer_size);
bool model_pubsub_build_client_id(char *buffer, size_t buffer_size);
bool model_pubsub_build_topic(char *buffer, size_t buffer_size, const char *root, const char *suffix, const char *leaf);
void model_pubsub_state_change_callback(UA_Server *server, const UA_NodeId id, UA_PubSubState state, UA_StatusCode status);
UA_StatusCode model_pubsub_register_mqtt_manager(UA_ServerConfig *config);
void model_pubsub_update_event_mirror(const UA_NodeId *event_type_id, const char *source_name, const char *message, UA_UInt16 severity, UA_DateTime event_time, const UA_ByteString *event_id);
void model_pubsub_trigger_event_publish(UA_Server *server);
UA_StatusCode model_pubsub_add_variable_field(UA_Server *server, const UA_NodeId pds_id, const UA_NodeId published_variable, const char *alias);
UA_StatusCode model_pubsub_add_mqtt_connection(UA_Server *server);
UA_StatusCode model_pubsub_add_writer_group(UA_Server *server, const char *name, const char *topic, UA_Duration publishing_interval, UA_UInt16 writer_group_id, UA_NodeId *out_writer_group_id);
UA_StatusCode model_pubsub_add_data_set_writer(UA_Server *server, const UA_NodeId writer_group_id, const UA_NodeId pds_id, const char *name, const char *topic, UA_UInt16 writer_id);
UA_StatusCode model_pubsub_add_telemetry_writer_for_channel(UA_Server *server, size_t channel_index, const telemetry_channel_t *channel);
UA_StatusCode model_pubsub_add_event_mirror_writer(UA_Server *server);
UA_StatusCode model_configure_pubsub_publishers(UA_Server *server, UA_ServerConfig *config);
void model_queue_pending_audit_event(const char *event, const char *actor, const char *subject, bool success, const char *details);
bool model_pop_pending_audit_event(model_pending_audit_event_t *out_event);
void model_security_audit_callback(const char *event, const char *actor, const char *subject, bool success, const char *details, void *context);
bool model_string_changed(const char *lhs, const char *rhs);
UA_StatusCode model_add_event_type(UA_Server *server, const char *node_id, const char *browse_name, const UA_NodeId parent_type_id);
UA_StatusCode model_add_event_property(UA_Server *server, const UA_NodeId event_type_id, const char *node_id, const char *browse_name, UA_NodeId data_type);
UA_StatusCode model_make_event_field_key(const char *browse_name, char field_key[MODEL_EVENT_FIELD_PATH_CAPACITY]);
UA_StatusCode model_set_event_string_field(UA_KeyValueMap *event_fields, const char *browse_name, const char *value);
UA_StatusCode model_enable_condition_source(UA_Server *server, const UA_NodeId parent_id, const UA_NodeId source_id);
UA_StatusCode model_add_condition_type(UA_Server *server, const char *node_id, const char *browse_name, const UA_NodeId parent_type_id);
UA_StatusCode model_condition_set_field_scalar(UA_Server *server, const UA_NodeId condition_id, const char *field_name, const void *value, const UA_DataType *type);
UA_StatusCode model_condition_set_message(UA_Server *server, const UA_NodeId condition_id, const char *message);
UA_StatusCode model_condition_set_two_state_id(UA_Server *server, const UA_NodeId condition_id, const char *field_name, UA_Boolean state);
UA_StatusCode model_condition_read_enabled(UA_Server *server, const UA_NodeId condition_id, bool *out_enabled);
UA_StatusCode model_add_amb_maintenance_property( UA_Server *server, const UA_NodeId condition_id, const char *node_id, const char *browse_name, UA_NodeId data_type, const void *value, const UA_DataType *value_type);
UA_StatusCode model_register_amb_name_node_id_type(UA_Server *server);
void model_make_amb_name_node_id(model_amb_name_node_id_t *value, const char *text);
UA_UInt32 model_maintenance_state_node_id( maintenance_service_state_t state);
bool model_maintenance_condition_is_active( const maintenance_service_snapshot_t *snapshot);
UA_UInt16 model_maintenance_condition_severity( const maintenance_service_snapshot_t *snapshot);
UA_StatusCode model_maintenance_set_read_error(UA_DataValue *value, UA_StatusCode status);
UA_StatusCode model_read_maintenance_value( UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *node_id, void *node_context, UA_Boolean include_source_timestamp, const UA_NumericRange *range, UA_DataValue *value);
UA_StatusCode model_install_maintenance_value_source( UA_Server *server, UA_NodeId node_id, model_maintenance_value_kind_t kind);
UA_StatusCode model_install_maintenance_value_source_at_path( UA_Server *server, const UA_NodeId condition_id, size_t path_size, UA_QualifiedName *path, model_maintenance_value_kind_t kind);
UA_StatusCode model_install_maintenance_condition_value_sources( UA_Server *server, const UA_NodeId condition_id);
UA_StatusCode model_add_amb_name_node_id_property( UA_Server *server, const UA_NodeId condition_id, const char *node_id, const char *browse_name, UA_Boolean is_array);
UA_StatusCode model_set_amb_maintenance_state( UA_Server *server, const UA_NodeId condition_id, const char *state_name, UA_UInt32 state_node_id);
UA_StatusCode model_initialize_amb_maintenance_condition( UA_Server *server, const UA_NodeId condition_id);
UA_StatusCode model_condition_write_common_fields(UA_Server *server, const UA_NodeId condition_id, UA_UInt16 severity, UA_StatusCode quality, const char *message);
UA_StatusCode model_condition_initialize_inactive(UA_Server *server, const UA_NodeId condition_id, const char *message);
UA_StatusCode model_condition_activate(UA_Server *server, const UA_NodeId condition_id, UA_UInt16 severity, UA_StatusCode quality, const char *message, bool reset_ack);
UA_StatusCode model_condition_update_active(UA_Server *server, const UA_NodeId condition_id, const UA_NodeId source_id, UA_UInt16 severity, UA_StatusCode quality, const char *message, bool reset_ack);
UA_StatusCode model_condition_deactivate(UA_Server *server, const UA_NodeId condition_id, const UA_NodeId source_id, const char *message);
UA_StatusCode model_sync_maintenance_condition( UA_Server *server, const maintenance_service_snapshot_t *snapshot);
UA_StatusCode model_create_condition_instance(UA_Server *server, const char *condition_node_id, const char *browse_name, const UA_NodeId condition_type_id, const UA_NodeId source_id, const char *normal_message);
UA_StatusCode model_create_device_health_condition(UA_Server *server, device_health_state_t state, const char *browse_name, UA_UInt32 condition_type_id);
UA_StatusCode model_build_alarm_conditions(UA_Server *server);
model_security_condition_reason_t model_evaluate_security_condition(char *message, size_t message_size, UA_UInt16 *out_severity, UA_StatusCode *out_quality);
void model_sync_condition(UA_Server *server, const UA_NodeId condition_id, const UA_NodeId source_id, bool *was_active, uint32_t *was_reason, bool *was_enabled, TickType_t *next_enable_poll, uint32_t reason, UA_UInt16 severity, UA_StatusCode quality, const char *active_message, const char *normal_message);
void model_process_device_health_conditions(UA_Server *server, model_event_runtime_state_t *state);
void model_process_alarm_conditions(UA_Server *server, model_event_runtime_state_t *state);
/* v0.4.3 per-channel alarm condition model (T6), defined in model_events.c. */
UA_StatusCode model_build_channel_alarm_conditions(UA_Server *server);
void model_process_channel_alarm_conditions(UA_Server *server);
UA_StatusCode model_fix_condition_method_arguments(UA_Server *server);
UA_StatusCode model_fix_server_condition_refresh_reference(UA_Server *server);
UA_StatusCode model_set_event_bool_field(UA_KeyValueMap *event_fields, const char *browse_name, UA_Boolean value);
UA_StatusCode model_set_event_uint32_field(UA_KeyValueMap *event_fields, const char *browse_name, UA_UInt32 value);
UA_StatusCode model_set_event_float_field(UA_KeyValueMap *event_fields, const char *browse_name, UA_Float value);
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
UA_StatusCode model_emit_event_ex(UA_Server *server, const UA_NodeId event_type_id, const UA_NodeId source_node_id, const char *source_name, const char *message, UA_UInt16 severity, model_event_fields_writer_t fields_writer, const void *fields_context, model_event_last_kind_t last_kind);
UA_StatusCode model_emit_event(UA_Server *server, const UA_NodeId event_type_id, const UA_NodeId source_node_id, const char *source_name, const char *message, UA_UInt16 severity);
void model_emit_network_runtime_event(UA_Server *server, const char *message, UA_UInt16 severity, const char *old_ip, const char *new_ip, bool dhcp_enabled, bool link_up);
void model_emit_sensor_runtime_event(UA_Server *server, const char *message, UA_UInt16 severity, uint32_t fault_code, const char *quality, float measured_value);
UA_StatusCode model_emit_maintenance_event(UA_Server *server, const char *message, UA_UInt16 severity, const char *operation, uint32_t result_code, const char *actor);
int64_t model_x509_days_from_civil(int32_t year, uint32_t month, uint32_t day);
bool model_x509_time_to_epoch(const mbedtls_x509_time *time, uint32_t *out_epoch_s);
bool model_remote_certificate_thumbprint(const UA_ByteString *remote_certificate, char *buffer, size_t buffer_size);
bool model_get_remote_certificate_expiry_info(const UA_ByteString *remote_certificate, model_remote_certificate_expiry_info_t *out_info);
void model_emit_client_certificate_expiry_notice(UA_Server *server, const UA_ByteString *remote_certificate, void *session_context);
void model_emit_config_changed_event(UA_Server *server, const char *message, UA_UInt16 severity, const char *changed_section, const char *changed_key, const char *actor);
void model_emit_firmware_event(UA_Server *server, const char *message, UA_UInt16 severity);
void model_process_pending_audit_events(UA_Server *server);
void model_event_runtime_state_init(model_event_runtime_state_t *state);
void model_process_certificate_expiry_runtime(UA_Server *server, model_event_runtime_state_t *state, char *message, size_t message_size);
void model_process_trust_expiry_runtime(UA_Server *server, model_event_runtime_state_t *state, char *message, size_t message_size);
void model_process_runtime_events(UA_Server *server, model_event_runtime_state_t *state);
void model_apply_now_source_timestamp(UA_Boolean include_source_timestamp, UA_DataValue *value);
void model_apply_epoch_source_timestamp(UA_Boolean include_source_timestamp, uint32_t epoch_seconds, UA_DataValue *value);
bool model_source_timestamp_to_datetime(uint64_t source_timestamp_unix_us,
                                        UA_DateTime *out_datetime);
void model_apply_channel_source_timestamp(UA_Boolean include_source_timestamp,
                                          uint64_t source_timestamp_unix_us,
                                          UA_DataValue *value);
UA_StatusCode model_set_uint32_value(UA_DataValue *value, UA_UInt32 data, UA_StatusCode status);
UA_StatusCode model_set_int32_value(UA_DataValue *value, UA_Int32 data, UA_StatusCode status);
UA_StatusCode model_set_float_value(UA_DataValue *value, UA_Float data, UA_StatusCode status);
UA_StatusCode model_set_double_value(UA_DataValue *value, UA_Double data, UA_StatusCode status);
UA_StatusCode model_set_boolean_value(UA_DataValue *value, UA_Boolean data, UA_StatusCode status);
UA_StatusCode model_set_uint64_value(UA_DataValue *value, UA_UInt64 data, UA_StatusCode status);
UA_StatusCode model_set_int64_value(UA_DataValue *value, UA_Int64 data, UA_StatusCode status);
UA_StatusCode model_set_int16_value(UA_DataValue *value, UA_Int16 data, UA_StatusCode status);
UA_StatusCode model_set_string_value(UA_DataValue *value, const char *data, UA_StatusCode status);
UA_StatusCode model_maintenance_result_status(maintenance_service_result_t result);
UA_StatusCode model_prepare_maintenance_event( model_prepared_maintenance_event_t *event, const char *message, const char *operation, const char *actor);
UA_StatusCode model_publish_prepared_maintenance_event( UA_Server *server, model_prepared_maintenance_event_t *event);
UA_StatusCode model_maintenance_mutations_available(void);
UA_StatusCode model_commit_maintenance_transaction(UA_Server *server, const maintenance_service_transaction_t *transaction, const char *operation, void *session_context);
bool model_copy_maintenance_string(const UA_Variant *value, char *out, size_t capacity);
bool model_copy_maintenance_parts(const UA_Variant *value, char out[MAINTENANCE_SERVICE_MAX_PARTS][MAINTENANCE_SERVICE_PART_TEXT_CAPACITY], uint8_t *count);
UA_StatusCode model_plan_maintenance(UA_Server *server, const UA_NodeId *sid, void *ctx, const UA_NodeId *mid, void *mc, const UA_NodeId *oid, void *oc, size_t n, const UA_Variant *in, size_t no, UA_Variant *out);
UA_StatusCode model_start_maintenance(UA_Server *server, const UA_NodeId *sid, void *ctx, const UA_NodeId *mid, void *mc, const UA_NodeId *oid, void *oc, size_t n, const UA_Variant *in, size_t no, UA_Variant *out);
UA_StatusCode model_reschedule_maintenance(UA_Server *server,const UA_NodeId *sid,void *ctx,const UA_NodeId *mid,void *mc,const UA_NodeId *oid,void *oc,size_t n,const UA_Variant *in,size_t no,UA_Variant *out);
UA_StatusCode model_finish_maintenance(UA_Server *server,const UA_NodeId *sid,void *ctx,const UA_NodeId *mid,void *mc,const UA_NodeId *oid,void *oc,size_t n,const UA_Variant *in,size_t no,UA_Variant *out);
UA_StatusCode model_reset_maintenance_journal( UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
bool model_export_append(size_t *offset, const char *format, ...);
bool model_export_append_json_string(size_t *offset, const char *text);
bool model_export_append_parts( size_t *offset, const char parts[MAINTENANCE_SERVICE_MAX_PARTS][MAINTENANCE_SERVICE_PART_TEXT_CAPACITY], uint8_t count);
bool model_parse_maintenance_export_token(const UA_Variant *input, uint32_t *out_token);
UA_StatusCode model_export_maintenance_failure( void *session_context, model_maintenance_export_continuation_t *continuation, UA_StatusCode status);
UA_StatusCode model_export_maintenance_journal( UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_set_localized_text_value(UA_DataValue *value, const char *data, UA_StatusCode status);
UA_StatusCode model_set_datetime_value(UA_DataValue *value, UA_DateTime data, UA_StatusCode status);
void model_set_default_variant_value(UA_VariableAttributes *attributes, UA_NodeId data_type);
UA_StatusCode model_set_channel_runtime_value(UA_DataValue *value, const telemetry_channel_t *channel, UA_StatusCode status);
uint32_t model_datetime_to_epoch_seconds(UA_DateTime value);
void model_fill_history_data_value(const history_service_channel_sample_t *sample, UA_TimestampsToReturn timestamps_to_return, UA_DataValue *value);
void model_history_database_clear(UA_HistoryDatabase *history_database);
void model_history_release_session_points(const void *session_context);
void model_history_read_raw(UA_Server *server, void *history_database_context, const UA_NodeId *session_id, void *session_context, const UA_RequestHeader *request_header, const UA_ReadRawModifiedDetails *history_read_details, UA_TimestampsToReturn timestamps_to_return, UA_Boolean release_continuation_points, size_t nodes_to_read_size, const UA_HistoryReadValueId *nodes_to_read, UA_HistoryReadResponse *response, UA_HistoryData *const *const history_data);
UA_StatusCode model_read_value(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *node_id, void *node_context, UA_Boolean include_source_timestamp, const UA_NumericRange *range, UA_DataValue *value);
UA_StatusCode model_write_value(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *node_id, void *node_context, const UA_NumericRange *range, const UA_DataValue *value);
void model_set_boolean_method_output(size_t output_size, UA_Variant *output, UA_Boolean result);
void model_schedule_reboot(uint32_t delay_ms);
void model_schedule_identify_blink(uint8_t blink_count, uint32_t period_ms);
UA_StatusCode model_acknowledge_alarms(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_request_reboot(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_factory_reset(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_sync_time_now(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_blink_identify(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_apply_config(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_save_config(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_set_mqtt_pubsub_enabled(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_set_device_identity(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_set_location_paths(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_set_network_config(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
void model_format_channel_config_json(size_t slot_index, const config_channel_definition_t *channel, char *buffer, size_t buffer_size);
void model_format_driver_catalog_json(char *buffer, size_t buffer_size);
UA_StatusCode model_get_channel_config(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_list_drivers(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_get_config_state(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_export_config(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_refresh_runtime_trust_backends(UA_Server *server);
void model_finalize_identity_generation_after_startup(void);
void model_write_identity_import_report_json(char *buffer, size_t buffer_size, const security_identity_import_service_report_t *report);
void model_write_certificate_metadata_json(char *buffer, size_t buffer_size);
void model_write_certificate_state_json(char *buffer, size_t buffer_size);
UA_StatusCode model_get_certificate_state(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_export_certificate_metadata(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_begin_identity_rotation(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_write_identity_rotation_chunk(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_abort_identity_rotation(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_validate_staged_identity(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_apply_staged_identity(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
void model_write_trust_import_report_json(char *buffer, size_t buffer_size, const security_trust_import_service_report_t *report);
void model_append_json_object_field(char *buffer, size_t buffer_size, const char *field_name, const char *object_json);
void model_write_trust_state_json(char *buffer, size_t buffer_size);
UA_StatusCode model_get_trust_state(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_export_trust_list(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_begin_trust_list_import(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_write_trust_list_chunk(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_abort_trust_list_import(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_validate_imported_trust_list(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_apply_imported_trust_list(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_begin_config_import(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_write_config_chunk(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_abort_config_import(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_validate_imported_config(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_apply_imported_config(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
void model_format_firmware_update_status_json(char *buffer, size_t buffer_size);
UA_StatusCode model_get_firmware_update_status(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_begin_firmware_update(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_write_firmware_chunk(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_finalize_firmware_update(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_abort_firmware_update(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_clear_channel_config(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_set_channel_config(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_self_test(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_clear_diagnostics(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_set_trust_list(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_enter_commissioning_mode(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_exit_commissioning_mode(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_add_user(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_change_password(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_set_role(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_disable_user(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_delete_user(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_add_object(UA_Server *server, const char *node_id, const char *browse_name, const char *display_name, UA_NodeId parent_node_id, UA_NodeId reference_type_id, UA_NodeId type_definition_id);
UA_StatusCode model_add_folder(UA_Server *server, const char *node_id, const char *display_name, UA_NodeId parent_node_id, UA_NodeId reference_type_id);
UA_StatusCode model_add_amb_folder(UA_Server *server, const char *node_id, const char *browse_name, const char *description, UA_NodeId parent_node_id);
UA_StatusCode model_add_static_organized_variable(UA_Server *server, const char *node_id, const char *browse_name, const char *description, UA_NodeId parent_node_id, const void *value, const UA_DataType *data_type);
UA_StatusCode model_build_amb_capabilities_and_requirements(UA_Server *server, UA_NodeId device_id);
UA_StatusCode model_add_location_node_version(UA_Server *server, const char *node_id, UA_NodeId parent_id);
UA_StatusCode model_write_location_node_version(UA_Server *server, const char *node_id);
UA_StatusCode model_delete_location_tree(UA_Server *server, const char *node_id_prefix);
UA_StatusCode model_build_location_path(UA_Server *server, const char *path, const char *node_id_prefix, UA_UInt32 entry_point_id, UA_UInt32 contains_reference_id, UA_NodeId device_id);
UA_StatusCode model_refresh_amb_locations(UA_Server *server, const device_profile_config_t *config);
UA_StatusCode model_build_amb_locations(UA_Server *server, const device_profile_config_t *config);
UA_StatusCode model_add_di_functional_group(UA_Server *server, const char *node_id, const char *browse_name, UA_NodeId parent_node_id);
UA_StatusCode model_add_di_identification_property( UA_Server *server, const char *node_id, const char *browse_name, UA_NodeId parent_node_id, UA_NodeId data_type, model_node_context_t *context, UA_Boolean writable);
bool model_alias_match_class(const UA_Byte *pattern, size_t pattern_length, UA_Byte value, size_t *consumed, bool *valid);
bool model_alias_pattern_matches(const UA_String *pattern, const char *value, bool *valid);
bool model_alias_pattern_is_valid(const UA_String *pattern);
UA_StatusCode model_find_asset_alias(UA_Server *server, const UA_NodeId *session_id, void *session_context, const UA_NodeId *method_id, void *method_context, const UA_NodeId *object_id, void *object_context, size_t input_size, const UA_Variant *input, size_t output_size, UA_Variant *output);
UA_StatusCode model_write_asset_alias_node_version(UA_Server *server, UA_UInt32 node_id);
UA_StatusCode model_write_alias_last_change(UA_Server *server);
UA_StatusCode model_update_asset_id_alias(UA_Server *server, const char *asset_id);
UA_StatusCode model_add_asset_alias(UA_Server *server, const char *node_id, const char *alias_name, UA_UInt32 category_id, UA_NodeId device_id);
UA_StatusCode model_build_asset_discovery(UA_Server *server, UA_NodeId device_id, const device_profile_config_t *config);
UA_StatusCode model_build_device_health_nodes(UA_Server *server, UA_NodeId device_id);
UA_StatusCode model_add_readonly_variable_ex(UA_Server *server, const char *node_id, const char *display_name, UA_NodeId parent_node_id, UA_NodeId data_type, model_node_context_t *context, UA_Byte access_level, UA_Boolean historizing);
UA_StatusCode model_add_readonly_variable(UA_Server *server, const char *node_id, const char *display_name, UA_NodeId parent_node_id, UA_NodeId data_type, model_node_context_t *context);
UA_StatusCode model_add_writable_variable(UA_Server *server, const char *node_id, const char *display_name, UA_NodeId parent_node_id, UA_NodeId data_type, model_node_context_t *context);
void model_fill_string_argument(UA_Argument *argument, const char *name, const char *description);
void model_fill_bytestring_argument(UA_Argument *argument, const char *name, const char *description);
void model_fill_datetime_argument(UA_Argument *argument, const char *name, const char *description);
void model_fill_uint32_argument(UA_Argument *argument, const char *name, const char *description);
void model_fill_double_argument(UA_Argument *argument, const char *name, const char *description);
void model_fill_boolean_argument(UA_Argument *argument, const char *name, const char *description);
void model_set_string_method_output(size_t output_size, UA_Variant *output, const char *result);
UA_StatusCode model_set_maintenance_export_method_output(size_t output_size,
                                                         UA_Variant *output,
                                                         size_t payload_length);
void model_set_bytestring_method_output(size_t output_size, UA_Variant *output, const UA_ByteString *result);
UA_StatusCode model_add_method_node_with_boolean_result(UA_Server *server, UA_NodeId maintenance_folder_id, const char *node_id, const char *method_name, const char *description, UA_MethodCallback callback, size_t input_size, UA_Argument *input_arguments);
UA_StatusCode model_add_method_node_with_string_result(UA_Server *server, UA_NodeId maintenance_folder_id, const char *node_id, const char *method_name, const char *description, UA_MethodCallback callback, size_t input_size, UA_Argument *input_arguments);
UA_StatusCode model_add_method_node_with_bytestring_result(UA_Server *server, UA_NodeId maintenance_folder_id, const char *node_id, const char *method_name, const char *description, UA_MethodCallback callback, size_t input_size, UA_Argument *input_arguments);
UA_StatusCode model_add_boolean_method_node(UA_Server *server, UA_NodeId maintenance_folder_id, const char *node_id, const char *method_name, const char *description, UA_MethodCallback callback);
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
UA_StatusCode model_add_maintenance_workflow_methods(UA_Server *server, UA_NodeId maintenance_id);
UA_StatusCode model_add_set_network_config_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_set_channel_config_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_get_channel_config_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_clear_channel_config_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
UA_StatusCode model_add_list_drivers_method_node(UA_Server *server, UA_NodeId maintenance_folder_id);
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
UA_StatusCode model_add_task_stack_high_water_node(UA_Server *server, UA_NodeId task_folder_id, const char *task_object_id, const char *task_display_name, const char *registered_task_name, model_node_context_t *task_context);
UA_StatusCode model_add_channel_nodes(UA_Server *server, UA_NodeId channels_folder_id, size_t channel_index, const telemetry_channel_t *channel);
UA_StatusCode model_add_legacy_temperature_nodes(UA_Server *server, UA_NodeId channels_folder_id, size_t channel_index, const telemetry_channel_t *channel);
UA_StatusCode model_build_address_space(UA_Server *server);
bool opcua_model_init(const opcua_model_config_t *config);
bool model_certificate_view_is_pem(const security_service_blob_view_t *certificate);
void model_clear_runtime_security_config(UA_ServerConfig *config);
void model_clear_server_urls(UA_ServerConfig *config);
UA_StatusCode model_configure_server_url(UA_ServerConfig *config, const char *hostname, uint16_t port);
UA_StatusCode model_configure_application_uri(UA_ServerConfig *config);
UA_StatusCode model_seed_bootstrap_identity_if_needed(void);
UA_StatusCode model_configure_secure_access_control(UA_ServerConfig *config);
UA_StatusCode model_keep_only_none_and_signandencrypt_endpoints(UA_ServerConfig *config);
UA_StatusCode model_keep_only_signandencrypt_endpoints(UA_ServerConfig *config);
UA_StatusCode model_limit_insecure_endpoints_to_anonymous(UA_ServerConfig *config);
UA_StatusCode model_configure_secure_server(UA_ServerConfig *config);
void model_bind_open62541_allocators(void);
void opcua_model_task(void *argument);

#endif
