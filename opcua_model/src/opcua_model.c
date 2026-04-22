#include "opcua_model/opcua_model.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>
#include "ua_port_alloc.h"

#include "drivers/led_driver.h"
#include "open62541.h"
#include "pico/stdio.h"
#include "services/alarm_service.h"
#include "services/channel_manager.h"
#include "services/config_service.h"
#include "services/config_export_service.h"
#include "services/config_import_service.h"
#include "services/config_reset_service.h"
#include <services/config_validate_service.h>
#include "services/device_channels.h"
#include "services/diag_service.h"
#include "services/firmware_update_service.h"
#include "services/history_service.h"
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
    MODEL_NODE_IDENT_MAC_ADDRESS,
    MODEL_NODE_IDENT_MANUFACTURE_DATE,
    MODEL_NODE_IDENT_CALIBRATION_DATE,
    MODEL_NODE_IDENT_LOCATION,
    MODEL_NODE_IDENT_ASSET_CLASS,
    MODEL_NODE_PROCESS_TEMPERATURE_VALUE,
    MODEL_NODE_PROCESS_TEMPERATURE_STATUS,
    MODEL_NODE_PROCESS_TEMPERATURE_SOURCE_TIMESTAMP,
    MODEL_NODE_PROCESS_TEMPERATURE_UNITS,
    MODEL_NODE_PROCESS_TEMPERATURE_UPDATE_PERIOD_MS,
    MODEL_NODE_PROCESS_TEMPERATURE_HISTORY_INTERVAL_MS,
    MODEL_NODE_PROCESS_TEMPERATURE_ALARM_STATE,
    MODEL_NODE_DIAG_UPTIME_SEC,
    MODEL_NODE_DIAG_RESET_CAUSE,
    MODEL_NODE_DIAG_FREE_HEAP,
    MODEL_NODE_DIAG_MIN_EVER_FREE_HEAP,
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
} model_session_context_t;

#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
typedef struct
{
    bool initialized;
    network_manager_status_t network_status;
    sensor_manager_snapshot_t sensor_snapshot;
    alarm_service_status_t alarm_status;
    bool server_certificate_expiry_seen;
    uint32_t server_certificate_expiry_state;
    bool trust_expiry_seen;
    bool trust_expiry_commissioning_mode;
    security_trust_expiry_service_report_t trust_expiry_report;
#if defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
    bool security_condition_active;
    uint32_t security_condition_reason;
    bool system_resource_condition_active;
    uint32_t system_resource_condition_reason;
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
    UA_NodeId telemetry_writer_group_ids[CHANNEL_MANAGER_MAX_CHANNELS];
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

typedef enum
{
    MODEL_SYSTEM_RESOURCE_CONDITION_REASON_NONE = 0,
    MODEL_SYSTEM_RESOURCE_CONDITION_REASON_MALLOC_FAILURE = 1,
    MODEL_SYSTEM_RESOURCE_CONDITION_REASON_STACK_OVERFLOW = 2,
    MODEL_SYSTEM_RESOURCE_CONDITION_REASON_LOW_CONTIGUOUS_HEAP = 3,
    MODEL_SYSTEM_RESOURCE_CONDITION_REASON_LOW_FREE_HEAP = 4,
    MODEL_SYSTEM_RESOURCE_CONDITION_REASON_SELF_TEST_FAILED = 5
} model_system_resource_condition_reason_t;
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
    FIXED_CTX_IDENT_MAC_ADDRESS,
    FIXED_CTX_IDENT_MANUFACTURE_DATE,
    FIXED_CTX_IDENT_CALIBRATION_DATE,
    FIXED_CTX_IDENT_LOCATION,
    FIXED_CTX_IDENT_ASSET_CLASS,
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
#define OPCUA_SEND_BUFFER_SIZE 8192u
#define OPCUA_RECV_BUFFER_SIZE 16384u
#define MODEL_NODE_MAINTENANCE_CONFIGURATION "Device.Maintenance.Configuration"
#define MODEL_NODE_MAINTENANCE_FIRMWARE "Device.Maintenance.Firmware"
#define MODEL_NODE_MAINTENANCE_SECURITY "Device.Maintenance.Security"
#define MODEL_NODE_MAINTENANCE_DIAGNOSTICS "Device.Maintenance.Diagnostics"
#define MODEL_NODE_MAINTENANCE_SYSTEM "Device.Maintenance.System"
#define MODEL_METHOD_SET_DEVICE_IDENTITY "Device.Maintenance.SetDeviceIdentity"
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
#define MODEL_CONDITION_TYPE_SYSTEM_RESOURCE "Types.Conditions.SystemResourceConditionType"
#define MODEL_CONDITION_SECURITY_FAULT "Device.Security.SecurityFaultCondition"
#define MODEL_CONDITION_SYSTEM_RESOURCE "Device.Diagnostics.SystemResourceCondition"
#define MODEL_CERTIFICATE_EXPIRY_WARN_DAYS 30u
#define MODEL_CERTIFICATE_EXPIRY_HIGH_WARN_DAYS 7u
#define MODEL_SYSTEM_RESOURCE_FREE_HEAP_WARN_BYTES 32768u
#define MODEL_SYSTEM_RESOURCE_LARGEST_BLOCK_WARN_BYTES 16384u
#define MODEL_AUDIT_QUEUE_CAPACITY 8u
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

#ifdef UA_ENABLE_ENCRYPTION_MBEDTLS
static const uint8_t g_bootstrap_device_certificate[] = TEST_SRV_CRT_RSA_SHA256_DER;
static const uint8_t g_bootstrap_device_private_key[] = TEST_SRV_KEY_RSA_PEM;
#endif

static opcua_model_config_t g_model_config;
static bool g_model_initialized = false;
static model_pending_actions_t g_pending_actions = {false, 0, false, 0, 0, false};
static device_profile_config_t g_model_address_space_config_snapshot;
static bool g_model_address_space_config_snapshot_valid = false;
static model_activate_session_fn g_default_activate_session = NULL;
static model_close_session_fn g_default_close_session = NULL;
static model_get_user_executable_fn g_default_get_user_executable = NULL;
static model_get_user_executable_on_object_fn g_default_get_user_executable_on_object = NULL;
static model_get_user_access_level_fn g_default_get_user_access_level = NULL;
static model_allow_browse_node_fn g_default_allow_browse_node = NULL;
static uint8_t g_history_backend_context = 0u;
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
static model_last_event_state_t g_last_event_state = {{0}};
static model_pending_audit_event_t g_pending_audit_events[MODEL_AUDIT_QUEUE_CAPACITY] = {{0}};
static size_t g_pending_audit_event_head = 0u;
static size_t g_pending_audit_event_count = 0u;
#if defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
static bool g_model_alarm_conditions_available = false;
#endif
#endif
#ifdef UA_ENABLE_PUBSUB
static model_pubsub_runtime_t g_model_pubsub = {0};
static model_pubsub_event_mirror_t g_model_pubsub_event_mirror = {0};
#endif

static const char *model_log_level_name(UA_LogLevel level)
{
    switch (level)
    {
    case UA_LOGLEVEL_FATAL:
        return "fatal";
    case UA_LOGLEVEL_ERROR:
        return "error";
    case UA_LOGLEVEL_WARNING:
        return "warn";
    case UA_LOGLEVEL_INFO:
        return "info";
    case UA_LOGLEVEL_DEBUG:
        return "debug";
    case UA_LOGLEVEL_TRACE:
        return "trace";
    default:
        return "log";
    }
}

static void model_ua_logger_log(void *context,
                                UA_LogLevel level,
                                UA_LogCategory category,
                                const char *msg,
                                va_list args)
{
    (void)context;
    (void)category;
    printf("[open62541/%s]\t", model_log_level_name(level));
    vprintf(msg, args);
    putchar('\n');
    stdio_flush();
}

static UA_Logger g_model_ua_logger = {model_ua_logger_log, NULL, NULL};

static void log_opcua_heap(const char *stage)
{
    printf("[OPC UA]\t%s, free heap=%u, min ever free=%u\n",
           stage,
           (unsigned)xPortGetFreeHeapSize(),
           (unsigned)xPortGetMinimumEverFreeHeapSize());
    fflush(stdout);
}

static void model_set_boolean_method_output(size_t output_size, UA_Variant *output, UA_Boolean result);
static void model_set_string_method_output(size_t output_size, UA_Variant *output, const char *result);
static void model_set_bytestring_method_output(size_t output_size,
                                               UA_Variant *output,
                                               const UA_ByteString *result);
static void model_set_default_variant_value(UA_VariableAttributes *attributes, UA_NodeId data_type);
static const UA_ByteString *model_session_get_username_bytes(const void *session_context);
static bool model_copy_session_username(const void *session_context, char *buffer, size_t buffer_size);
static void model_copy_string_safe(char *buffer, size_t buffer_size, const char *text);
static void model_emit_client_certificate_expiry_notice(UA_Server *server,
                                                        const UA_ByteString *remote_certificate,
                                                        void *session_context);
static bool model_is_admin_only_method(const UA_NodeId *method_id);
static bool model_string_changed(const char *lhs, const char *rhs);

static bool model_session_is_secure(const void *session_context)
{
    const model_session_context_t *context = (const model_session_context_t *)session_context;
    return context != NULL && context->secure_channel;
}

static bool model_nodeid_string_has_prefix(const UA_NodeId *node_id, const char *prefix)
{
    const size_t prefix_length = strlen(prefix);

    return node_id != NULL &&
           prefix != NULL &&
           node_id->namespaceIndex == 1u &&
           node_id->identifierType == UA_NODEIDTYPE_STRING &&
           node_id->identifier.string.data != NULL &&
           node_id->identifier.string.length >= prefix_length &&
           memcmp(node_id->identifier.string.data, prefix, prefix_length) == 0;
}

static bool model_session_is_anonymous(const void *session_context)
{
    const UA_ByteString *username = model_session_get_username_bytes(session_context);
    return username == NULL || username->data == NULL || username->length == 0u;
}

static bool model_node_is_anonymous_hidden(const UA_NodeId *node_id)
{
    return model_nodeid_string_has_prefix(node_id, "Device.Maintenance") ||
           model_nodeid_string_has_prefix(node_id, "Device.Identification") ||
           model_nodeid_string_has_prefix(node_id, "Device.Security");
}

static bool model_node_is_maintenance_method(const UA_NodeId *method_id)
{
    return model_nodeid_string_has_prefix(method_id, "Device.Maintenance");
}

static UA_Byte model_strip_anonymous_write_access(UA_Byte access_level)
{
    return access_level &
           (UA_Byte)~(UA_ACCESSLEVELMASK_WRITE |
                      UA_ACCESSLEVELMASK_HISTORYWRITE |
                      UA_ACCESSLEVELMASK_STATUSWRITE |
                      UA_ACCESSLEVELMASK_TIMESTAMPWRITE);
}

static const UA_ByteString *model_session_get_username_bytes(const void *session_context)
{
    const model_session_context_t *context = (const model_session_context_t *)session_context;

    if (context == NULL || context->base_context == NULL)
    {
        return NULL;
    }

    return (const UA_ByteString *)context->base_context;
}

static bool model_copy_session_username(const void *session_context, char *buffer, size_t buffer_size)
{
    const UA_ByteString *username = model_session_get_username_bytes(session_context);

    if (username == NULL || buffer == NULL || buffer_size == 0u || username->length >= buffer_size)
    {
        return false;
    }

    memcpy(buffer, username->data, username->length);
    buffer[username->length] = '\0';
    return true;
}

static UA_ByteString *model_allocate_session_username(const UA_String *user_name)
{
    UA_ByteString *stored_username = NULL;

    if (user_name == NULL)
    {
        return NULL;
    }

    stored_username = (UA_ByteString *)UA_calloc(1u, sizeof(UA_ByteString));
    if (stored_username == NULL)
    {
        return NULL;
    }

    if (UA_ByteString_allocBuffer(stored_username, user_name->length) != UA_STATUSCODE_GOOD)
    {
        UA_free(stored_username);
        return NULL;
    }

    memcpy(stored_username->data, user_name->data, user_name->length);
    return stored_username;
}

static void model_free_session_username(void *base_context)
{
    UA_ByteString *stored_username = (UA_ByteString *)base_context;

    if (stored_username == NULL)
    {
        return;
    }

    UA_ByteString_clear(stored_username);
    UA_free(stored_username);
}

static bool model_session_has_role(const void *session_context, security_user_role_t required_role)
{
    char username[SECURITY_SERVICE_USERNAME_MAX_LEN];
    security_user_role_t current_role = SECURITY_ROLE_VIEWER;

    if (!model_copy_session_username(session_context, username, sizeof(username)))
    {
        return false;
    }

    if (!security_service_get_role(username, &current_role))
    {
        return false;
    }

    return current_role >= required_role;
}

static bool model_session_get_role(const void *session_context, security_user_role_t *out_role)
{
    char username[SECURITY_SERVICE_USERNAME_MAX_LEN];

    if (out_role == NULL)
    {
        return false;
    }

    if (!model_copy_session_username(session_context, username, sizeof(username)))
    {
        return false;
    }

    return security_service_get_role(username, out_role);
}

static const char *model_session_role_to_string(const void *session_context)
{
    security_user_role_t role = SECURITY_ROLE_VIEWER;

    if (model_session_is_anonymous(session_context))
    {
        return "Anonymous";
    }

    if (!model_session_get_role(session_context, &role))
    {
        return "Unknown";
    }

    return security_service_role_to_string(role);
}

static const char *model_session_actor_name(void *session_context, char *buffer, size_t buffer_size)
{
    if (model_copy_session_username(session_context, buffer, buffer_size))
    {
        return buffer;
    }

    return model_session_is_anonymous(session_context) ? "anonymous" : "system";
}

static void model_audit_session_event(void *session_context,
                                      const char *event,
                                      const char *subject,
                                      bool success,
                                      const char *details)
{
    char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];

    security_service_audit_log(event,
                               model_session_actor_name(session_context, actor, sizeof(actor)),
                               subject,
                               success,
                               details);
}

static UA_StatusCode model_require_session_role(void *session_context,
                                                security_user_role_t required_role,
                                                size_t output_size,
                                                UA_Variant *output)
{
    security_user_role_t current_role = SECURITY_ROLE_VIEWER;

    if (required_role > SECURITY_ROLE_VIEWER && !model_session_is_secure(session_context))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADSECURITYMODEINSUFFICIENT;
    }

    if (!model_session_get_role(session_context, &current_role) || current_role < required_role)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADUSERACCESSDENIED;
    }

    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_require_admin_session(void *session_context,
                                                 size_t output_size,
                                                 UA_Variant *output)
{
    return model_require_session_role(session_context,
                                      SECURITY_ROLE_ADMIN,
                                      output_size,
                                      output);
}

static security_user_role_t model_required_role_for_method(const UA_NodeId *method_id)
{
    if (method_id == NULL)
    {
        return SECURITY_ROLE_ADMIN;
    }

    if (model_is_admin_only_method(method_id))
    {
        return SECURITY_ROLE_ADMIN;
    }

    if (model_nodeid_string_has_prefix(method_id, "Device.Security"))
    {
        return SECURITY_ROLE_ADMIN;
    }

    if (model_nodeid_string_has_prefix(method_id, "Device.Maintenance.AcknowledgeAllAlarms") ||
        model_nodeid_string_has_prefix(method_id, "Device.Maintenance.ApplyConfig") ||
        model_nodeid_string_has_prefix(method_id, "Device.Maintenance.SaveConfig") ||
        model_nodeid_string_has_prefix(method_id, MODEL_METHOD_SET_MQTT_PUBSUB_ENABLED))
    {
        return SECURITY_ROLE_OPERATOR;
    }

    if (model_node_is_maintenance_method(method_id))
    {
        return SECURITY_ROLE_SERVICE;
    }

    return SECURITY_ROLE_VIEWER;
}

static security_user_role_t model_required_role_for_write_kind(model_node_kind_t kind)
{
    switch (kind)
    {
    case MODEL_NODE_PROCESS_TEMPERATURE_UPDATE_PERIOD_MS:
    case MODEL_NODE_PROCESS_TEMPERATURE_HISTORY_INTERVAL_MS:
    case MODEL_NODE_TIME_TIMEZONE_OFFSET_MINUTES:
    case MODEL_NODE_CONFIG_SAMPLING_PERIOD_MS:
    case MODEL_NODE_CONFIG_PUBLISH_PERIOD_MS:
    case MODEL_NODE_CONFIG_TEMP_OFFSET:
    case MODEL_NODE_CONFIG_TEMP_SCALE:
    case MODEL_NODE_CONFIG_HIGH_ALARM:
    case MODEL_NODE_CONFIG_HIGH_HIGH_ALARM:
        return SECURITY_ROLE_OPERATOR;
    default:
        return SECURITY_ROLE_ADMIN;
    }
}

static UA_StatusCode model_require_method_role(void *session_context,
                                               const UA_NodeId *method_id,
                                               size_t output_size,
                                               UA_Variant *output)
{
    return model_require_session_role(session_context,
                                      model_required_role_for_method(method_id),
                                      output_size,
                                      output);
}

static bool model_is_admin_only_method(const UA_NodeId *method_id)
{
    const UA_NodeId add_user = UA_NODEID_STRING(1, (char *)MODEL_METHOD_ADD_USER);
    const UA_NodeId change_password = UA_NODEID_STRING(1, (char *)MODEL_METHOD_CHANGE_PASSWORD);
    const UA_NodeId set_role = UA_NODEID_STRING(1, (char *)MODEL_METHOD_SET_ROLE);
    const UA_NodeId disable_user = UA_NODEID_STRING(1, (char *)MODEL_METHOD_DISABLE_USER);
    const UA_NodeId delete_user = UA_NODEID_STRING(1, (char *)MODEL_METHOD_DELETE_USER);

    return method_id != NULL &&
           (UA_NodeId_equal(method_id, &add_user) ||
            UA_NodeId_equal(method_id, &change_password) ||
            UA_NodeId_equal(method_id, &set_role) ||
            UA_NodeId_equal(method_id, &disable_user) ||
            UA_NodeId_equal(method_id, &delete_user));
}

static bool model_copy_input_string(const UA_Variant *value, char *buffer, size_t buffer_size)
{
    const UA_String *input_string = NULL;

    if (value == NULL || buffer == NULL || buffer_size == 0u ||
        !UA_Variant_hasScalarType(value, &UA_TYPES[UA_TYPES_STRING]))
    {
        return false;
    }

    input_string = (const UA_String *)value->data;
    if (input_string == NULL || input_string->length >= buffer_size)
    {
        return false;
    }

    memcpy(buffer, input_string->data, input_string->length);
    buffer[input_string->length] = '\0';
    return true;
}

static bool model_get_input_ua_string(const UA_Variant *value, const UA_String **out_string)
{
    if (value == NULL || out_string == NULL ||
        !UA_Variant_hasScalarType(value, &UA_TYPES[UA_TYPES_STRING]) ||
        value->data == NULL)
    {
        return false;
    }

    *out_string = (const UA_String *)value->data;
    return true;
}

static bool model_get_input_ua_bytestring(const UA_Variant *value, const UA_ByteString **out_bytes)
{
    if (value == NULL || out_bytes == NULL ||
        !UA_Variant_hasScalarType(value, &UA_TYPES[UA_TYPES_BYTESTRING]) ||
        value->data == NULL)
    {
        return false;
    }

    *out_bytes = (const UA_ByteString *)value->data;
    return true;
}

static bool model_has_config_text(const char *value)
{
    return value != NULL && value[0] != '\0';
}

static void model_format_mac_string(const uint8_t mac[6], char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0u || mac == NULL)
    {
        return;
    }

    (void)snprintf(buffer,
                   buffer_size,
                   "%02X:%02X:%02X:%02X:%02X:%02X",
                   mac[0],
                   mac[1],
                   mac[2],
                   mac[3],
                   mac[4],
                   mac[5]);
    buffer[buffer_size - 1u] = '\0';
}

static bool model_parse_ipv4_string(const char *text, uint8_t out_ipv4[4])
{
    unsigned int octets[4] = {0};

    if (text == NULL || out_ipv4 == NULL ||
        sscanf(text, "%u.%u.%u.%u", &octets[0], &octets[1], &octets[2], &octets[3]) != 4)
    {
        return false;
    }

    for (size_t index = 0u; index < 4u; ++index)
    {
        if (octets[index] > 255u)
        {
            return false;
        }

        out_ipv4[index] = (uint8_t)octets[index];
    }

    return true;
}

static bool model_parse_mac_string(const char *text, uint8_t out_mac[6])
{
    unsigned int octets[6] = {0};

    if (text == NULL || out_mac == NULL)
    {
        return false;
    }

    if (sscanf(text,
               "%x:%x:%x:%x:%x:%x",
               &octets[0],
               &octets[1],
               &octets[2],
               &octets[3],
               &octets[4],
               &octets[5]) != 6 &&
        sscanf(text,
               "%x-%x-%x-%x-%x-%x",
               &octets[0],
               &octets[1],
               &octets[2],
               &octets[3],
               &octets[4],
               &octets[5]) != 6)
    {
        return false;
    }

    for (size_t index = 0u; index < 6u; ++index)
    {
        if (octets[index] > 255u)
        {
            return false;
        }

        out_mac[index] = (uint8_t)octets[index];
    }

    return true;
}

static bool model_identity_is_complete(const device_profile_config_t *config)
{
    return config != NULL &&
           model_has_config_text(config->device_name) &&
           model_has_config_text(config->opcua_hostname) &&
           model_has_config_text(config->product_instance_uri) &&
           model_has_config_text(config->serial_number) &&
           model_has_config_text(config->asset_id) &&
           model_has_config_text(config->hw_revision) &&
           model_has_config_text(config->fw_revision) &&
           model_has_config_text(config->location) &&
           model_has_config_text(config->asset_class) &&
           config->manufacture_date_utc > 0 &&
           config->calibration_date_utc > 0;
}

static bool model_network_config_is_complete(const device_profile_config_t *config)
{
    if (config == NULL || config->opcua_port == 0u || !model_has_config_text(config->opcua_hostname))
    {
        return false;
    }

    if (config->network_mode == NETWORK_MODE_DHCP)
    {
        return true;
    }

    return !(config->static_ip[0] == 0u && config->static_ip[1] == 0u &&
             config->static_ip[2] == 0u && config->static_ip[3] == 0u);
}

static bool model_has_trust_anchors(void)
{
    security_service_blob_view_t trust_list = {0};
    return security_service_get_trust_list(&trust_list) && trust_list.length > 0u;
}

static bool model_is_commissioned(void)
{
    const device_profile_config_t *config = config_service_get();

    return model_identity_is_complete(config) &&
           model_network_config_is_complete(config) &&
           model_has_trust_anchors() &&
           !security_service_is_commissioning_mode();
}

static UA_StatusCode model_login_callback(const UA_String *user_name,
                                          const UA_ByteString *password,
                                          size_t username_password_login_size,
                                          const UA_UsernamePasswordLogin *username_password_login,
                                          void **session_context,
                                          void *login_context)
{
    char username[SECURITY_SERVICE_USERNAME_MAX_LEN];
    security_user_role_t role = SECURITY_ROLE_VIEWER;
    uint32_t retry_after_ms = 0u;
    security_auth_result_t auth_result;
    (void)username_password_login_size;
    (void)username_password_login;
    (void)session_context;
    (void)login_context;

    if (user_name == NULL || password == NULL || user_name->length >= sizeof(username))
    {
        return UA_STATUSCODE_BADUSERACCESSDENIED;
    }

    memcpy(username, user_name->data, user_name->length);
    username[user_name->length] = '\0';

    auth_result = security_service_authenticate_user(username,
                                                     password->data,
                                                     password->length,
                                                     &role,
                                                     &retry_after_ms);
    if (auth_result != SECURITY_AUTH_RESULT_SUCCESS)
    {
        if (auth_result == SECURITY_AUTH_RESULT_LOCKED_OUT)
        {
            printf("[OPC UA]\tLogin rejected for user '%s': locked retry_after_ms=%u\n",
                   username,
                   (unsigned)retry_after_ms);
        }
        else
        {
            printf("[OPC UA]\tLogin rejected for user '%s'\n", username);
        }
        fflush(stdout);
        return UA_STATUSCODE_BADUSERACCESSDENIED;
    }

    printf("[OPC UA]\tLogin accepted for user '%s' as %s\n",
           username,
           security_service_role_to_string(role));
    fflush(stdout);

    if (session_context != NULL)
    {
        *session_context = model_allocate_session_username(user_name);
        if (*session_context == NULL)
        {
            return UA_STATUSCODE_BADOUTOFMEMORY;
        }
    }

    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_activate_session_secure_wrapper(UA_Server *server,
                                                           UA_AccessControl *ac,
                                                           const UA_EndpointDescription *endpoint_description,
                                                           const UA_ByteString *secure_channel_remote_certificate,
                                                           const UA_NodeId *session_id,
                                                           const UA_ExtensionObject *user_identity_token,
                                                           void **session_context)
{
    model_session_context_t *wrapped_context = NULL;
    void *base_context = NULL;
    UA_StatusCode status;

    if (g_default_activate_session == NULL)
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    status = g_default_activate_session(server,
                                        ac,
                                        endpoint_description,
                                        secure_channel_remote_certificate,
                                        session_id,
                                        user_identity_token,
                                        &base_context);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tActivateSession rejected: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
        return status;
    }

    wrapped_context = (model_session_context_t *)UA_calloc(1u, sizeof(model_session_context_t));
    if (wrapped_context == NULL)
    {
        if (base_context != NULL && g_default_close_session != NULL)
        {
            g_default_close_session(server, ac, session_id, base_context);
        }
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }

    wrapped_context->base_context = base_context;
    wrapped_context->secure_channel = endpoint_description != NULL &&
                                      endpoint_description->securityMode != UA_MESSAGESECURITYMODE_NONE &&
                                      !UA_ByteString_equal(&endpoint_description->securityPolicyUri,
                                                           &UA_SECURITY_POLICY_NONE_URI);
    *session_context = wrapped_context;
    if (wrapped_context->secure_channel)
    {
        model_emit_client_certificate_expiry_notice(server,
                                                    secure_channel_remote_certificate,
                                                    wrapped_context);
    }
    return UA_STATUSCODE_GOOD;
}

static void model_close_session_secure_wrapper(UA_Server *server,
                                               UA_AccessControl *ac,
                                               const UA_NodeId *session_id,
                                               void *session_context)
{
    model_session_context_t *wrapped_context = (model_session_context_t *)session_context;
    void *base_context = wrapped_context != NULL ? wrapped_context->base_context : NULL;

    if (g_default_close_session != NULL)
    {
        g_default_close_session(server, ac, session_id, base_context);
    }

    model_free_session_username(base_context);
    UA_free(wrapped_context);
}

static UA_Boolean model_get_user_executable_wrapper(UA_Server *server,
                                                    UA_AccessControl *ac,
                                                    const UA_NodeId *session_id,
                                                    void *session_context,
                                                    const UA_NodeId *method_id,
                                                    void *method_context)
{
    model_session_context_t *wrapped_context = (model_session_context_t *)session_context;
    const UA_Boolean base_allowed =
        g_default_get_user_executable != NULL
            ? g_default_get_user_executable(server,
                                            ac,
                                            session_id,
                                            wrapped_context != NULL ? wrapped_context->base_context : NULL,
                                            method_id,
                                            method_context)
            : true;

    if (!base_allowed)
    {
        return false;
    }

    if (model_session_is_anonymous(session_context) && model_node_is_maintenance_method(method_id))
    {
        return false;
    }

    return model_session_has_role(session_context, model_required_role_for_method(method_id));
}

static UA_Boolean model_get_user_executable_on_object_wrapper(UA_Server *server,
                                                              UA_AccessControl *ac,
                                                              const UA_NodeId *session_id,
                                                              void *session_context,
                                                              const UA_NodeId *method_id,
                                                              void *method_context,
                                                              const UA_NodeId *object_id,
                                                              void *object_context)
{
    model_session_context_t *wrapped_context = (model_session_context_t *)session_context;
    const UA_Boolean base_allowed =
        g_default_get_user_executable_on_object != NULL
            ? g_default_get_user_executable_on_object(server,
                                                      ac,
                                                      session_id,
                                                      wrapped_context != NULL ? wrapped_context->base_context : NULL,
                                                      method_id,
                                                      method_context,
                                                      object_id,
                                                      object_context)
            : true;

    if (!base_allowed)
    {
        return false;
    }

    if (model_session_is_anonymous(session_context) && model_node_is_maintenance_method(method_id))
    {
        return false;
    }

    return model_session_has_role(session_context, model_required_role_for_method(method_id));
}

static UA_Byte model_get_user_access_level_wrapper(UA_Server *server,
                                                   UA_AccessControl *ac,
                                                   const UA_NodeId *session_id,
                                                   void *session_context,
                                                   const UA_NodeId *node_id,
                                                   void *node_context)
{
    model_session_context_t *wrapped_context = (model_session_context_t *)session_context;
    const UA_Byte base_access =
        g_default_get_user_access_level != NULL
            ? g_default_get_user_access_level(server,
                                              ac,
                                              session_id,
                                              wrapped_context != NULL ? wrapped_context->base_context : NULL,
                                              node_id,
                                              node_context)
            : 0u;

    if (model_session_is_anonymous(session_context))
    {
        if (model_node_is_anonymous_hidden(node_id))
        {
            return 0u;
        }

        return model_strip_anonymous_write_access(base_access);
    }

    if (node_context != NULL &&
        !model_session_has_role(session_context,
                                model_required_role_for_write_kind(((const model_node_context_t *)node_context)->kind)))
    {
        return model_strip_anonymous_write_access(base_access);
    }

    return base_access;
}

static UA_Boolean model_allow_browse_node_wrapper(UA_Server *server,
                                                  UA_AccessControl *ac,
                                                  const UA_NodeId *session_id,
                                                  void *session_context,
                                                  const UA_NodeId *node_id,
                                                  void *node_context)
{
    model_session_context_t *wrapped_context = (model_session_context_t *)session_context;
    const UA_Boolean base_allowed =
        g_default_allow_browse_node != NULL
            ? g_default_allow_browse_node(server,
                                          ac,
                                          session_id,
                                          wrapped_context != NULL ? wrapped_context->base_context : NULL,
                                          node_id,
                                          node_context)
            : true;

    if (!base_allowed)
    {
        return false;
    }

    if (model_session_is_anonymous(session_context) && model_node_is_anonymous_hidden(node_id))
    {
        return false;
    }

    return true;
}

static model_node_context_t g_fixed_node_contexts[FIXED_CTX_COUNT] = {
    {MODEL_NODE_IDENT_MANUFACTURER, NULL},
    {MODEL_NODE_IDENT_MODEL, NULL},
    {MODEL_NODE_IDENT_SERIAL_NUMBER, NULL},
    {MODEL_NODE_IDENT_HW_REVISION, NULL},
    {MODEL_NODE_IDENT_FW_REVISION, NULL},
    {MODEL_NODE_IDENT_BUILD_TIME, NULL},
    {MODEL_NODE_IDENT_GIT_HASH, NULL},
    {MODEL_NODE_IDENT_DEVICE_NAME, NULL},
    {MODEL_NODE_IDENT_PRODUCT_INSTANCE_URI, NULL},
    {MODEL_NODE_IDENT_ASSET_ID, NULL},
    {MODEL_NODE_IDENT_MAC_ADDRESS, NULL},
    {MODEL_NODE_IDENT_MANUFACTURE_DATE, NULL},
    {MODEL_NODE_IDENT_CALIBRATION_DATE, NULL},
    {MODEL_NODE_IDENT_LOCATION, NULL},
    {MODEL_NODE_IDENT_ASSET_CLASS, NULL},
    {MODEL_NODE_PROCESS_TEMPERATURE_VALUE, NULL},
    {MODEL_NODE_PROCESS_TEMPERATURE_STATUS, NULL},
    {MODEL_NODE_PROCESS_TEMPERATURE_SOURCE_TIMESTAMP, NULL},
    {MODEL_NODE_PROCESS_TEMPERATURE_UNITS, NULL},
    {MODEL_NODE_PROCESS_TEMPERATURE_UPDATE_PERIOD_MS, NULL},
    {MODEL_NODE_PROCESS_TEMPERATURE_HISTORY_INTERVAL_MS, NULL},
    {MODEL_NODE_PROCESS_TEMPERATURE_ALARM_STATE, NULL},
    {MODEL_NODE_DIAG_UPTIME_SEC, NULL},
    {MODEL_NODE_DIAG_RESET_CAUSE, NULL},
    {MODEL_NODE_DIAG_FREE_HEAP, NULL},
    {MODEL_NODE_DIAG_MIN_EVER_FREE_HEAP, NULL},
    {MODEL_NODE_DIAG_CPU_LOAD_APPROX, NULL},
    {MODEL_NODE_DIAG_LAST_FAULT_CODE, NULL},
    {MODEL_NODE_NETWORK_MAC_ADDRESS, NULL},
    {MODEL_NODE_NETWORK_IP_MODE, NULL},
    {MODEL_NODE_NETWORK_IPV4, NULL},
    {MODEL_NODE_NETWORK_MASK, NULL},
    {MODEL_NODE_NETWORK_GATEWAY, NULL},
    {MODEL_NODE_NETWORK_LINK_UP, NULL},
    {MODEL_NODE_NETWORK_DHCP_STATE, NULL},
    {MODEL_NODE_NETWORK_RECONNECT_COUNT, NULL},
    {MODEL_NODE_TIME_SYNCED, NULL},
    {MODEL_NODE_TIME_NTP_SERVER, NULL},
    {MODEL_NODE_TIME_LAST_SYNC_EPOCH, NULL},
    {MODEL_NODE_TIME_CLOCK_QUALITY, NULL},
    {MODEL_NODE_TIME_TIMEZONE_OFFSET_MINUTES, NULL},
    {MODEL_NODE_SECURITY_MODE, NULL},
    {MODEL_NODE_SECURITY_COMMISSIONING_MODE, NULL},
    {MODEL_NODE_SECURITY_TRUST_LIST_VERSION, NULL},
    {MODEL_NODE_SECURITY_TRUSTED_CLIENT_COUNT, NULL},
    {MODEL_NODE_SECURITY_LAST_AUTH_FAILURE, NULL},
    {MODEL_NODE_SECURITY_FAILED_LOGIN_COUNT, NULL},
    {MODEL_NODE_SECURITY_CERT_THUMBPRINT, NULL},
    {MODEL_NODE_SECURITY_CERT_NOT_BEFORE, NULL},
    {MODEL_NODE_SECURITY_CERT_NOT_AFTER, NULL},
    {MODEL_NODE_SECURITY_DAYS_UNTIL_EXPIRY, NULL},
    {MODEL_NODE_SECURITY_EXPIRY_STATE, NULL},
    {MODEL_NODE_SECURITY_COMMISSIONING_STATE, NULL},
    {MODEL_NODE_SECURITY_ACTIVE_IDENTITY_GENERATION, NULL},
    {MODEL_NODE_SECURITY_ACTIVE_TRUST_GENERATION, NULL},
    {MODEL_NODE_SECURITY_CURRENT_USER_ROLE, NULL},
    {MODEL_NODE_SECURITY_LAST_EVENT_ID, NULL},
    {MODEL_NODE_NETWORK_LAST_NETWORK_CHANGE, NULL},
    {MODEL_NODE_DIAG_LAST_FAULT, NULL},
    {MODEL_NODE_CONFIG_LAST_CONFIG_CHANGE, NULL},
    {MODEL_NODE_CONFIG_SAMPLING_PERIOD_MS, NULL},
    {MODEL_NODE_CONFIG_PUBLISH_PERIOD_MS, NULL},
    {MODEL_NODE_CONFIG_TEMP_OFFSET, NULL},
    {MODEL_NODE_CONFIG_TEMP_SCALE, NULL},
    {MODEL_NODE_CONFIG_HIGH_ALARM, NULL},
    {MODEL_NODE_CONFIG_HIGH_HIGH_ALARM, NULL},
    {MODEL_NODE_CONFIG_REBOOT_REQUIRED, NULL},
    {MODEL_NODE_CONFIG_COMMISSIONED, NULL},
    {MODEL_NODE_MESSAGING_EVENT_SEQUENCE, NULL},
    {MODEL_NODE_MESSAGING_EVENT_ID, NULL},
    {MODEL_NODE_MESSAGING_EVENT_TYPE, NULL},
    {MODEL_NODE_MESSAGING_EVENT_SOURCE, NULL},
    {MODEL_NODE_MESSAGING_EVENT_MESSAGE, NULL},
    {MODEL_NODE_MESSAGING_EVENT_SEVERITY, NULL},
    {MODEL_NODE_MESSAGING_EVENT_TIME, NULL}};
static model_node_context_t g_task_node_contexts[3] = {
    {MODEL_NODE_DIAG_TASK_STACK_HIGH_WATER, NULL, "OpcUaTask"},
    {MODEL_NODE_DIAG_TASK_STACK_HIGH_WATER, NULL, "NetworkTask"},
    {MODEL_NODE_DIAG_TASK_STACK_HIGH_WATER, NULL, "SensorTask"}};
#define MODEL_CHANNEL_FIELD_CONTEXT_COUNT 7u
static model_node_context_t g_channel_node_contexts[CHANNEL_MANAGER_MAX_CHANNELS][MODEL_CHANNEL_FIELD_CONTEXT_COUNT];

static UA_StatusCode model_quality_to_status(service_quality_t quality)
{
    switch (quality)
    {
    case SERVICE_QUALITY_GOOD:
        return UA_STATUSCODE_GOOD;
    case SERVICE_QUALITY_UNCERTAIN:
        return UA_STATUSCODE_UNCERTAIN;
    case SERVICE_QUALITY_BAD:
    default:
        return UA_STATUSCODE_BADNOTCONNECTED;
    }
}

static const char *model_quality_to_string(service_quality_t quality)
{
    switch (quality)
    {
    case SERVICE_QUALITY_GOOD:
        return "Good";
    case SERVICE_QUALITY_UNCERTAIN:
        return "Uncertain";
    case SERVICE_QUALITY_BAD:
    default:
        return "Bad";
    }
}

static const char *model_alarm_state_to_string(telemetry_alarm_state_t alarm_state)
{
    switch (alarm_state)
    {
    case TELEMETRY_ALARM_STATE_NORMAL:
        return "Normal";
    case TELEMETRY_ALARM_STATE_WARNING:
        return "Warning";
    case TELEMETRY_ALARM_STATE_CRITICAL:
        return "Critical";
    case TELEMETRY_ALARM_STATE_INVALID:
    default:
        return "Invalid";
    }
}

static bool model_copy_channel_by_id(const char *channel_id, telemetry_channel_t *out_channel)
{
    if (channel_id == NULL || out_channel == NULL)
    {
        return false;
    }

    return channel_manager_copy_channel(channel_id, out_channel);
}

static bool model_copy_channel_from_context(const model_node_context_t *context,
                                            telemetry_channel_t *out_channel)
{
    return context != NULL &&
           context->channel_id != NULL &&
           model_copy_channel_by_id(context->channel_id, out_channel);
}

static bool model_copy_channel_settings(const char *channel_id, telemetry_channel_settings_t *out_settings)
{
    telemetry_channel_t channel = {0};

    if (channel_id == NULL || out_settings == NULL || !model_copy_channel_by_id(channel_id, &channel))
    {
        return false;
    }

    *out_settings = channel.settings;
    return true;
}

static UA_NodeId model_channel_value_type_to_data_type(telemetry_channel_value_type_t value_type)
{
    switch (value_type)
    {
    case TELEMETRY_CHANNEL_VALUE_DOUBLE:
        return UA_TYPES[UA_TYPES_DOUBLE].typeId;
    case TELEMETRY_CHANNEL_VALUE_INT32:
        return UA_TYPES[UA_TYPES_INT32].typeId;
    case TELEMETRY_CHANNEL_VALUE_UINT32:
        return UA_TYPES[UA_TYPES_UINT32].typeId;
    case TELEMETRY_CHANNEL_VALUE_INT64:
        return UA_TYPES[UA_TYPES_INT64].typeId;
    case TELEMETRY_CHANNEL_VALUE_UINT64:
        return UA_TYPES[UA_TYPES_UINT64].typeId;
    case TELEMETRY_CHANNEL_VALUE_BOOLEAN:
        return UA_TYPES[UA_TYPES_BOOLEAN].typeId;
    case TELEMETRY_CHANNEL_VALUE_STRING:
        return UA_TYPES[UA_TYPES_STRING].typeId;
    case TELEMETRY_CHANNEL_VALUE_FLOAT:
    default:
        return UA_TYPES[UA_TYPES_FLOAT].typeId;
    }
}

static bool model_build_channel_node_id(const telemetry_channel_t *channel,
                                        const char *field_name,
                                        char *buffer,
                                        size_t buffer_size)
{
    int written = 0;

    if (channel == NULL || channel->metadata.name == NULL || buffer == NULL || buffer_size == 0u)
    {
        return false;
    }

    written = snprintf(buffer,
                       buffer_size,
                       field_name != NULL && field_name[0] != '\0'
                           ? "Device.Channels.%s.%s"
                           : "Device.Channels.%s",
                       channel->metadata.name,
                       field_name != NULL ? field_name : "");
    return written > 0 && (size_t)written < buffer_size;
}

static bool model_channel_requires_server_rebuild(const config_channel_definition_t *lhs,
                                                  const config_channel_definition_t *rhs)
{
    const uint32_t lhs_flags = lhs != NULL ? (lhs->flags & TELEMETRY_CHANNEL_FLAG_HISTORIZED) : 0u;
    const uint32_t rhs_flags = rhs != NULL ? (rhs->flags & TELEMETRY_CHANNEL_FLAG_HISTORIZED) : 0u;

    if (lhs == NULL || rhs == NULL)
    {
        return lhs != rhs;
    }

    return lhs->in_use != rhs->in_use ||
           lhs->value_type != rhs->value_type ||
           lhs_flags != rhs_flags ||
           model_string_changed(lhs->id, rhs->id) ||
           model_string_changed(lhs->name, rhs->name);
}

static bool model_byte_array_changed(const uint8_t *lhs, const uint8_t *rhs, size_t size)
{
    if (lhs == NULL || rhs == NULL)
    {
        return lhs != rhs;
    }

    return memcmp(lhs, rhs, size) != 0;
}

static bool model_config_requires_server_reboot(const device_profile_config_t *lhs,
                                                const device_profile_config_t *rhs)
{
    if (lhs == NULL || rhs == NULL)
    {
        return lhs != rhs;
    }

    if (lhs->opcua_port != rhs->opcua_port ||
        lhs->enable_mqtt_pubsub != rhs->enable_mqtt_pubsub ||
        lhs->mqtt_pubsub_use_json != rhs->mqtt_pubsub_use_json ||
        model_string_changed(lhs->device_name, rhs->device_name) ||
        model_string_changed(lhs->opcua_hostname, rhs->opcua_hostname) ||
        model_string_changed(lhs->ntp_server, rhs->ntp_server) ||
        model_string_changed(lhs->mqtt_broker_url, rhs->mqtt_broker_url) ||
        model_string_changed(lhs->mqtt_topic_root, rhs->mqtt_topic_root) ||
        model_string_changed(lhs->product_instance_uri, rhs->product_instance_uri) ||
        model_byte_array_changed(lhs->mac, rhs->mac, sizeof(lhs->mac)) ||
        lhs->network_mode != rhs->network_mode ||
        model_byte_array_changed(lhs->static_ip, rhs->static_ip, sizeof(lhs->static_ip)) ||
        model_byte_array_changed(lhs->static_netmask, rhs->static_netmask, sizeof(lhs->static_netmask)) ||
        model_byte_array_changed(lhs->static_gateway, rhs->static_gateway, sizeof(lhs->static_gateway)) ||
        model_byte_array_changed(lhs->static_dns, rhs->static_dns, sizeof(lhs->static_dns)))
    {
        return true;
    }

    for (size_t index = 0u; index < CONFIG_SERVICE_MAX_CHANNELS; ++index)
    {
        if (model_channel_requires_server_rebuild(&lhs->channels[index], &rhs->channels[index]))
        {
            return true;
        }
    }

    return false;
}

static bool model_is_reboot_required(void)
{
    const device_profile_config_t *running = config_service_get();
    const device_profile_config_t *staged = config_service_get_staged();

    if (!g_model_address_space_config_snapshot_valid || running == NULL || staged == NULL)
    {
        return false;
    }

    return model_config_requires_server_reboot(&g_model_address_space_config_snapshot, running) ||
           model_config_requires_server_reboot(running, staged);
}

static const char *model_security_mode_to_string(void)
{
    return security_service_is_commissioning_mode() ? "Commissioning" : "Strict";
}

static model_certificate_expiry_state_t model_get_certificate_expiry_state(uint32_t *out_days_until_expiry,
                                                                           bool *out_time_check_performed)
{
    time_service_status_t time_status = {0};
    uint32_t not_after_epoch_s = 0u;
    uint32_t remaining_days = 0u;

    if (out_days_until_expiry != NULL)
    {
        *out_days_until_expiry = 0u;
    }

    if (out_time_check_performed != NULL)
    {
        *out_time_check_performed = false;
    }

    if (!security_service_has_device_identity())
    {
        return MODEL_CERTIFICATE_EXPIRY_STATE_MISSING;
    }

    if (!security_service_get_device_certificate_not_after(&not_after_epoch_s) || not_after_epoch_s == 0u)
    {
        return MODEL_CERTIFICATE_EXPIRY_STATE_UNKNOWN;
    }

    time_service_get_status(&time_status);
    if (time_status.quality == SERVICE_QUALITY_BAD)
    {
        return MODEL_CERTIFICATE_EXPIRY_STATE_UNKNOWN;
    }

    if (out_time_check_performed != NULL)
    {
        *out_time_check_performed = true;
    }

    if (time_status.unix_time_s >= not_after_epoch_s)
    {
        return MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRED;
    }

    remaining_days = (not_after_epoch_s - time_status.unix_time_s + 86399u) / 86400u;

    if (out_days_until_expiry != NULL)
    {
        *out_days_until_expiry = remaining_days;
    }

    if (remaining_days <= MODEL_CERTIFICATE_EXPIRY_HIGH_WARN_DAYS)
    {
        return MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_HIGH;
    }

    if (remaining_days <= MODEL_CERTIFICATE_EXPIRY_WARN_DAYS)
    {
        return MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_MEDIUM;
    }

    return MODEL_CERTIFICATE_EXPIRY_STATE_VALID;
}

static const char *model_certificate_expiry_state_to_string(model_certificate_expiry_state_t state)
{
    switch (state)
    {
    case MODEL_CERTIFICATE_EXPIRY_STATE_MISSING:
        return "missing";
    case MODEL_CERTIFICATE_EXPIRY_STATE_UNKNOWN:
        return "unknown";
    case MODEL_CERTIFICATE_EXPIRY_STATE_VALID:
        return "valid";
    case MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_MEDIUM:
        return "expires_soon_medium";
    case MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_HIGH:
        return "expires_soon_high";
    case MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRED:
        return "expired";
    default:
        return "unknown";
    }
}

static bool model_certificate_expiry_requires_attention(model_certificate_expiry_state_t state)
{
    return state == MODEL_CERTIFICATE_EXPIRY_STATE_MISSING ||
           state == MODEL_CERTIFICATE_EXPIRY_STATE_UNKNOWN ||
           state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_MEDIUM ||
           state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_HIGH ||
           state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRED;
}

static bool model_format_certificate_expiry_notice(model_certificate_expiry_state_t state,
                                                   uint32_t days_until_expiry,
                                                   char *message,
                                                   size_t message_size,
                                                   UA_UInt16 *out_severity)
{
    if (message == NULL || message_size == 0u || out_severity == NULL)
    {
        return false;
    }

    if (state == MODEL_CERTIFICATE_EXPIRY_STATE_MISSING)
    {
        model_copy_string_safe(message, message_size, model_certificate_expiry_state_to_string(state));
        *out_severity = 900u;
        return true;
    }

    if (state == MODEL_CERTIFICATE_EXPIRY_STATE_UNKNOWN)
    {
        model_copy_string_safe(message, message_size, model_certificate_expiry_state_to_string(state));
        *out_severity = 350u;
        return true;
    }

    if (state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRED)
    {
        model_copy_string_safe(message, message_size, model_certificate_expiry_state_to_string(state));
        *out_severity = 900u;
        return true;
    }

    if (state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_HIGH)
    {
        model_copy_string_safe(message, message_size, model_certificate_expiry_state_to_string(state));
        *out_severity = 750u;
        return true;
    }

    if (state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_MEDIUM)
    {
        model_copy_string_safe(message, message_size, model_certificate_expiry_state_to_string(state));
        *out_severity = 550u;
        return true;
    }

    (void)state;
    (void)days_until_expiry;
    return false;
}

static bool model_build_certificate_expiry_notice(model_certificate_expiry_state_t state,
                                                  uint32_t days_until_expiry,
                                                  char *message,
                                                  size_t message_size,
                                                  UA_UInt16 *out_severity)
{
    if (message == NULL || message_size == 0u || out_severity == NULL)
    {
        return false;
    }

    return model_format_certificate_expiry_notice(state,
                                                  days_until_expiry,
                                                  message,
                                                  message_size,
                                                  out_severity);
}

static bool model_trust_expiry_requires_attention(const security_trust_expiry_service_report_t *report,
                                                  bool commissioning_mode)
{
    if (report == NULL)
    {
        return false;
    }

    if (report->state == SECURITY_TRUST_EXPIRY_STATE_EMPTY)
    {
        return !commissioning_mode;
    }

    if (report->state == SECURITY_TRUST_EXPIRY_STATE_UNKNOWN ||
        report->state == SECURITY_TRUST_EXPIRY_STATE_EXPIRES_SOON ||
        report->state == SECURITY_TRUST_EXPIRY_STATE_EXPIRES_SOON_HIGH ||
        report->state == SECURITY_TRUST_EXPIRY_STATE_EXPIRED ||
        report->state == SECURITY_TRUST_EXPIRY_STATE_PARSE_FAILED ||
        report->state == SECURITY_TRUST_EXPIRY_STATE_INTERNAL_ERROR)
    {
        return true;
    }

    return false;
}

static bool model_build_trust_expiry_notice(const security_trust_expiry_service_report_t *report,
                                            bool commissioning_mode,
                                            char *message,
                                            size_t message_size,
                                            UA_UInt16 *out_severity)
{
    if (report == NULL || message == NULL || message_size == 0u || out_severity == NULL)
    {
        return false;
    }

    if (!model_trust_expiry_requires_attention(report, commissioning_mode))
    {
        return false;
    }

    model_copy_string_safe(message,
                           message_size,
                           security_trust_expiry_service_state_to_string(report->state));

    if (report->state == SECURITY_TRUST_EXPIRY_STATE_EMPTY)
    {
        *out_severity = 850u;
        return true;
    }

    if (report->state == SECURITY_TRUST_EXPIRY_STATE_UNKNOWN)
    {
        *out_severity = 350u;
        return true;
    }

    if (report->state == SECURITY_TRUST_EXPIRY_STATE_EXPIRES_SOON)
    {
        *out_severity = 550u;
        return true;
    }

    if (report->state == SECURITY_TRUST_EXPIRY_STATE_EXPIRES_SOON_HIGH)
    {
        *out_severity = 750u;
        return true;
    }

    if (report->state == SECURITY_TRUST_EXPIRY_STATE_EXPIRED)
    {
        *out_severity = 880u;
        return true;
    }

    *out_severity = 820u;
    return true;
}

static bool model_trust_expiry_report_changed(const security_trust_expiry_service_report_t *lhs,
                                              const security_trust_expiry_service_report_t *rhs)
{
    if (lhs == NULL || rhs == NULL)
    {
        return false;
    }

    return lhs->state != rhs->state ||
           lhs->have_trust_list != rhs->have_trust_list ||
           lhs->parse_ok != rhs->parse_ok ||
           lhs->time_check_performed != rhs->time_check_performed ||
           lhs->trusted_client_count != rhs->trusted_client_count ||
           lhs->trusted_client_expiring_soon_count != rhs->trusted_client_expiring_soon_count ||
           lhs->trusted_client_expiring_high_count != rhs->trusted_client_expiring_high_count ||
           lhs->trusted_client_expired_count != rhs->trusted_client_expired_count;
}

static const char *model_security_commissioning_state_to_string(void)
{
    security_state_service_snapshot_t snapshot = {0};

    if (!security_state_service_get_snapshot(&snapshot))
    {
        return security_service_is_commissioning_mode() ? "commissioning_open" : "strict";
    }

    if (snapshot.pending_identity_generation > snapshot.active_identity_generation ||
        snapshot.pending_trust_generation > snapshot.active_trust_generation)
    {
        return "rotation_pending";
    }

    return snapshot.commissioning_mode ? "commissioning_open" : "strict";
}

#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
static UA_NodeId model_device_node_id(void)
{
    return UA_NODEID_STRING(1, (char *)"Device");
}

static UA_NodeId model_network_node_id(void)
{
    return UA_NODEID_STRING(1, (char *)"Device.Network");
}

static UA_NodeId model_diagnostics_node_id(void)
{
    return UA_NODEID_STRING(1, (char *)"Device.Diagnostics");
}

static UA_NodeId model_temperature_node_id(void)
{
    return UA_NODEID_STRING(1, (char *)DEVICE_CHANNEL_NODE_TEMPERATURE_1);
}

static UA_NodeId model_maintenance_node_id(void)
{
    return UA_NODEID_STRING(1, (char *)"Device.Maintenance");
}

static UA_NodeId model_configuration_node_id(void)
{
    return UA_NODEID_STRING(1, (char *)"Device.Configuration");
}

static UA_NodeId model_security_node_id(void)
{
    return UA_NODEID_STRING(1, (char *)"Device.Security");
}

static UA_NodeId model_event_type_node_id(const char *node_id)
{
    return UA_NODEID_STRING(1, (char *)(node_id != NULL ? node_id : MODEL_EVENT_TYPE_DEVICE));
}

#if defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
static UA_NodeId model_condition_type_node_id(const char *node_id)
{
    return UA_NODEID_STRING(1, (char *)(node_id != NULL ? node_id : MODEL_CONDITION_TYPE_SECURITY_FAULT));
}

static UA_NodeId model_security_condition_node_id(void)
{
    return UA_NODEID_STRING(1, (char *)MODEL_CONDITION_SECURITY_FAULT);
}

static UA_NodeId model_system_resource_condition_node_id(void)
{
    return UA_NODEID_STRING(1, (char *)MODEL_CONDITION_SYSTEM_RESOURCE);
}
#endif

static UA_StatusCode model_add_object_type(UA_Server *server,
                                           const char *node_id,
                                           const char *browse_name,
                                           const UA_NodeId parent_type_id)
{
    UA_ObjectTypeAttributes attributes = UA_ObjectTypeAttributes_default;

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browse_name);
    attributes.isAbstract = false;

    return UA_Server_addObjectTypeNode(server,
                                       UA_NODEID_STRING(1, (char *)node_id),
                                       parent_type_id,
                                       UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE),
                                       UA_QUALIFIEDNAME(1, (char *)browse_name),
                                       attributes,
                                       NULL,
                                       NULL);
}

static void model_copy_string_safe(char *buffer, size_t buffer_size, const char *text)
{
    size_t length = 0u;

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    if (text != NULL)
    {
        length = strlen(text);
        if (length >= buffer_size)
        {
            length = buffer_size - 1u;
        }
        if (length > 0u)
        {
            memcpy(buffer, text, length);
        }
    }

    buffer[length] = '\0';
}

static void model_event_id_to_hex(const UA_ByteString *event_id, char *buffer, size_t buffer_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t offset = 0u;

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    buffer[0] = '\0';
    if (event_id == NULL || event_id->data == NULL || event_id->length == 0u)
    {
        return;
    }

    for (size_t index = 0u; index < event_id->length && (offset + 2u) < buffer_size; ++index)
    {
        buffer[offset++] = hex[(event_id->data[index] >> 4u) & 0x0Fu];
        buffer[offset++] = hex[event_id->data[index] & 0x0Fu];
    }

    buffer[offset] = '\0';
}

static void model_store_last_event(model_event_last_kind_t kind,
                                   const char *event_id_hex,
                                   const char *message)
{
    model_copy_string_safe(g_last_event_state.last_event_id,
                           sizeof(g_last_event_state.last_event_id),
                           event_id_hex);

    switch (kind)
    {
    case MODEL_EVENT_LAST_KIND_CONFIG:
        model_copy_string_safe(g_last_event_state.last_config_change,
                               sizeof(g_last_event_state.last_config_change),
                               message);
        break;
    case MODEL_EVENT_LAST_KIND_NETWORK:
        model_copy_string_safe(g_last_event_state.last_network_change,
                               sizeof(g_last_event_state.last_network_change),
                               message);
        break;
    case MODEL_EVENT_LAST_KIND_FAULT:
        model_copy_string_safe(g_last_event_state.last_fault,
                               sizeof(g_last_event_state.last_fault),
                               message);
        break;
    case MODEL_EVENT_LAST_KIND_GENERIC:
    default:
        break;
    }
}

#ifdef UA_ENABLE_PUBSUB
static void model_pubsub_reset_runtime(void)
{
    UA_NodeId_clear(&g_model_pubsub.connection_id);
    UA_NodeId_clear(&g_model_pubsub.event_writer_group_id);
    for (size_t index = 0u; index < CHANNEL_MANAGER_MAX_CHANNELS; ++index)
    {
        UA_NodeId_clear(&g_model_pubsub.telemetry_writer_group_ids[index]);
    }

    memset(&g_model_pubsub, 0, sizeof(g_model_pubsub));
    memset(&g_model_pubsub_event_mirror, 0, sizeof(g_model_pubsub_event_mirror));
}

static bool model_pubsub_requested(void)
{
    return g_model_config.enable_mqtt_pubsub &&
           g_model_config.mqtt_broker_url != NULL &&
           g_model_config.mqtt_broker_url[0] != '\0' &&
           g_model_config.mqtt_topic_root != NULL &&
           g_model_config.mqtt_topic_root[0] != '\0';
}

static const char *model_pubsub_component_type_to_string(UA_PubSubComponentType type)
{
    switch (type)
    {
    case UA_PUBSUBCOMPONENT_CONNECTION:
        return "Connection";
    case UA_PUBSUBCOMPONENT_WRITERGROUP:
        return "WriterGroup";
    case UA_PUBSUBCOMPONENT_DATASETWRITER:
        return "DataSetWriter";
    case UA_PUBSUBCOMPONENT_READERGROUP:
        return "ReaderGroup";
    case UA_PUBSUBCOMPONENT_DATASETREADER:
        return "DataSetReader";
    case UA_PUBSUBCOMPONENT_PUBLISHEDDATASET:
        return "PublishedDataSet";
    case UA_PUBSUBCOMPONENT_SUBSCRIBEDDDATASET:
        return "SubscribedDataSet";
    default:
        return "Component";
    }
}

static const char *model_pubsub_state_to_string(UA_PubSubState state)
{
    switch (state)
    {
    case UA_PUBSUBSTATE_DISABLED:
        return "Disabled";
    case UA_PUBSUBSTATE_PAUSED:
        return "Paused";
    case UA_PUBSUBSTATE_OPERATIONAL:
        return "Operational";
    case UA_PUBSUBSTATE_ERROR:
        return "Error";
    case UA_PUBSUBSTATE_PREOPERATIONAL:
        return "PreOperational";
    default:
        return "Unknown";
    }
}

static void model_node_id_to_text(const UA_NodeId *node_id, char *buffer, size_t buffer_size)
{
    UA_String rendered = UA_STRING_NULL;

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    buffer[0] = '\0';
    if (node_id == NULL)
    {
        return;
    }

    if (node_id->identifierType == UA_NODEIDTYPE_STRING &&
        node_id->identifier.string.data != NULL &&
        node_id->identifier.string.length > 0u)
    {
        size_t length = node_id->identifier.string.length;
        if (length >= buffer_size)
        {
            length = buffer_size - 1u;
        }

        memcpy(buffer, node_id->identifier.string.data, length);
        buffer[length] = '\0';
        return;
    }

    if (UA_NodeId_print(node_id, &rendered) == UA_STATUSCODE_GOOD &&
        rendered.data != NULL &&
        rendered.length > 0u)
    {
        size_t length = rendered.length;
        if (length >= buffer_size)
        {
            length = buffer_size - 1u;
        }

        memcpy(buffer, rendered.data, length);
        buffer[length] = '\0';
    }

    UA_String_clear(&rendered);
}

static void model_pubsub_extract_tail(const char *text, char *buffer, size_t buffer_size)
{
    const char *tail = text;
    const char *last_dot = NULL;

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    if (text != NULL)
    {
        last_dot = strrchr(text, '.');
        if (last_dot != NULL && last_dot[1] != '\0')
        {
            tail = last_dot + 1;
        }
    }

    model_copy_string_safe(buffer, buffer_size, tail);
}

static bool model_pubsub_build_client_id(char *buffer, size_t buffer_size)
{
    const device_profile_config_t *device_config = config_service_get();
    const char *base = NULL;
    int written = 0;

    if (buffer == NULL || buffer_size == 0u)
    {
        return false;
    }

    base = (g_model_config.hostname != NULL && g_model_config.hostname[0] != '\0')
               ? g_model_config.hostname
               : ((device_config != NULL && device_config->opcua_hostname[0] != '\0')
                      ? device_config->opcua_hostname
                      : "rp2-opcua");

    written = snprintf(buffer, buffer_size, "rp2-%s", base);
    return written > 0 && (size_t)written < buffer_size;
}

static bool model_pubsub_build_topic(char *buffer,
                                     size_t buffer_size,
                                     const char *root,
                                     const char *suffix,
                                     const char *leaf)
{
    int written = 0;

    if (buffer == NULL || buffer_size == 0u || root == NULL || suffix == NULL)
    {
        return false;
    }

    written = snprintf(buffer,
                       buffer_size,
                       (leaf != NULL && leaf[0] != '\0') ? "%s/%s/%s" : "%s/%s",
                       root,
                       suffix,
                       leaf != NULL ? leaf : "");
    return written > 0 && (size_t)written < buffer_size;
}

static void model_pubsub_state_change_callback(UA_Server *server,
                                               const UA_NodeId id,
                                               UA_PubSubState state,
                                               UA_StatusCode status)
{
    UA_PubSubComponentType component_type = UA_PUBSUBCOMPONENT_CONNECTION;
    char id_text[64] = {0};
    const char *component_name = "Component";

    if (UA_Server_getPubSubComponentType(server, id, &component_type) == UA_STATUSCODE_GOOD)
    {
        component_name = model_pubsub_component_type_to_string(component_type);
    }

    model_node_id_to_text(&id, id_text, sizeof(id_text));
    printf("[OPC UA]\tPubSub %s %s -> %s (0x%x %s)\n",
           component_name,
           id_text[0] != '\0' ? id_text : "<id>",
           model_pubsub_state_to_string(state),
           status,
           UA_StatusCode_name(status));
    fflush(stdout);
}

static UA_StatusCode model_pubsub_register_mqtt_manager(UA_ServerConfig *config)
{
    UA_ConnectionManager *mqtt_manager = NULL;

    if (config == NULL || config->eventLoop == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    mqtt_manager = UA_ConnectionManager_new_MQTT(UA_STRING("mqtt connection manager"));
    if (mqtt_manager == NULL)
    {
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }

    return config->eventLoop->registerEventSource(config->eventLoop, (UA_EventSource *)mqtt_manager);
}

static void model_pubsub_update_event_mirror(const UA_NodeId *event_type_id,
                                             const char *source_name,
                                             const char *message,
                                             UA_UInt16 severity,
                                             UA_DateTime event_time,
                                             const UA_ByteString *event_id)
{
    char type_text[64] = {0};
    char event_id_hex[33] = {0};

    g_model_pubsub_event_mirror.sequence++;
    g_model_pubsub_event_mirror.time = event_time;
    g_model_pubsub_event_mirror.severity = (UA_UInt32)severity;

    model_node_id_to_text(event_type_id, type_text, sizeof(type_text));
    model_pubsub_extract_tail(type_text,
                              g_model_pubsub_event_mirror.event_type,
                              sizeof(g_model_pubsub_event_mirror.event_type));
    model_copy_string_safe(g_model_pubsub_event_mirror.source,
                           sizeof(g_model_pubsub_event_mirror.source),
                           source_name);
    model_copy_string_safe(g_model_pubsub_event_mirror.message,
                           sizeof(g_model_pubsub_event_mirror.message),
                           message);
    model_event_id_to_hex(event_id, event_id_hex, sizeof(event_id_hex));
    model_copy_string_safe(g_model_pubsub_event_mirror.event_id,
                           sizeof(g_model_pubsub_event_mirror.event_id),
                           event_id_hex);
}

static void model_pubsub_trigger_event_publish(UA_Server *server)
{
    UA_StatusCode status;

    if (server == NULL || !g_model_pubsub.configured || UA_NodeId_isNull(&g_model_pubsub.event_writer_group_id))
    {
        return;
    }

    status = UA_Server_triggerWriterGroupPublish(server, g_model_pubsub.event_writer_group_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tPubSub event publish trigger failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
    }
}

static UA_StatusCode model_pubsub_add_variable_field(UA_Server *server,
                                                     const UA_NodeId pds_id,
                                                     const UA_NodeId published_variable,
                                                     const char *alias)
{
    UA_DataSetFieldConfig field_config;
    UA_DataSetFieldResult result;

    memset(&field_config, 0, sizeof(field_config));
    field_config.dataSetFieldType = UA_PUBSUB_DATASETFIELD_VARIABLE;
    field_config.field.variable.fieldNameAlias = UA_STRING((char *)(alias != NULL ? alias : ""));
    field_config.field.variable.publishParameters.publishedVariable = published_variable;
    field_config.field.variable.publishParameters.attributeId = UA_ATTRIBUTEID_VALUE;
    field_config.field.variable.promotedField = false;

    result = UA_Server_addDataSetField(server, pds_id, &field_config, NULL);
    return result.result;
}

static UA_StatusCode model_pubsub_add_mqtt_connection(UA_Server *server)
{
    UA_PubSubConnectionConfig connection_config;
    UA_NetworkAddressUrlDataType broker_address;
    UA_KeyValuePair connection_properties[1];
    UA_String mqtt_client_id;

    memset(&connection_config, 0, sizeof(connection_config));
    connection_config.name = UA_STRING((char *)MODEL_PUBSUB_CONNECTION_NAME);
    connection_config.transportProfileUri =
        UA_STRING((char *)(g_model_pubsub.use_json ? MODEL_PUBSUB_MQTT_PROFILE_JSON : MODEL_PUBSUB_MQTT_PROFILE_UADP));
    connection_config.publisherId.idType = UA_PUBLISHERIDTYPE_STRING;
    connection_config.publisherId.id.string = UA_STRING(g_model_pubsub.client_id);

    broker_address.networkInterface = UA_STRING_NULL;
    broker_address.url = UA_STRING(g_model_pubsub.broker_url);
    UA_Variant_setScalar(&connection_config.address,
                         &broker_address,
                         &UA_TYPES[UA_TYPES_NETWORKADDRESSURLDATATYPE]);

    memset(connection_properties, 0, sizeof(connection_properties));
    mqtt_client_id = UA_STRING(g_model_pubsub.client_id);
    connection_properties[0].key = UA_QUALIFIEDNAME(0, (char *)"mqttClientId");
    UA_Variant_setScalar(&connection_properties[0].value,
                         &mqtt_client_id,
                         &UA_TYPES[UA_TYPES_STRING]);
    connection_config.connectionProperties.map = connection_properties;
    connection_config.connectionProperties.mapSize = 1u;

    return UA_Server_addPubSubConnection(server, &connection_config, &g_model_pubsub.connection_id);
}

static UA_StatusCode model_pubsub_add_writer_group(UA_Server *server,
                                                   const char *name,
                                                   const char *topic,
                                                   UA_Duration publishing_interval,
                                                   UA_UInt16 writer_group_id,
                                                   UA_NodeId *out_writer_group_id)
{
    UA_WriterGroupConfig writer_group_config;
    UA_BrokerWriterGroupTransportDataType broker_transport;
    UA_ExtensionObject transport_settings;
    UA_StatusCode status;

    memset(&writer_group_config, 0, sizeof(writer_group_config));
    writer_group_config.name = UA_STRING((char *)(name != NULL ? name : ""));
    writer_group_config.publishingInterval = publishing_interval;
    writer_group_config.writerGroupId = writer_group_id;

    if (g_model_pubsub.use_json)
    {
#ifdef UA_ENABLE_JSON_ENCODING
        UA_JsonWriterGroupMessageDataType json_message;
        UA_ExtensionObject message_settings;

        memset(&json_message, 0, sizeof(json_message));
        json_message.networkMessageContentMask =
            (UA_JsonNetworkMessageContentMask)(UA_JSONNETWORKMESSAGECONTENTMASK_NETWORKMESSAGEHEADER |
                                               UA_JSONNETWORKMESSAGECONTENTMASK_DATASETMESSAGEHEADER |
                                               UA_JSONNETWORKMESSAGECONTENTMASK_SINGLEDATASETMESSAGE |
                                               UA_JSONNETWORKMESSAGECONTENTMASK_PUBLISHERID);
        memset(&message_settings, 0, sizeof(message_settings));
        message_settings.encoding = UA_EXTENSIONOBJECT_DECODED;
        message_settings.content.decoded.type = &UA_TYPES[UA_TYPES_JSONWRITERGROUPMESSAGEDATATYPE];
        message_settings.content.decoded.data = &json_message;
        writer_group_config.encodingMimeType = UA_PUBSUB_ENCODING_JSON;
        writer_group_config.messageSettings = message_settings;
#endif
    }
    else
    {
        UA_UadpWriterGroupMessageDataType uadp_message;
        UA_ExtensionObject message_settings;

        memset(&uadp_message, 0, sizeof(uadp_message));
        uadp_message.networkMessageContentMask =
            (UA_UadpNetworkMessageContentMask)(UA_UADPNETWORKMESSAGECONTENTMASK_PUBLISHERID |
                                               UA_UADPNETWORKMESSAGECONTENTMASK_GROUPHEADER |
                                               UA_UADPNETWORKMESSAGECONTENTMASK_WRITERGROUPID |
                                               UA_UADPNETWORKMESSAGECONTENTMASK_PAYLOADHEADER);
        memset(&message_settings, 0, sizeof(message_settings));
        message_settings.encoding = UA_EXTENSIONOBJECT_DECODED;
        message_settings.content.decoded.type = &UA_TYPES[UA_TYPES_UADPWRITERGROUPMESSAGEDATATYPE];
        message_settings.content.decoded.data = &uadp_message;
        writer_group_config.encodingMimeType = UA_PUBSUB_ENCODING_UADP;
        writer_group_config.messageSettings = message_settings;
    }

    memset(&broker_transport, 0, sizeof(broker_transport));
    broker_transport.queueName = UA_STRING((char *)(topic != NULL ? topic : ""));
    broker_transport.resourceUri = UA_STRING_NULL;
    broker_transport.authenticationProfileUri = UA_STRING_NULL;
    broker_transport.requestedDeliveryGuarantee = UA_BROKERTRANSPORTQUALITYOFSERVICE_BESTEFFORT;

    memset(&transport_settings, 0, sizeof(transport_settings));
    transport_settings.encoding = UA_EXTENSIONOBJECT_DECODED;
    transport_settings.content.decoded.type = &UA_TYPES[UA_TYPES_BROKERWRITERGROUPTRANSPORTDATATYPE];
    transport_settings.content.decoded.data = &broker_transport;
    writer_group_config.transportSettings = transport_settings;

    status = UA_Server_addWriterGroup(server,
                                      g_model_pubsub.connection_id,
                                      &writer_group_config,
                                      out_writer_group_id);
    return status;
}

static UA_StatusCode model_pubsub_add_data_set_writer(UA_Server *server,
                                                      const UA_NodeId writer_group_id,
                                                      const UA_NodeId pds_id,
                                                      const char *name,
                                                      const char *topic,
                                                      UA_UInt16 writer_id)
{
    UA_DataSetWriterConfig writer_config;
    UA_BrokerDataSetWriterTransportDataType broker_transport;
    UA_ExtensionObject transport_settings;

    memset(&writer_config, 0, sizeof(writer_config));
    writer_config.name = UA_STRING((char *)(name != NULL ? name : ""));
    writer_config.dataSetWriterId = writer_id;
    writer_config.keyFrameCount = 1u;
    writer_config.dataSetName = UA_STRING((char *)(name != NULL ? name : ""));

    if (g_model_pubsub.use_json)
    {
#ifdef UA_ENABLE_JSON_ENCODING
        UA_JsonDataSetWriterMessageDataType json_message;
        UA_ExtensionObject message_settings;

        memset(&json_message, 0, sizeof(json_message));
        json_message.dataSetMessageContentMask =
            (UA_JsonDataSetMessageContentMask)(UA_JSONDATASETMESSAGECONTENTMASK_DATASETWRITERID |
                                               UA_JSONDATASETMESSAGECONTENTMASK_SEQUENCENUMBER |
                                               UA_JSONDATASETMESSAGECONTENTMASK_STATUS |
                                               UA_JSONDATASETMESSAGECONTENTMASK_TIMESTAMP);
        memset(&message_settings, 0, sizeof(message_settings));
        message_settings.encoding = UA_EXTENSIONOBJECT_DECODED;
        message_settings.content.decoded.type = &UA_TYPES[UA_TYPES_JSONDATASETWRITERMESSAGEDATATYPE];
        message_settings.content.decoded.data = &json_message;
        writer_config.messageSettings = message_settings;
#endif
    }

    memset(&broker_transport, 0, sizeof(broker_transport));
    broker_transport.queueName = UA_STRING((char *)(topic != NULL ? topic : ""));
    broker_transport.resourceUri = UA_STRING_NULL;
    broker_transport.authenticationProfileUri = UA_STRING_NULL;
    broker_transport.metaDataQueueName = UA_STRING_NULL;
    broker_transport.metaDataUpdateTime = 0.0;
    broker_transport.requestedDeliveryGuarantee = UA_BROKERTRANSPORTQUALITYOFSERVICE_BESTEFFORT;

    memset(&transport_settings, 0, sizeof(transport_settings));
    transport_settings.encoding = UA_EXTENSIONOBJECT_DECODED;
    transport_settings.content.decoded.type = &UA_TYPES[UA_TYPES_BROKERDATASETWRITERTRANSPORTDATATYPE];
    transport_settings.content.decoded.data = &broker_transport;
    writer_config.transportSettings = transport_settings;

    return UA_Server_addDataSetWriter(server, writer_group_id, pds_id, &writer_config, NULL);
}

static UA_StatusCode model_pubsub_add_telemetry_writer_for_channel(UA_Server *server,
                                                                   size_t channel_index,
                                                                   const telemetry_channel_t *channel)
{
    char pds_name[64];
    char writer_group_name[64];
    char data_set_writer_name[64];
    char topic[MODEL_PUBSUB_TOPIC_MAX_LEN];
    char value_node_id[128];
    char status_node_id[128];
    char source_timestamp_node_id[128];
    char alarm_state_node_id[128];
    UA_PublishedDataSetConfig pds_config;
    UA_AddPublishedDataSetResult pds_result;
    UA_NodeId pds_id = UA_NODEID_NULL;
    UA_NodeId writer_group_id = UA_NODEID_NULL;
    UA_StatusCode status;
    UA_Duration publishing_interval = 1000.0;

    if (server == NULL || channel == NULL || channel->metadata.name == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    if (!channel->settings.enabled || (channel->metadata.flags & TELEMETRY_CHANNEL_FLAG_ENABLED) == 0u)
    {
        return UA_STATUSCODE_GOOD;
    }

    if (!model_pubsub_build_topic(topic,
                                  sizeof(topic),
                                  g_model_pubsub.topic_root,
                                  MODEL_PUBSUB_TELEMETRY_TOPIC_SUFFIX,
                                  channel->metadata.name))
    {
        return UA_STATUSCODE_BADCONFIGURATIONERROR;
    }

    if (!model_build_channel_node_id(channel, "Value", value_node_id, sizeof(value_node_id)) ||
        !model_build_channel_node_id(channel, "Status", status_node_id, sizeof(status_node_id)) ||
        !model_build_channel_node_id(channel, "SourceTimestamp", source_timestamp_node_id, sizeof(source_timestamp_node_id)) ||
        !model_build_channel_node_id(channel, "AlarmState", alarm_state_node_id, sizeof(alarm_state_node_id)))
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    snprintf(pds_name, sizeof(pds_name), "%s.%s", MODEL_PUBSUB_TELEMETRY_PDS_PREFIX, channel->metadata.name);
    snprintf(writer_group_name, sizeof(writer_group_name), "%s.%s", MODEL_PUBSUB_TELEMETRY_WG_PREFIX, channel->metadata.name);
    snprintf(data_set_writer_name, sizeof(data_set_writer_name), "%s.%s", MODEL_PUBSUB_TELEMETRY_DSW_PREFIX, channel->metadata.name);

    memset(&pds_config, 0, sizeof(pds_config));
    pds_config.publishedDataSetType = UA_PUBSUB_DATASET_PUBLISHEDITEMS;
    pds_config.name = UA_STRING(pds_name);
    pds_result = UA_Server_addPublishedDataSet(server, &pds_config, &pds_id);
    if (pds_result.addResult != UA_STATUSCODE_GOOD)
    {
        return pds_result.addResult;
    }

    status = model_pubsub_add_variable_field(server,
                                             pds_id,
                                             UA_NODEID_STRING(1, value_node_id),
                                             "Value");
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_pubsub_add_variable_field(server,
                                                 pds_id,
                                                 UA_NODEID_STRING(1, status_node_id),
                                                 "Status");
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_pubsub_add_variable_field(server,
                                                 pds_id,
                                                 UA_NODEID_STRING(1, source_timestamp_node_id),
                                                 "SourceTimestamp");
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_pubsub_add_variable_field(server,
                                                 pds_id,
                                                 UA_NODEID_STRING(1, alarm_state_node_id),
                                                 "AlarmState");
    }
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (channel->settings.publish_period_ms > 0u)
    {
        publishing_interval = (UA_Duration)channel->settings.publish_period_ms;
    }

    status = model_pubsub_add_writer_group(server,
                                           writer_group_name,
                                           topic,
                                           publishing_interval,
                                           (UA_UInt16)(100u + channel_index),
                                           &writer_group_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_pubsub_add_data_set_writer(server,
                                              writer_group_id,
                                              pds_id,
                                              data_set_writer_name,
                                              topic,
                                              (UA_UInt16)(1000u + channel_index));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (channel_index < CHANNEL_MANAGER_MAX_CHANNELS)
    {
        g_model_pubsub.telemetry_writer_group_ids[channel_index] = writer_group_id;
        g_model_pubsub.telemetry_writer_group_count++;
    }

    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_pubsub_add_event_mirror_writer(UA_Server *server)
{
    char topic[MODEL_PUBSUB_TOPIC_MAX_LEN];
    UA_PublishedDataSetConfig pds_config;
    UA_AddPublishedDataSetResult pds_result;
    UA_NodeId pds_id = UA_NODEID_NULL;
    UA_StatusCode status = UA_STATUSCODE_GOOD;

    if (!model_pubsub_build_topic(topic,
                                  sizeof(topic),
                                  g_model_pubsub.topic_root,
                                  MODEL_PUBSUB_EVENTS_TOPIC_SUFFIX,
                                  NULL))
    {
        return UA_STATUSCODE_BADCONFIGURATIONERROR;
    }

    memset(&pds_config, 0, sizeof(pds_config));
    pds_config.publishedDataSetType = UA_PUBSUB_DATASET_PUBLISHEDITEMS;
    pds_config.name = UA_STRING((char *)MODEL_PUBSUB_EVENTS_PDS_NAME);
    pds_result = UA_Server_addPublishedDataSet(server, &pds_config, &pds_id);
    if (pds_result.addResult != UA_STATUSCODE_GOOD)
    {
        return pds_result.addResult;
    }

    status = model_pubsub_add_variable_field(server,
                                             pds_id,
                                             UA_NODEID_STRING(1, (char *)MODEL_NODE_MESSAGING_LAST_EVENT_SEQUENCE),
                                             "Sequence");
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_pubsub_add_variable_field(server,
                                                 pds_id,
                                                 UA_NODEID_STRING(1, (char *)MODEL_NODE_MESSAGING_LAST_EVENT_ID),
                                                 "EventId");
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_pubsub_add_variable_field(server,
                                                 pds_id,
                                                 UA_NODEID_STRING(1, (char *)MODEL_NODE_MESSAGING_LAST_EVENT_TYPE),
                                                 "EventType");
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_pubsub_add_variable_field(server,
                                                 pds_id,
                                                 UA_NODEID_STRING(1, (char *)MODEL_NODE_MESSAGING_LAST_EVENT_SOURCE),
                                                 "Source");
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_pubsub_add_variable_field(server,
                                                 pds_id,
                                                 UA_NODEID_STRING(1, (char *)MODEL_NODE_MESSAGING_LAST_EVENT_MESSAGE),
                                                 "Message");
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_pubsub_add_variable_field(server,
                                                 pds_id,
                                                 UA_NODEID_STRING(1, (char *)MODEL_NODE_MESSAGING_LAST_EVENT_SEVERITY),
                                                 "Severity");
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_pubsub_add_variable_field(server,
                                                 pds_id,
                                                 UA_NODEID_STRING(1, (char *)MODEL_NODE_MESSAGING_LAST_EVENT_TIME),
                                                 "Time");
    }
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_pubsub_add_writer_group(server,
                                           MODEL_PUBSUB_EVENTS_WG_NAME,
                                           topic,
                                           60000.0,
                                           500u,
                                           &g_model_pubsub.event_writer_group_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    return model_pubsub_add_data_set_writer(server,
                                            g_model_pubsub.event_writer_group_id,
                                            pds_id,
                                            MODEL_PUBSUB_EVENTS_DSW_NAME,
                                            topic,
                                            5000u);
}

static UA_StatusCode model_configure_pubsub_publishers(UA_Server *server, UA_ServerConfig *config)
{
    UA_StatusCode status = UA_STATUSCODE_GOOD;

    if (server == NULL || config == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    model_pubsub_reset_runtime();
    g_model_pubsub.enabled = model_pubsub_requested();
    if (!g_model_pubsub.enabled)
    {
        return UA_STATUSCODE_GOOD;
    }

    g_model_pubsub.use_json = g_model_config.mqtt_pubsub_use_json;
    model_copy_string_safe(g_model_pubsub.broker_url,
                           sizeof(g_model_pubsub.broker_url),
                           g_model_config.mqtt_broker_url);
    model_copy_string_safe(g_model_pubsub.topic_root,
                           sizeof(g_model_pubsub.topic_root),
                           g_model_config.mqtt_topic_root);
    if (!model_pubsub_build_client_id(g_model_pubsub.client_id, sizeof(g_model_pubsub.client_id)))
    {
        return UA_STATUSCODE_BADCONFIGURATIONERROR;
    }

    config->pubSubConfig.stateChangeCallback = model_pubsub_state_change_callback;
    config->pubSubConfig.enableDeltaFrames = false;

    status = model_pubsub_add_mqtt_connection(server);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    for (size_t channel_index = 0u; channel_index < channel_manager_count(); ++channel_index)
    {
        telemetry_channel_t *channel = channel_manager_get_by_index(channel_index);

        if (channel == NULL)
        {
            continue;
        }

        status = model_pubsub_add_telemetry_writer_for_channel(server, channel_index, channel);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
    }

    status = model_pubsub_add_event_mirror_writer(server);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    g_model_pubsub.configured = true;
    printf("[OPC UA]\tPubSub configured: broker=%s, topicRoot=%s, telemetryWriters=%u, events=yes, encoding=%s\n",
           g_model_pubsub.broker_url,
           g_model_pubsub.topic_root,
           (unsigned)g_model_pubsub.telemetry_writer_group_count,
           g_model_pubsub.use_json ? "JSON" : "UADP");
    fflush(stdout);
    return UA_STATUSCODE_GOOD;
}
#endif

static void model_queue_pending_audit_event(const char *event,
                                            const char *actor,
                                            const char *subject,
                                            bool success,
                                            const char *details)
{
    size_t slot;
    model_pending_audit_event_t *entry = NULL;

    taskENTER_CRITICAL();
    if (g_pending_audit_event_count == MODEL_AUDIT_QUEUE_CAPACITY)
    {
        g_pending_audit_event_head = (g_pending_audit_event_head + 1u) % MODEL_AUDIT_QUEUE_CAPACITY;
        --g_pending_audit_event_count;
    }

    slot = (g_pending_audit_event_head + g_pending_audit_event_count) % MODEL_AUDIT_QUEUE_CAPACITY;
    entry = &g_pending_audit_events[slot];
    memset(entry, 0, sizeof(*entry));
    model_copy_string_safe(entry->event, sizeof(entry->event), event);
    model_copy_string_safe(entry->actor, sizeof(entry->actor), actor);
    model_copy_string_safe(entry->subject, sizeof(entry->subject), subject);
    model_copy_string_safe(entry->details, sizeof(entry->details), details);
    entry->success = success;
    ++g_pending_audit_event_count;
    taskEXIT_CRITICAL();
}

static bool model_pop_pending_audit_event(model_pending_audit_event_t *out_event)
{
    bool has_event = false;

    taskENTER_CRITICAL();
    if (g_pending_audit_event_count > 0u)
    {
        if (out_event != NULL)
        {
            *out_event = g_pending_audit_events[g_pending_audit_event_head];
        }
        g_pending_audit_event_head = (g_pending_audit_event_head + 1u) % MODEL_AUDIT_QUEUE_CAPACITY;
        --g_pending_audit_event_count;
        has_event = true;
    }
    taskEXIT_CRITICAL();

    return has_event;
}

static void model_security_audit_callback(const char *event,
                                          const char *actor,
                                          const char *subject,
                                          bool success,
                                          const char *details,
                                          void *context)
{
    (void)context;
    model_queue_pending_audit_event(event, actor, subject, success, details);
}

static bool model_string_changed(const char *lhs, const char *rhs)
{
    if (lhs == NULL)
    {
        lhs = "";
    }

    if (rhs == NULL)
    {
        rhs = "";
    }

    return strcmp(lhs, rhs) != 0;
}

static UA_StatusCode model_add_event_type(UA_Server *server,
                                          const char *node_id,
                                          const char *browse_name,
                                          const UA_NodeId parent_type_id)
{
    return model_add_object_type(server, node_id, browse_name, parent_type_id);
}

static UA_StatusCode model_add_event_property(UA_Server *server,
                                              const UA_NodeId event_type_id,
                                              const char *node_id,
                                              const char *browse_name,
                                              UA_NodeId data_type)
{
    UA_VariableAttributes attributes = UA_VariableAttributes_default;
    UA_NodeId property_id = UA_NODEID_NULL;
    UA_StatusCode status;

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browse_name);
    attributes.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browse_name);
    attributes.dataType = data_type;
    attributes.valueRank = UA_VALUERANK_SCALAR;
    attributes.accessLevel = UA_ACCESSLEVELMASK_READ;
    attributes.userAccessLevel = UA_ACCESSLEVELMASK_READ;
    model_set_default_variant_value(&attributes, data_type);

    status = UA_Server_addVariableNode(server,
                                       UA_NODEID_STRING(1, (char *)node_id),
                                       event_type_id,
                                       UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
                                       UA_QUALIFIEDNAME(1, (char *)browse_name),
                                       UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE),
                                       attributes,
                                       NULL,
                                       &property_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    /* MINIMAL Namespace 0 does not instantiate the ModellingRule objects.
       Keep the custom event fields usable even when the Mandatory rule target
       is absent. */
    {
        UA_NodeClass modelling_rule_class = UA_NODECLASS_UNSPECIFIED;
        status = UA_Server_readNodeClass(server,
                                         UA_NODEID_NUMERIC(0, UA_NS0ID_MODELLINGRULE_MANDATORY),
                                         &modelling_rule_class);
        if (status != UA_STATUSCODE_GOOD)
        {
            return UA_STATUSCODE_GOOD;
        }
    }

    return UA_Server_addReference(server,
                                  property_id,
                                  UA_NODEID_NUMERIC(0, UA_NS0ID_HASMODELLINGRULE),
                                  UA_EXPANDEDNODEID_NUMERIC(0, UA_NS0ID_MODELLINGRULE_MANDATORY),
                                  true);
}

static UA_StatusCode model_set_event_string_field(UA_KeyValueMap *event_fields,
                                                  const char *browse_name,
                                                  const char *value)
{
    UA_String string_value = UA_STRING((char *)(value != NULL ? value : ""));
    return UA_KeyValueMap_setScalar(event_fields,
                                    UA_QUALIFIEDNAME(1, (char *)browse_name),
                                    &string_value,
                                    &UA_TYPES[UA_TYPES_STRING]);
}

#if defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
static const char *model_diag_fault_code_to_string(diag_fault_code_t fault_code)
{
    switch (fault_code)
    {
    case DIAG_FAULT_CODE_MALLOC_FAILED:
        return "MallocFailed";
    case DIAG_FAULT_CODE_STACK_OVERFLOW:
        return "StackOverflow";
    case DIAG_FAULT_CODE_SELF_TEST_FAILED:
        return "SelfTestFailed";
    case DIAG_FAULT_CODE_NONE:
    default:
        return "None";
    }
}

static UA_StatusCode model_enable_condition_source(UA_Server *server,
                                                   const UA_NodeId parent_id,
                                                   const UA_NodeId source_id)
{
    UA_ExpandedNodeId expanded_source = UA_EXPANDEDNODEID_NULL;
    UA_StatusCode status;

    status = UA_Server_writeEventNotifier(server,
                                          source_id,
                                          UA_EVENTNOTIFIER_SUBSCRIBE_TO_EVENT);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    expanded_source.nodeId = source_id;
    status = UA_Server_addReference(server,
                                    parent_id,
                                    UA_NODEID_NUMERIC(0, UA_NS0ID_HASNOTIFIER),
                                    expanded_source,
                                    true);
    if (status == UA_STATUSCODE_BADDUPLICATEREFERENCENOTALLOWED)
    {
        return UA_STATUSCODE_GOOD;
    }

    return status;
}

static UA_StatusCode model_add_condition_type(UA_Server *server,
                                              const char *node_id,
                                              const char *browse_name,
                                              const UA_NodeId parent_type_id)
{
    return model_add_object_type(server, node_id, browse_name, parent_type_id);
}

static UA_StatusCode model_condition_set_field_scalar(UA_Server *server,
                                                      const UA_NodeId condition_id,
                                                      const char *field_name,
                                                      const void *value,
                                                      const UA_DataType *type)
{
    UA_Variant variant;

    UA_Variant_init(&variant);
    UA_Variant_setScalar(&variant, (void *)value, type);
    return UA_Server_setConditionField(server,
                                       condition_id,
                                       &variant,
                                       UA_QUALIFIEDNAME(0, (char *)field_name));
}

static UA_StatusCode model_condition_set_message(UA_Server *server,
                                                 const UA_NodeId condition_id,
                                                 const char *message)
{
    UA_LocalizedText text =
        UA_LOCALIZEDTEXT((char *)"en-US", (char *)(message != NULL ? message : ""));
    UA_Variant variant;

    UA_Variant_init(&variant);
    UA_Variant_setScalar(&variant, &text, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);
    return UA_Server_setConditionField(server,
                                       condition_id,
                                       &variant,
                                       UA_QUALIFIEDNAME(0, (char *)"Message"));
}

static UA_StatusCode model_condition_set_two_state_id(UA_Server *server,
                                                      const UA_NodeId condition_id,
                                                      const char *field_name,
                                                      UA_Boolean state)
{
    UA_Variant variant;

    UA_Variant_init(&variant);
    UA_Variant_setScalar(&variant, &state, &UA_TYPES[UA_TYPES_BOOLEAN]);
    return UA_Server_setConditionVariableFieldProperty(server,
                                                       condition_id,
                                                       &variant,
                                                       UA_QUALIFIEDNAME(0, (char *)field_name),
                                                       UA_QUALIFIEDNAME(0, (char *)"Id"));
}

static UA_StatusCode model_condition_write_common_fields(UA_Server *server,
                                                         const UA_NodeId condition_id,
                                                         UA_UInt16 severity,
                                                         UA_StatusCode quality,
                                                         const char *message)
{
    UA_DateTime now = UA_DateTime_now();
    UA_StatusCode status;

    status = model_condition_set_field_scalar(server,
                                              condition_id,
                                              "Time",
                                              &now,
                                              &UA_TYPES[UA_TYPES_DATETIME]);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_field_scalar(server,
                                                  condition_id,
                                                  "ReceiveTime",
                                                  &now,
                                                  &UA_TYPES[UA_TYPES_DATETIME]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_field_scalar(server,
                                                  condition_id,
                                                  "Severity",
                                                  &severity,
                                                  &UA_TYPES[UA_TYPES_UINT16]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_field_scalar(server,
                                                  condition_id,
                                                  "Quality",
                                                  &quality,
                                                  &UA_TYPES[UA_TYPES_STATUSCODE]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_message(server, condition_id, message);
    }

    return status;
}

static UA_StatusCode model_condition_initialize_inactive(UA_Server *server,
                                                         const UA_NodeId condition_id,
                                                         const char *message)
{
    UA_Boolean retain = false;
    UA_StatusCode status;

    status = model_condition_write_common_fields(server,
                                                 condition_id,
                                                 100u,
                                                 UA_STATUSCODE_GOOD,
                                                 message);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_field_scalar(server,
                                                  condition_id,
                                                  "Retain",
                                                  &retain,
                                                  &UA_TYPES[UA_TYPES_BOOLEAN]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_two_state_id(server, condition_id, "AckedState", true);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_two_state_id(server, condition_id, "ActiveState", false);
    }

    return status;
}

static UA_StatusCode model_condition_activate(UA_Server *server,
                                              const UA_NodeId condition_id,
                                              UA_UInt16 severity,
                                              UA_StatusCode quality,
                                              const char *message,
                                              bool reset_ack)
{
    UA_Boolean retain = true;
    UA_StatusCode status;

    status = model_condition_write_common_fields(server,
                                                 condition_id,
                                                 severity,
                                                 quality,
                                                 message);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_field_scalar(server,
                                                  condition_id,
                                                  "Retain",
                                                  &retain,
                                                  &UA_TYPES[UA_TYPES_BOOLEAN]);
    }
    if (status == UA_STATUSCODE_GOOD && reset_ack)
    {
        status = model_condition_set_two_state_id(server, condition_id, "AckedState", false);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_two_state_id(server, condition_id, "ActiveState", true);
    }

    return status;
}

static UA_StatusCode model_condition_update_active(UA_Server *server,
                                                   const UA_NodeId condition_id,
                                                   const UA_NodeId source_id,
                                                   UA_UInt16 severity,
                                                   UA_StatusCode quality,
                                                   const char *message,
                                                   bool reset_ack)
{
    UA_Boolean retain = true;
    UA_StatusCode status;

    status = model_condition_write_common_fields(server,
                                                 condition_id,
                                                 severity,
                                                 quality,
                                                 message);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_field_scalar(server,
                                                  condition_id,
                                                  "Retain",
                                                  &retain,
                                                  &UA_TYPES[UA_TYPES_BOOLEAN]);
    }
    if (status == UA_STATUSCODE_GOOD && reset_ack)
    {
        status = model_condition_set_two_state_id(server, condition_id, "AckedState", false);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = UA_Server_triggerConditionEvent(server, condition_id, source_id, NULL);
    }

    return status;
}

static UA_StatusCode model_condition_deactivate(UA_Server *server,
                                                const UA_NodeId condition_id,
                                                const UA_NodeId source_id,
                                                const char *message)
{
    UA_Boolean retain = false;
    UA_StatusCode status;

    status = model_condition_write_common_fields(server,
                                                 condition_id,
                                                 100u,
                                                 UA_STATUSCODE_GOOD,
                                                 message);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_field_scalar(server,
                                                  condition_id,
                                                  "Retain",
                                                  &retain,
                                                  &UA_TYPES[UA_TYPES_BOOLEAN]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_two_state_id(server, condition_id, "AckedState", true);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_two_state_id(server, condition_id, "ActiveState", false);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = UA_Server_triggerConditionEvent(server, condition_id, source_id, NULL);
    }

    return status;
}

static UA_StatusCode model_create_condition_instance(UA_Server *server,
                                                     const char *condition_node_id,
                                                     const char *browse_name,
                                                     const UA_NodeId condition_type_id,
                                                     const UA_NodeId source_id,
                                                     const char *normal_message)
{
    UA_NodeId created_id = UA_NODEID_NULL;
    UA_StatusCode status;

    status = UA_Server_createCondition(server,
                                       UA_NODEID_STRING(1, (char *)condition_node_id),
                                       condition_type_id,
                                       UA_QUALIFIEDNAME(1, (char *)browse_name),
                                       source_id,
                                       UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                       &created_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    return model_condition_initialize_inactive(server, created_id, normal_message);
}

static UA_StatusCode model_build_alarm_conditions(UA_Server *server)
{
    UA_NodeClass condition_type_class = UA_NODECLASS_UNSPECIFIED;
    UA_NodeClass security_parent_class = UA_NODECLASS_UNSPECIFIED;
    UA_NodeClass system_parent_class = UA_NODECLASS_UNSPECIFIED;
    UA_StatusCode status;

    status = UA_Server_readNodeClass(server,
                                     UA_NODEID_NUMERIC(0, UA_NS0ID_CONDITIONTYPE),
                                     &condition_type_class);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tA&C disabled: ConditionType is not present in namespace zero\n");
        fflush(stdout);
        g_model_alarm_conditions_available = false;
        return UA_STATUSCODE_GOOD;
    }

    status = UA_Server_readNodeClass(server,
                                     UA_NODEID_NUMERIC(0, UA_NS0ID_SYSTEMOFFNORMALALARMTYPE),
                                     &security_parent_class);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tA&C disabled: SystemOffNormalAlarmType is not present in namespace zero\n");
        fflush(stdout);
        g_model_alarm_conditions_available = false;
        return UA_STATUSCODE_GOOD;
    }

    status = UA_Server_readNodeClass(server,
                                     UA_NODEID_NUMERIC(0, UA_NS0ID_SYSTEMDIAGNOSTICALARMTYPE),
                                     &system_parent_class);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tA&C disabled: SystemDiagnosticAlarmType is not present in namespace zero\n");
        fflush(stdout);
        g_model_alarm_conditions_available = false;
        return UA_STATUSCODE_GOOD;
    }

    status = model_enable_condition_source(server,
                                           model_device_node_id(),
                                           model_security_node_id());
    if (status != UA_STATUSCODE_GOOD)
    {
        g_model_alarm_conditions_available = false;
        return status;
    }

    status = model_enable_condition_source(server,
                                           model_device_node_id(),
                                           model_diagnostics_node_id());
    if (status != UA_STATUSCODE_GOOD)
    {
        g_model_alarm_conditions_available = false;
        return status;
    }

    status = model_add_condition_type(server,
                                      MODEL_CONDITION_TYPE_SECURITY_FAULT,
                                      "SecurityFaultConditionType",
                                      UA_NODEID_NUMERIC(0, UA_NS0ID_SYSTEMOFFNORMALALARMTYPE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_condition_type(server,
                                      MODEL_CONDITION_TYPE_SYSTEM_RESOURCE,
                                      "SystemResourceConditionType",
                                      UA_NODEID_NUMERIC(0, UA_NS0ID_SYSTEMDIAGNOSTICALARMTYPE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_create_condition_instance(server,
                                             MODEL_CONDITION_SECURITY_FAULT,
                                             "SecurityFaultCondition",
                                             model_condition_type_node_id(MODEL_CONDITION_TYPE_SECURITY_FAULT),
                                             model_security_node_id(),
                                             "Security state normal");
    if (status != UA_STATUSCODE_GOOD)
    {
        g_model_alarm_conditions_available = false;
        return status;
    }

    status = model_create_condition_instance(server,
                                             MODEL_CONDITION_SYSTEM_RESOURCE,
                                             "SystemResourceCondition",
                                             model_condition_type_node_id(MODEL_CONDITION_TYPE_SYSTEM_RESOURCE),
                                             model_diagnostics_node_id(),
                                             "System resources normal");
    if (status == UA_STATUSCODE_GOOD)
    {
        g_model_alarm_conditions_available = true;
    }
    else
    {
        g_model_alarm_conditions_available = false;
    }
    return status;
}

static model_security_condition_reason_t model_evaluate_security_condition(char *message,
                                                                          size_t message_size,
                                                                          UA_UInt16 *out_severity,
                                                                          UA_StatusCode *out_quality)
{
    const bool commissioning_mode = security_service_is_commissioning_mode();
    const uint32_t trusted_client_count = security_service_get_trusted_client_count();
    const uint32_t failed_login_count = security_service_get_failed_login_count();
    uint32_t days_until_expiry = 0u;
    bool time_check_performed = false;
    const model_certificate_expiry_state_t expiry_state =
        model_get_certificate_expiry_state(&days_until_expiry, &time_check_performed);

    (void)time_check_performed;

    if (expiry_state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRED)
    {
        model_copy_string_safe(message, message_size, "Device certificate expired");
        *out_severity = 900u;
        *out_quality = UA_STATUSCODE_BADCERTIFICATETIMEINVALID;
        return MODEL_SECURITY_CONDITION_REASON_CERT_EXPIRED;
    }

    if (expiry_state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_HIGH)
    {
        snprintf(message,
                 message_size,
                 "Device certificate expires soon (%" PRIu32 " days remaining)",
                 days_until_expiry);
        *out_severity = 750u;
        *out_quality = UA_STATUSCODE_UNCERTAIN;
        return MODEL_SECURITY_CONDITION_REASON_CERT_EXPIRES_SOON_HIGH;
    }

    if (expiry_state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_MEDIUM)
    {
        snprintf(message,
                 message_size,
                 "Device certificate approaching expiry (%" PRIu32 " days remaining)",
                 days_until_expiry);
        *out_severity = 550u;
        *out_quality = UA_STATUSCODE_UNCERTAIN;
        return MODEL_SECURITY_CONDITION_REASON_CERT_EXPIRES_SOON_MEDIUM;
    }

    if (!commissioning_mode && trusted_client_count == 0u)
    {
        model_copy_string_safe(message, message_size, "Strict mode active with an empty trust list");
        *out_severity = 850u;
        *out_quality = UA_STATUSCODE_BADSECURITYCHECKSFAILED;
        return MODEL_SECURITY_CONDITION_REASON_TRUST_LIST_EMPTY;
    }

    if (failed_login_count > 0u)
    {
        snprintf(message,
                 message_size,
                 "Authentication failures detected (%" PRIu32 ")",
                 failed_login_count);
        *out_severity = 650u;
        *out_quality = UA_STATUSCODE_BADUSERACCESSDENIED;
        return MODEL_SECURITY_CONDITION_REASON_AUTH_FAILURES;
    }

    if (commissioning_mode)
    {
        model_copy_string_safe(message, message_size, "Commissioning mode is active");
        *out_severity = 350u;
        *out_quality = UA_STATUSCODE_UNCERTAIN;
        return MODEL_SECURITY_CONDITION_REASON_COMMISSIONING;
    }

    model_copy_string_safe(message, message_size, "Security state normal");
    *out_severity = 100u;
    *out_quality = UA_STATUSCODE_GOOD;
    return MODEL_SECURITY_CONDITION_REASON_NONE;
}

static model_system_resource_condition_reason_t model_evaluate_system_resource_condition(char *message,
                                                                                         size_t message_size,
                                                                                         UA_UInt16 *out_severity,
                                                                                         UA_StatusCode *out_quality)
{
    diag_service_heap_stats_t heap_stats = {0};
    diag_service_system_stats_t system_stats = {0};
    diag_fault_code_t last_fault_code;

    diag_service_get_heap_stats(&heap_stats);
    diag_service_get_system_stats(&system_stats);
    last_fault_code = (diag_fault_code_t)system_stats.last_fault_code;

    if (last_fault_code == DIAG_FAULT_CODE_MALLOC_FAILED)
    {
        model_copy_string_safe(message, message_size, "Malloc failure detected");
        *out_severity = 950u;
        *out_quality = UA_STATUSCODE_BADOUTOFMEMORY;
        return MODEL_SYSTEM_RESOURCE_CONDITION_REASON_MALLOC_FAILURE;
    }

    if (last_fault_code == DIAG_FAULT_CODE_STACK_OVERFLOW)
    {
        model_copy_string_safe(message, message_size, "Stack overflow detected");
        *out_severity = 975u;
        *out_quality = UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
        return MODEL_SYSTEM_RESOURCE_CONDITION_REASON_STACK_OVERFLOW;
    }

    if (last_fault_code == DIAG_FAULT_CODE_SELF_TEST_FAILED)
    {
        model_copy_string_safe(message, message_size, "Self-test failed");
        *out_severity = 700u;
        *out_quality = UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
        return MODEL_SYSTEM_RESOURCE_CONDITION_REASON_SELF_TEST_FAILED;
    }

    if (heap_stats.largest_free_block_bytes < MODEL_SYSTEM_RESOURCE_LARGEST_BLOCK_WARN_BYTES)
    {
        snprintf(message,
                 message_size,
                 "Largest free heap block below safe threshold (%" PRIu32 " bytes, last fault=%s)",
                 heap_stats.largest_free_block_bytes,
                 model_diag_fault_code_to_string(last_fault_code));
        *out_severity = 700u;
        *out_quality = UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
        return MODEL_SYSTEM_RESOURCE_CONDITION_REASON_LOW_CONTIGUOUS_HEAP;
    }

    if (heap_stats.available_heap_bytes < MODEL_SYSTEM_RESOURCE_FREE_HEAP_WARN_BYTES)
    {
        snprintf(message,
                 message_size,
                 "Available heap below safe threshold (%" PRIu32 " bytes, last fault=%s)",
                 heap_stats.available_heap_bytes,
                 model_diag_fault_code_to_string(last_fault_code));
        *out_severity = 600u;
        *out_quality = UA_STATUSCODE_BADOUTOFMEMORY;
        return MODEL_SYSTEM_RESOURCE_CONDITION_REASON_LOW_FREE_HEAP;
    }

    model_copy_string_safe(message, message_size, "System resources normal");
    *out_severity = 100u;
    *out_quality = UA_STATUSCODE_GOOD;
    return MODEL_SYSTEM_RESOURCE_CONDITION_REASON_NONE;
}

static void model_sync_condition(UA_Server *server,
                                 const UA_NodeId condition_id,
                                 const UA_NodeId source_id,
                                 bool *was_active,
                                 uint32_t *was_reason,
                                 uint32_t reason,
                                 UA_UInt16 severity,
                                 UA_StatusCode quality,
                                 const char *active_message,
                                 const char *normal_message)
{
    UA_StatusCode status = UA_STATUSCODE_GOOD;

    if (reason != 0u)
    {
        if (!*was_active)
        {
            status = model_condition_activate(server,
                                              condition_id,
                                              severity,
                                              quality,
                                              active_message,
                                              true);
        }
        else if (*was_reason != reason)
        {
            status = model_condition_update_active(server,
                                                   condition_id,
                                                   source_id,
                                                   severity,
                                                   quality,
                                                   active_message,
                                                   true);
        }

        if (status == UA_STATUSCODE_GOOD)
        {
            *was_active = true;
            *was_reason = reason;
        }
    }
    else if (*was_active)
    {
        status = model_condition_deactivate(server,
                                            condition_id,
                                            source_id,
                                            normal_message);
        if (status == UA_STATUSCODE_GOOD)
        {
            *was_active = false;
            *was_reason = 0u;
        }
    }

    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tCondition sync failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
    }
}

static void model_process_alarm_conditions(UA_Server *server, model_event_runtime_state_t *state)
{
    char message[192];
    UA_UInt16 severity = 100u;
    UA_StatusCode quality = UA_STATUSCODE_GOOD;
    model_security_condition_reason_t security_reason;
    model_system_resource_condition_reason_t system_resource_reason;

    if (server == NULL || state == NULL)
    {
        return;
    }

    if (!g_model_alarm_conditions_available)
    {
        return;
    }

    security_reason = model_evaluate_security_condition(message,
                                                        sizeof(message),
                                                        &severity,
                                                        &quality);
    model_sync_condition(server,
                         model_security_condition_node_id(),
                         model_security_node_id(),
                         &state->security_condition_active,
                         &state->security_condition_reason,
                         (uint32_t)security_reason,
                         severity,
                         quality,
                         message,
                         "Security state normal");

    system_resource_reason = model_evaluate_system_resource_condition(message,
                                                                      sizeof(message),
                                                                      &severity,
                                                                      &quality);
    model_sync_condition(server,
                         model_system_resource_condition_node_id(),
                         model_diagnostics_node_id(),
                         &state->system_resource_condition_active,
                         &state->system_resource_condition_reason,
                         (uint32_t)system_resource_reason,
                         severity,
                         quality,
                         message,
                         "System resources normal");
}
#endif

static UA_StatusCode model_set_event_bool_field(UA_KeyValueMap *event_fields,
                                                const char *browse_name,
                                                UA_Boolean value)
{
    return UA_KeyValueMap_setScalar(event_fields,
                                    UA_QUALIFIEDNAME(1, (char *)browse_name),
                                    &value,
                                    &UA_TYPES[UA_TYPES_BOOLEAN]);
}

static UA_StatusCode model_set_event_uint32_field(UA_KeyValueMap *event_fields,
                                                  const char *browse_name,
                                                  UA_UInt32 value)
{
    return UA_KeyValueMap_setScalar(event_fields,
                                    UA_QUALIFIEDNAME(1, (char *)browse_name),
                                    &value,
                                    &UA_TYPES[UA_TYPES_UINT32]);
}

static UA_StatusCode model_set_event_float_field(UA_KeyValueMap *event_fields,
                                                 const char *browse_name,
                                                 UA_Float value)
{
    return UA_KeyValueMap_setScalar(event_fields,
                                    UA_QUALIFIEDNAME(1, (char *)browse_name),
                                    &value,
                                    &UA_TYPES[UA_TYPES_FLOAT]);
}

static UA_StatusCode model_write_config_event_fields(UA_KeyValueMap *event_fields,
                                                     const void *fields_context)
{
    const model_config_event_fields_t *fields = (const model_config_event_fields_t *)fields_context;
    UA_StatusCode status;

    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_set_event_string_field(event_fields, "ChangedSection", fields->changed_section);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "ChangedKey", fields->changed_key);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "Actor", fields->actor);
    }
    return status;
}

static UA_StatusCode model_write_network_event_fields(UA_KeyValueMap *event_fields,
                                                      const void *fields_context)
{
    const model_network_event_fields_t *fields = (const model_network_event_fields_t *)fields_context;
    UA_StatusCode status;

    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_set_event_string_field(event_fields, "OldIp", fields->old_ip);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "NewIp", fields->new_ip);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_bool_field(event_fields, "DhcpEnabled", fields->dhcp_enabled ? UA_TRUE : UA_FALSE);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_bool_field(event_fields, "LinkUp", fields->link_up ? UA_TRUE : UA_FALSE);
    }
    return status;
}

static UA_StatusCode model_write_sensor_fault_event_fields(UA_KeyValueMap *event_fields,
                                                           const void *fields_context)
{
    const model_sensor_fault_event_fields_t *fields = (const model_sensor_fault_event_fields_t *)fields_context;
    UA_StatusCode status;

    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_set_event_string_field(event_fields, "SensorId", fields->sensor_id);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_uint32_field(event_fields, "FaultCode", fields->fault_code);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "Quality", fields->quality);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_float_field(event_fields, "MeasuredValue", fields->measured_value);
    }
    return status;
}

static UA_StatusCode model_write_maintenance_event_fields(UA_KeyValueMap *event_fields,
                                                          const void *fields_context)
{
    const model_maintenance_event_fields_t *fields = (const model_maintenance_event_fields_t *)fields_context;
    UA_StatusCode status;

    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_set_event_string_field(event_fields, "Operation", fields->operation);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_uint32_field(event_fields, "ResultCode", fields->result_code);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "Actor", fields->actor);
    }
    return status;
}

static UA_StatusCode model_write_firmware_event_fields(UA_KeyValueMap *event_fields,
                                                       const void *fields_context)
{
    const model_firmware_event_fields_t *fields = (const model_firmware_event_fields_t *)fields_context;
    UA_StatusCode status;

    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_set_event_string_field(event_fields, "Version", fields->version);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "BuildTime", fields->build_time);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "GitHash", fields->git_hash);
    }
    return status;
}

static UA_StatusCode model_write_audit_event_fields(UA_KeyValueMap *event_fields,
                                                    const void *fields_context)
{
    const model_audit_event_fields_t *fields = (const model_audit_event_fields_t *)fields_context;
    UA_StatusCode status;

    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_set_event_string_field(event_fields, "Actor", fields->actor);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "Subject", fields->subject);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_bool_field(event_fields, "Result", fields->result ? UA_TRUE : UA_FALSE);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "Details", fields->details);
    }
    return status;
}

static UA_StatusCode model_write_audit_config_event_fields(UA_KeyValueMap *event_fields,
                                                           const void *fields_context)
{
    const model_audit_config_event_fields_t *fields = (const model_audit_config_event_fields_t *)fields_context;
    UA_StatusCode status;

    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_set_event_string_field(event_fields, "ChangedSection", fields->changed_section);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "ChangedKey", fields->changed_key);
    }
    return status;
}

static UA_StatusCode model_write_audit_security_event_fields(UA_KeyValueMap *event_fields,
                                                             const void *fields_context)
{
    const model_audit_security_event_fields_t *fields = (const model_audit_security_event_fields_t *)fields_context;
    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    return model_set_event_string_field(event_fields, "Operation", fields->operation);
}

static UA_StatusCode model_write_audit_config_emit_fields(UA_KeyValueMap *event_fields,
                                                          const void *fields_context)
{
    const model_audit_config_emit_fields_t *fields = (const model_audit_config_emit_fields_t *)fields_context;
    UA_StatusCode status;

    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_write_audit_event_fields(event_fields, &fields->audit);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_write_audit_config_event_fields(event_fields, &fields->config);
    }
    return status;
}

static UA_StatusCode model_write_audit_security_emit_fields(UA_KeyValueMap *event_fields,
                                                            const void *fields_context)
{
    const model_audit_security_emit_fields_t *fields = (const model_audit_security_emit_fields_t *)fields_context;
    UA_StatusCode status;

    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_write_audit_event_fields(event_fields, &fields->audit);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_write_audit_security_event_fields(event_fields, &fields->security);
    }
    return status;
}

static UA_StatusCode model_emit_event_ex(UA_Server *server,
                                         const UA_NodeId event_type_id,
                                         const UA_NodeId source_node_id,
                                         const char *source_name,
                                         const char *message,
                                         UA_UInt16 severity,
                                         model_event_fields_writer_t fields_writer,
                                         const void *fields_context,
                                         model_event_last_kind_t last_kind)
{
    UA_KeyValueMap event_fields = {0};
    UA_DateTime event_time = UA_DateTime_now();
    UA_String source_name_string =
        UA_STRING((char *)(source_name != NULL ? source_name : ""));
    UA_LocalizedText event_message =
        UA_LOCALIZEDTEXT((char *)"en-US", (char *)(message != NULL ? message : ""));
    UA_ByteString event_id = UA_BYTESTRING_NULL;
    char event_id_hex[33];
    UA_StatusCode status;

    if (server == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    status = UA_KeyValueMap_setScalar(&event_fields,
                                      UA_QUALIFIEDNAME(0, (char *)"Time"),
                                      &event_time,
                                      &UA_TYPES[UA_TYPES_DATETIME]);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = UA_KeyValueMap_setScalar(&event_fields,
                                          UA_QUALIFIEDNAME(0, (char *)"ReceiveTime"),
                                          &event_time,
                                          &UA_TYPES[UA_TYPES_DATETIME]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = UA_KeyValueMap_setScalar(&event_fields,
                                          UA_QUALIFIEDNAME(0, (char *)"SourceNode"),
                                          &source_node_id,
                                          &UA_TYPES[UA_TYPES_NODEID]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = UA_KeyValueMap_setScalar(&event_fields,
                                          UA_QUALIFIEDNAME(0, (char *)"SourceName"),
                                          &source_name_string,
                                          &UA_TYPES[UA_TYPES_STRING]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = fields_writer != NULL ? fields_writer(&event_fields, fields_context)
                                       : UA_STATUSCODE_GOOD;
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = UA_Server_createEvent(server,
                                       source_node_id,
                                       event_type_id,
                                       severity,
                                       event_message,
                                       &event_fields,
                                       NULL,
                                       &event_id);
    }

    if (status == UA_STATUSCODE_GOOD)
    {
        model_event_id_to_hex(&event_id, event_id_hex, sizeof(event_id_hex));
        model_store_last_event(last_kind, event_id_hex, message);
#ifdef UA_ENABLE_PUBSUB
        model_pubsub_update_event_mirror(&event_type_id,
                                         source_name,
                                         message,
                                         severity,
                                         event_time,
                                         &event_id);
        model_pubsub_trigger_event_publish(server);
#endif
    }

    UA_KeyValueMap_clear(&event_fields);
    UA_ByteString_clear(&event_id);

    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tEvent emit failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
    }

    return status;
}

static UA_StatusCode model_emit_event(UA_Server *server,
                                      const UA_NodeId event_type_id,
                                      const UA_NodeId source_node_id,
                                      const char *source_name,
                                      const char *message,
                                      UA_UInt16 severity)
{
    return model_emit_event_ex(server,
                               event_type_id,
                               source_node_id,
                               source_name,
                               message,
                               severity,
                               NULL,
                               NULL,
                               MODEL_EVENT_LAST_KIND_GENERIC);
}

static void model_emit_network_runtime_event(UA_Server *server,
                                             const char *message,
                                             UA_UInt16 severity,
                                             const char *old_ip,
                                             const char *new_ip,
                                             bool dhcp_enabled,
                                             bool link_up)
{
    const model_network_event_fields_t fields = {
        old_ip,
        new_ip,
        dhcp_enabled,
        link_up};
    (void)model_emit_event_ex(server,
                              model_event_type_node_id(MODEL_EVENT_TYPE_NETWORK),
                              model_network_node_id(),
                              "Device.Network",
                              message,
                              severity,
                              model_write_network_event_fields,
                              &fields,
                              MODEL_EVENT_LAST_KIND_NETWORK);
}

static void model_emit_sensor_runtime_event(UA_Server *server,
                                            const char *message,
                                            UA_UInt16 severity,
                                            uint32_t fault_code,
                                            const char *quality,
                                            float measured_value)
{
    const model_sensor_fault_event_fields_t fields = {
        "Temperature1",
        fault_code,
        quality,
        measured_value};
    (void)model_emit_event_ex(server,
                              model_event_type_node_id(MODEL_EVENT_TYPE_SENSOR_FAULT),
                              model_temperature_node_id(),
                              DEVICE_CHANNEL_NODE_TEMPERATURE_1,
                              message,
                              severity,
                              model_write_sensor_fault_event_fields,
                              &fields,
                              MODEL_EVENT_LAST_KIND_FAULT);
}

static void model_emit_maintenance_event(UA_Server *server,
                                         const char *message,
                                         UA_UInt16 severity,
                                         const char *operation,
                                         uint32_t result_code,
                                         const char *actor)
{
    const model_maintenance_event_fields_t fields = {operation, result_code, actor};
    (void)model_emit_event_ex(server,
                              model_event_type_node_id(MODEL_EVENT_TYPE_MAINTENANCE),
                              model_maintenance_node_id(),
                              "Device.Maintenance",
                              message,
                              severity,
                              model_write_maintenance_event_fields,
                              &fields,
                              MODEL_EVENT_LAST_KIND_GENERIC);
}

#ifdef UA_ENABLE_ENCRYPTION_MBEDTLS
static int64_t model_x509_days_from_civil(int32_t year, uint32_t month, uint32_t day)
{
    const int32_t adjusted_year = year - (month <= 2u ? 1 : 0);
    const int32_t era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    const uint32_t year_of_era = (uint32_t)(adjusted_year - era * 400);
    const uint32_t shifted_month = (month > 2u) ? (month - 3u) : (month + 9u);
    const uint32_t day_of_year = (153u * shifted_month + 2u) / 5u + day - 1u;
    const uint32_t day_of_era = year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;

    return (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
}

static bool model_x509_time_to_epoch(const mbedtls_x509_time *time, uint32_t *out_epoch_s)
{
    int64_t days;
    int64_t seconds;

    if (time == NULL || out_epoch_s == NULL ||
        time->year < 1970 ||
        time->mon < 1 || time->mon > 12 ||
        time->day < 1 || time->day > 31 ||
        time->hour < 0 || time->hour > 23 ||
        time->min < 0 || time->min > 59 ||
        time->sec < 0 || time->sec > 60)
    {
        return false;
    }

    days = model_x509_days_from_civil(time->year, (uint32_t)time->mon, (uint32_t)time->day);
    if (days < 0)
    {
        return false;
    }

    seconds = days * 86400 +
              (int64_t)time->hour * 3600 +
              (int64_t)time->min * 60 +
              (int64_t)time->sec;
    if (seconds < 0 || (uint64_t)seconds > UINT32_MAX)
    {
        return false;
    }

    *out_epoch_s = (uint32_t)seconds;
    return true;
}

static bool model_remote_certificate_thumbprint(const UA_ByteString *remote_certificate,
                                                char *buffer,
                                                size_t buffer_size)
{
    unsigned char digest[20];
    static const char hex[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F', '\0'};

    if (remote_certificate == NULL || remote_certificate->data == NULL || remote_certificate->length == 0u ||
        buffer == NULL || buffer_size < (SECURITY_SERVICE_CERT_THUMBPRINT_LEN + 1u))
    {
        return false;
    }

    if (mbedtls_sha1(remote_certificate->data, remote_certificate->length, digest) != 0)
    {
        return false;
    }

    for (size_t i = 0u; i < sizeof(digest); ++i)
    {
        buffer[i * 2u] = hex[(digest[i] >> 4u) & 0x0Fu];
        buffer[i * 2u + 1u] = hex[digest[i] & 0x0Fu];
    }
    buffer[SECURITY_SERVICE_CERT_THUMBPRINT_LEN] = '\0';
    return true;
}

static bool model_get_remote_certificate_expiry_info(const UA_ByteString *remote_certificate,
                                                     model_remote_certificate_expiry_info_t *out_info)
{
    mbedtls_x509_crt certificate;
    time_service_status_t time_status = {0};

    if (remote_certificate == NULL || remote_certificate->data == NULL || remote_certificate->length == 0u ||
        out_info == NULL)
    {
        return false;
    }

    memset(out_info, 0, sizeof(*out_info));
    out_info->state = MODEL_CERTIFICATE_EXPIRY_STATE_UNKNOWN;
    (void)model_remote_certificate_thumbprint(remote_certificate,
                                              out_info->thumbprint,
                                              sizeof(out_info->thumbprint));

    mbedtls_x509_crt_init(&certificate);
    if (mbedtls_x509_crt_parse(&certificate, remote_certificate->data, remote_certificate->length) != 0)
    {
        mbedtls_x509_crt_free(&certificate);
        return false;
    }

    if (!model_x509_time_to_epoch(&certificate.valid_to, &out_info->not_after_epoch_s) ||
        out_info->not_after_epoch_s == 0u)
    {
        mbedtls_x509_crt_free(&certificate);
        return true;
    }

    time_service_get_status(&time_status);
    if (time_status.quality == SERVICE_QUALITY_BAD)
    {
        mbedtls_x509_crt_free(&certificate);
        return true;
    }

    out_info->time_check_performed = true;
    if (time_status.unix_time_s >= out_info->not_after_epoch_s)
    {
        out_info->state = MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRED;
        mbedtls_x509_crt_free(&certificate);
        return true;
    }

    out_info->days_until_expiry = (out_info->not_after_epoch_s - time_status.unix_time_s + 86399u) / 86400u;
    if (out_info->days_until_expiry <= MODEL_CERTIFICATE_EXPIRY_HIGH_WARN_DAYS)
    {
        out_info->state = MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_HIGH;
    }
    else if (out_info->days_until_expiry <= MODEL_CERTIFICATE_EXPIRY_WARN_DAYS)
    {
        out_info->state = MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_MEDIUM;
    }
    else
    {
        out_info->state = MODEL_CERTIFICATE_EXPIRY_STATE_VALID;
    }

    mbedtls_x509_crt_free(&certificate);
    return true;
}

static void model_emit_client_certificate_expiry_notice(UA_Server *server,
                                                        const UA_ByteString *remote_certificate,
                                                        void *session_context)
{
    model_remote_certificate_expiry_info_t info = {0};
    char message[224];
    char actor[SECURITY_SERVICE_USERNAME_MAX_LEN] = {0};
    const char *actor_name;
    UA_UInt16 severity = 0u;

    if (server == NULL || !model_get_remote_certificate_expiry_info(remote_certificate, &info) ||
        info.state == MODEL_CERTIFICATE_EXPIRY_STATE_VALID ||
        info.state == MODEL_CERTIFICATE_EXPIRY_STATE_MISSING)
    {
        return;
    }

    actor_name = model_session_actor_name(session_context, actor, sizeof(actor));

    if (info.state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRED)
    {
        severity = 900u;
    }
    else if (info.state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_HIGH)
    {
        severity = 750u;
    }
    else if (info.state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_MEDIUM)
    {
        severity = 550u;
    }
    else
    {
        severity = 350u;
    }

    model_copy_string_safe(message, sizeof(message), model_certificate_expiry_state_to_string(info.state));

    model_emit_maintenance_event(server,
                                 message,
                                 severity,
                                 model_certificate_expiry_state_to_string(info.state),
                                 (uint32_t)info.state,
                                 actor_name);
    puts(message);
    fflush(stdout);
}
#endif

static void model_emit_config_changed_event(UA_Server *server,
                                            const char *message,
                                            UA_UInt16 severity,
                                            const char *changed_section,
                                            const char *changed_key,
                                            const char *actor)
{
    const model_config_event_fields_t fields = {changed_section, changed_key, actor};
    (void)model_emit_event_ex(server,
                              model_event_type_node_id(MODEL_EVENT_TYPE_CONFIG_CHANGED),
                              model_configuration_node_id(),
                              "Device.Configuration",
                              message,
                              severity,
                              model_write_config_event_fields,
                              &fields,
                              MODEL_EVENT_LAST_KIND_CONFIG);
}

static void model_emit_firmware_event(UA_Server *server,
                                      const char *message,
                                      UA_UInt16 severity)
{
    const device_profile_config_t *config = config_service_get();
    const model_firmware_event_fields_t fields = {
        config != NULL ? config->fw_revision : "",
        config != NULL ? config->build_time : "",
        config != NULL ? config->git_hash : ""};
    (void)model_emit_event_ex(server,
                              model_event_type_node_id(MODEL_EVENT_TYPE_FIRMWARE),
                              model_device_node_id(),
                              "Device",
                              message,
                              severity,
                              model_write_firmware_event_fields,
                              &fields,
                              MODEL_EVENT_LAST_KIND_GENERIC);
}

static void model_process_pending_audit_events(UA_Server *server)
{
    model_pending_audit_event_t pending = {{0}};

    while (model_pop_pending_audit_event(&pending))
    {
        model_audit_event_fields_t audit_fields = {pending.actor,
                                                   pending.subject,
                                                   pending.details,
                                                   pending.success};
        UA_NodeId event_type_id = model_event_type_node_id(MODEL_EVENT_TYPE_AUDIT_SECURITY_CHANGE);
        UA_NodeId source_node_id = model_security_node_id();
        const char *source_name = "Device.Security";
        char message[192];
        model_event_fields_writer_t writer = model_write_audit_event_fields;
        const void *writer_context = &audit_fields;

        if (strcmp(pending.event, "login") == 0)
        {
            event_type_id = model_event_type_node_id(
                pending.success ? MODEL_EVENT_TYPE_AUDIT_LOGIN_SUCCESS : MODEL_EVENT_TYPE_AUDIT_LOGIN_FAILURE);
            snprintf(message,
                     sizeof(message),
                     "Login %s for %s",
                     pending.success ? "succeeded" : "failed",
                     pending.actor[0] != '\0' ? pending.actor : "unknown");
        }
        else if (strcmp(pending.event, "config-save") == 0)
        {
            const model_audit_config_emit_fields_t config_fields = {
                {pending.actor, pending.subject, pending.details, pending.success},
                {"Configuration", "*"}};
            event_type_id = model_event_type_node_id(MODEL_EVENT_TYPE_AUDIT_CONFIG_CHANGE);
            source_node_id = model_configuration_node_id();
            source_name = "Device.Configuration";
            writer = model_write_audit_config_emit_fields;
            writer_context = &config_fields;
            snprintf(message,
                     sizeof(message),
                     "Configuration %s",
                     pending.success ? "saved" : "save failed");
            (void)model_emit_event_ex(server,
                                      event_type_id,
                                      source_node_id,
                                      source_name,
                                      message,
                                      pending.success ? 250u : 700u,
                                      writer,
                                      writer_context,
                                      MODEL_EVENT_LAST_KIND_GENERIC);
            continue;
        }
        else
        {
            const model_audit_security_emit_fields_t security_fields = {
                {pending.actor, pending.subject, pending.details, pending.success},
                {pending.event}};
            if (strcmp(pending.event, "reboot") == 0 || strcmp(pending.event, "factory-reset") == 0)
            {
                source_node_id = model_maintenance_node_id();
                source_name = "Device.Maintenance";
            }
            writer = model_write_audit_security_emit_fields;
            writer_context = &security_fields;
            snprintf(message,
                     sizeof(message),
                     "%s %s",
                     pending.event[0] != '\0' ? pending.event : "audit",
                     pending.success ? "succeeded" : "failed");
            (void)model_emit_event_ex(server,
                                      event_type_id,
                                      source_node_id,
                                      source_name,
                                      message,
                                      pending.success ? 250u : 700u,
                                      writer,
                                      writer_context,
                                      MODEL_EVENT_LAST_KIND_GENERIC);
            continue;
        }

        (void)model_emit_event_ex(server,
                                  event_type_id,
                                  source_node_id,
                                  source_name,
                                  message,
                                  pending.success ? 250u : 700u,
                                  writer,
                                  writer_context,
                                  MODEL_EVENT_LAST_KIND_GENERIC);
    }
}

static void model_event_runtime_state_init(model_event_runtime_state_t *state)
{
    if (state == NULL)
    {
        return;
    }

    memset(state, 0, sizeof(*state));
    network_manager_get_status(&state->network_status);
    sensor_manager_get_snapshot(&state->sensor_snapshot);
    alarm_service_get_status(&state->alarm_status);
#if defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
    state->security_condition_active = false;
    state->security_condition_reason = 0u;
    state->system_resource_condition_active = false;
    state->system_resource_condition_reason = 0u;
#endif
    state->initialized = true;
}

static void model_process_certificate_expiry_runtime(UA_Server *server,
                                                     model_event_runtime_state_t *state,
                                                     char *message,
                                                     size_t message_size)
{
    model_certificate_expiry_state_t current_state;
    uint32_t days_until_expiry = 0u;
    UA_UInt16 severity = 0u;
    bool current_attention;
    bool previous_attention;

    if (server == NULL || state == NULL || message == NULL || message_size == 0u)
    {
        return;
    }

    current_state = model_get_certificate_expiry_state(&days_until_expiry, NULL);
    current_attention = model_certificate_expiry_requires_attention(current_state);
    previous_attention = state->server_certificate_expiry_seen &&
                         model_certificate_expiry_requires_attention(state->server_certificate_expiry_state);

    if (!state->server_certificate_expiry_seen)
    {
        state->server_certificate_expiry_seen = true;
        state->server_certificate_expiry_state = current_state;
        if (current_attention)
        {
            if (model_build_certificate_expiry_notice(current_state,
                                                      days_until_expiry,
                                                      message,
                                                      message_size,
                                                      &severity))
            {
                model_emit_maintenance_event(server,
                                             message,
                                             severity,
                                             model_certificate_expiry_state_to_string(current_state),
                                             (uint32_t)current_state,
                                             model_security_commissioning_state_to_string());
                puts(message);
                fflush(stdout);
            }
        }
        return;
    }

    if (state->server_certificate_expiry_state == current_state)
    {
        return;
    }

    state->server_certificate_expiry_state = current_state;
    if (current_attention &&
        model_build_certificate_expiry_notice(current_state,
                                              days_until_expiry,
                                              message,
                                              message_size,
                                              &severity))
    {
        model_emit_maintenance_event(server,
                                     message,
                                     severity,
                                     model_certificate_expiry_state_to_string(current_state),
                                     (uint32_t)current_state,
                                     model_security_commissioning_state_to_string());
        puts(message);
        fflush(stdout);
    }
    else if (previous_attention)
    {
        model_copy_string_safe(message, message_size, model_certificate_expiry_state_to_string(current_state));
        model_emit_maintenance_event(server,
                                     message,
                                     200u,
                                     model_certificate_expiry_state_to_string(current_state),
                                     (uint32_t)current_state,
                                     model_security_commissioning_state_to_string());
        puts(message);
        fflush(stdout);
    }
}

static void model_process_trust_expiry_runtime(UA_Server *server,
                                               model_event_runtime_state_t *state,
                                               char *message,
                                               size_t message_size)
{
    security_trust_expiry_service_report_t report = {0};
    UA_UInt16 severity = 0u;
    const bool commissioning_mode = security_service_is_commissioning_mode();
    bool current_attention;
    bool previous_attention;

    if (server == NULL || state == NULL || message == NULL || message_size == 0u)
    {
        return;
    }

    if (!security_trust_expiry_service_get_report(&report))
    {
        return;
    }

    current_attention = model_trust_expiry_requires_attention(&report, commissioning_mode);
    previous_attention = state->trust_expiry_seen &&
                         model_trust_expiry_requires_attention(&state->trust_expiry_report,
                                                               state->trust_expiry_commissioning_mode);

    if (!state->trust_expiry_seen)
    {
        state->trust_expiry_seen = true;
        state->trust_expiry_report = report;
        state->trust_expiry_commissioning_mode = commissioning_mode;
        if (current_attention &&
            model_build_trust_expiry_notice(&report,
                                            commissioning_mode,
                                            message,
                                            message_size,
                                            &severity))
        {
            model_emit_maintenance_event(server,
                                         message,
                                         severity,
                                         security_trust_expiry_service_state_to_string(report.state),
                                         (uint32_t)report.state,
                                         model_security_commissioning_state_to_string());
            puts(message);
            fflush(stdout);
        }
        return;
    }

    if (!model_trust_expiry_report_changed(&state->trust_expiry_report, &report) &&
        state->trust_expiry_commissioning_mode == commissioning_mode)
    {
        return;
    }

    state->trust_expiry_report = report;
    state->trust_expiry_commissioning_mode = commissioning_mode;

    if (current_attention &&
        model_build_trust_expiry_notice(&report,
                                        commissioning_mode,
                                        message,
                                        message_size,
                                        &severity))
    {
        model_emit_maintenance_event(server,
                                     message,
                                     severity,
                                     security_trust_expiry_service_state_to_string(report.state),
                                     (uint32_t)report.state,
                                     model_security_commissioning_state_to_string());
        puts(message);
        fflush(stdout);
    }
    else if (previous_attention)
    {
        model_copy_string_safe(message,
                               message_size,
                               security_trust_expiry_service_state_to_string(report.state));
        model_emit_maintenance_event(server,
                                     message,
                                     200u,
                                     security_trust_expiry_service_state_to_string(report.state),
                                     (uint32_t)report.state,
                                     model_security_commissioning_state_to_string());
        puts(message);
        fflush(stdout);
    }
}

static void model_process_runtime_events(UA_Server *server, model_event_runtime_state_t *state)
{
    network_manager_status_t current_network = {0};
    sensor_manager_snapshot_t current_sensor = {0};
    alarm_service_status_t current_alarm = {0};
    char message[160];

    if (server == NULL || state == NULL)
    {
        return;
    }

    if (!state->initialized)
    {
        model_event_runtime_state_init(state);
        return;
    }

    network_manager_get_status(&current_network);
    sensor_manager_get_snapshot(&current_sensor);
    alarm_service_get_status(&current_alarm);

    if (model_string_changed(state->network_status.ip_mode, current_network.ip_mode))
    {
        snprintf(message,
                 sizeof(message),
                 "Network mode changed: %s -> %s",
                 state->network_status.ip_mode,
                 current_network.ip_mode);
        model_emit_network_runtime_event(server,
                                         message,
                                         400u,
                                         state->network_status.ipv4_address,
                                         current_network.ipv4_address,
                                         strcmp(current_network.ip_mode, "DHCP") == 0,
                                         current_network.link_up);
    }

    if (model_string_changed(state->network_status.ipv4_address, current_network.ipv4_address))
    {
        snprintf(message,
                 sizeof(message),
                 "IPv4 changed: %s -> %s",
                 state->network_status.ipv4_address,
                 current_network.ipv4_address);
        model_emit_network_runtime_event(server,
                                         message,
                                         350u,
                                         state->network_status.ipv4_address,
                                         current_network.ipv4_address,
                                         strcmp(current_network.ip_mode, "DHCP") == 0,
                                         current_network.link_up);
    }

    if (state->network_status.link_up != current_network.link_up)
    {
        snprintf(message,
                 sizeof(message),
                 "Link state changed: %s",
                 current_network.link_up ? "up" : "down");
        model_emit_network_runtime_event(server,
                                         message,
                                         current_network.link_up ? 250u : 700u,
                                         state->network_status.ipv4_address,
                                         current_network.ipv4_address,
                                         strcmp(current_network.ip_mode, "DHCP") == 0,
                                         current_network.link_up);
    }

    if (current_sensor.quality != state->sensor_snapshot.quality)
    {
        snprintf(message,
                 sizeof(message),
                 "Sensor quality changed: %s -> %s",
                 model_quality_to_string(state->sensor_snapshot.quality),
                 model_quality_to_string(current_sensor.quality));
        model_emit_sensor_runtime_event(server,
                                        message,
                                        current_sensor.quality == SERVICE_QUALITY_GOOD ? 250u : 800u,
                                        current_sensor.quality == SERVICE_QUALITY_GOOD ? 0u : 1001u,
                                        model_quality_to_string(current_sensor.quality),
                                        current_sensor.temperature_c);
    }

    if (current_alarm.state != state->alarm_status.state)
    {
        snprintf(message,
                 sizeof(message),
                 "%s alarm changed: %s -> %s",
                 current_alarm.channel_name != NULL ? current_alarm.channel_name : "Channel",
                 model_alarm_state_to_string(state->alarm_status.state),
                 model_alarm_state_to_string(current_alarm.state));
        model_emit_sensor_runtime_event(server,
                                        message,
                                        current_alarm.state == TELEMETRY_ALARM_STATE_NORMAL ? 250u :
                                        current_alarm.state == TELEMETRY_ALARM_STATE_WARNING ? 650u : 850u,
                                        current_alarm.state == TELEMETRY_ALARM_STATE_WARNING ? 2001u :
                                        current_alarm.state == TELEMETRY_ALARM_STATE_CRITICAL ? 2002u : 0u,
                                        model_quality_to_string(current_alarm.quality),
                                        current_sensor.temperature_c);
    }

    model_process_certificate_expiry_runtime(server, state, message, sizeof(message));
    model_process_trust_expiry_runtime(server, state, message, sizeof(message));

#if defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
    model_process_alarm_conditions(server, state);
#endif
    model_process_pending_audit_events(server);

    state->network_status = current_network;
    state->sensor_snapshot = current_sensor;
    state->alarm_status = current_alarm;
}
#else
typedef struct
{
    bool initialized;
} model_event_runtime_state_t;

static UA_StatusCode model_add_event_type(UA_Server *server,
                                          const char *node_id,
                                          const char *browse_name,
                                          const UA_NodeId parent_type_id)
{
    (void)server;
    (void)node_id;
    (void)browse_name;
    (void)parent_type_id;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_emit_event(UA_Server *server,
                                      const UA_NodeId event_type_id,
                                      const UA_NodeId source_node_id,
                                      const char *source_name,
                                      const char *message,
                                      UA_UInt16 severity)
{
    (void)server;
    (void)event_type_id;
    (void)source_node_id;
    (void)source_name;
    (void)message;
    (void)severity;
    return UA_STATUSCODE_GOOD;
}

static void model_process_runtime_events(UA_Server *server, model_event_runtime_state_t *state)
{
    (void)server;
    (void)state;
}
#endif

static void model_apply_now_source_timestamp(UA_Boolean include_source_timestamp, UA_DataValue *value)
{
    if (!include_source_timestamp)
    {
        return;
    }

    value->hasSourceTimestamp = true;
    value->sourceTimestamp = UA_DateTime_now();
}

static void model_apply_epoch_source_timestamp(UA_Boolean include_source_timestamp, uint32_t epoch_seconds, UA_DataValue *value)
{
    if (!include_source_timestamp)
    {
        return;
    }

    value->hasSourceTimestamp = true;
    if (epoch_seconds > 0u)
    {
        value->sourceTimestamp = UA_DateTime_fromUnixTime((UA_Int64)epoch_seconds);
    }
    else
    {
        value->sourceTimestamp = UA_DateTime_now();
    }
}

static UA_StatusCode model_set_uint32_value(UA_DataValue *value, UA_UInt32 data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_UINT32]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_int32_value(UA_DataValue *value, UA_Int32 data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_INT32]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_float_value(UA_DataValue *value, UA_Float data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_FLOAT]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_double_value(UA_DataValue *value, UA_Double data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_DOUBLE]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_boolean_value(UA_DataValue *value, UA_Boolean data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_BOOLEAN]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_uint64_value(UA_DataValue *value, UA_UInt64 data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_UINT64]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_int64_value(UA_DataValue *value, UA_Int64 data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_INT64]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_int16_value(UA_DataValue *value, UA_Int16 data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_INT16]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_string_value(UA_DataValue *value, const char *data, UA_StatusCode status)
{
    UA_String string_value = UA_STRING((char *)(data != NULL ? data : ""));
    UA_Variant_setScalarCopy(&value->value, &string_value, &UA_TYPES[UA_TYPES_STRING]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_datetime_value(UA_DataValue *value, UA_DateTime data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_DATETIME]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

static void model_set_default_variant_value(UA_VariableAttributes *attributes, UA_NodeId data_type)
{
    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_BOOLEAN].typeId))
    {
        UA_Boolean default_value = false;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_BOOLEAN]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_UINT32].typeId))
    {
        UA_UInt32 default_value = 0u;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_UINT32]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_INT32].typeId))
    {
        UA_Int32 default_value = 0;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_INT32]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_INT16].typeId))
    {
        UA_Int16 default_value = 0;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_INT16]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_FLOAT].typeId))
    {
        UA_Float default_value = 0.0f;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_FLOAT]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_DOUBLE].typeId))
    {
        UA_Double default_value = 0.0;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_DOUBLE]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_INT64].typeId))
    {
        UA_Int64 default_value = 0;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_INT64]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_UINT64].typeId))
    {
        UA_UInt64 default_value = 0u;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_UINT64]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_DATETIME].typeId))
    {
        UA_DateTime default_value = 0;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_DATETIME]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_STRING].typeId))
    {
        UA_String default_value = UA_STRING((char *)"");
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_STRING]);
    }
}

static UA_StatusCode model_set_channel_runtime_value(UA_DataValue *value,
                                                     const telemetry_channel_t *channel,
                                                     UA_StatusCode status)
{
    if (value == NULL || channel == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    switch (channel->metadata.value_type)
    {
    case TELEMETRY_CHANNEL_VALUE_DOUBLE:
        return model_set_double_value(value, channel->runtime.value.f64, status);
    case TELEMETRY_CHANNEL_VALUE_INT32:
        return model_set_int32_value(value, channel->runtime.value.i32, status);
    case TELEMETRY_CHANNEL_VALUE_UINT32:
        return model_set_uint32_value(value, channel->runtime.value.u32, status);
    case TELEMETRY_CHANNEL_VALUE_INT64:
        return model_set_int64_value(value, (UA_Int64)channel->runtime.value.i64, status);
    case TELEMETRY_CHANNEL_VALUE_UINT64:
        return model_set_uint64_value(value, (UA_UInt64)channel->runtime.value.u64, status);
    case TELEMETRY_CHANNEL_VALUE_BOOLEAN:
        return model_set_boolean_value(value, channel->runtime.value.boolean, status);
    case TELEMETRY_CHANNEL_VALUE_STRING:
        return model_set_string_value(value, channel->runtime.value.string, status);
    case TELEMETRY_CHANNEL_VALUE_FLOAT:
    default:
        return model_set_float_value(value, channel->runtime.value.f32, status);
    }
}

#ifdef UA_ENABLE_HISTORIZING
static const char *model_find_history_channel_id(const UA_NodeId *node_id)
{
    char channel_value_node_id[128];

    if (node_id == NULL)
    {
        return NULL;
    }

    for (size_t channel_index = 0u; channel_index < channel_manager_count(); ++channel_index)
    {
        telemetry_channel_t *channel = channel_manager_get_by_index(channel_index);
        UA_NodeId channel_value_node;

        if (channel == NULL ||
            (channel->metadata.flags & TELEMETRY_CHANNEL_FLAG_HISTORIZED) == 0u ||
            !model_build_channel_node_id(channel, "Value", channel_value_node_id, sizeof(channel_value_node_id)))
        {
            continue;
        }

        channel_value_node = UA_NODEID_STRING(1, channel_value_node_id);
        if (UA_NodeId_equal(node_id, &channel_value_node))
        {
            return channel->metadata.id;
        }
    }

    return NULL;
}

static uint32_t model_datetime_to_epoch_seconds(UA_DateTime value)
{
    if (value <= 0)
    {
        return 0u;
    }

    return (uint32_t)UA_DateTime_toUnixTime(value);
}

static void model_fill_history_data_value(const history_service_channel_sample_t *sample,
                                          UA_TimestampsToReturn timestamps_to_return,
                                          UA_DataValue *value)
{
    const UA_DateTime timestamp = UA_DateTime_fromUnixTime((UA_Int64)sample->source_timestamp_epoch_s);

    UA_DataValue_init(value);
    UA_Variant_setScalarCopy(&value->value, &sample->value, &UA_TYPES[UA_TYPES_FLOAT]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = model_quality_to_status(sample->quality);

    if (timestamps_to_return == UA_TIMESTAMPSTORETURN_SOURCE ||
        timestamps_to_return == UA_TIMESTAMPSTORETURN_BOTH)
    {
        value->hasSourceTimestamp = true;
        value->sourceTimestamp = timestamp;
    }

    if (timestamps_to_return == UA_TIMESTAMPSTORETURN_SERVER ||
        timestamps_to_return == UA_TIMESTAMPSTORETURN_BOTH)
    {
        value->hasServerTimestamp = true;
        value->serverTimestamp = timestamp;
    }
}

static void model_history_database_clear(UA_HistoryDatabase *history_database)
{
    (void)history_database;
}

static void model_history_read_raw(UA_Server *server,
                                   void *history_database_context,
                                   const UA_NodeId *session_id,
                                   void *session_context,
                                   const UA_RequestHeader *request_header,
                                   const UA_ReadRawModifiedDetails *history_read_details,
                                   UA_TimestampsToReturn timestamps_to_return,
                                   UA_Boolean release_continuation_points,
                                   size_t nodes_to_read_size,
                                   const UA_HistoryReadValueId *nodes_to_read,
                                   UA_HistoryReadResponse *response,
                                   UA_HistoryData *const *const history_data)
{
    history_service_channel_sample_t sample_buffer[HISTORY_SERVICE_CHANNEL_CAPACITY];
    const uint32_t start_epoch_s = history_read_details != NULL
                                       ? model_datetime_to_epoch_seconds(history_read_details->startTime)
                                       : 0u;
    const uint32_t end_epoch_s = history_read_details != NULL
                                     ? model_datetime_to_epoch_seconds(history_read_details->endTime)
                                     : 0u;
    const bool descending = history_read_details != NULL &&
                            history_read_details->startTime > history_read_details->endTime;
    const size_t max_values_per_node = (history_read_details == NULL ||
                                        history_read_details->numValuesPerNode == 0u ||
                                        history_read_details->numValuesPerNode > HISTORY_SERVICE_CHANNEL_CAPACITY)
                                           ? HISTORY_SERVICE_CHANNEL_CAPACITY
                                           : (size_t)history_read_details->numValuesPerNode;
    (void)server;
    (void)history_database_context;
    (void)session_id;
    (void)session_context;
    (void)request_header;
    (void)release_continuation_points;

    for (size_t index = 0u; index < nodes_to_read_size; ++index)
    {
        UA_HistoryData *result_history_data = history_data[index];
        size_t sample_count = 0u;
        bool truncated = false;

        if (result_history_data == NULL)
        {
            response->results[index].statusCode = UA_STATUSCODE_BADOUTOFMEMORY;
            continue;
        }

        if (nodes_to_read[index].continuationPoint.length > 0u)
        {
            response->results[index].statusCode = UA_STATUSCODE_BADCONTINUATIONPOINTINVALID;
            continue;
        }

        if (nodes_to_read[index].indexRange.length > 0u)
        {
            response->results[index].statusCode = UA_STATUSCODE_BADINDEXRANGENODATA;
            continue;
        }

        const char *history_channel_id = model_find_history_channel_id(&nodes_to_read[index].nodeId);

        if (history_channel_id == NULL)
        {
            response->results[index].statusCode = UA_STATUSCODE_BADHISTORYOPERATIONUNSUPPORTED;
            continue;
        }

        sample_count = history_service_read_channel_samples(history_channel_id,
                                                            start_epoch_s,
                                                            end_epoch_s,
                                                            descending,
                                                            max_values_per_node,
                                                            sample_buffer,
                                                            HISTORY_SERVICE_CHANNEL_CAPACITY,
                                                            &truncated);

        if (sample_count > 0u)
        {
            result_history_data->dataValues =
                (UA_DataValue *)UA_Array_new(sample_count, &UA_TYPES[UA_TYPES_DATAVALUE]);
            if (result_history_data->dataValues == NULL)
            {
                response->results[index].statusCode = UA_STATUSCODE_BADOUTOFMEMORY;
                result_history_data->dataValuesSize = 0u;
                continue;
            }

            result_history_data->dataValuesSize = sample_count;
            for (size_t sample_index = 0u; sample_index < sample_count; ++sample_index)
            {
                model_fill_history_data_value(&sample_buffer[sample_index],
                                              timestamps_to_return,
                                              &result_history_data->dataValues[sample_index]);
            }
        }

        (void)truncated;
        response->results[index].statusCode = UA_STATUSCODE_GOOD;
    }
}
#endif

static UA_StatusCode model_read_value(UA_Server *server,
                                      const UA_NodeId *session_id,
                                      void *session_context,
                                      const UA_NodeId *node_id,
                                      void *node_context,
                                      UA_Boolean include_source_timestamp,
                                      const UA_NumericRange *range,
                                      UA_DataValue *value)
{
    model_node_context_t *context = (model_node_context_t *)node_context;
    const device_profile_config_t *config = config_service_get();
    (void)server;
    (void)session_id;
    (void)session_context;
    (void)node_id;

    if (context == NULL)
    {
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADINTERNALERROR;
        return UA_STATUSCODE_GOOD;
    }

    if (range != NULL)
    {
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADINDEXRANGEINVALID;
        return UA_STATUSCODE_GOOD;
    }

    if (model_session_is_anonymous(session_context) && model_node_is_anonymous_hidden(node_id))
    {
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADUSERACCESSDENIED;
        return UA_STATUSCODE_GOOD;
    }

    switch (context->kind)
    {
    case MODEL_NODE_IDENT_MANUFACTURER:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->manufacturer, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_MODEL:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->model, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_SERIAL_NUMBER:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->serial_number, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_HW_REVISION:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->hw_revision, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_FW_REVISION:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->fw_revision, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_BUILD_TIME:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->build_time, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_GIT_HASH:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->git_hash, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_DEVICE_NAME:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->device_name, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_PRODUCT_INSTANCE_URI:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->product_instance_uri, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_ASSET_ID:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->asset_id, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_MAC_ADDRESS:
    {
        char mac_address[18] = {0};
        model_format_mac_string(config->mac, mac_address, sizeof(mac_address));
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, mac_address, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_IDENT_MANUFACTURE_DATE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_datetime_value(value, (UA_DateTime)config->manufacture_date_utc, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_CALIBRATION_DATE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_datetime_value(value, (UA_DateTime)config->calibration_date_utc, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_LOCATION:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->location, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_ASSET_CLASS:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->asset_class, UA_STATUSCODE_GOOD);
    case MODEL_NODE_PROCESS_TEMPERATURE_VALUE:
    {
        telemetry_channel_t channel = {0};

        if (!model_copy_channel_from_context(context, &channel))
        {
            value->hasStatus = true;
            value->status = UA_STATUSCODE_BADNODEIDUNKNOWN;
            return UA_STATUSCODE_GOOD;
        }

        model_apply_epoch_source_timestamp(include_source_timestamp, channel.runtime.source_timestamp_epoch_s, value);
        return model_set_channel_runtime_value(value, &channel, model_quality_to_status(channel.runtime.quality));
    }
    case MODEL_NODE_PROCESS_TEMPERATURE_STATUS:
    {
        telemetry_channel_t channel = {0};

        if (!model_copy_channel_from_context(context, &channel))
        {
            value->hasStatus = true;
            value->status = UA_STATUSCODE_BADNODEIDUNKNOWN;
            return UA_STATUSCODE_GOOD;
        }

        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value,
                                      model_quality_to_string(channel.runtime.quality),
                                      model_quality_to_status(channel.runtime.quality));
    }
    case MODEL_NODE_PROCESS_TEMPERATURE_SOURCE_TIMESTAMP:
    {
        telemetry_channel_t channel = {0};
        UA_DateTime timestamp = 0;

        if (!model_copy_channel_from_context(context, &channel))
        {
            value->hasStatus = true;
            value->status = UA_STATUSCODE_BADNODEIDUNKNOWN;
            return UA_STATUSCODE_GOOD;
        }

        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (channel.runtime.source_timestamp_epoch_s > 0u)
        {
            timestamp = UA_DateTime_fromUnixTime((UA_Int64)channel.runtime.source_timestamp_epoch_s);
        }
        return model_set_datetime_value(value, timestamp, model_quality_to_status(channel.runtime.quality));
    }
    case MODEL_NODE_PROCESS_TEMPERATURE_UNITS:
    {
        telemetry_channel_t channel = {0};

        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_from_context(context, &channel) &&
            channel.metadata.unit != NULL &&
            channel.metadata.unit[0] != '\0')
        {
            return model_set_string_value(value, channel.metadata.unit, UA_STATUSCODE_GOOD);
        }
        return model_set_string_value(value, "", UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_PROCESS_TEMPERATURE_UPDATE_PERIOD_MS:
    {
        telemetry_channel_t channel = {0};

        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_from_context(context, &channel) && channel.settings.sample_period_ms > 0u)
        {
            return model_set_uint32_value(value, channel.settings.sample_period_ms, UA_STATUSCODE_GOOD);
        }
        return model_set_uint32_value(value, 0u, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_PROCESS_TEMPERATURE_HISTORY_INTERVAL_MS:
    {
        telemetry_channel_t channel = {0};

        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_from_context(context, &channel))
        {
            return model_set_uint32_value(value, channel.settings.history_interval_ms, UA_STATUSCODE_GOOD);
        }
        return model_set_uint32_value(value, 0u, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_PROCESS_TEMPERATURE_ALARM_STATE:
    {
        alarm_service_status_t status = {0};
        alarm_service_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (context != NULL && context->channel_id != NULL &&
            status.channel_id != NULL &&
            strcmp(context->channel_id, status.channel_id) == 0)
        {
            return model_set_string_value(value,
                                          model_alarm_state_to_string(status.state),
                                          model_quality_to_status(status.quality));
        }
        return model_set_string_value(value, "Invalid", UA_STATUSCODE_BADNOTSUPPORTED);
    }
    case MODEL_NODE_NETWORK_IPV4:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.ipv4_address, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_NETWORK_MAC_ADDRESS:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.mac_address, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_NETWORK_IP_MODE:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.ip_mode, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_NETWORK_MASK:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.netmask, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_NETWORK_GATEWAY:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.gateway, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_NETWORK_LINK_UP:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_boolean_value(value, status.link_up, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_NETWORK_DHCP_STATE:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.dhcp_state, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_NETWORK_RECONNECT_COUNT:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, status.reconnect_count, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_TIME_LAST_SYNC_EPOCH:
    {
        time_service_status_t status = {0};
        time_service_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, status.last_sync_epoch_s, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_TIME_SYNCED:
    {
        time_service_status_t status = {0};
        time_service_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_boolean_value(value, status.synchronized, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_TIME_NTP_SERVER:
    {
        time_service_status_t status = {0};
        time_service_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.ntp_server, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_TIME_CLOCK_QUALITY:
    {
        time_service_status_t status = {0};
        time_service_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value,
                                      model_quality_to_string(status.quality),
                                      model_quality_to_status(status.quality));
    }
    case MODEL_NODE_TIME_TIMEZONE_OFFSET_MINUTES:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_int16_value(value, (UA_Int16)config->timezone_offset_minutes, UA_STATUSCODE_GOOD);
    case MODEL_NODE_SECURITY_MODE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, model_security_mode_to_string(), UA_STATUSCODE_GOOD);
    case MODEL_NODE_SECURITY_COMMISSIONING_MODE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_boolean_value(value,
                                       security_service_is_commissioning_mode(),
                                       UA_STATUSCODE_GOOD);
    case MODEL_NODE_SECURITY_TRUST_LIST_VERSION:
    {
        uint32_t trust_list_version = 0u;
        (void)security_service_get_trust_list_version(&trust_list_version);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, trust_list_version, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_SECURITY_TRUSTED_CLIENT_COUNT:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value,
                                      security_service_get_trusted_client_count(),
                                      UA_STATUSCODE_GOOD);
    case MODEL_NODE_SECURITY_LAST_AUTH_FAILURE:
    {
        uint32_t last_auth_failure_epoch_s = 0u;
        UA_DateTime timestamp = 0;
        if (security_service_get_last_auth_failure(&last_auth_failure_epoch_s))
        {
            timestamp = UA_DateTime_fromUnixTime((UA_Int64)last_auth_failure_epoch_s);
        }
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_datetime_value(value, timestamp, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_SECURITY_FAILED_LOGIN_COUNT:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value,
                                      security_service_get_failed_login_count(),
                                      UA_STATUSCODE_GOOD);
    case MODEL_NODE_SECURITY_CERT_THUMBPRINT:
    {
        char thumbprint[SECURITY_SERVICE_CERT_THUMBPRINT_LEN + 1u];
        const char *value_text = "";

        if (security_service_get_device_certificate_thumbprint(thumbprint, sizeof(thumbprint)))
        {
            value_text = thumbprint;
        }

        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, value_text, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_SECURITY_CERT_NOT_BEFORE:
    {
        uint32_t not_before_epoch_s = 0u;
        UA_DateTime timestamp = 0;
        if (security_service_get_device_certificate_not_before(&not_before_epoch_s))
        {
            timestamp = UA_DateTime_fromUnixTime((UA_Int64)not_before_epoch_s);
        }
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_datetime_value(value, timestamp, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_SECURITY_CERT_NOT_AFTER:
    {
        uint32_t not_after_epoch_s = 0u;
        UA_DateTime timestamp = 0;
        if (security_service_get_device_certificate_not_after(&not_after_epoch_s))
        {
            timestamp = UA_DateTime_fromUnixTime((UA_Int64)not_after_epoch_s);
        }
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_datetime_value(value, timestamp, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_SECURITY_DAYS_UNTIL_EXPIRY:
    {
        uint32_t days_until_expiry = 0u;

        (void)model_get_certificate_expiry_state(&days_until_expiry, NULL);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, days_until_expiry, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_SECURITY_EXPIRY_STATE:
    {
        const model_certificate_expiry_state_t expiry_state =
            model_get_certificate_expiry_state(NULL, NULL);

        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value,
                                      model_certificate_expiry_state_to_string(expiry_state),
                                      UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_SECURITY_COMMISSIONING_STATE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value,
                                      model_security_commissioning_state_to_string(),
                                      UA_STATUSCODE_GOOD);
    case MODEL_NODE_SECURITY_ACTIVE_IDENTITY_GENERATION:
    {
        security_state_service_snapshot_t snapshot = {0};

        (void)security_state_service_get_snapshot(&snapshot);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, snapshot.active_identity_generation, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_SECURITY_ACTIVE_TRUST_GENERATION:
    {
        security_state_service_snapshot_t snapshot = {0};

        (void)security_state_service_get_snapshot(&snapshot);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, snapshot.active_trust_generation, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_SECURITY_CURRENT_USER_ROLE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value,
                                      model_session_role_to_string(session_context),
                                      UA_STATUSCODE_GOOD);
    case MODEL_NODE_SECURITY_LAST_EVENT_ID:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, g_last_event_state.last_event_id, UA_STATUSCODE_GOOD);
    case MODEL_NODE_NETWORK_LAST_NETWORK_CHANGE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, g_last_event_state.last_network_change, UA_STATUSCODE_GOOD);
    case MODEL_NODE_DIAG_LAST_FAULT:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, g_last_event_state.last_fault, UA_STATUSCODE_GOOD);
    case MODEL_NODE_CONFIG_LAST_CONFIG_CHANGE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, g_last_event_state.last_config_change, UA_STATUSCODE_GOOD);
    case MODEL_NODE_CONFIG_REBOOT_REQUIRED:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_boolean_value(value, model_is_reboot_required() ? UA_TRUE : UA_FALSE, UA_STATUSCODE_GOOD);
    case MODEL_NODE_CONFIG_COMMISSIONED:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_boolean_value(value, model_is_commissioned() ? UA_TRUE : UA_FALSE, UA_STATUSCODE_GOOD);
    case MODEL_NODE_DIAG_FREE_HEAP:
    {
        diag_service_heap_stats_t stats = {0};
        diag_service_get_heap_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, stats.available_heap_bytes, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_MIN_EVER_FREE_HEAP:
    {
        diag_service_heap_stats_t stats = {0};
        diag_service_get_heap_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, stats.minimum_ever_free_bytes, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_UPTIME_SEC:
    {
        diag_service_system_stats_t stats = {0};
        diag_service_get_system_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, stats.uptime_sec, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_RESET_CAUSE:
    {
        diag_service_system_stats_t stats = {0};
        diag_service_get_system_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, stats.reset_cause, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_CPU_LOAD_APPROX:
    {
        diag_service_system_stats_t stats = {0};
        diag_service_get_system_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, stats.cpu_like_load_approx, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_LAST_FAULT_CODE:
    {
        diag_service_system_stats_t stats = {0};
        diag_service_get_system_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, stats.last_fault_code, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_TASK_STACK_HIGH_WATER:
    {
        diag_service_task_info_t task_info = {0};
        if (context->task_name == NULL || !diag_service_get_task_info_by_name(context->task_name, &task_info))
        {
            value->hasStatus = true;
            value->status = UA_STATUSCODE_BADNOTFOUND;
            return UA_STATUSCODE_GOOD;
        }

        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, task_info.stack_high_water_mark, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_CONFIG_SAMPLING_PERIOD_MS:
    {
        telemetry_channel_t channel = {0};
        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_by_id(DEVICE_CHANNEL_ID_TEMPERATURE_1, &channel))
        {
            return model_set_uint32_value(value, channel.settings.sample_period_ms, UA_STATUSCODE_GOOD);
        }
        return model_set_uint32_value(value, 0u, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_CONFIG_PUBLISH_PERIOD_MS:
    {
        telemetry_channel_t channel = {0};
        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_by_id(DEVICE_CHANNEL_ID_TEMPERATURE_1, &channel))
        {
            return model_set_uint32_value(value, channel.settings.publish_period_ms, UA_STATUSCODE_GOOD);
        }
        return model_set_uint32_value(value, 0u, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_CONFIG_TEMP_OFFSET:
    {
        telemetry_channel_t channel = {0};
        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_by_id(DEVICE_CHANNEL_ID_TEMPERATURE_1, &channel))
        {
            return model_set_float_value(value, (UA_Float)channel.settings.offset, UA_STATUSCODE_GOOD);
        }
        return model_set_float_value(value, 0.0f, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_CONFIG_TEMP_SCALE:
    {
        telemetry_channel_t channel = {0};
        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_by_id(DEVICE_CHANNEL_ID_TEMPERATURE_1, &channel))
        {
            return model_set_float_value(value, (UA_Float)channel.settings.scale, UA_STATUSCODE_GOOD);
        }
        return model_set_float_value(value, 1.0f, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_CONFIG_HIGH_ALARM:
    {
        telemetry_channel_t channel = {0};
        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_by_id(DEVICE_CHANNEL_ID_TEMPERATURE_1, &channel))
        {
            return model_set_float_value(value, (UA_Float)channel.settings.warning_threshold, UA_STATUSCODE_GOOD);
        }
        return model_set_float_value(value, 0.0f, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_CONFIG_HIGH_HIGH_ALARM:
    {
        telemetry_channel_t channel = {0};
        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_by_id(DEVICE_CHANNEL_ID_TEMPERATURE_1, &channel))
        {
            return model_set_float_value(value, (UA_Float)channel.settings.critical_threshold, UA_STATUSCODE_GOOD);
        }
        return model_set_float_value(value, 0.0f, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_MESSAGING_EVENT_SEQUENCE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, g_model_pubsub_event_mirror.sequence, UA_STATUSCODE_GOOD);
    case MODEL_NODE_MESSAGING_EVENT_ID:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, g_model_pubsub_event_mirror.event_id, UA_STATUSCODE_GOOD);
    case MODEL_NODE_MESSAGING_EVENT_TYPE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, g_model_pubsub_event_mirror.event_type, UA_STATUSCODE_GOOD);
    case MODEL_NODE_MESSAGING_EVENT_SOURCE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, g_model_pubsub_event_mirror.source, UA_STATUSCODE_GOOD);
    case MODEL_NODE_MESSAGING_EVENT_MESSAGE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, g_model_pubsub_event_mirror.message, UA_STATUSCODE_GOOD);
    case MODEL_NODE_MESSAGING_EVENT_SEVERITY:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, g_model_pubsub_event_mirror.severity, UA_STATUSCODE_GOOD);
    case MODEL_NODE_MESSAGING_EVENT_TIME:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_datetime_value(value, g_model_pubsub_event_mirror.time, UA_STATUSCODE_GOOD);
    default:
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADNOTSUPPORTED;
        return UA_STATUSCODE_GOOD;
    }
}

static UA_StatusCode model_write_value(UA_Server *server,
                                       const UA_NodeId *session_id,
                                       void *session_context,
                                       const UA_NodeId *node_id,
                                       void *node_context,
                                       const UA_NumericRange *range,
                                       const UA_DataValue *value)
{
    model_node_context_t *context = (model_node_context_t *)node_context;
    telemetry_channel_settings_t channel_settings = {0};
    const char *settings_channel_id = NULL;
    bool save_device_config = false;
    (void)server;
    (void)session_id;
    (void)session_context;
    (void)node_id;

    if (context == NULL)
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    if (range != NULL)
    {
        return UA_STATUSCODE_BADINDEXRANGEINVALID;
    }

    if (value == NULL || !value->hasValue)
    {
        return UA_STATUSCODE_BADNODATA;
    }

    if (model_session_is_anonymous(session_context))
    {
        return UA_STATUSCODE_BADUSERACCESSDENIED;
    }

    if (!model_session_has_role(session_context, model_required_role_for_write_kind(context->kind)))
    {
        return UA_STATUSCODE_BADUSERACCESSDENIED;
    }

    if (context->channel_id != NULL)
    {
        settings_channel_id = context->channel_id;
    }
    else if (context->kind == MODEL_NODE_CONFIG_SAMPLING_PERIOD_MS ||
             context->kind == MODEL_NODE_CONFIG_PUBLISH_PERIOD_MS ||
             context->kind == MODEL_NODE_CONFIG_TEMP_OFFSET ||
             context->kind == MODEL_NODE_CONFIG_TEMP_SCALE ||
             context->kind == MODEL_NODE_CONFIG_HIGH_ALARM ||
             context->kind == MODEL_NODE_CONFIG_HIGH_HIGH_ALARM)
    {
        settings_channel_id = DEVICE_CHANNEL_ID_TEMPERATURE_1;
    }

    switch (context->kind)
    {
    case MODEL_NODE_PROCESS_TEMPERATURE_UPDATE_PERIOD_MS:
    case MODEL_NODE_CONFIG_SAMPLING_PERIOD_MS:
        if (!UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_UINT32]))
        {
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
        if (settings_channel_id == NULL || !model_copy_channel_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADNODEIDUNKNOWN;
        }
        channel_settings.sample_period_ms = *(const UA_UInt32 *)value->value.data;
        if (channel_settings.sample_period_ms == 0u ||
            !channel_manager_update_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }
        break;
    case MODEL_NODE_PROCESS_TEMPERATURE_HISTORY_INTERVAL_MS:
        if (!UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_UINT32]))
        {
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
        if (settings_channel_id == NULL || !model_copy_channel_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADNODEIDUNKNOWN;
        }
        channel_settings.history_interval_ms = *(const UA_UInt32 *)value->value.data;
        if (!channel_manager_update_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }
        break;
    case MODEL_NODE_CONFIG_PUBLISH_PERIOD_MS:
        if (!UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_UINT32]))
        {
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
        if (settings_channel_id == NULL || !model_copy_channel_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADNODEIDUNKNOWN;
        }
        channel_settings.publish_period_ms = *(const UA_UInt32 *)value->value.data;
        if (channel_settings.publish_period_ms == 0u ||
            !channel_manager_update_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }
        break;
    case MODEL_NODE_TIME_TIMEZONE_OFFSET_MINUTES:
        if (!UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_INT16]))
        {
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
        if (!config_service_set_timezone_offset_minutes(*(const UA_Int16 *)value->value.data))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }
        save_device_config = true;
        break;
    case MODEL_NODE_CONFIG_TEMP_OFFSET:
        if (!UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_FLOAT]))
        {
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
        if (settings_channel_id == NULL || !model_copy_channel_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADNODEIDUNKNOWN;
        }
        channel_settings.offset = *(const UA_Float *)value->value.data;
        if (!channel_manager_update_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }
        break;
    case MODEL_NODE_CONFIG_TEMP_SCALE:
        if (!UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_FLOAT]))
        {
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
        if (settings_channel_id == NULL || !model_copy_channel_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADNODEIDUNKNOWN;
        }
        channel_settings.scale = *(const UA_Float *)value->value.data;
        if (channel_settings.scale == 0.0 ||
            !channel_manager_update_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }
        break;
    case MODEL_NODE_CONFIG_HIGH_ALARM:
        if (!UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_FLOAT]))
        {
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
        if (settings_channel_id == NULL || !model_copy_channel_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADNODEIDUNKNOWN;
        }
        channel_settings.warning_threshold = *(const UA_Float *)value->value.data;
        if (channel_settings.warning_threshold > channel_settings.critical_threshold ||
            !channel_manager_update_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }
        break;
    case MODEL_NODE_CONFIG_HIGH_HIGH_ALARM:
        if (!UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_FLOAT]))
        {
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
        if (settings_channel_id == NULL || !model_copy_channel_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADNODEIDUNKNOWN;
        }
        channel_settings.critical_threshold = *(const UA_Float *)value->value.data;
        if (channel_settings.critical_threshold < channel_settings.warning_threshold ||
            !channel_manager_update_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }
        break;
    default:
        return UA_STATUSCODE_BADNOTWRITABLE;
    }

    if (save_device_config)
    {
        (void)config_service_save();
    }
    return UA_STATUSCODE_GOOD;
}

static void model_set_boolean_method_output(size_t output_size, UA_Variant *output, UA_Boolean result)
{
    if (output_size > 0)
    {
        UA_Variant_setScalarCopy(&output[0], &result, &UA_TYPES[UA_TYPES_BOOLEAN]);
    }
}

static void model_schedule_reboot(uint32_t delay_ms)
{
    const TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms > 0u ? delay_ms : 1u);

    g_pending_actions.reboot_pending = true;
    g_pending_actions.reboot_due_tick = xTaskGetTickCount() + delay_ticks;
}

static void model_schedule_identify_blink(uint8_t blink_count, uint32_t period_ms)
{
    TickType_t delay_ticks;

    if (blink_count == 0u)
    {
        g_pending_actions.identify_blink_active = false;
        g_pending_actions.identify_toggles_remaining = 0u;
        g_pending_actions.identify_led_on = false;
        led_driver_set(false);
        return;
    }

    delay_ticks = pdMS_TO_TICKS(period_ms > 0u ? period_ms : 1u);
    if (delay_ticks == 0)
    {
        delay_ticks = 1;
    }

    g_pending_actions.identify_blink_active = true;
    g_pending_actions.identify_next_tick = xTaskGetTickCount();
    g_pending_actions.identify_toggles_remaining = (uint8_t)(blink_count * 2u);
    g_pending_actions.identify_led_on = false;
}

static UA_StatusCode model_acknowledge_alarms(UA_Server *server,
                                              const UA_NodeId *session_id,
                                              void *session_context,
                                              const UA_NodeId *method_id,
                                              void *method_context,
                                              const UA_NodeId *object_id,
                                              void *object_context,
                                              size_t input_size,
                                              const UA_Variant *input,
                                              size_t output_size,
                                              UA_Variant *output)
{
    UA_StatusCode status;
    UA_Boolean result = true;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    alarm_service_acknowledge_all();
    model_set_boolean_method_output(output_size, output, result);

    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_request_reboot(UA_Server *server,
                                          const UA_NodeId *session_id,
                                          void *session_context,
                                          const UA_NodeId *method_id,
                                          void *method_context,
                                          const UA_NodeId *object_id,
                                          void *object_context,
                                          size_t input_size,
                                          const UA_Variant *input,
                                          size_t output_size,
                                          UA_Variant *output)
{
    UA_StatusCode status;
    UA_Boolean result = true;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    model_schedule_reboot(250u);
    printf("[OPC UA]\tReboot requested\n");
    fflush(stdout);
    model_audit_session_event(session_context, "reboot", "device", true, "reboot scheduled");
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_maintenance_event(server,
                                     "Reboot requested",
                                     450u,
                                     "Reboot",
                                     0u,
                                     model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif
    model_set_boolean_method_output(output_size, output, result);

    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_factory_reset(UA_Server *server,
                                         const UA_NodeId *session_id,
                                         void *session_context,
                                         const UA_NodeId *method_id,
                                         void *method_context,
                                         const UA_NodeId *object_id,
                                         void *object_context,
                                         size_t input_size,
                                         const UA_Variant *input,
                                         size_t output_size,
                                         UA_Variant *output)
{
    UA_StatusCode status;
    UA_Boolean result = false;
    const UA_String *scope_string = NULL;
    char scope_text[48] = {0};
    config_reset_scope_t scope = CONFIG_RESET_SCOPE_ALL_KEEP_IDENTITY;
    const char *details = "factory reset failed";
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size > 1u || (input_size == 1u && (input == NULL || !model_get_input_ua_string(&input[0], &scope_string))))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    if (scope_string != NULL)
    {
        if (scope_string->length >= sizeof(scope_text))
        {
            model_set_boolean_method_output(output_size, output, false);
            return UA_STATUSCODE_BADINVALIDARGUMENT;
        }

        memcpy(scope_text, scope_string->data, scope_string->length);
        scope_text[scope_string->length] = '\0';
    }

    if (!config_reset_service_parse_scope(scope_text, &scope))
    {
        details = "unsupported factory reset scope";
    }
    else if (!config_reset_service_execute(scope))
    {
        details = "factory reset execution failed";
    }
    else
    {
        result = true;
        details = "factory reset scheduled";
        model_schedule_reboot(250u);
    }

    printf("[OPC UA]\tFactoryReset %s scope=%s\n",
           result == UA_TRUE ? "requested" : "failed",
           config_reset_service_scope_to_string(scope));
    fflush(stdout);

    model_audit_session_event(session_context,
                              "factory-reset",
                              "device",
                              result == UA_TRUE,
                              details);
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_maintenance_event(server,
                                     result == UA_TRUE ? "Factory reset requested" : "Factory reset failed",
                                     result == UA_TRUE ? 600u : 800u,
                                     "FactoryReset",
                                     result == UA_TRUE ? 0u : 1u,
                                     model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif

    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_sync_time_now(UA_Server *server,
                                         const UA_NodeId *session_id,
                                         void *session_context,
                                         const UA_NodeId *method_id,
                                         void *method_context,
                                         const UA_NodeId *object_id,
                                         void *object_context,
                                         size_t input_size,
                                         const UA_Variant *input,
                                         size_t output_size,
                                         UA_Variant *output)
{
    UA_StatusCode status;
    UA_Boolean result = false;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    result = time_service_request_sync_now();
    printf("[OPC UA]\tTime sync request %s\n", result ? "accepted" : "rejected");
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_blink_identify(UA_Server *server,
                                          const UA_NodeId *session_id,
                                          void *session_context,
                                          const UA_NodeId *method_id,
                                          void *method_context,
                                          const UA_NodeId *object_id,
                                          void *object_context,
                                          size_t input_size,
                                          const UA_Variant *input,
                                          size_t output_size,
                                          UA_Variant *output)
{
    UA_StatusCode status;
    UA_Boolean result = false;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (led_driver_is_initialized())
    {
        model_schedule_identify_blink(5u, 200u);
        result = true;
        printf("[OPC UA]\tIdentify blink requested\n");
        fflush(stdout);
    }

    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_apply_config(UA_Server *server,
                                        const UA_NodeId *session_id,
                                        void *session_context,
                                        const UA_NodeId *method_id,
                                        void *method_context,
                                        const UA_NodeId *object_id,
                                        void *object_context,
                                        size_t input_size,
                                        const UA_Variant *input,
                                        size_t output_size,
                                        UA_Variant *output)
{
    UA_StatusCode status;
    UA_Boolean result = false;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    result = config_service_apply();
    printf("[OPC UA]\tApplyConfig %s\n", result ? "succeeded" : "failed");
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_save_config(UA_Server *server,
                                       const UA_NodeId *session_id,
                                       void *session_context,
                                       const UA_NodeId *method_id,
                                       void *method_context,
                                       const UA_NodeId *object_id,
                                       void *object_context,
                                       size_t input_size,
                                       const UA_Variant *input,
                                       size_t output_size,
                                       UA_Variant *output)
{
    UA_StatusCode status;
    UA_Boolean result = false;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    result = config_service_save();
    printf("[OPC UA]\tSaveConfig %s\n", result ? "succeeded" : "failed");
    fflush(stdout);
    model_audit_session_event(session_context,
                              "config-save",
                              "device-configuration",
                              result,
                              result ? "configuration saved" : "configuration save failed");
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_config_changed_event(server,
                                        result ? "Configuration saved" : "Configuration save failed",
                                        result ? 300u : 700u,
                                        "Configuration",
                                        "*",
                                        model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_mqtt_pubsub_enabled(UA_Server *server,
                                                   const UA_NodeId *session_id,
                                                   void *session_context,
                                                   const UA_NodeId *method_id,
                                                   void *method_context,
                                                   const UA_NodeId *object_id,
                                                   void *object_context,
                                                   size_t input_size,
                                                   const UA_Variant *input,
                                                   size_t output_size,
                                                   UA_Variant *output)
{
    UA_StatusCode status;
    UA_Boolean result = false;
    UA_Boolean enabled = false;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != 1u || input == NULL ||
        !UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_BOOLEAN]) ||
        input[0].data == NULL)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    enabled = *(const UA_Boolean *)input[0].data;
    result = config_service_stage_set_mqtt_pubsub_enabled(enabled == UA_TRUE);

    printf("[OPC UA]\tSetMqttPubSubEnabled %s (%s in staged config after SaveConfig + Reboot)\n",
           result ? "accepted" : "failed",
           enabled == UA_TRUE ? "enable" : "disable");
    fflush(stdout);

    model_audit_session_event(session_context,
                              "mqtt-pubsub-enable",
                              "device-messaging",
                              result,
                              enabled == UA_TRUE ? "mqtt pubsub enabled in runtime config" : "mqtt pubsub disabled in runtime config");
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_config_changed_event(server,
                                        enabled == UA_TRUE ? "MQTT PubSub enabled in runtime configuration" : "MQTT PubSub disabled in runtime configuration",
                                        result ? 250u : 700u,
                                        "Messaging",
                                        "EnableMqttPubSub",
                                        model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif

    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_device_identity(UA_Server *server,
                                               const UA_NodeId *session_id,
                                               void *session_context,
                                               const UA_NodeId *method_id,
                                               void *method_context,
                                               const UA_NodeId *object_id,
                                               void *object_context,
                                               size_t input_size,
                                               const UA_Variant *input,
                                               size_t output_size,
                                               UA_Variant *output)
{
    static const size_t expected_input_size = 10u;
    UA_StatusCode status;
    UA_Boolean result = false;
    device_identity_config_t identity = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != expected_input_size || input == NULL ||
        !model_copy_input_string(&input[0], identity.device_name, sizeof(identity.device_name)) ||
        !model_copy_input_string(&input[1], identity.product_instance_uri, sizeof(identity.product_instance_uri)) ||
        !model_copy_input_string(&input[2], identity.serial_number, sizeof(identity.serial_number)) ||
        !model_copy_input_string(&input[3], identity.asset_id, sizeof(identity.asset_id)) ||
        !model_copy_input_string(&input[4], identity.hw_revision, sizeof(identity.hw_revision)) ||
        !model_copy_input_string(&input[5], identity.fw_revision, sizeof(identity.fw_revision)) ||
        !UA_Variant_hasScalarType(&input[6], &UA_TYPES[UA_TYPES_DATETIME]) || input[6].data == NULL ||
        !UA_Variant_hasScalarType(&input[7], &UA_TYPES[UA_TYPES_DATETIME]) || input[7].data == NULL ||
        !model_copy_input_string(&input[8], identity.location, sizeof(identity.location)) ||
        !model_copy_input_string(&input[9], identity.asset_class, sizeof(identity.asset_class)))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    identity.manufacture_date_utc = (int64_t)*(const UA_DateTime *)input[6].data;
    identity.calibration_date_utc = (int64_t)*(const UA_DateTime *)input[7].data;
    result = config_service_stage_set_identity(&identity);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_network_config(UA_Server *server,
                                              const UA_NodeId *session_id,
                                              void *session_context,
                                              const UA_NodeId *method_id,
                                              void *method_context,
                                              const UA_NodeId *object_id,
                                              void *object_context,
                                              size_t input_size,
                                              const UA_Variant *input,
                                              size_t output_size,
                                              UA_Variant *output)
{
    static const size_t expected_input_size = 9u;
    UA_StatusCode status;
    UA_Boolean result = false;
    device_network_config_t network = {0};
    UA_Boolean use_dhcp = false;
    UA_UInt32 opcua_port = 0u;
    char mac_address[18] = {0};
    char ipv4[16] = {0};
    char netmask[16] = {0};
    char gateway[16] = {0};
    char dns[16] = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != expected_input_size || input == NULL ||
        !UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_BOOLEAN]) || input[0].data == NULL ||
        !model_copy_input_string(&input[1], mac_address, sizeof(mac_address)) ||
        !model_copy_input_string(&input[2], network.opcua_hostname, sizeof(network.opcua_hostname)) ||
        !UA_Variant_hasScalarType(&input[3], &UA_TYPES[UA_TYPES_UINT32]) || input[3].data == NULL ||
        !model_copy_input_string(&input[4], network.ntp_server, sizeof(network.ntp_server)) ||
        !model_copy_input_string(&input[5], ipv4, sizeof(ipv4)) ||
        !model_copy_input_string(&input[6], netmask, sizeof(netmask)) ||
        !model_copy_input_string(&input[7], gateway, sizeof(gateway)) ||
        !model_copy_input_string(&input[8], dns, sizeof(dns)))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    use_dhcp = *(const UA_Boolean *)input[0].data;
    opcua_port = *(const UA_UInt32 *)input[3].data;
    network.use_dhcp = use_dhcp == UA_TRUE;

    if (opcua_port > UINT16_MAX ||
        !model_parse_mac_string(mac_address, network.mac) ||
        !model_parse_ipv4_string(ipv4, network.static_ip) ||
        !model_parse_ipv4_string(netmask, network.static_netmask) ||
        !model_parse_ipv4_string(gateway, network.static_gateway) ||
        !model_parse_ipv4_string(dns, network.static_dns))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADOUTOFRANGE;
    }

    network.opcua_port = (uint16_t)opcua_port;
    result = config_service_stage_set_network(&network);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static void model_format_channel_config_json(size_t slot_index,
                                             const config_channel_definition_t *channel,
                                             char *buffer,
                                             size_t buffer_size)
{
    const driver_catalog_entry_t *driver = NULL;

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    if (channel == NULL || channel->in_use == 0u)
    {
        (void)snprintf(buffer,
                       buffer_size,
                       "{\"slot\":%u,\"inUse\":false}",
                       (unsigned)slot_index);
        return;
    }

    driver = driver_catalog_get_by_id(channel->driver_id);
    (void)snprintf(buffer,
                   buffer_size,
                   "{\"slot\":%u,\"inUse\":true,\"enabled\":%s,\"driverId\":%u,\"driver\":\"%s\",\"sourceIndex\":%u,\"sourceFlags\":%u,\"id\":\"%s\",\"name\":\"%s\",\"unit\":\"%s\",\"samplePeriodMs\":%u,\"publishPeriodMs\":%u,\"historyIntervalMs\":%u,\"scale\":%.6f,\"offset\":%.6f,\"warningThreshold\":%.6f,\"criticalThreshold\":%.6f}",
                   (unsigned)slot_index,
                   channel->settings.enabled ? "true" : "false",
                   (unsigned)channel->driver_id,
                   driver != NULL && driver->name != NULL ? driver->name : "unknown",
                   (unsigned)channel->source_index,
                   (unsigned)channel->source_flags,
                   channel->id,
                   channel->name,
                   channel->unit,
                   (unsigned)channel->settings.sample_period_ms,
                   (unsigned)channel->settings.publish_period_ms,
                   (unsigned)channel->settings.history_interval_ms,
                   channel->settings.scale,
                   channel->settings.offset,
                   channel->settings.warning_threshold,
                   channel->settings.critical_threshold);
}

static void model_format_driver_catalog_json(char *buffer, size_t buffer_size)
{
    size_t offset = 0u;

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    offset += (size_t)snprintf(buffer + offset, buffer_size - offset, "[");
    for (size_t index = 0u; index < driver_catalog_count() && offset < buffer_size; ++index)
    {
        const driver_catalog_entry_t *driver = driver_catalog_get_by_index(index);
        const int written = snprintf(buffer + offset,
                                     buffer_size - offset,
                                     "%s{\"id\":%u,\"name\":\"%s\",\"analog\":%s,\"defaultUnit\":\"%s\"}",
                                     index > 0u ? "," : "",
                                     driver != NULL ? (unsigned)driver->driver_id : 0u,
                                     driver != NULL && driver->name != NULL ? driver->name : "unknown",
                                     driver != NULL && driver->analog ? "true" : "false",
                                     driver != NULL && driver->default_unit != NULL ? driver->default_unit : "");

        if (written < 0)
        {
            break;
        }

        offset += (size_t)written;
    }

    if (offset < buffer_size)
    {
        (void)snprintf(buffer + offset, buffer_size - offset, "]");
    }
    else
    {
        buffer[buffer_size - 1u] = '\0';
    }
}

static UA_StatusCode model_get_channel_config(UA_Server *server,
                                              const UA_NodeId *session_id,
                                              void *session_context,
                                              const UA_NodeId *method_id,
                                              void *method_context,
                                              const UA_NodeId *object_id,
                                              void *object_context,
                                              size_t input_size,
                                              const UA_Variant *input,
                                              size_t output_size,
                                              UA_Variant *output)
{
    UA_StatusCode status;
    UA_UInt32 slot_index = 0u;
    config_channel_definition_t channel = {0};
    char payload[512] = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != 1u || input == NULL ||
        !UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_UINT32]) || input[0].data == NULL)
    {
        model_set_string_method_output(output_size, output, "{}");
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    slot_index = *(const UA_UInt32 *)input[0].data;
    if (slot_index >= CONFIG_SERVICE_MAX_CHANNELS ||
        !config_service_stage_copy_channel((size_t)slot_index, &channel))
    {
        model_set_string_method_output(output_size, output, "{}");
        return UA_STATUSCODE_BADOUTOFRANGE;
    }

    model_format_channel_config_json((size_t)slot_index, &channel, payload, sizeof(payload));
    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_list_drivers(UA_Server *server,
                                        const UA_NodeId *session_id,
                                        void *session_context,
                                        const UA_NodeId *method_id,
                                        void *method_context,
                                        const UA_NodeId *object_id,
                                        void *object_context,
                                        size_t input_size,
                                        const UA_Variant *input,
                                        size_t output_size,
                                        UA_Variant *output)
{
    UA_StatusCode status;
    char payload[768] = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    model_format_driver_catalog_json(payload, sizeof(payload));
    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_get_config_state(UA_Server *server,
                                            const UA_NodeId *session_id,
                                            void *session_context,
                                            const UA_NodeId *method_id,
                                            void *method_context,
                                            const UA_NodeId *object_id,
                                            void *object_context,
                                            size_t input_size,
                                            const UA_Variant *input,
                                            size_t output_size,
                                            UA_Variant *output)
{
    UA_StatusCode status;
    char payload[384] = {0};
    const bool reboot_required = model_is_reboot_required();
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (!config_export_service_write_state_json(payload, sizeof(payload), reboot_required))
    {
        model_set_string_method_output(output_size, output, "{}");
        return UA_STATUSCODE_GOOD;
    }

    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_export_config(UA_Server *server,
                                         const UA_NodeId *session_id,
                                         void *session_context,
                                         const UA_NodeId *method_id,
                                         void *method_context,
                                         const UA_NodeId *object_id,
                                         void *object_context,
                                         size_t input_size,
                                         const UA_Variant *input,
                                         size_t output_size,
                                         UA_Variant *output)
{
    UA_StatusCode status;
    char payload[4096] = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (!config_export_service_write_running_json(payload, sizeof(payload)))
    {
        model_set_string_method_output(output_size, output, "{}");
        return UA_STATUSCODE_GOOD;
    }

    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_refresh_runtime_trust_backends(UA_Server *server)
{
    UA_ServerConfig *config;
    UA_StatusCode refresh_status = UA_STATUSCODE_GOOD;

    if (server == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    config = UA_Server_getConfig(server);
    if (config == NULL)
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    if (security_certificate_group_service_manages(&config->secureChannelPKI))
    {
        refresh_status = security_certificate_group_service_refresh(&config->secureChannelPKI);
    }

    if (refresh_status == UA_STATUSCODE_GOOD &&
        security_certificate_group_service_manages(&config->sessionPKI))
    {
        refresh_status = security_certificate_group_service_refresh(&config->sessionPKI);
    }

    return refresh_status;
}

static void model_finalize_identity_generation_after_startup(void)
{
    security_state_service_snapshot_t snapshot = {0};
    security_service_blob_view_t certificate = {0};
    security_service_blob_view_t private_key = {0};
    bool have_identity = false;
    uint32_t next_active_generation = 0u;
    uint32_t next_pending_generation = 0u;

    if (!security_state_service_get_snapshot(&snapshot))
    {
        return;
    }

    have_identity = security_service_get_device_certificate(&certificate) &&
                    security_service_get_device_private_key(&private_key) &&
                    certificate.length > 0u &&
                    private_key.length > 0u;

    next_active_generation = snapshot.active_identity_generation;
    next_pending_generation = snapshot.pending_identity_generation;

    if (snapshot.pending_identity_generation > snapshot.active_identity_generation)
    {
        next_active_generation = snapshot.pending_identity_generation;
        next_pending_generation = 0u;
    }
    else if (snapshot.active_identity_generation == 0u &&
             snapshot.pending_identity_generation == 0u &&
             have_identity)
    {
        next_active_generation = 1u;
    }

    if (next_active_generation == snapshot.active_identity_generation &&
        next_pending_generation == snapshot.pending_identity_generation)
    {
        return;
    }

    if (security_state_service_set_identity_generations(next_active_generation,
                                                        next_pending_generation))
    {
        printf("[OPC UA]\tIdentity generation active=%lu pending=%lu\n",
               (unsigned long)next_active_generation,
               (unsigned long)next_pending_generation);
        fflush(stdout);
    }
}

static void model_write_identity_import_report_json(char *buffer,
                                                    size_t buffer_size,
                                                    const security_identity_import_service_report_t *report)
{
    if (buffer == NULL || buffer_size == 0u || report == NULL)
    {
        return;
    }

    snprintf(buffer,
             buffer_size,
             "{\"sessionActive\":%s,\"payloadComplete\":%s,\"payloadCrc32Matches\":%s,\"valid\":%s,\"timeCheckPerformed\":%s,\"rebootRequired\":%s,\"payloadSizeBytes\":%lu,\"payloadCrc32\":%lu,\"schemaVersion\":%lu,\"certificateSizeBytes\":%lu,\"privateKeySizeBytes\":%lu,\"notBeforeEpochS\":%lu,\"notAfterEpochS\":%lu,\"result\":\"%s\"}",
             report->session_active ? "true" : "false",
             report->payload_complete ? "true" : "false",
             report->payload_crc32_matches ? "true" : "false",
             report->valid ? "true" : "false",
             report->time_check_performed ? "true" : "false",
             report->reboot_required ? "true" : "false",
             (unsigned long)report->payload_size,
             (unsigned long)report->expected_payload_crc32,
             (unsigned long)report->schema_version,
             (unsigned long)report->certificate_size,
             (unsigned long)report->private_key_size,
             (unsigned long)report->not_before_epoch_s,
             (unsigned long)report->not_after_epoch_s,
             security_identity_import_service_result_to_string(report->result));
}

static void model_write_certificate_metadata_json(char *buffer, size_t buffer_size)
{
    security_service_blob_view_t certificate = {0};
    security_service_blob_view_t private_key = {0};
    security_state_service_snapshot_t snapshot = {0};
    char thumbprint[SECURITY_SERVICE_CERT_THUMBPRINT_LEN + 1u] = {0};
    const device_profile_config_t *config = config_service_get();
    const bool have_identity =
        security_service_get_device_certificate(&certificate) &&
        security_service_get_device_private_key(&private_key) &&
        certificate.length > 0u &&
        private_key.length > 0u;
    uint32_t not_before_epoch_s = 0u;
    uint32_t not_after_epoch_s = 0u;
    uint32_t days_until_expiry = 0u;
    bool time_check_performed = false;
    bool expiry_known = false;
    const model_certificate_expiry_state_t expiry_state =
        model_get_certificate_expiry_state(&days_until_expiry, &time_check_performed);

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    if (have_identity)
    {
        (void)security_service_get_device_certificate_thumbprint(thumbprint, sizeof(thumbprint));
        (void)security_service_get_device_certificate_not_before(&not_before_epoch_s);
        if (security_service_get_device_certificate_not_after(&not_after_epoch_s))
        {
            expiry_known = true;
        }
    }

    (void)security_state_service_get_snapshot(&snapshot);
    snprintf(buffer,
             buffer_size,
             "{\"haveDeviceIdentity\":%s,\"certificateSizeBytes\":%lu,\"privateKeySizeBytes\":%lu,\"applicationUri\":\"%s\",\"thumbprint\":\"%s\",\"notBeforeEpochS\":%lu,\"notAfterEpochS\":%lu,\"expiryKnown\":%s,\"timeCheckPerformed\":%s,\"daysUntilExpiry\":%lu,\"expiryState\":\"%s\",\"commissioningState\":\"%s\",\"activeIdentityGeneration\":%lu,\"pendingIdentityGeneration\":%lu,\"activeTrustGeneration\":%lu,\"pendingTrustGeneration\":%lu,\"rebootRequired\":%s}",
             have_identity ? "true" : "false",
             (unsigned long)certificate.length,
             (unsigned long)private_key.length,
             (config != NULL) ? config->product_instance_uri : "",
             thumbprint,
             (unsigned long)not_before_epoch_s,
             (unsigned long)not_after_epoch_s,
             expiry_known ? "true" : "false",
             time_check_performed ? "true" : "false",
             (unsigned long)days_until_expiry,
             model_certificate_expiry_state_to_string(expiry_state),
             model_security_commissioning_state_to_string(),
             (unsigned long)snapshot.active_identity_generation,
             (unsigned long)snapshot.pending_identity_generation,
             (unsigned long)snapshot.active_trust_generation,
             (unsigned long)snapshot.pending_trust_generation,
             snapshot.pending_identity_generation > snapshot.active_identity_generation ? "true" : "false");
}

static void model_write_certificate_state_json(char *buffer, size_t buffer_size)
{
    security_identity_import_service_status_t import_status = {0};
    security_identity_import_service_report_t report = {0};
    char metadata[768] = {0};
    const char *validation_result = "NONE";
    bool validation_valid = false;

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    model_write_certificate_metadata_json(metadata, sizeof(metadata));
    security_identity_import_service_get_status(&import_status);
    if (import_status.session_active)
    {
        (void)security_identity_import_service_validate_staged(&report);
        validation_result = security_identity_import_service_result_to_string(report.result);
        validation_valid = report.valid;
    }

    snprintf(buffer,
             buffer_size,
             "{\"metadata\":%s,\"maxChunkBytes\":%u,\"import\":{\"sessionActive\":%s,\"expectedPayloadSizeBytes\":%lu,\"payloadCrc32\":%lu,\"schemaVersion\":%lu,\"bytesWritten\":%lu,\"remainingBytes\":%lu,\"payloadComplete\":%s,\"payloadCrc32Matches\":%s,\"lastResult\":\"%s\"},\"stagedValidation\":{\"available\":%s,\"valid\":%s,\"certificateSizeBytes\":%lu,\"privateKeySizeBytes\":%lu,\"notBeforeEpochS\":%lu,\"notAfterEpochS\":%lu,\"timeCheckPerformed\":%s,\"result\":\"%s\"}}",
             metadata,
             (unsigned)SECURITY_IDENTITY_IMPORT_SERVICE_MAX_CHUNK_BYTES,
             import_status.session_active ? "true" : "false",
             (unsigned long)import_status.expected_payload_size,
             (unsigned long)import_status.expected_payload_crc32,
             (unsigned long)import_status.schema_version,
             (unsigned long)import_status.bytes_written,
             (unsigned long)import_status.remaining_bytes,
             import_status.payload_complete ? "true" : "false",
             import_status.payload_crc32_matches ? "true" : "false",
             security_identity_import_service_result_to_string(import_status.last_result),
             import_status.session_active ? "true" : "false",
             validation_valid ? "true" : "false",
             (unsigned long)report.certificate_size,
             (unsigned long)report.private_key_size,
             (unsigned long)report.not_before_epoch_s,
             (unsigned long)report.not_after_epoch_s,
             report.time_check_performed ? "true" : "false",
             validation_result);
}

static UA_StatusCode model_get_certificate_state(UA_Server *server,
                                                 const UA_NodeId *session_id,
                                                 void *session_context,
                                                 const UA_NodeId *method_id,
                                                 void *method_context,
                                                 const UA_NodeId *object_id,
                                                 void *object_context,
                                                 size_t input_size,
                                                 const UA_Variant *input,
                                                 size_t output_size,
                                                 UA_Variant *output)
{
    UA_StatusCode status_code;
    char payload[1536] = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    model_write_certificate_state_json(payload, sizeof(payload));
    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_export_certificate_metadata(UA_Server *server,
                                                       const UA_NodeId *session_id,
                                                       void *session_context,
                                                       const UA_NodeId *method_id,
                                                       void *method_context,
                                                       const UA_NodeId *object_id,
                                                       void *object_context,
                                                       size_t input_size,
                                                       const UA_Variant *input,
                                                       size_t output_size,
                                                       UA_Variant *output)
{
    UA_StatusCode status_code;
    char payload[896] = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    model_write_certificate_metadata_json(payload, sizeof(payload));
    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_begin_identity_rotation(UA_Server *server,
                                                   const UA_NodeId *session_id,
                                                   void *session_context,
                                                   const UA_NodeId *method_id,
                                                   void *method_context,
                                                   const UA_NodeId *object_id,
                                                   void *object_context,
                                                   size_t input_size,
                                                   const UA_Variant *input,
                                                   size_t output_size,
                                                   UA_Variant *output)
{
    UA_Boolean result = false;
    UA_UInt32 payload_size = 0u;
    UA_UInt32 payload_crc32 = 0u;
    UA_UInt32 schema_version = 0u;
    security_identity_import_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    if (model_require_admin_session(session_context, output_size, output) != UA_STATUSCODE_GOOD)
    {
        return UA_STATUSCODE_GOOD;
    }

    if (input_size != 3u || input == NULL ||
        !UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_UINT32]) || input[0].data == NULL ||
        !UA_Variant_hasScalarType(&input[1], &UA_TYPES[UA_TYPES_UINT32]) || input[1].data == NULL ||
        !UA_Variant_hasScalarType(&input[2], &UA_TYPES[UA_TYPES_UINT32]) || input[2].data == NULL)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    payload_size = *(const UA_UInt32 *)input[0].data;
    payload_crc32 = *(const UA_UInt32 *)input[1].data;
    schema_version = *(const UA_UInt32 *)input[2].data;

    service_result = security_identity_import_service_begin((uint32_t)payload_size,
                                                            (uint32_t)payload_crc32,
                                                            (uint32_t)schema_version);
    result = (service_result == SECURITY_IDENTITY_IMPORT_RESULT_OK) ? UA_TRUE : UA_FALSE;
    printf("[OPC UA]\tBeginIdentityRotation %s size=%lu crc32=0x%08lx schema=%lu\n",
           result == UA_TRUE ? "accepted" : security_identity_import_service_result_to_string(service_result),
           (unsigned long)payload_size,
           (unsigned long)payload_crc32,
           (unsigned long)schema_version);
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_write_identity_rotation_chunk(UA_Server *server,
                                                         const UA_NodeId *session_id,
                                                         void *session_context,
                                                         const UA_NodeId *method_id,
                                                         void *method_context,
                                                         const UA_NodeId *object_id,
                                                         void *object_context,
                                                         size_t input_size,
                                                         const UA_Variant *input,
                                                         size_t output_size,
                                                         UA_Variant *output)
{
    const UA_ByteString *chunk = NULL;
    UA_Boolean result = false;
    security_identity_import_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    if (model_require_admin_session(session_context, output_size, output) != UA_STATUSCODE_GOOD)
    {
        return UA_STATUSCODE_GOOD;
    }

    if (input_size != 1u || input == NULL || !model_get_input_ua_bytestring(&input[0], &chunk))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    service_result = security_identity_import_service_write_chunk(chunk->data, chunk->length);
    result = (service_result == SECURITY_IDENTITY_IMPORT_RESULT_OK) ? UA_TRUE : UA_FALSE;
    if (result != UA_TRUE)
    {
        printf("[OPC UA]\tWriteIdentityRotationChunk failed: %s (size=%lu)\n",
               security_identity_import_service_result_to_string(service_result),
               (unsigned long)chunk->length);
        fflush(stdout);
    }
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_abort_identity_rotation(UA_Server *server,
                                                   const UA_NodeId *session_id,
                                                   void *session_context,
                                                   const UA_NodeId *method_id,
                                                   void *method_context,
                                                   const UA_NodeId *object_id,
                                                   void *object_context,
                                                   size_t input_size,
                                                   const UA_Variant *input,
                                                   size_t output_size,
                                                   UA_Variant *output)
{
    UA_Boolean result = false;
    security_identity_import_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    if (model_require_admin_session(session_context, output_size, output) != UA_STATUSCODE_GOOD)
    {
        return UA_STATUSCODE_GOOD;
    }

    service_result = security_identity_import_service_abort();
    result = (service_result == SECURITY_IDENTITY_IMPORT_RESULT_OK) ? UA_TRUE : UA_FALSE;
    printf("[OPC UA]\tAbortIdentityRotation %s\n",
           result == UA_TRUE ? "succeeded" : security_identity_import_service_result_to_string(service_result));
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_validate_staged_identity(UA_Server *server,
                                                    const UA_NodeId *session_id,
                                                    void *session_context,
                                                    const UA_NodeId *method_id,
                                                    void *method_context,
                                                    const UA_NodeId *object_id,
                                                    void *object_context,
                                                    size_t input_size,
                                                    const UA_Variant *input,
                                                    size_t output_size,
                                                    UA_Variant *output)
{
    security_identity_import_service_report_t report = {0};
    char payload[512] = {0};
    (void)server;
    (void)session_id;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    if (model_require_admin_session(session_context, output_size, output) != UA_STATUSCODE_GOOD)
    {
        return UA_STATUSCODE_GOOD;
    }

    (void)security_identity_import_service_validate_staged(&report);
    model_write_identity_import_report_json(payload, sizeof(payload), &report);

    printf("[OPC UA]\tValidateStagedIdentity %s\n",
           report.valid ? "OK" : security_identity_import_service_result_to_string(report.result));
    fflush(stdout);
    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_apply_staged_identity(UA_Server *server,
                                                 const UA_NodeId *session_id,
                                                 void *session_context,
                                                 const UA_NodeId *method_id,
                                                 void *method_context,
                                                 const UA_NodeId *object_id,
                                                 void *object_context,
                                                 size_t input_size,
                                                 const UA_Variant *input,
                                                 size_t output_size,
                                                 UA_Variant *output)
{
    UA_Boolean result = false;
    security_identity_import_service_report_t report = {0};
    const char *details = "staged identity validation failed";
    (void)server;
    (void)session_id;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    if (model_require_admin_session(session_context, output_size, output) != UA_STATUSCODE_GOOD)
    {
        return UA_STATUSCODE_GOOD;
    }

    if (security_identity_import_service_apply_staged(&report) == SECURITY_IDENTITY_IMPORT_RESULT_OK)
    {
        result = true;
        details = "staged identity persisted; reboot required";
    }
    else
    {
        details = security_identity_import_service_result_to_string(report.result);
    }

    if (result == UA_TRUE)
    {
        printf("[OPC UA]\tApplyStagedIdentity succeeded; reboot required\n");
    }
    else
    {
        printf("[OPC UA]\tApplyStagedIdentity failed: %s\n", details);
    }
    fflush(stdout);
    model_audit_session_event(session_context,
                              "identity-rotation-apply",
                              "device-identity",
                              result == UA_TRUE,
                              details);
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_maintenance_event(server,
                                     result == UA_TRUE ? "Staged identity applied; reboot required"
                                                       : "Staged identity apply failed",
                                     result == UA_TRUE ? 300u : 700u,
                                     "IdentityRotation",
                                     result == UA_TRUE ? 0u : 1u,
                                     model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static void model_write_trust_import_report_json(char *buffer,
                                                 size_t buffer_size,
                                                 const security_trust_import_service_report_t *report)
{
    if (buffer == NULL || buffer_size == 0u || report == NULL)
    {
        return;
    }

    snprintf(buffer,
             buffer_size,
             "{\"sessionActive\":%s,\"payloadComplete\":%s,\"payloadCrc32Matches\":%s,\"valid\":%s,\"payloadSizeBytes\":%lu,\"payloadCrc32\":%lu,\"schemaVersion\":%lu,\"certificateCount\":%lu,\"result\":\"%s\"}",
             report->session_active ? "true" : "false",
             report->payload_complete ? "true" : "false",
             report->payload_crc32_matches ? "true" : "false",
             report->valid ? "true" : "false",
             (unsigned long)report->payload_size,
             (unsigned long)report->expected_payload_crc32,
             (unsigned long)report->schema_version,
             (unsigned long)report->certificate_count,
             security_trust_import_service_result_to_string(report->result));
}

static void model_append_json_object_field(char *buffer,
                                           size_t buffer_size,
                                           const char *field_name,
                                           const char *object_json)
{
    size_t length;
    const char quote = '"';
    int written;

    if (buffer == NULL || buffer_size == 0u || field_name == NULL || field_name[0] == '\0' ||
        object_json == NULL || object_json[0] == '\0')
    {
        return;
    }

    length = strlen(buffer);
    if (length == 0u || buffer[length - 1u] != '}')
    {
        return;
    }

    buffer[length - 1u] = '\0';
    written = snprintf(buffer + length - 1u,
                       buffer_size - (length - 1u),
                       ",%c%s%c:%s}",
                       quote,
                       field_name,
                       quote,
                       object_json);
    if (written < 0 || (size_t)written >= (buffer_size - (length - 1u)))
    {
        buffer[buffer_size - 1u] = '\0';
    }
}

static void model_write_trust_state_json(char *buffer, size_t buffer_size)
{
    security_service_blob_view_t trust_list = {0};
    security_trust_import_service_status_t import_status = {0};
    security_trust_import_service_report_t report = {0};
    security_state_service_snapshot_t snapshot = {0};
    char expiry_summary[SECURITY_TRUST_EXPIRY_SERVICE_SUMMARY_MAX_LEN] = "{}";
    uint32_t trust_list_version = 0u;
    const bool have_trust_list = security_service_get_trust_list(&trust_list);
    const uint32_t trusted_client_count = security_service_get_trusted_client_count();
    const char *validation_result = "NONE";
    bool validation_valid = false;
    uint32_t validation_cert_count = 0u;

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    security_trust_import_service_get_status(&import_status);
    if (import_status.session_active)
    {
        (void)security_trust_import_service_validate_staged(&report);
        validation_result = security_trust_import_service_result_to_string(report.result);
        validation_valid = report.valid;
        validation_cert_count = report.certificate_count;
    }

    (void)security_service_get_trust_list_version(&trust_list_version);
    (void)security_state_service_get_snapshot(&snapshot);
    (void)security_trust_expiry_service_get_summary_json(expiry_summary, sizeof(expiry_summary));
    snprintf(buffer,
             buffer_size,
             "{\"commissioningMode\":%s,\"commissioningState\":\"%s\",\"haveTrustList\":%s,\"trustListSizeBytes\":%lu,\"trustListVersion\":%lu,\"activeTrustGeneration\":%lu,\"pendingTrustGeneration\":%lu,\"trustedClientCount\":%lu,\"maxChunkBytes\":%u,\"import\":{\"sessionActive\":%s,\"expectedPayloadSizeBytes\":%lu,\"payloadCrc32\":%lu,\"schemaVersion\":%lu,\"bytesWritten\":%lu,\"remainingBytes\":%lu,\"payloadComplete\":%s,\"payloadCrc32Matches\":%s,\"lastResult\":\"%s\"},\"stagedValidation\":{\"available\":%s,\"valid\":%s,\"certificateCount\":%lu,\"result\":\"%s\"}}",
             snapshot.commissioning_mode ? "true" : "false",
             model_security_commissioning_state_to_string(),
             have_trust_list ? "true" : "false",
             (unsigned long)trust_list.length,
             (unsigned long)trust_list_version,
             (unsigned long)snapshot.active_trust_generation,
             (unsigned long)snapshot.pending_trust_generation,
             (unsigned long)trusted_client_count,
             (unsigned)SECURITY_TRUST_IMPORT_SERVICE_MAX_CHUNK_BYTES,
             import_status.session_active ? "true" : "false",
             (unsigned long)import_status.expected_payload_size,
             (unsigned long)import_status.expected_payload_crc32,
             (unsigned long)import_status.schema_version,
             (unsigned long)import_status.bytes_written,
             (unsigned long)import_status.remaining_bytes,
             import_status.payload_complete ? "true" : "false",
             import_status.payload_crc32_matches ? "true" : "false",
             security_trust_import_service_result_to_string(import_status.last_result),
             import_status.session_active ? "true" : "false",
             validation_valid ? "true" : "false",
             (unsigned long)validation_cert_count,
             validation_result);
    model_append_json_object_field(buffer, buffer_size, "expiry", expiry_summary);
}

static UA_StatusCode model_get_trust_state(UA_Server *server,
                                           const UA_NodeId *session_id,
                                           void *session_context,
                                           const UA_NodeId *method_id,
                                           void *method_context,
                                           const UA_NodeId *object_id,
                                           void *object_context,
                                           size_t input_size,
                                           const UA_Variant *input,
                                           size_t output_size,
                                           UA_Variant *output)
{
    UA_StatusCode status_code;
    char payload[1408] = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    model_write_trust_state_json(payload, sizeof(payload));
    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_export_trust_list(UA_Server *server,
                                             const UA_NodeId *session_id,
                                             void *session_context,
                                             const UA_NodeId *method_id,
                                             void *method_context,
                                             const UA_NodeId *object_id,
                                             void *object_context,
                                             size_t input_size,
                                             const UA_Variant *input,
                                             size_t output_size,
                                             UA_Variant *output)
{
    UA_StatusCode status_code;
    security_service_blob_view_t trust_list = {0};
    UA_ByteString payload = UA_BYTESTRING_NULL;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    if (security_service_get_trust_list(&trust_list) && trust_list.length > 0u)
    {
        payload.data = (UA_Byte *)(uintptr_t)trust_list.data;
        payload.length = trust_list.length;
    }

    model_set_bytestring_method_output(output_size, output, &payload);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_begin_trust_list_import(UA_Server *server,
                                                   const UA_NodeId *session_id,
                                                   void *session_context,
                                                   const UA_NodeId *method_id,
                                                   void *method_context,
                                                   const UA_NodeId *object_id,
                                                   void *object_context,
                                                   size_t input_size,
                                                   const UA_Variant *input,
                                                   size_t output_size,
                                                   UA_Variant *output)
{
    UA_StatusCode status_code;
    UA_Boolean result = false;
    UA_UInt32 payload_size = 0u;
    UA_UInt32 payload_crc32 = 0u;
    UA_UInt32 schema_version = 0u;
    security_trust_import_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    if (input_size != 3u || input == NULL ||
        !UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_UINT32]) || input[0].data == NULL ||
        !UA_Variant_hasScalarType(&input[1], &UA_TYPES[UA_TYPES_UINT32]) || input[1].data == NULL ||
        !UA_Variant_hasScalarType(&input[2], &UA_TYPES[UA_TYPES_UINT32]) || input[2].data == NULL)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    payload_size = *(const UA_UInt32 *)input[0].data;
    payload_crc32 = *(const UA_UInt32 *)input[1].data;
    schema_version = *(const UA_UInt32 *)input[2].data;

    service_result = security_trust_import_service_begin((uint32_t)payload_size,
                                                         (uint32_t)payload_crc32,
                                                         (uint32_t)schema_version);
    result = (service_result == SECURITY_TRUST_IMPORT_RESULT_OK) ? UA_TRUE : UA_FALSE;
    printf("[OPC UA]\tBeginTrustListImport %s size=%lu crc32=0x%08lx schema=%lu\n",
           result == UA_TRUE ? "accepted" : security_trust_import_service_result_to_string(service_result),
           (unsigned long)payload_size,
           (unsigned long)payload_crc32,
           (unsigned long)schema_version);
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_write_trust_list_chunk(UA_Server *server,
                                                  const UA_NodeId *session_id,
                                                  void *session_context,
                                                  const UA_NodeId *method_id,
                                                  void *method_context,
                                                  const UA_NodeId *object_id,
                                                  void *object_context,
                                                  size_t input_size,
                                                  const UA_Variant *input,
                                                  size_t output_size,
                                                  UA_Variant *output)
{
    const UA_ByteString *chunk = NULL;
    UA_StatusCode status_code;
    UA_Boolean result = false;
    security_trust_import_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    if (input_size != 1u || input == NULL || !model_get_input_ua_bytestring(&input[0], &chunk))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    service_result = security_trust_import_service_write_chunk(chunk->data, chunk->length);
    result = (service_result == SECURITY_TRUST_IMPORT_RESULT_OK) ? UA_TRUE : UA_FALSE;
    if (result != UA_TRUE)
    {
        printf("[OPC UA]\tWriteTrustListChunk failed: %s (size=%lu)\n",
               security_trust_import_service_result_to_string(service_result),
               (unsigned long)chunk->length);
        fflush(stdout);
    }
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_abort_trust_list_import(UA_Server *server,
                                                   const UA_NodeId *session_id,
                                                   void *session_context,
                                                   const UA_NodeId *method_id,
                                                   void *method_context,
                                                   const UA_NodeId *object_id,
                                                   void *object_context,
                                                   size_t input_size,
                                                   const UA_Variant *input,
                                                   size_t output_size,
                                                   UA_Variant *output)
{
    UA_StatusCode status_code;
    UA_Boolean result = false;
    security_trust_import_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    service_result = security_trust_import_service_abort();
    result = (service_result == SECURITY_TRUST_IMPORT_RESULT_OK) ? UA_TRUE : UA_FALSE;
    printf("[OPC UA]\tAbortTrustListImport %s\n",
           result == UA_TRUE ? "succeeded" : security_trust_import_service_result_to_string(service_result));
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_validate_imported_trust_list(UA_Server *server,
                                                        const UA_NodeId *session_id,
                                                        void *session_context,
                                                        const UA_NodeId *method_id,
                                                        void *method_context,
                                                        const UA_NodeId *object_id,
                                                        void *object_context,
                                                        size_t input_size,
                                                        const UA_Variant *input,
                                                        size_t output_size,
                                                        UA_Variant *output)
{
    UA_StatusCode status_code;
    security_trust_import_service_report_t report = {0};
    char payload[384] = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    (void)security_trust_import_service_validate_staged(&report);
    model_write_trust_import_report_json(payload, sizeof(payload), &report);

    printf("[OPC UA]\tValidateImportedTrustList %s\n",
           report.valid ? "OK" : security_trust_import_service_result_to_string(report.result));
    fflush(stdout);
    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_apply_imported_trust_list(UA_Server *server,
                                                     const UA_NodeId *session_id,
                                                     void *session_context,
                                                     const UA_NodeId *method_id,
                                                     void *method_context,
                                                     const UA_NodeId *object_id,
                                                     void *object_context,
                                                     size_t input_size,
                                                     const UA_Variant *input,
                                                     size_t output_size,
                                                     UA_Variant *output)
{
    UA_StatusCode status_code;
    UA_Boolean result = false;
    UA_StatusCode refresh_status = UA_STATUSCODE_GOOD;
    security_trust_import_service_report_t report = {0};
    const char *details = "imported trust list validation failed";
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    if (security_trust_import_service_apply_staged(&report) == SECURITY_TRUST_IMPORT_RESULT_OK)
    {
        result = true;
        details = "imported trust list applied";
        refresh_status = model_refresh_runtime_trust_backends(server);
        if (refresh_status != UA_STATUSCODE_GOOD)
        {
            details = "trust list applied but runtime PKI refresh failed";
            printf("[OPC UA]\tApplyImportedTrustList runtime PKI refresh failed: %s\n",
                   UA_StatusCode_name(refresh_status));
            fflush(stdout);
        }
    }
    else
    {
        details = security_trust_import_service_result_to_string(report.result);
    }

    if (result == UA_TRUE)
    {
        printf("[OPC UA]\tApplyImportedTrustList succeeded\n");
    }
    else
    {
        printf("[OPC UA]\tApplyImportedTrustList failed: %s\n", details);
    }
    fflush(stdout);
    model_audit_session_event(session_context,
                              "trust-import-apply",
                              "device-security",
                              result == UA_TRUE,
                              details);
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_maintenance_event(server,
                                     result == UA_TRUE
                                         ? (refresh_status == UA_STATUSCODE_GOOD
                                                ? "Imported trust list applied"
                                                : "Imported trust list applied; runtime PKI refresh failed")
                                         : "Imported trust list apply failed",
                                     result == UA_TRUE
                                         ? (refresh_status == UA_STATUSCODE_GOOD ? 300u : 600u)
                                         : 700u,
                                     "TrustListImport",
                                     result == UA_TRUE ? 0u : 1u,
                                     model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_begin_config_import(UA_Server *server,
                                               const UA_NodeId *session_id,
                                               void *session_context,
                                               const UA_NodeId *method_id,
                                               void *method_context,
                                               const UA_NodeId *object_id,
                                               void *object_context,
                                               size_t input_size,
                                               const UA_Variant *input,
                                               size_t output_size,
                                               UA_Variant *output)
{
    UA_StatusCode status_code;
    UA_Boolean result = false;
    UA_UInt32 payload_size = 0u;
    UA_UInt32 payload_crc32 = 0u;
    UA_UInt32 schema_version = 0u;
    config_import_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    if (input_size != 3u || input == NULL ||
        !UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_UINT32]) || input[0].data == NULL ||
        !UA_Variant_hasScalarType(&input[1], &UA_TYPES[UA_TYPES_UINT32]) || input[1].data == NULL ||
        !UA_Variant_hasScalarType(&input[2], &UA_TYPES[UA_TYPES_UINT32]) || input[2].data == NULL)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    payload_size = *(const UA_UInt32 *)input[0].data;
    payload_crc32 = *(const UA_UInt32 *)input[1].data;
    schema_version = *(const UA_UInt32 *)input[2].data;

    service_result = config_import_service_begin((uint32_t)payload_size,
                                                 (uint32_t)payload_crc32,
                                                 (uint32_t)schema_version);
    result = (service_result == CONFIG_IMPORT_RESULT_OK) ? UA_TRUE : UA_FALSE;
    printf("[OPC UA]\tBeginConfigImport %s size=%lu crc32=0x%08lx schema=%lu\n",
           result == UA_TRUE ? "accepted" : config_import_service_result_to_string(service_result),
           (unsigned long)payload_size,
           (unsigned long)payload_crc32,
           (unsigned long)schema_version);
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_write_config_chunk(UA_Server *server,
                                              const UA_NodeId *session_id,
                                              void *session_context,
                                              const UA_NodeId *method_id,
                                              void *method_context,
                                              const UA_NodeId *object_id,
                                              void *object_context,
                                              size_t input_size,
                                              const UA_Variant *input,
                                              size_t output_size,
                                              UA_Variant *output)
{
    const UA_ByteString *chunk = NULL;
    UA_StatusCode status_code;
    UA_Boolean result = false;
    config_import_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    if (input_size != 1u || input == NULL || !model_get_input_ua_bytestring(&input[0], &chunk))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    service_result = config_import_service_write_chunk(chunk->data, chunk->length);
    result = (service_result == CONFIG_IMPORT_RESULT_OK) ? UA_TRUE : UA_FALSE;
    if (result != UA_TRUE)
    {
        printf("[OPC UA]\tWriteConfigChunk failed: %s (size=%lu)\n",
               config_import_service_result_to_string(service_result),
               (unsigned long)chunk->length);
        fflush(stdout);
    }
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_abort_config_import(UA_Server *server,
                                               const UA_NodeId *session_id,
                                               void *session_context,
                                               const UA_NodeId *method_id,
                                               void *method_context,
                                               const UA_NodeId *object_id,
                                               void *object_context,
                                               size_t input_size,
                                               const UA_Variant *input,
                                               size_t output_size,
                                               UA_Variant *output)
{
    UA_StatusCode status_code;
    UA_Boolean result = false;
    config_import_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    service_result = config_import_service_abort();
    result = (service_result == CONFIG_IMPORT_RESULT_OK) ? UA_TRUE : UA_FALSE;
    printf("[OPC UA]\tAbortConfigImport %s\n",
           result == UA_TRUE ? "succeeded" : config_import_service_result_to_string(service_result));
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_validate_imported_config(UA_Server *server,
                                                    const UA_NodeId *session_id,
                                                    void *session_context,
                                                    const UA_NodeId *method_id,
                                                    void *method_context,
                                                    const UA_NodeId *object_id,
                                                    void *object_context,
                                                    size_t input_size,
                                                    const UA_Variant *input,
                                                    size_t output_size,
                                                    UA_Variant *output)
{
    UA_StatusCode status_code;
    config_validate_service_report_t report = {0};
    char payload[384] = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    (void)config_validate_service_validate_imported(&report, NULL);
    if (!config_validate_service_write_report_json(payload, sizeof(payload), &report))
    {
        model_set_string_method_output(output_size, output, "{}");
        return UA_STATUSCODE_GOOD;
    }

    printf("[OPC UA]\tValidateImportedConfig %s\n",
           report.valid ? "OK" : config_validate_service_result_to_string(report.result));
    fflush(stdout);
    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_apply_imported_config(UA_Server *server,
                                                 const UA_NodeId *session_id,
                                                 void *session_context,
                                                 const UA_NodeId *method_id,
                                                 void *method_context,
                                                 const UA_NodeId *object_id,
                                                 void *object_context,
                                                 size_t input_size,
                                                 const UA_Variant *input,
                                                 size_t output_size,
                                                 UA_Variant *output)
{
    UA_StatusCode status_code;
    UA_Boolean result = false;
    config_validate_service_report_t report = {0};
    device_profile_config_t candidate = {0};
    const char *details = "imported configuration validation failed";
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    if (config_validate_service_validate_imported(&report, &candidate) != CONFIG_VALIDATE_RESULT_OK)
    {
        if (report.message[0] != '\0')
        {
            details = report.message;
        }
    }
    else if (!config_service_replace_and_save(&candidate, true))
    {
        details = "failed to persist imported configuration";
    }
    else
    {
        (void)config_import_service_abort();
        result = true;
        details = "imported configuration applied";
    }

    if (result == UA_TRUE)
    {
        printf("[OPC UA]\tApplyImportedConfig succeeded\n");
    }
    else
    {
        printf("[OPC UA]\tApplyImportedConfig failed: %s\n", details);
    }
    fflush(stdout);
    model_audit_session_event(session_context,
                              "config-import-apply",
                              "device-configuration",
                              result == UA_TRUE,
                              details);
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    if (result == UA_TRUE)
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_config_changed_event(server,
                                        "Imported configuration applied",
                                        300u,
                                        "ConfigurationImport",
                                        "*",
                                        model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}
static void model_format_firmware_update_status_json(char *buffer, size_t buffer_size)
{
    firmware_update_service_status_t status = {0};

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    firmware_update_service_get_status(&status);
    if (!status.ota_state_valid)
    {
        (void)snprintf(buffer,
                       buffer_size,
                       "{\"sessionActive\":%s,\"bytesWritten\":%lu,\"expectedImageSize\":%lu,\"remainingBytes\":%lu,\"expectedImageCrc32\":\"0x%08lx\",\"versionNumber\":%lu,\"lastResult\":\"%s\",\"maxChunkBytes\":%u,\"otaStateValid\":false}",
                       status.session_active ? "true" : "false",
                       (unsigned long)status.bytes_written,
                       (unsigned long)status.expected_image_size,
                       (unsigned long)status.remaining_bytes,
                       (unsigned long)status.expected_image_crc32,
                       (unsigned long)status.version_number,
                       firmware_update_service_result_to_string(status.last_result),
                       (unsigned)FIRMWARE_UPDATE_SERVICE_MAX_CHUNK_BYTES);
        return;
    }

    (void)snprintf(buffer,
                   buffer_size,
                   "{\"sessionActive\":%s,\"bytesWritten\":%lu,\"expectedImageSize\":%lu,\"remainingBytes\":%lu,\"expectedImageCrc32\":\"0x%08lx\",\"versionNumber\":%lu,\"lastResult\":\"%s\",\"maxChunkBytes\":%u,\"otaStateValid\":true,\"otaState\":\"%s\",\"otaImageSize\":%lu,\"otaImageCrc32\":\"0x%08lx\",\"otaStagedOffset\":\"0x%08lx\",\"otaStagedSize\":\"0x%08lx\",\"otaBootAttempts\":%lu,\"otaLastError\":\"%s\"}",
                   status.session_active ? "true" : "false",
                   (unsigned long)status.bytes_written,
                   (unsigned long)status.expected_image_size,
                   (unsigned long)status.remaining_bytes,
                   (unsigned long)status.expected_image_crc32,
                   (unsigned long)status.version_number,
                   firmware_update_service_result_to_string(status.last_result),
                   (unsigned)FIRMWARE_UPDATE_SERVICE_MAX_CHUNK_BYTES,
                   ota_state_to_string(status.ota_state.state),
                   (unsigned long)status.ota_state.image_size,
                   (unsigned long)status.ota_state.image_crc32,
                   (unsigned long)status.ota_state.staged_bank_offset,
                   (unsigned long)status.ota_state.staged_bank_size,
                   (unsigned long)status.ota_state.boot_attempts,
                   ota_error_to_string(status.ota_state.last_error));
}

static UA_StatusCode model_get_firmware_update_status(UA_Server *server,
                                                      const UA_NodeId *session_id,
                                                      void *session_context,
                                                      const UA_NodeId *method_id,
                                                      void *method_context,
                                                      const UA_NodeId *object_id,
                                                      void *object_context,
                                                      size_t input_size,
                                                      const UA_Variant *input,
                                                      size_t output_size,
                                                      UA_Variant *output)
{
    UA_StatusCode status_code;
    char payload[512] = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    model_format_firmware_update_status_json(payload, sizeof(payload));
    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_begin_firmware_update(UA_Server *server,
                                                 const UA_NodeId *session_id,
                                                 void *session_context,
                                                 const UA_NodeId *method_id,
                                                 void *method_context,
                                                 const UA_NodeId *object_id,
                                                 void *object_context,
                                                 size_t input_size,
                                                 const UA_Variant *input,
                                                 size_t output_size,
                                                 UA_Variant *output)
{
    UA_StatusCode status_code;
    UA_Boolean result = false;
    UA_UInt32 image_size = 0u;
    UA_UInt32 image_crc32 = 0u;
    UA_UInt32 version_number = 0u;
    firmware_update_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    if (input_size != 3u || input == NULL ||
        !UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_UINT32]) || input[0].data == NULL ||
        !UA_Variant_hasScalarType(&input[1], &UA_TYPES[UA_TYPES_UINT32]) || input[1].data == NULL ||
        !UA_Variant_hasScalarType(&input[2], &UA_TYPES[UA_TYPES_UINT32]) || input[2].data == NULL)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    image_size = *(const UA_UInt32 *)input[0].data;
    image_crc32 = *(const UA_UInt32 *)input[1].data;
    version_number = *(const UA_UInt32 *)input[2].data;

    service_result = firmware_update_service_begin((uint32_t)image_size,
                                                   (uint32_t)image_crc32,
                                                   (uint32_t)version_number);
    result = (service_result == FIRMWARE_UPDATE_RESULT_OK) ? UA_TRUE : UA_FALSE;
    printf("[OPC UA]\tBeginFirmwareUpdate %s size=%lu crc32=0x%08lx version=%lu\n",
           result == UA_TRUE ? "accepted" : firmware_update_service_result_to_string(service_result),
           (unsigned long)image_size,
           (unsigned long)image_crc32,
           (unsigned long)version_number);
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_write_firmware_chunk(UA_Server *server,
                                                const UA_NodeId *session_id,
                                                void *session_context,
                                                const UA_NodeId *method_id,
                                                void *method_context,
                                                const UA_NodeId *object_id,
                                                void *object_context,
                                                size_t input_size,
                                                const UA_Variant *input,
                                                size_t output_size,
                                                UA_Variant *output)
{
    const UA_ByteString *chunk = NULL;
    UA_StatusCode status_code;
    UA_Boolean result = false;
    firmware_update_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    if (input_size != 1u || input == NULL || !model_get_input_ua_bytestring(&input[0], &chunk))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    service_result = firmware_update_service_write_chunk(chunk->data, chunk->length);
    result = (service_result == FIRMWARE_UPDATE_RESULT_OK) ? UA_TRUE : UA_FALSE;
    if (result != UA_TRUE)
    {
        printf("[OPC UA]\tWriteFirmwareChunk failed: %s (size=%lu)\n",
               firmware_update_service_result_to_string(service_result),
               (unsigned long)chunk->length);
        fflush(stdout);
    }
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_finalize_firmware_update(UA_Server *server,
                                                    const UA_NodeId *session_id,
                                                    void *session_context,
                                                    const UA_NodeId *method_id,
                                                    void *method_context,
                                                    const UA_NodeId *object_id,
                                                    void *object_context,
                                                    size_t input_size,
                                                    const UA_Variant *input,
                                                    size_t output_size,
                                                    UA_Variant *output)
{
    UA_StatusCode status_code;
    UA_Boolean result = false;
    firmware_update_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    service_result = firmware_update_service_finalize();
    result = (service_result == FIRMWARE_UPDATE_RESULT_OK) ? UA_TRUE : UA_FALSE;
    printf("[OPC UA]\tFinalizeFirmwareUpdate %s\n",
           result == UA_TRUE ? "succeeded" : firmware_update_service_result_to_string(service_result));
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_abort_firmware_update(UA_Server *server,
                                                 const UA_NodeId *session_id,
                                                 void *session_context,
                                                 const UA_NodeId *method_id,
                                                 void *method_context,
                                                 const UA_NodeId *object_id,
                                                 void *object_context,
                                                 size_t input_size,
                                                 const UA_Variant *input,
                                                 size_t output_size,
                                                 UA_Variant *output)
{
    UA_StatusCode status_code;
    UA_Boolean result = false;
    firmware_update_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    service_result = firmware_update_service_abort();
    result = (service_result == FIRMWARE_UPDATE_RESULT_OK) ? UA_TRUE : UA_FALSE;
    printf("[OPC UA]\tAbortFirmwareUpdate %s\n",
           result == UA_TRUE ? "succeeded" : firmware_update_service_result_to_string(service_result));
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_clear_channel_config(UA_Server *server,
                                                const UA_NodeId *session_id,
                                                void *session_context,
                                                const UA_NodeId *method_id,
                                                void *method_context,
                                                const UA_NodeId *object_id,
                                                void *object_context,
                                                size_t input_size,
                                                const UA_Variant *input,
                                                size_t output_size,
                                                UA_Variant *output)
{
    UA_StatusCode status;
    UA_Boolean result = false;
    UA_UInt32 slot_index = 0u;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != 1u || input == NULL ||
        !UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_UINT32]) || input[0].data == NULL)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    slot_index = *(const UA_UInt32 *)input[0].data;
    if (slot_index >= CONFIG_SERVICE_MAX_CHANNELS)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADOUTOFRANGE;
    }

    result = config_service_stage_clear_channel((size_t)slot_index);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_channel_config(UA_Server *server,
                                              const UA_NodeId *session_id,
                                              void *session_context,
                                              const UA_NodeId *method_id,
                                              void *method_context,
                                              const UA_NodeId *object_id,
                                              void *object_context,
                                              size_t input_size,
                                              const UA_Variant *input,
                                              size_t output_size,
                                              UA_Variant *output)
{
    static const size_t expected_input_size = 13u;
    UA_StatusCode status;
    UA_Boolean result = false;
    UA_UInt32 slot_index = 0u;
    UA_Boolean in_use = false;
    UA_Boolean enabled = false;
    UA_UInt32 driver_id = 0u;
    UA_UInt32 source_index = 0u;
    UA_UInt32 source_flags = 0u;
    config_channel_definition_t channel = {0};
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != expected_input_size || input == NULL ||
        !UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_UINT32]) || input[0].data == NULL ||
        !UA_Variant_hasScalarType(&input[1], &UA_TYPES[UA_TYPES_BOOLEAN]) || input[1].data == NULL ||
        !UA_Variant_hasScalarType(&input[2], &UA_TYPES[UA_TYPES_BOOLEAN]) || input[2].data == NULL ||
        !UA_Variant_hasScalarType(&input[3], &UA_TYPES[UA_TYPES_UINT32]) || input[3].data == NULL ||
        !UA_Variant_hasScalarType(&input[4], &UA_TYPES[UA_TYPES_UINT32]) || input[4].data == NULL ||
        !UA_Variant_hasScalarType(&input[5], &UA_TYPES[UA_TYPES_UINT32]) || input[5].data == NULL ||
        !UA_Variant_hasScalarType(&input[6], &UA_TYPES[UA_TYPES_UINT32]) || input[6].data == NULL ||
        !UA_Variant_hasScalarType(&input[7], &UA_TYPES[UA_TYPES_UINT32]) || input[7].data == NULL ||
        !UA_Variant_hasScalarType(&input[8], &UA_TYPES[UA_TYPES_UINT32]) || input[8].data == NULL ||
        !UA_Variant_hasScalarType(&input[9], &UA_TYPES[UA_TYPES_DOUBLE]) || input[9].data == NULL ||
        !UA_Variant_hasScalarType(&input[10], &UA_TYPES[UA_TYPES_DOUBLE]) || input[10].data == NULL ||
        !UA_Variant_hasScalarType(&input[11], &UA_TYPES[UA_TYPES_DOUBLE]) || input[11].data == NULL ||
        !UA_Variant_hasScalarType(&input[12], &UA_TYPES[UA_TYPES_DOUBLE]) || input[12].data == NULL)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    slot_index = *(const UA_UInt32 *)input[0].data;
    in_use = *(const UA_Boolean *)input[1].data;
    enabled = *(const UA_Boolean *)input[2].data;
    driver_id = *(const UA_UInt32 *)input[3].data;
    source_index = *(const UA_UInt32 *)input[4].data;
    source_flags = *(const UA_UInt32 *)input[5].data;

    if (slot_index >= CONFIG_SERVICE_MAX_CHANNELS ||
        driver_id > UINT8_MAX ||
        source_index > UINT8_MAX ||
        source_flags > UINT8_MAX)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADOUTOFRANGE;
    }

    if (in_use != UA_TRUE)
    {
        result = config_service_stage_clear_channel((size_t)slot_index);
    }
    else
    {
        (void)config_service_stage_copy_channel((size_t)slot_index, &channel);
        channel.in_use = 1u;
        channel.driver_id = (uint8_t)driver_id;
        channel.source_index = (uint8_t)source_index;
        channel.source_flags = (uint8_t)source_flags;
        channel.settings.enabled = enabled == UA_TRUE;
        channel.settings.sample_period_ms = *(const UA_UInt32 *)input[6].data;
        channel.settings.publish_period_ms = *(const UA_UInt32 *)input[7].data;
        channel.settings.history_interval_ms = *(const UA_UInt32 *)input[8].data;
        channel.settings.scale = *(const UA_Double *)input[9].data;
        channel.settings.offset = *(const UA_Double *)input[10].data;
        channel.settings.warning_threshold = *(const UA_Double *)input[11].data;
        channel.settings.critical_threshold = *(const UA_Double *)input[12].data;
        if (channel.flags == 0u)
        {
            channel.kind = 0u;
            channel.value_type = 0u;
        }

        result = config_service_stage_set_channel((size_t)slot_index, &channel);
    }

    printf("[OPC UA]\tSetChannelConfig %s for slot %u\n",
           result ? "accepted" : "failed",
           (unsigned)slot_index);
    fflush(stdout);

    model_audit_session_event(session_context,
                              "channel-stage-update",
                              "device-channel-configuration",
                              result,
                              result ? "channel configuration staged" : "channel configuration staging failed");
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        char changed_key[32];

        snprintf(changed_key, sizeof(changed_key), "ChannelSlot%u", (unsigned)slot_index);
        model_emit_config_changed_event(server,
                                        result ? "Channel configuration staged" : "Channel configuration staging failed",
                                        result ? 250u : 700u,
                                        "Channels",
                                        changed_key,
                                        model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif

    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_self_test(UA_Server *server,
                                     const UA_NodeId *session_id,
                                     void *session_context,
                                     const UA_NodeId *method_id,
                                     void *method_context,
                                     const UA_NodeId *object_id,
                                     void *object_context,
                                     size_t input_size,
                                     const UA_Variant *input,
                                     size_t output_size,
                                     UA_Variant *output)
{
    UA_StatusCode status;
    UA_Boolean result = true;
    network_manager_status_t network_status = {0};
    time_service_status_t time_status = {0};
    sensor_manager_snapshot_t sensor_snapshot = {0};
    telemetry_channel_t channel = {0};
    const device_profile_config_t *config = NULL;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    config = config_service_get();
    if (config == NULL || config->opcua_port == 0u || config->device_name[0] == '\0')
    {
        result = false;
    }

    network_manager_get_status(&network_status);
    if (!network_status.initialized)
    {
        result = false;
    }

    time_service_get_status(&time_status);
    if (time_status.quality == SERVICE_QUALITY_BAD)
    {
        result = false;
    }

    if (model_copy_channel_by_id(DEVICE_CHANNEL_ID_TEMPERATURE_1, &channel))
    {
        sensor_manager_get_snapshot(&sensor_snapshot);
        if (!channel.settings.enabled || !sensor_snapshot.enabled)
        {
            result = false;
        }
    }

    diag_service_set_last_fault_code(result ? DIAG_FAULT_CODE_NONE : DIAG_FAULT_CODE_SELF_TEST_FAILED);
    printf("[OPC UA]\tSelfTest %s\n", result ? "passed" : "failed");
    fflush(stdout);
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_maintenance_event(server,
                                     result ? "Self-test passed" : "Self-test failed",
                                     result ? 250u : 750u,
                                     "SelfTest",
                                     result ? 0u : 1u,
                                     model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_clear_diagnostics(UA_Server *server,
                                             const UA_NodeId *session_id,
                                             void *session_context,
                                             const UA_NodeId *method_id,
                                             void *method_context,
                                             const UA_NodeId *object_id,
                                             void *object_context,
                                             size_t input_size,
                                             const UA_Variant *input,
                                             size_t output_size,
                                             UA_Variant *output)
{
    UA_StatusCode status;
    UA_Boolean result = true;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    diag_service_clear();
    printf("[OPC UA]\tDiagnostics cleared\n");
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_trust_list(UA_Server *server,
                                          const UA_NodeId *session_id,
                                          void *session_context,
                                          const UA_NodeId *method_id,
                                          void *method_context,
                                          const UA_NodeId *object_id,
                                          void *object_context,
                                          size_t input_size,
                                          const UA_Variant *input,
                                          size_t output_size,
                                          UA_Variant *output)
{
    const UA_ByteString *trust_list = NULL;
    UA_StatusCode status;
    UA_StatusCode refresh_status = UA_STATUSCODE_GOOD;
    UA_Boolean result = false;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != 1u || input == NULL || !model_get_input_ua_bytestring(&input[0], &trust_list))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    result = security_service_set_trust_list(trust_list->data, trust_list->length);
    if (result && server != NULL)
    {
        refresh_status = model_refresh_runtime_trust_backends(server);
        if (refresh_status != UA_STATUSCODE_GOOD)
        {
            printf("[OPC UA]\tTrust list persisted but runtime PKI refresh failed: %s\n",
                   UA_StatusCode_name(refresh_status));
            fflush(stdout);
        }
    }

    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_enter_commissioning_mode(UA_Server *server,
                                                    const UA_NodeId *session_id,
                                                    void *session_context,
                                                    const UA_NodeId *method_id,
                                                    void *method_context,
                                                    const UA_NodeId *object_id,
                                                    void *object_context,
                                                    size_t input_size,
                                                    const UA_Variant *input,
                                                    size_t output_size,
                                                    UA_Variant *output)
{
    UA_StatusCode status;
    UA_Boolean result = false;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    result = security_service_set_commissioning_mode(true);
    if (result)
    {
        model_schedule_reboot(250u);
    }
    printf("[OPC UA]\tEnterCommissioningMode %s\n", result ? "succeeded" : "failed");
    fflush(stdout);
    model_audit_session_event(session_context,
                              "commissioning-enter",
                              "device-security",
                              result,
                              result ? "commissioning mode enabled" : "commissioning enable failed");
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_maintenance_event(server,
                                     result ? "Commissioning mode enabled" : "Commissioning mode enable failed",
                                     result ? 400u : 700u,
                                     "EnterCommissioningMode",
                                     result ? 0u : 1u,
                                     model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_exit_commissioning_mode(UA_Server *server,
                                                   const UA_NodeId *session_id,
                                                   void *session_context,
                                                   const UA_NodeId *method_id,
                                                   void *method_context,
                                                   const UA_NodeId *object_id,
                                                   void *object_context,
                                                   size_t input_size,
                                                   const UA_Variant *input,
                                                   size_t output_size,
                                                   UA_Variant *output)
{
    UA_StatusCode status;
    UA_Boolean result = false;
    security_service_blob_view_t trust_list = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (!security_service_get_trust_list(&trust_list) || trust_list.length == 0u)
    {
        printf("[OPC UA]\tExitCommissioningMode failed: trust list is empty\n");
        fflush(stdout);
        model_audit_session_event(session_context,
                                  "commissioning-exit",
                                  "device-security",
                                  false,
                                  "trust list is empty");
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
        {
            char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
            model_emit_maintenance_event(server,
                                         "Exit commissioning mode failed: trust list is empty",
                                         700u,
                                         "ExitCommissioningMode",
                                         1u,
                                         model_session_actor_name(session_context, actor, sizeof(actor)));
        }
#endif
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_GOOD;
    }

    result = security_service_set_commissioning_mode(false);
    if (result)
    {
        model_schedule_reboot(250u);
    }
    printf("[OPC UA]\tExitCommissioningMode %s\n", result ? "succeeded" : "failed");
    fflush(stdout);
    model_audit_session_event(session_context,
                              "commissioning-exit",
                              "device-security",
                              result,
                              result ? "strict mode scheduled after reboot" : "commissioning disable failed");
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_maintenance_event(server,
                                     result ? "Commissioning mode disabled" : "Commissioning mode disable failed",
                                     result ? 400u : 700u,
                                     "ExitCommissioningMode",
                                     result ? 0u : 1u,
                                     model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_add_user(UA_Server *server,
                                    const UA_NodeId *session_id,
                                    void *session_context,
                                    const UA_NodeId *method_id,
                                    void *method_context,
                                    const UA_NodeId *object_id,
                                    void *object_context,
                                    size_t input_size,
                                    const UA_Variant *input,
                                    size_t output_size,
                                    UA_Variant *output)
{
    UA_StatusCode status;
    UA_Boolean result = false;
    char username[SECURITY_SERVICE_USERNAME_MAX_LEN];
    char role_text[16];
    const UA_String *password = NULL;
    security_user_role_t role = SECURITY_ROLE_VIEWER;
    (void)server;
    (void)session_id;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_admin_session(session_context, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != 3u ||
        !model_copy_input_string(&input[0], username, sizeof(username)) ||
        !model_get_input_ua_string(&input[1], &password) ||
        !model_copy_input_string(&input[2], role_text, sizeof(role_text)))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    if (!security_service_role_from_string(role_text, &role))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADOUTOFRANGE;
    }

    result = security_service_add_user(username, password->data, password->length, role);
    printf("[OPC UA]\tAddUser %s for %s\n",
           result ? "succeeded" : "failed",
           username);
    fflush(stdout);
    model_audit_session_event(session_context,
                              "user-add",
                              username,
                              result,
                              security_service_role_to_string(role));
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_change_password(UA_Server *server,
                                           const UA_NodeId *session_id,
                                           void *session_context,
                                           const UA_NodeId *method_id,
                                           void *method_context,
                                           const UA_NodeId *object_id,
                                           void *object_context,
                                           size_t input_size,
                                           const UA_Variant *input,
                                           size_t output_size,
                                           UA_Variant *output)
{
    UA_StatusCode status;
    UA_Boolean result = false;
    char username[SECURITY_SERVICE_USERNAME_MAX_LEN];
    const UA_String *old_password = NULL;
    const UA_String *new_password = NULL;
    (void)server;
    (void)session_id;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_admin_session(session_context, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != 3u ||
        !model_copy_input_string(&input[0], username, sizeof(username)) ||
        !model_get_input_ua_string(&input[1], &old_password) ||
        !model_get_input_ua_string(&input[2], &new_password))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    result = security_service_change_password(username,
                                              old_password->data,
                                              old_password->length,
                                              new_password->data,
                                              new_password->length);
    printf("[OPC UA]\tChangePassword %s for %s\n",
           result ? "succeeded" : "failed",
           username);
    fflush(stdout);
    model_audit_session_event(session_context,
                              "password-change",
                              username,
                              result,
                              result ? "password updated" : "password update failed");
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_role(UA_Server *server,
                                    const UA_NodeId *session_id,
                                    void *session_context,
                                    const UA_NodeId *method_id,
                                    void *method_context,
                                    const UA_NodeId *object_id,
                                    void *object_context,
                                    size_t input_size,
                                    const UA_Variant *input,
                                    size_t output_size,
                                    UA_Variant *output)
{
    UA_StatusCode status;
    UA_Boolean result = false;
    char username[SECURITY_SERVICE_USERNAME_MAX_LEN];
    char role_text[16];
    security_user_role_t role = SECURITY_ROLE_VIEWER;
    (void)server;
    (void)session_id;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_admin_session(session_context, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != 2u ||
        !model_copy_input_string(&input[0], username, sizeof(username)) ||
        !model_copy_input_string(&input[1], role_text, sizeof(role_text)))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    if (!security_service_role_from_string(role_text, &role))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADOUTOFRANGE;
    }

    result = security_service_set_role(username, role);
    printf("[OPC UA]\tSetRole %s for %s -> %s\n",
           result ? "succeeded" : "failed",
           username,
           security_service_role_to_string(role));
    fflush(stdout);
    model_audit_session_event(session_context,
                              "role-change",
                              username,
                              result,
                              security_service_role_to_string(role));
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_disable_user(UA_Server *server,
                                        const UA_NodeId *session_id,
                                        void *session_context,
                                        const UA_NodeId *method_id,
                                        void *method_context,
                                        const UA_NodeId *object_id,
                                        void *object_context,
                                        size_t input_size,
                                        const UA_Variant *input,
                                        size_t output_size,
                                        UA_Variant *output)
{
    UA_StatusCode status;
    UA_Boolean result = false;
    char username[SECURITY_SERVICE_USERNAME_MAX_LEN];
    (void)server;
    (void)session_id;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_admin_session(session_context, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != 1u || !model_copy_input_string(&input[0], username, sizeof(username)))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    result = security_service_disable_user(username);
    printf("[OPC UA]\tDisableUser %s for %s\n",
           result ? "succeeded" : "failed",
           username);
    fflush(stdout);
    model_audit_session_event(session_context,
                              "user-disable",
                              username,
                              result,
                              result ? "user disabled" : "disable failed");
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_delete_user(UA_Server *server,
                                       const UA_NodeId *session_id,
                                       void *session_context,
                                       const UA_NodeId *method_id,
                                       void *method_context,
                                       const UA_NodeId *object_id,
                                       void *object_context,
                                       size_t input_size,
                                       const UA_Variant *input,
                                       size_t output_size,
                                       UA_Variant *output)
{
    UA_StatusCode status;
    UA_Boolean result = false;
    char username[SECURITY_SERVICE_USERNAME_MAX_LEN];
    (void)server;
    (void)session_id;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_admin_session(session_context, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != 1u || !model_copy_input_string(&input[0], username, sizeof(username)))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    result = security_service_delete_user(username);
    printf("[OPC UA]\tDeleteUser %s for %s\n",
           result ? "succeeded" : "failed",
           username);
    fflush(stdout);
    model_audit_session_event(session_context,
                              "user-delete",
                              username,
                              result,
                              result ? "user deleted" : "delete failed");
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_add_object(UA_Server *server,
                                      const char *node_id,
                                      const char *browse_name,
                                      const char *display_name,
                                      UA_NodeId parent_node_id,
                                      UA_NodeId reference_type_id,
                                      UA_NodeId type_definition_id)
{
    UA_ObjectAttributes attributes = UA_ObjectAttributes_default;
    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)display_name);

    return UA_Server_addObjectNode(server,
                                   UA_NODEID_STRING(1, (char *)node_id),
                                   parent_node_id,
                                   reference_type_id,
                                   UA_QUALIFIEDNAME(1, (char *)browse_name),
                                   type_definition_id,
                                   attributes,
                                   NULL,
                                   NULL);
}

static UA_StatusCode model_add_folder(UA_Server *server,
                                      const char *node_id,
                                      const char *display_name,
                                      UA_NodeId parent_node_id,
                                      UA_NodeId reference_type_id)
{
    return model_add_object(server,
                            node_id,
                            display_name,
                            display_name,
                            parent_node_id,
                            reference_type_id,
                            UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE));
}

static UA_StatusCode model_add_readonly_variable_ex(UA_Server *server,
                                                    const char *node_id,
                                                    const char *display_name,
                                                    UA_NodeId parent_node_id,
                                                    UA_NodeId data_type,
                                                    model_node_context_t *context,
                                                    UA_Byte access_level,
                                                    UA_Boolean historizing)
{
    UA_VariableAttributes attributes = UA_VariableAttributes_default;
    UA_DataSource data_source;
    UA_NodeId created_node_id = UA_NODEID_NULL;
    UA_StatusCode status = UA_STATUSCODE_GOOD;

    memset(&data_source, 0, sizeof(data_source));
    data_source.read = model_read_value;
    data_source.write = NULL;

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)display_name);
    attributes.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)display_name);
    attributes.dataType = data_type;
    attributes.valueRank = UA_VALUERANK_SCALAR;
    attributes.accessLevel = access_level;
    attributes.userAccessLevel = access_level;
    attributes.historizing = historizing;
    model_set_default_variant_value(&attributes, data_type);

    status = UA_Server_addVariableNode(server,
                                       UA_NODEID_STRING(1, (char *)node_id),
                                       parent_node_id,
                                       UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                       UA_QUALIFIEDNAME(1, (char *)display_name),
                                       UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
                                       attributes,
                                       context,
                                       &created_node_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    return UA_Server_setVariableNode_dataSource(server, created_node_id, data_source);
}

static UA_StatusCode model_add_readonly_variable(UA_Server *server,
                                                 const char *node_id,
                                                 const char *display_name,
                                                 UA_NodeId parent_node_id,
                                                 UA_NodeId data_type,
                                                 model_node_context_t *context)
{
    return model_add_readonly_variable_ex(server,
                                          node_id,
                                          display_name,
                                          parent_node_id,
                                          data_type,
                                          context,
                                          UA_ACCESSLEVELMASK_READ,
                                          false);
}

static UA_StatusCode model_add_writable_variable(UA_Server *server,
                                                 const char *node_id,
                                                 const char *display_name,
                                                 UA_NodeId parent_node_id,
                                                 UA_NodeId data_type,
                                                 model_node_context_t *context)
{
    UA_VariableAttributes attributes = UA_VariableAttributes_default;
    UA_DataSource data_source;
    UA_NodeId created_node_id = UA_NODEID_NULL;
    UA_StatusCode status = UA_STATUSCODE_GOOD;

    memset(&data_source, 0, sizeof(data_source));
    data_source.read = model_read_value;
    data_source.write = model_write_value;

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)display_name);
    attributes.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)display_name);
    attributes.dataType = data_type;
    attributes.valueRank = UA_VALUERANK_SCALAR;
    attributes.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    attributes.userAccessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    model_set_default_variant_value(&attributes, data_type);

    status = UA_Server_addVariableNode(server,
                                       UA_NODEID_STRING(1, (char *)node_id),
                                       parent_node_id,
                                       UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                       UA_QUALIFIEDNAME(1, (char *)display_name),
                                       UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
                                       attributes,
                                       context,
                                       &created_node_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    return UA_Server_setVariableNode_dataSource(server, created_node_id, data_source);
}

static void model_fill_string_argument(UA_Argument *argument,
                                       const char *name,
                                       const char *description)
{
    UA_Argument_init(argument);
    argument->name = UA_STRING((char *)(name != NULL ? name : ""));
    argument->description = UA_LOCALIZEDTEXT((char *)"en-US",
                                             (char *)(description != NULL ? description : ""));
    argument->dataType = UA_TYPES[UA_TYPES_STRING].typeId;
    argument->valueRank = UA_VALUERANK_SCALAR;
}

static void model_fill_bytestring_argument(UA_Argument *argument,
                                           const char *name,
                                           const char *description)
{
    UA_Argument_init(argument);
    argument->name = UA_STRING((char *)(name != NULL ? name : ""));
    argument->description = UA_LOCALIZEDTEXT((char *)"en-US",
                                             (char *)(description != NULL ? description : ""));
    argument->dataType = UA_TYPES[UA_TYPES_BYTESTRING].typeId;
    argument->valueRank = UA_VALUERANK_SCALAR;
}

static void model_fill_datetime_argument(UA_Argument *argument,
                                         const char *name,
                                         const char *description)
{
    UA_Argument_init(argument);
    argument->name = UA_STRING((char *)(name != NULL ? name : ""));
    argument->description = UA_LOCALIZEDTEXT((char *)"en-US",
                                             (char *)(description != NULL ? description : ""));
    argument->dataType = UA_TYPES[UA_TYPES_DATETIME].typeId;
    argument->valueRank = UA_VALUERANK_SCALAR;
}

static void model_fill_uint32_argument(UA_Argument *argument,
                                       const char *name,
                                       const char *description)
{
    UA_Argument_init(argument);
    argument->name = UA_STRING((char *)(name != NULL ? name : ""));
    argument->description = UA_LOCALIZEDTEXT((char *)"en-US",
                                             (char *)(description != NULL ? description : ""));
    argument->dataType = UA_TYPES[UA_TYPES_UINT32].typeId;
    argument->valueRank = UA_VALUERANK_SCALAR;
}

static void model_fill_double_argument(UA_Argument *argument,
                                       const char *name,
                                       const char *description)
{
    UA_Argument_init(argument);
    argument->name = UA_STRING((char *)(name != NULL ? name : ""));
    argument->description = UA_LOCALIZEDTEXT((char *)"en-US",
                                             (char *)(description != NULL ? description : ""));
    argument->dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
    argument->valueRank = UA_VALUERANK_SCALAR;
}

static void model_fill_boolean_argument(UA_Argument *argument,
                                        const char *name,
                                        const char *description)
{
    UA_Argument_init(argument);
    argument->name = UA_STRING((char *)(name != NULL ? name : ""));
    argument->description = UA_LOCALIZEDTEXT((char *)"en-US",
                                             (char *)(description != NULL ? description : ""));
    argument->dataType = UA_TYPES[UA_TYPES_BOOLEAN].typeId;
    argument->valueRank = UA_VALUERANK_SCALAR;
}

static void model_set_string_method_output(size_t output_size, UA_Variant *output, const char *result)
{
    if (output_size > 0)
    {
        UA_String string_value = UA_STRING((char *)(result != NULL ? result : ""));
        UA_Variant_setScalarCopy(&output[0], &string_value, &UA_TYPES[UA_TYPES_STRING]);
    }
}

static void model_set_bytestring_method_output(size_t output_size,
                                               UA_Variant *output,
                                               const UA_ByteString *result)
{
    if (output_size > 0)
    {
        UA_ByteString bytestring_value = UA_BYTESTRING_NULL;

        if (result != NULL)
        {
            bytestring_value = *result;
        }

        UA_Variant_setScalarCopy(&output[0], &bytestring_value, &UA_TYPES[UA_TYPES_BYTESTRING]);
    }
}

static UA_StatusCode model_add_method_node_with_boolean_result(UA_Server *server,
                                                               UA_NodeId maintenance_folder_id,
                                                               const char *node_id,
                                                               const char *method_name,
                                                               const char *description,
                                                               UA_MethodCallback callback,
                                                               size_t input_size,
                                                               UA_Argument *input_arguments)
{
    UA_MethodAttributes attributes = UA_MethodAttributes_default;
    UA_Argument output_argument;

    UA_Argument_init(&output_argument);
    output_argument.name = UA_STRING((char *)"Success");
    output_argument.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)"True when the method was accepted");
    output_argument.dataType = UA_TYPES[UA_TYPES_BOOLEAN].typeId;
    output_argument.valueRank = UA_VALUERANK_SCALAR;

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)method_name);
    attributes.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)description);
    attributes.executable = true;
    attributes.userExecutable = true;

    return UA_Server_addMethodNode(server,
                                   UA_NODEID_STRING(1, (char *)node_id),
                                   maintenance_folder_id,
                                   UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                   UA_QUALIFIEDNAME(1, (char *)method_name),
                                   attributes,
                                   callback,
                                   input_size,
                                   input_arguments,
                                   1,
                                   &output_argument,
                                   NULL,
                                   NULL);
}

static UA_StatusCode model_add_method_node_with_string_result(UA_Server *server,
                                                              UA_NodeId maintenance_folder_id,
                                                              const char *node_id,
                                                              const char *method_name,
                                                              const char *description,
                                                              UA_MethodCallback callback,
                                                              size_t input_size,
                                                              UA_Argument *input_arguments)
{
    UA_MethodAttributes attributes = UA_MethodAttributes_default;
    UA_Argument output_argument;

    UA_Argument_init(&output_argument);
    output_argument.name = UA_STRING((char *)"Result");
    output_argument.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)"Method result payload as JSON");
    output_argument.dataType = UA_TYPES[UA_TYPES_STRING].typeId;
    output_argument.valueRank = UA_VALUERANK_SCALAR;

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)method_name);
    attributes.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)description);
    attributes.executable = true;
    attributes.userExecutable = true;

    return UA_Server_addMethodNode(server,
                                   UA_NODEID_STRING(1, (char *)node_id),
                                   maintenance_folder_id,
                                   UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                   UA_QUALIFIEDNAME(1, (char *)method_name),
                                   attributes,
                                   callback,
                                   input_size,
                                   input_arguments,
                                   1,
                                   &output_argument,
                                    NULL,
                                    NULL);
}

static UA_StatusCode model_add_method_node_with_bytestring_result(UA_Server *server,
                                                                  UA_NodeId maintenance_folder_id,
                                                                  const char *node_id,
                                                                  const char *method_name,
                                                                  const char *description,
                                                                  UA_MethodCallback callback,
                                                                  size_t input_size,
                                                                  UA_Argument *input_arguments)
{
    UA_MethodAttributes attributes = UA_MethodAttributes_default;
    UA_Argument output_argument;

    UA_Argument_init(&output_argument);
    output_argument.name = UA_STRING((char *)"Payload");
    output_argument.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)"Method result payload as ByteString");
    output_argument.dataType = UA_TYPES[UA_TYPES_BYTESTRING].typeId;
    output_argument.valueRank = UA_VALUERANK_SCALAR;

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)method_name);
    attributes.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)description);
    attributes.executable = true;
    attributes.userExecutable = true;

    return UA_Server_addMethodNode(server,
                                   UA_NODEID_STRING(1, (char *)node_id),
                                   maintenance_folder_id,
                                   UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                   UA_QUALIFIEDNAME(1, (char *)method_name),
                                   attributes,
                                   callback,
                                   input_size,
                                   input_arguments,
                                   1,
                                   &output_argument,
                                   NULL,
                                   NULL);
}

static UA_StatusCode model_add_boolean_method_node(UA_Server *server,
                                                   UA_NodeId maintenance_folder_id,
                                                   const char *node_id,
                                                   const char *method_name,
                                                   const char *description,
                                                   UA_MethodCallback callback)
{
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     node_id,
                                                     method_name,
                                                     description,
                                                     callback,
                                                     0u,
                                                     NULL);
}

static UA_StatusCode model_add_alarm_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         "Device.Maintenance.AcknowledgeAllAlarms",
                                         "AcknowledgeAllAlarms",
                                         "Acknowledges all active alarms",
                                         model_acknowledge_alarms);
}

static UA_StatusCode model_add_reboot_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         "Device.Maintenance.Reboot",
                                         "Reboot",
                                         "Schedules a software reboot",
                                         model_request_reboot);
}

static UA_StatusCode model_add_factory_reset_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_string_argument(&input_argument,
                               "Scope",
                               "Reset scope. Supported value in this build: all_keep_identity. Empty string also maps to that default.");
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     "Device.Maintenance.FactoryReset",
                                                     "FactoryReset",
                                                     "Erases persisted configuration slots for the requested scope and reboots the device.",
                                                     model_factory_reset,
                                                     1u,
                                                     &input_argument);
}

static UA_StatusCode model_add_sync_time_now_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         "Device.Maintenance.SyncTimeNow",
                                         "SyncTimeNow",
                                         "Forces immediate SNTP resynchronization",
                                         model_sync_time_now);
}

static UA_StatusCode model_add_blink_identify_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         "Device.Maintenance.BlinkIdentify",
                                         "BlinkIdentify",
                                         "Blinks the onboard identify LED",
                                         model_blink_identify);
}

static UA_StatusCode model_add_apply_config_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         "Device.Maintenance.ApplyConfig",
                                         "ApplyConfig",
                                         "Applies the current runtime configuration",
                                         model_apply_config);
}

static UA_StatusCode model_add_save_config_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         "Device.Maintenance.SaveConfig",
                                         "SaveConfig",
                                         "Persists the current configuration to flash",
                                         model_save_config);
}

static UA_StatusCode model_add_set_mqtt_pubsub_enabled_method_node(UA_Server *server,
                                                                   UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_boolean_argument(&input_argument,
                                "Enabled",
                                "True enables MQTT PubSub in configuration; requires SaveConfig and Reboot to take effect");
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_SET_MQTT_PUBSUB_ENABLED,
                                                     "SetMqttPubSubEnabled",
                                                     "Updates the MQTT PubSub enable flag in runtime configuration",
                                                     model_set_mqtt_pubsub_enabled,
                                                     1u,
                                                     &input_argument);
}

static UA_StatusCode model_add_set_device_identity_method_node(UA_Server *server,
                                                               UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[10];

    model_fill_string_argument(&input_arguments[0], "DeviceName", "Human-readable device name");
    model_fill_string_argument(&input_arguments[1], "ProductInstanceUri", "Stable production URI");
    model_fill_string_argument(&input_arguments[2], "SerialNumber", "Production serial number");
    model_fill_string_argument(&input_arguments[3], "AssetId", "Plant asset identifier");
    model_fill_string_argument(&input_arguments[4], "HWRevision", "Hardware revision");
    model_fill_string_argument(&input_arguments[5], "FWRevision", "Firmware revision");
    model_fill_datetime_argument(&input_arguments[6], "ManufactureDate", "Manufacturing timestamp in UTC");
    model_fill_datetime_argument(&input_arguments[7], "CalibrationDate", "Calibration timestamp in UTC");
    model_fill_string_argument(&input_arguments[8], "Location", "Site, line and position string");
    model_fill_string_argument(&input_arguments[9], "AssetClass", "Asset classification");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_SET_DEVICE_IDENTITY,
                                                     "SetDeviceIdentity",
                                                     "Stages production identity fields.",
                                                     model_set_device_identity,
                                                     10u,
                                                     input_arguments);
}

static UA_StatusCode model_add_set_network_config_method_node(UA_Server *server,
                                                              UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[9];

    model_fill_boolean_argument(&input_arguments[0], "UseDhcp", "True selects DHCP, false selects static IPv4");
    model_fill_string_argument(&input_arguments[1], "MacAddress", "MAC address in AA:BB:CC:DD:EE:FF format");
    model_fill_string_argument(&input_arguments[2], "Hostname", "Hostname used in the endpoint URL");
    model_fill_uint32_argument(&input_arguments[3], "OpcUaPort", "TCP port for the OPC UA endpoint");
    model_fill_string_argument(&input_arguments[4], "NtpServer", "NTP server hostname");
    model_fill_string_argument(&input_arguments[5], "IPv4", "Static IPv4 address");
    model_fill_string_argument(&input_arguments[6], "Netmask", "Static IPv4 netmask");
    model_fill_string_argument(&input_arguments[7], "Gateway", "Static IPv4 gateway");
    model_fill_string_argument(&input_arguments[8], "Dns", "Static DNS server IPv4 address");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_SET_NETWORK_CONFIG,
                                                     "SetNetworkConfig",
                                                     "Stages network and endpoint settings.",
                                                     model_set_network_config,
                                                     9u,
                                                     input_arguments);
}

static UA_StatusCode model_add_set_channel_config_method_node(UA_Server *server,
                                                              UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[13];

    model_fill_uint32_argument(&input_arguments[0], "SlotIndex", "Configuration slot index in the staged channel table");
    model_fill_boolean_argument(&input_arguments[1], "InUse", "True keeps the slot active; false clears it");
    model_fill_boolean_argument(&input_arguments[2], "Enabled", "True enables acquisition for the slot once applied");
    model_fill_uint32_argument(&input_arguments[3], "DriverId", "Built-in driver id: 1 internal_temp, 2 adc_voltage, 3 gpio_input");
    model_fill_uint32_argument(&input_arguments[4], "SourceIndex", "Driver-specific source index, for example ADC channel or GPIO number");
    model_fill_uint32_argument(&input_arguments[5], "SourceFlags", "Driver-specific flags such as invert or pull configuration");
    model_fill_uint32_argument(&input_arguments[6], "SamplePeriodMs", "Sampling period in milliseconds");
    model_fill_uint32_argument(&input_arguments[7], "PublishPeriodMs", "Publish period in milliseconds");
    model_fill_uint32_argument(&input_arguments[8], "HistoryIntervalMs", "Historian record interval in milliseconds");
    model_fill_double_argument(&input_arguments[9], "Scale", "Linear scale multiplier");
    model_fill_double_argument(&input_arguments[10], "Offset", "Linear offset added after scale");
    model_fill_double_argument(&input_arguments[11], "WarningThreshold", "Warning threshold for analog channels");
    model_fill_double_argument(&input_arguments[12], "CriticalThreshold", "Critical threshold for analog channels");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_SET_CHANNEL_CONFIG,
                                                     "SetChannelConfig",
                                                     "Stages one channel slot. ApplyConfig activates runtime sampling changes; SaveConfig persists them; reboot refreshes OPC UA channel objects after topology changes.",
                                                     model_set_channel_config,
                                                     13u,
                                                     input_arguments);
}

static UA_StatusCode model_add_get_channel_config_method_node(UA_Server *server,
                                                              UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_uint32_argument(&input_argument, "SlotIndex", "Configuration slot index in the staged channel table");
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_GET_CHANNEL_CONFIG,
                                                    "GetChannelConfig",
                                                    "Returns one staged channel configuration as JSON.",
                                                    model_get_channel_config,
                                                    1u,
                                                    &input_argument);
}

static UA_StatusCode model_add_clear_channel_config_method_node(UA_Server *server,
                                                                UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_uint32_argument(&input_argument, "SlotIndex", "Configuration slot index to clear from the staged channel table");
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_CLEAR_CHANNEL_CONFIG,
                                                     "ClearChannelConfig",
                                                     "Clears one staged channel configuration slot.",
                                                     model_clear_channel_config,
                                                     1u,
                                                     &input_argument);
}

static UA_StatusCode model_add_list_drivers_method_node(UA_Server *server,
                                                        UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_LIST_DRIVERS,
                                                    "ListDrivers",
                                                    "Returns the built-in driver catalog as JSON.",
                                                    model_list_drivers,
                                                    0u,
                                                    NULL);
}

static UA_StatusCode model_add_get_config_state_method_node(UA_Server *server,
                                                            UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_GET_CONFIG_STATE,
                                                    "GetConfigState",
                                                    "Returns running and staged configuration state as JSON.",
                                                    model_get_config_state,
                                                    0u,
                                                    NULL);
}

static UA_StatusCode model_add_export_config_method_node(UA_Server *server,
                                                         UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_EXPORT_CONFIG,
                                                    "ExportConfig",
                                                    "Returns the current effective device configuration as JSON.",
                                                    model_export_config,
                                                    0u,
                                                    NULL);
}

static UA_StatusCode model_add_begin_config_import_method_node(UA_Server *server,
                                                               UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[3];

    model_fill_uint32_argument(&input_arguments[0], "PayloadSizeBytes", "Total configuration payload size in bytes");
    model_fill_uint32_argument(&input_arguments[1], "PayloadCrc32", "Expected CRC32 of the configuration payload");
    model_fill_uint32_argument(&input_arguments[2], "SchemaVersion", "Configuration schema version declared by the client payload");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_BEGIN_CONFIG_IMPORT,
                                                     "BeginConfigImport",
                                                     "Starts staged configuration upload into a temporary import buffer.",
                                                     model_begin_config_import,
                                                     3u,
                                                     input_arguments);
}

static UA_StatusCode model_add_write_config_chunk_method_node(UA_Server *server,
                                                              UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_bytestring_argument(&input_argument,
                                   "Chunk",
                                   "Next configuration payload chunk. Maximum accepted chunk is 4096 bytes and data must be sent sequentially.");
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_WRITE_CONFIG_CHUNK,
                                                     "WriteConfigChunk",
                                                     "Appends the next sequential configuration payload chunk to the staged import buffer.",
                                                     model_write_config_chunk,
                                                     1u,
                                                     &input_argument);
}

static UA_StatusCode model_add_abort_config_import_method_node(UA_Server *server,
                                                               UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         MODEL_METHOD_ABORT_CONFIG_IMPORT,
                                         "AbortConfigImport",
                                         "Aborts the staged configuration upload session and frees the import buffer.",
                                         model_abort_config_import);
}

static UA_StatusCode model_add_validate_imported_config_method_node(UA_Server *server,
                                                                    UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_VALIDATE_IMPORTED_CONFIG,
                                                    "ValidateImportedConfig",
                                                    "Validates the currently staged imported configuration payload and returns JSON diagnostics.",
                                                    model_validate_imported_config,
                                                    0u,
                                                    NULL);
}

static UA_StatusCode model_add_apply_imported_config_method_node(UA_Server *server,
                                                                 UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         MODEL_METHOD_APPLY_IMPORTED_CONFIG,
                                         "ApplyImportedConfig",
                                         "Validates, persists, and applies the currently staged imported configuration payload.",
                                         model_apply_imported_config);
}

static UA_StatusCode model_add_get_trust_state_method_node(UA_Server *server,
                                                           UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_GET_TRUST_STATE,
                                                    "GetTrustState",
                                                    "Returns persistent trust-list and staged import state as JSON.",
                                                    model_get_trust_state,
                                                    0u,
                                                    NULL);
}

static UA_StatusCode model_add_export_trust_list_method_node(UA_Server *server,
                                                             UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_bytestring_result(server,
                                                        maintenance_folder_id,
                                                        MODEL_METHOD_EXPORT_TRUST_LIST,
                                                        "ExportTrustList",
                                                        "Returns the currently active persisted trust-list payload as a raw ByteString.",
                                                        model_export_trust_list,
                                                        0u,
                                                        NULL);
}

static UA_StatusCode model_add_begin_trust_list_import_method_node(UA_Server *server,
                                                                   UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[3];

    model_fill_uint32_argument(&input_arguments[0], "PayloadSizeBytes", "Total trust-list payload size in bytes");
    model_fill_uint32_argument(&input_arguments[1], "PayloadCrc32", "Expected CRC32 of the trust-list payload");
    model_fill_uint32_argument(&input_arguments[2], "SchemaVersion", "Trust-list payload schema version declared by the client");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_BEGIN_TRUST_LIST_IMPORT,
                                                     "BeginTrustListImport",
                                                     "Starts staged trust-list upload into a temporary import buffer.",
                                                     model_begin_trust_list_import,
                                                     3u,
                                                     input_arguments);
}

static UA_StatusCode model_add_write_trust_list_chunk_method_node(UA_Server *server,
                                                                  UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_bytestring_argument(&input_argument,
                                   "Chunk",
                                   "Next trust-list payload chunk. Maximum accepted chunk is 4096 bytes and data must be sent sequentially.");
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_WRITE_TRUST_LIST_CHUNK,
                                                     "WriteTrustListChunk",
                                                     "Appends the next sequential trust-list payload chunk to the staged import buffer.",
                                                     model_write_trust_list_chunk,
                                                     1u,
                                                     &input_argument);
}

static UA_StatusCode model_add_abort_trust_list_import_method_node(UA_Server *server,
                                                                   UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         MODEL_METHOD_ABORT_TRUST_LIST_IMPORT,
                                         "AbortTrustListImport",
                                         "Aborts the staged trust-list upload session and frees the import buffer.",
                                         model_abort_trust_list_import);
}

static UA_StatusCode model_add_validate_imported_trust_list_method_node(UA_Server *server,
                                                                        UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_VALIDATE_IMPORTED_TRUST_LIST,
                                                    "ValidateImportedTrustList",
                                                    "Validates the currently staged trust-list payload and returns JSON diagnostics.",
                                                    model_validate_imported_trust_list,
                                                    0u,
                                                    NULL);
}

static UA_StatusCode model_add_apply_imported_trust_list_method_node(UA_Server *server,
                                                                     UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         MODEL_METHOD_APPLY_IMPORTED_TRUST_LIST,
                                         "ApplyImportedTrustList",
                                         "Validates, persists, and activates the currently staged trust-list payload.",
                                         model_apply_imported_trust_list);
}

static UA_StatusCode model_add_get_certificate_state_method_node(UA_Server *server,
                                                                 UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_GET_CERTIFICATE_STATE,
                                                    "GetCertificateState",
                                                    "Returns active certificate metadata and staged identity import state as JSON.",
                                                    model_get_certificate_state,
                                                    0u,
                                                    NULL);
}

static UA_StatusCode model_add_export_certificate_metadata_method_node(UA_Server *server,
                                                                       UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_EXPORT_CERTIFICATE_METADATA,
                                                    "ExportCertificateMetadata",
                                                    "Returns the currently active device certificate metadata as JSON.",
                                                    model_export_certificate_metadata,
                                                    0u,
                                                    NULL);
}

static UA_StatusCode model_add_begin_identity_rotation_method_node(UA_Server *server,
                                                                   UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[3];

    model_fill_uint32_argument(&input_arguments[0], "PayloadSizeBytes", "Total identity rotation package size in bytes");
    model_fill_uint32_argument(&input_arguments[1], "PayloadCrc32", "Expected CRC32 of the identity rotation package");
    model_fill_uint32_argument(&input_arguments[2], "SchemaVersion", "Identity rotation package schema version declared by the client");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_BEGIN_IDENTITY_ROTATION,
                                                     "BeginIdentityRotation",
                                                     "Starts staged upload of a new device certificate and private key package.",
                                                     model_begin_identity_rotation,
                                                     3u,
                                                     input_arguments);
}

static UA_StatusCode model_add_write_identity_rotation_chunk_method_node(UA_Server *server,
                                                                         UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_bytestring_argument(&input_argument,
                                   "Chunk",
                                   "Next identity rotation payload chunk. Maximum accepted chunk is 4096 bytes and data must be sent sequentially.");
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_WRITE_IDENTITY_ROTATION_CHUNK,
                                                     "WriteIdentityRotationChunk",
                                                     "Appends the next sequential identity rotation payload chunk to the staged import buffer.",
                                                     model_write_identity_rotation_chunk,
                                                     1u,
                                                     &input_argument);
}

static UA_StatusCode model_add_abort_identity_rotation_method_node(UA_Server *server,
                                                                   UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         MODEL_METHOD_ABORT_IDENTITY_ROTATION,
                                         "AbortIdentityRotation",
                                         "Aborts the staged identity rotation upload session and frees the import buffer.",
                                         model_abort_identity_rotation);
}

static UA_StatusCode model_add_validate_staged_identity_method_node(UA_Server *server,
                                                                    UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_VALIDATE_STAGED_IDENTITY,
                                                    "ValidateStagedIdentity",
                                                    "Validates the currently staged identity rotation package and returns JSON diagnostics.",
                                                    model_validate_staged_identity,
                                                    0u,
                                                    NULL);
}

static UA_StatusCode model_add_apply_staged_identity_method_node(UA_Server *server,
                                                                 UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         MODEL_METHOD_APPLY_STAGED_IDENTITY,
                                         "ApplyStagedIdentity",
                                         "Validates and persists the currently staged identity rotation package for activation after reboot.",
                                         model_apply_staged_identity);
}

static UA_StatusCode model_add_get_firmware_update_status_method_node(UA_Server *server,
                                                                      UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_GET_FIRMWARE_UPDATE_STATUS,
                                                    "GetFirmwareUpdateStatus",
                                                    "Returns OTA staging session status and persistent OTA state as JSON.",
                                                    model_get_firmware_update_status,
                                                    0u,
                                                    NULL);
}

static UA_StatusCode model_add_begin_firmware_update_method_node(UA_Server *server,
                                                                 UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[3];

    model_fill_uint32_argument(&input_arguments[0], "ImageSizeBytes", "Total OTA payload size in bytes");
    model_fill_uint32_argument(&input_arguments[1], "ImageCrc32", "Expected CRC32 of the OTA payload");
    model_fill_uint32_argument(&input_arguments[2], "VersionNumber", "Monotonic firmware version number for OTA state tracking");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_BEGIN_FIRMWARE_UPDATE,
                                                     "BeginFirmwareUpdate",
                                                     "Starts staging a new firmware image into Bank B and resets OTA state to IDLE for the new session.",
                                                     model_begin_firmware_update,
                                                     3u,
                                                     input_arguments);
}

static UA_StatusCode model_add_write_firmware_chunk_method_node(UA_Server *server,
                                                                UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_bytestring_argument(&input_argument,
                                   "Chunk",
                                   "Next firmware payload chunk. Maximum accepted chunk is 4096 bytes and data must be sent sequentially.");
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_WRITE_FIRMWARE_CHUNK,
                                                     "WriteFirmwareChunk",
                                                     "Appends the next sequential firmware payload chunk to Bank B staging storage.",
                                                     model_write_firmware_chunk,
                                                     1u,
                                                     &input_argument);
}

static UA_StatusCode model_add_finalize_firmware_update_method_node(UA_Server *server,
                                                                    UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         MODEL_METHOD_FINALIZE_FIRMWARE_UPDATE,
                                         "FinalizeFirmwareUpdate",
                                         "Verifies the staged Bank B image by CRC32 and writes OTA state as STAGED_IN_BANK_B.",
                                         model_finalize_firmware_update);
}

static UA_StatusCode model_add_abort_firmware_update_method_node(UA_Server *server,
                                                                 UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         MODEL_METHOD_ABORT_FIRMWARE_UPDATE,
                                         "AbortFirmwareUpdate",
                                         "Aborts the active staging session and resets OTA state back to IDLE.",
                                         model_abort_firmware_update);
}

static UA_StatusCode model_add_self_test_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         "Device.Maintenance.SelfTest",
                                         "SelfTest",
                                         "Runs a basic service health check",
                                         model_self_test);
}

static UA_StatusCode model_add_clear_diagnostics_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         "Device.Maintenance.ClearDiagnostics",
                                         "ClearDiagnostics",
                                         "Clears software diagnostic fault state",
                                         model_clear_diagnostics);
}

static UA_StatusCode model_add_set_trust_list_method_node(UA_Server *server, UA_NodeId security_folder_id)
{
    UA_Argument input_argument;

    model_fill_bytestring_argument(&input_argument,
                                   "TrustList",
                                   "Trusted client certificate or bundle as a raw ByteString. Empty clears the trust list.");
    return model_add_method_node_with_boolean_result(server,
                                                     security_folder_id,
                                                     MODEL_METHOD_SET_TRUST_LIST,
                                                     "SetTrustList",
                                                     "Stores trust anchors in persistent security storage.",
                                                     model_set_trust_list,
                                                     1u,
                                                     &input_argument);
}

static UA_StatusCode model_add_enter_commissioning_method_node(UA_Server *server, UA_NodeId security_folder_id)
{
    return model_add_boolean_method_node(server,
                                         security_folder_id,
                                         MODEL_METHOD_ENTER_COMMISSIONING,
                                         "EnterCommissioningMode",
                                         "Temporarily enables commissioning mode with weakened certificate checks",
                                         model_enter_commissioning_mode);
}

static UA_StatusCode model_add_exit_commissioning_method_node(UA_Server *server, UA_NodeId security_folder_id)
{
    return model_add_boolean_method_node(server,
                                         security_folder_id,
                                         MODEL_METHOD_EXIT_COMMISSIONING,
                                         "ExitCommissioningMode",
                                         "Disables commissioning mode and requires a provisioned trust list",
                                         model_exit_commissioning_mode);
}

static UA_StatusCode model_add_add_user_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[3];

    model_fill_string_argument(&input_arguments[0], "Username", "Name of the user to create");
    model_fill_string_argument(&input_arguments[1], "Password", "Initial password");
    model_fill_string_argument(&input_arguments[2], "Role", "One of Viewer, Operator, Service or Admin");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_ADD_USER,
                                                     "AddUser",
                                                     "Creates a new local user",
                                                     model_add_user,
                                                     3u,
                                                     input_arguments);
}

static UA_StatusCode model_add_change_password_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[3];

    model_fill_string_argument(&input_arguments[0], "Username", "Name of the user");
    model_fill_string_argument(&input_arguments[1], "OldPassword", "Current password");
    model_fill_string_argument(&input_arguments[2], "NewPassword", "Replacement password");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_CHANGE_PASSWORD,
                                                     "ChangePassword",
                                                     "Changes an existing local user password",
                                                     model_change_password,
                                                     3u,
                                                     input_arguments);
}

static UA_StatusCode model_add_set_role_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[2];

    model_fill_string_argument(&input_arguments[0], "Username", "Name of the user");
    model_fill_string_argument(&input_arguments[1], "Role", "One of Viewer, Operator, Service or Admin");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_SET_ROLE,
                                                     "SetRole",
                                                     "Updates the local role assignment of a user",
                                                     model_set_role,
                                                     2u,
                                                     input_arguments);
}

static UA_StatusCode model_add_disable_user_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_string_argument(&input_argument, "Username", "Name of the user to disable");
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_DISABLE_USER,
                                                     "DisableUser",
                                                     "Disables an existing local user",
                                                     model_disable_user,
                                                     1u,
                                                     &input_argument);
}

static UA_StatusCode model_add_delete_user_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_string_argument(&input_argument, "Username", "Name of the user to delete");
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_DELETE_USER,
                                                     "DeleteUser",
                                                     "Deletes an existing local user",
                                                     model_delete_user,
                                                     1u,
                                                     &input_argument);
}

static UA_StatusCode model_add_task_stack_high_water_node(UA_Server *server,
                                                          UA_NodeId task_folder_id,
                                                          const char *task_object_id,
                                                          const char *task_display_name,
                                                          model_node_context_t *task_context)
{
    char stack_node_id[96] = {0};
    UA_StatusCode status = UA_STATUSCODE_GOOD;

    status = model_add_object(server,
                              task_object_id,
                              task_display_name,
                              task_display_name,
                              task_folder_id,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    snprintf(stack_node_id, sizeof(stack_node_id), "%s.StackHighWatermark", task_object_id);

    return model_add_readonly_variable(server,
                                       stack_node_id,
                                       "StackHighWatermark",
                                       UA_NODEID_STRING(1, (char *)task_object_id),
                                       UA_TYPES[UA_TYPES_UINT32].typeId,
                                       task_context);
}

static model_node_context_t *model_prepare_channel_context(size_t channel_index,
                                                           size_t field_index,
                                                           model_node_kind_t kind,
                                                           const char *channel_id)
{
    model_node_context_t *context = NULL;

    if (channel_index >= CHANNEL_MANAGER_MAX_CHANNELS || field_index >= MODEL_CHANNEL_FIELD_CONTEXT_COUNT)
    {
        return NULL;
    }

    context = &g_channel_node_contexts[channel_index][field_index];
    context->kind = kind;
    context->channel_id = channel_id;
    context->task_name = NULL;
    return context;
}

static UA_StatusCode model_add_channel_nodes(UA_Server *server,
                                             UA_NodeId channels_folder_id,
                                             size_t channel_index,
                                             const telemetry_channel_t *channel)
{
    char channel_node_id[128];
    char value_node_id[128];
    char status_node_id[128];
    char source_timestamp_node_id[128];
    char units_node_id[128];
    char update_period_node_id[128];
    char history_interval_node_id[128];
    char alarm_state_node_id[128];
    UA_NodeId channel_object_id = UA_NODEID_NULL;
    UA_StatusCode status = UA_STATUSCODE_GOOD;
    UA_NodeId value_data_type;

    if (server == NULL || channel == NULL || channel->metadata.id == NULL || channel->metadata.name == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    if (!model_build_channel_node_id(channel, NULL, channel_node_id, sizeof(channel_node_id)) ||
        !model_build_channel_node_id(channel, "Value", value_node_id, sizeof(value_node_id)) ||
        !model_build_channel_node_id(channel, "Status", status_node_id, sizeof(status_node_id)) ||
        !model_build_channel_node_id(channel, "SourceTimestamp", source_timestamp_node_id, sizeof(source_timestamp_node_id)) ||
        !model_build_channel_node_id(channel, "Units", units_node_id, sizeof(units_node_id)) ||
        !model_build_channel_node_id(channel, "UpdatePeriodMs", update_period_node_id, sizeof(update_period_node_id)) ||
        !model_build_channel_node_id(channel, "HistoryIntervalMs", history_interval_node_id, sizeof(history_interval_node_id)) ||
        !model_build_channel_node_id(channel, "AlarmState", alarm_state_node_id, sizeof(alarm_state_node_id)))
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    channel_object_id = UA_NODEID_STRING(1, channel_node_id);
    value_data_type = model_channel_value_type_to_data_type(channel->metadata.value_type);

    status = model_add_object(server,
                              channel_node_id,
                              channel->metadata.name,
                              channel->metadata.name,
                              channels_folder_id,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable_ex(server,
                                            value_node_id,
                                            "Value",
                                            channel_object_id,
                                            value_data_type,
                                            model_prepare_channel_context(channel_index,
                                                                          0u,
                                                                          MODEL_NODE_PROCESS_TEMPERATURE_VALUE,
                                                                          channel->metadata.id),
                                            UA_ACCESSLEVELMASK_READ |
                                                (((channel->metadata.flags & TELEMETRY_CHANNEL_FLAG_HISTORIZED) != 0u)
                                                     ? UA_ACCESSLEVELMASK_HISTORYREAD
                                                     : 0u),
                                            ((channel->metadata.flags & TELEMETRY_CHANNEL_FLAG_HISTORIZED) != 0u));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         status_node_id,
                                         "Status",
                                         channel_object_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         model_prepare_channel_context(channel_index,
                                                                       1u,
                                                                       MODEL_NODE_PROCESS_TEMPERATURE_STATUS,
                                                                       channel->metadata.id));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         source_timestamp_node_id,
                                         "SourceTimestamp",
                                         channel_object_id,
                                         UA_TYPES[UA_TYPES_DATETIME].typeId,
                                         model_prepare_channel_context(channel_index,
                                                                       2u,
                                                                       MODEL_NODE_PROCESS_TEMPERATURE_SOURCE_TIMESTAMP,
                                                                       channel->metadata.id));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         units_node_id,
                                         "Units",
                                         channel_object_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         model_prepare_channel_context(channel_index,
                                                                       3u,
                                                                       MODEL_NODE_PROCESS_TEMPERATURE_UNITS,
                                                                       channel->metadata.id));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_writable_variable(server,
                                         update_period_node_id,
                                         "UpdatePeriodMs",
                                         channel_object_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         model_prepare_channel_context(channel_index,
                                                                       4u,
                                                                       MODEL_NODE_PROCESS_TEMPERATURE_UPDATE_PERIOD_MS,
                                                                       channel->metadata.id));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_writable_variable(server,
                                         history_interval_node_id,
                                         "HistoryIntervalMs",
                                         channel_object_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         model_prepare_channel_context(channel_index,
                                                                       5u,
                                                                       MODEL_NODE_PROCESS_TEMPERATURE_HISTORY_INTERVAL_MS,
                                                                       channel->metadata.id));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    return model_add_readonly_variable(server,
                                       alarm_state_node_id,
                                       "AlarmState",
                                       channel_object_id,
                                       UA_TYPES[UA_TYPES_STRING].typeId,
                                       model_prepare_channel_context(channel_index,
                                                                     6u,
                                                                     MODEL_NODE_PROCESS_TEMPERATURE_ALARM_STATE,
                                                                     channel->metadata.id));
}

static UA_StatusCode model_build_address_space(UA_Server *server)
{
    const device_profile_config_t *device_config = config_service_get();
    const char *device_display_name = device_config->device_name[0] != '\0' ? device_config->device_name : "Device";
    UA_NodeId device_id = UA_NODEID_STRING(1, (char *)"Device");
    UA_NodeId identification_id = UA_NODEID_STRING(1, (char *)"Device.Identification");
    UA_NodeId channels_id = UA_NODEID_STRING(1, (char *)DEVICE_CHANNELS_NODE_ID);
    UA_NodeId diagnostics_id = UA_NODEID_STRING(1, (char *)"Device.Diagnostics");
    UA_NodeId task_id = UA_NODEID_STRING(1, (char *)"Device.Diagnostics.Task");
    UA_NodeId network_id = UA_NODEID_STRING(1, (char *)"Device.Network");
    UA_NodeId time_id = UA_NODEID_STRING(1, (char *)"Device.Time");
    UA_NodeId security_id = UA_NODEID_STRING(1, (char *)"Device.Security");
    UA_NodeId configuration_id = UA_NODEID_STRING(1, (char *)"Device.Configuration");
    UA_NodeId maintenance_id = UA_NODEID_STRING(1, (char *)"Device.Maintenance");
    UA_NodeId maintenance_configuration_id = UA_NODEID_STRING(1, (char *)MODEL_NODE_MAINTENANCE_CONFIGURATION);
    UA_NodeId maintenance_firmware_id = UA_NODEID_STRING(1, (char *)MODEL_NODE_MAINTENANCE_FIRMWARE);
    UA_NodeId maintenance_security_id = UA_NODEID_STRING(1, (char *)MODEL_NODE_MAINTENANCE_SECURITY);
    UA_NodeId maintenance_diagnostics_id = UA_NODEID_STRING(1, (char *)MODEL_NODE_MAINTENANCE_DIAGNOSTICS);
    UA_NodeId maintenance_system_id = UA_NODEID_STRING(1, (char *)MODEL_NODE_MAINTENANCE_SYSTEM);
    UA_NodeId messaging_id = UA_NODEID_STRING(1, (char *)MODEL_NODE_MESSAGING);
    UA_StatusCode status = UA_STATUSCODE_GOOD;

    status = model_add_object(server,
                              "Device",
                              "Device",
                              device_display_name,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    status = UA_Server_writeEventNotifier(server,
                                          device_id,
                                          UA_EVENTNOTIFIER_SUBSCRIBE_TO_EVENT);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
#endif

    status = model_add_folder(server, "Device.Identification", "Identification", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server, DEVICE_CHANNELS_NODE_ID, "Channels", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server, "Device.Diagnostics", "Diagnostics", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server, "Device.Network", "Network", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server, "Device.Time", "Time", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server, "Device.Security", "Security", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server, "Device.Configuration", "Configuration", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server, "Device.Maintenance", "Maintenance", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server,
                              MODEL_NODE_MAINTENANCE_CONFIGURATION,
                              "Configuration",
                              maintenance_id,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server,
                              MODEL_NODE_MAINTENANCE_FIRMWARE,
                              "Firmware",
                              maintenance_id,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server,
                              MODEL_NODE_MAINTENANCE_SECURITY,
                              "Security",
                              maintenance_id,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server,
                              MODEL_NODE_MAINTENANCE_DIAGNOSTICS,
                              "Diagnostics",
                              maintenance_id,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server,
                              MODEL_NODE_MAINTENANCE_SYSTEM,
                              "System",
                              maintenance_id,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server, MODEL_NODE_MESSAGING, "Messaging", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

#if defined(UA_ENABLE_SUBSCRIPTIONS_EVENTS) && defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
    status = model_build_alarm_conditions(server);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
#endif

#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_DEVICE,
                                  "DeviceEvent",
                                  UA_NODEID_NUMERIC(0, UA_NS0ID_BASEEVENTTYPE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_CONFIG_CHANGED,
                                  "ConfigChangedEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_DEVICE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_NETWORK,
                                  "NetworkEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_DEVICE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_SENSOR_FAULT,
                                  "SensorFaultEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_DEVICE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_MAINTENANCE,
                                  "MaintenanceEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_DEVICE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_FIRMWARE,
                                  "FirmwareEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_DEVICE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_AUDIT,
                                  "AuditEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_DEVICE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_AUDIT_LOGIN_SUCCESS,
                                  "AuditLoginSuccessEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_AUDIT_LOGIN_FAILURE,
                                  "AuditLoginFailureEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_AUDIT_CONFIG_CHANGE,
                                  "AuditConfigChangeEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_AUDIT_SECURITY_CHANGE,
                                  "AuditSecurityChangeEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_property(server,
                                      UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_CONFIG_CHANGED),
                                      "Types.Events.ConfigChangedEvent.ChangedSection",
                                      "ChangedSection",
                                      UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_CONFIG_CHANGED),
                                       "Types.Events.ConfigChangedEvent.ChangedKey",
                                       "ChangedKey",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_CONFIG_CHANGED),
                                       "Types.Events.ConfigChangedEvent.Actor",
                                       "Actor",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_NETWORK),
                                       "Types.Events.NetworkEvent.OldIp",
                                       "OldIp",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_NETWORK),
                                       "Types.Events.NetworkEvent.NewIp",
                                       "NewIp",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_NETWORK),
                                       "Types.Events.NetworkEvent.DhcpEnabled",
                                       "DhcpEnabled",
                                       UA_TYPES[UA_TYPES_BOOLEAN].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_NETWORK),
                                       "Types.Events.NetworkEvent.LinkUp",
                                       "LinkUp",
                                       UA_TYPES[UA_TYPES_BOOLEAN].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_SENSOR_FAULT),
                                       "Types.Events.SensorFaultEvent.SensorId",
                                       "SensorId",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_SENSOR_FAULT),
                                       "Types.Events.SensorFaultEvent.FaultCode",
                                       "FaultCode",
                                       UA_TYPES[UA_TYPES_UINT32].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_SENSOR_FAULT),
                                       "Types.Events.SensorFaultEvent.Quality",
                                       "Quality",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_SENSOR_FAULT),
                                       "Types.Events.SensorFaultEvent.MeasuredValue",
                                       "MeasuredValue",
                                       UA_TYPES[UA_TYPES_FLOAT].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_MAINTENANCE),
                                       "Types.Events.MaintenanceEvent.Operation",
                                       "Operation",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_MAINTENANCE),
                                       "Types.Events.MaintenanceEvent.ResultCode",
                                       "ResultCode",
                                       UA_TYPES[UA_TYPES_UINT32].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_MAINTENANCE),
                                       "Types.Events.MaintenanceEvent.Actor",
                                       "Actor",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_FIRMWARE),
                                       "Types.Events.FirmwareEvent.Version",
                                       "Version",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_FIRMWARE),
                                       "Types.Events.FirmwareEvent.BuildTime",
                                       "BuildTime",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_FIRMWARE),
                                       "Types.Events.FirmwareEvent.GitHash",
                                       "GitHash",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT),
                                       "Types.Events.AuditEvent.Actor",
                                       "Actor",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT),
                                       "Types.Events.AuditEvent.Subject",
                                       "Subject",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT),
                                       "Types.Events.AuditEvent.Result",
                                       "Result",
                                       UA_TYPES[UA_TYPES_BOOLEAN].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT),
                                       "Types.Events.AuditEvent.Details",
                                       "Details",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT_CONFIG_CHANGE),
                                       "Types.Events.AuditConfigChangeEvent.ChangedSection",
                                       "ChangedSection",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT_CONFIG_CHANGE),
                                       "Types.Events.AuditConfigChangeEvent.ChangedKey",
                                       "ChangedKey",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT_SECURITY_CHANGE),
                                       "Types.Events.AuditSecurityChangeEvent.Operation",
                                       "Operation",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
#endif

    status = model_add_readonly_variable(server,
                                         "Device.Identification.Manufacturer",
                                         "Manufacturer",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_MANUFACTURER]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.Model",
                                         "Model",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_MODEL]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.SerialNumber",
                                         "SerialNumber",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_SERIAL_NUMBER]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.HWRevision",
                                         "HWRevision",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_HW_REVISION]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.FWRevision",
                                         "FWRevision",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_FW_REVISION]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.BuildTime",
                                         "BuildTime",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_BUILD_TIME]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.GitHash",
                                         "GitHash",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_GIT_HASH]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.DeviceName",
                                         "DeviceName",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_DEVICE_NAME]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.ProductInstanceUri",
                                         "ProductInstanceUri",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_PRODUCT_INSTANCE_URI]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.AssetId",
                                         "AssetId",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_ASSET_ID]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.MacAddress",
                                         "MacAddress",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_MAC_ADDRESS]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.ManufactureDate",
                                         "ManufactureDate",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_DATETIME].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_MANUFACTURE_DATE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.CalibrationDate",
                                         "CalibrationDate",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_DATETIME].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_CALIBRATION_DATE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.Location",
                                         "Location",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_LOCATION]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.AssetClass",
                                         "AssetClass",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_ASSET_CLASS]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    for (size_t channel_index = 0u; channel_index < channel_manager_count(); ++channel_index)
    {
        telemetry_channel_t *channel = channel_manager_get_by_index(channel_index);

        if (channel == NULL)
        {
            continue;
        }

        status = model_add_channel_nodes(server, channels_id, channel_index, channel);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
    }

    status = model_add_readonly_variable(server,
                                         "Device.Diagnostics.UptimeSec",
                                         "UptimeSec",
                                         diagnostics_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_DIAG_UPTIME_SEC]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Diagnostics.ResetCause",
                                         "ResetCause",
                                         diagnostics_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_DIAG_RESET_CAUSE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (g_model_config.enable_heap_diagnostics)
    {
        status = model_add_readonly_variable(server,
                                             "Device.Diagnostics.FreeHeap",
                                             "FreeHeap",
                                             diagnostics_id,
                                             UA_TYPES[UA_TYPES_UINT32].typeId,
                                             &g_fixed_node_contexts[FIXED_CTX_DIAG_FREE_HEAP]);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_readonly_variable(server,
                                             "Device.Diagnostics.MinEverFreeHeap",
                                             "MinEverFreeHeap",
                                             diagnostics_id,
                                             UA_TYPES[UA_TYPES_UINT32].typeId,
                                             &g_fixed_node_contexts[FIXED_CTX_DIAG_MIN_EVER_FREE_HEAP]);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
    }

    status = model_add_readonly_variable(server,
                                         "Device.Diagnostics.CpuLikeLoadApprox",
                                         "CpuLikeLoadApprox",
                                         diagnostics_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_DIAG_CPU_LOAD_APPROX]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Diagnostics.LastFaultCode",
                                         "LastFaultCode",
                                         diagnostics_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_DIAG_LAST_FAULT_CODE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Diagnostics.LastFault",
                                         "LastFault",
                                         diagnostics_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_DIAG_LAST_FAULT]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (g_model_config.enable_task_diagnostics)
    {
        status = model_add_folder(server, "Device.Diagnostics.Task", "Task", diagnostics_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_task_stack_high_water_node(server,
                                                      task_id,
                                                      "Device.Diagnostics.Task.OPC",
                                                      "OPC",
                                                      &g_task_node_contexts[0]);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_task_stack_high_water_node(server,
                                                      task_id,
                                                      "Device.Diagnostics.Task.SPI",
                                                      "SPI",
                                                      &g_task_node_contexts[1]);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_task_stack_high_water_node(server,
                                                      task_id,
                                                      "Device.Diagnostics.Task.TEMP",
                                                      "TEMP",
                                                      &g_task_node_contexts[2]);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
    }

    status = model_add_readonly_variable(server,
                                         "Device.Network.MacAddress",
                                         "MacAddress",
                                         network_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_NETWORK_MAC_ADDRESS]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Network.IpMode",
                                         "IpMode",
                                         network_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_NETWORK_IP_MODE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Network.IPv4",
                                         "IPv4",
                                         network_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_NETWORK_IPV4]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Network.Mask",
                                         "Mask",
                                         network_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_NETWORK_MASK]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Network.Gateway",
                                         "Gateway",
                                         network_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_NETWORK_GATEWAY]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Network.LinkUp",
                                         "LinkUp",
                                         network_id,
                                         UA_TYPES[UA_TYPES_BOOLEAN].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_NETWORK_LINK_UP]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Network.DhcpState",
                                         "DhcpState",
                                         network_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_NETWORK_DHCP_STATE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Network.ReconnectCount",
                                         "ReconnectCount",
                                         network_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_NETWORK_RECONNECT_COUNT]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Network.LastNetworkChange",
                                         "LastNetworkChange",
                                         network_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_NETWORK_LAST_NETWORK_CHANGE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Time.TimeSynced",
                                         "TimeSynced",
                                         time_id,
                                         UA_TYPES[UA_TYPES_BOOLEAN].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_TIME_SYNCED]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Time.NtpServer",
                                         "NtpServer",
                                         time_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_TIME_NTP_SERVER]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Time.LastSyncEpoch",
                                         "LastSyncEpoch",
                                         time_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_TIME_LAST_SYNC_EPOCH]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Time.ClockQuality",
                                         "ClockQuality",
                                         time_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_TIME_CLOCK_QUALITY]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_writable_variable(server,
                                         "Device.Time.TimeZoneOffsetMinutes",
                                         "TimeZoneOffsetMinutes",
                                         time_id,
                                         UA_TYPES[UA_TYPES_INT16].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_TIME_TIMEZONE_OFFSET_MINUTES]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.SecurityMode",
                                         "SecurityMode",
                                         security_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_MODE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.CommissioningMode",
                                         "CommissioningMode",
                                         security_id,
                                         UA_TYPES[UA_TYPES_BOOLEAN].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_COMMISSIONING_MODE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.TrustListVersion",
                                         "TrustListVersion",
                                         security_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_TRUST_LIST_VERSION]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.TrustedClientCount",
                                         "TrustedClientCount",
                                         security_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_TRUSTED_CLIENT_COUNT]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.LastAuthFailure",
                                         "LastAuthFailure",
                                         security_id,
                                         UA_TYPES[UA_TYPES_DATETIME].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_LAST_AUTH_FAILURE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.FailedLoginCount",
                                         "FailedLoginCount",
                                         security_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_FAILED_LOGIN_COUNT]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.CertThumbprint",
                                         "CertThumbprint",
                                         security_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_CERT_THUMBPRINT]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.CertNotBefore",
                                         "CertNotBefore",
                                         security_id,
                                         UA_TYPES[UA_TYPES_DATETIME].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_CERT_NOT_BEFORE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.CertNotAfter",
                                         "CertNotAfter",
                                         security_id,
                                         UA_TYPES[UA_TYPES_DATETIME].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_CERT_NOT_AFTER]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.DaysUntilExpiry",
                                         "DaysUntilExpiry",
                                         security_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_DAYS_UNTIL_EXPIRY]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.ExpiryState",
                                         "ExpiryState",
                                         security_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_EXPIRY_STATE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.CommissioningState",
                                         "CommissioningState",
                                         security_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_COMMISSIONING_STATE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.ActiveIdentityGeneration",
                                         "ActiveIdentityGeneration",
                                         security_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_ACTIVE_IDENTITY_GENERATION]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.ActiveTrustGeneration",
                                         "ActiveTrustGeneration",
                                         security_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_ACTIVE_TRUST_GENERATION]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.CurrentUserRole",
                                         "CurrentUserRole",
                                         security_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_CURRENT_USER_ROLE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.LastEventId",
                                         "LastEventId",
                                         security_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_LAST_EVENT_ID]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Configuration.LastConfigChange",
                                         "LastConfigChange",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_LAST_CONFIG_CHANGE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Configuration.RebootRequired",
                                         "RebootRequired",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_BOOLEAN].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_REBOOT_REQUIRED]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Configuration.Commissioned",
                                         "Commissioned",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_BOOLEAN].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_COMMISSIONED]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_writable_variable(server,
                                         "Device.Configuration.SamplingPeriodMs",
                                         "SamplingPeriodMs",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_SAMPLING_PERIOD_MS]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_writable_variable(server,
                                         "Device.Configuration.PublishPeriodMs",
                                         "PublishPeriodMs",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_PUBLISH_PERIOD_MS]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_writable_variable(server,
                                         "Device.Configuration.TempOffset",
                                         "TempOffset",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_FLOAT].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_TEMP_OFFSET]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_writable_variable(server,
                                         "Device.Configuration.TempScale",
                                         "TempScale",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_FLOAT].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_TEMP_SCALE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_writable_variable(server,
                                         "Device.Configuration.HighAlarm",
                                         "HighAlarm",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_FLOAT].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_HIGH_ALARM]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_writable_variable(server,
                                         "Device.Configuration.HighHighAlarm",
                                         "HighHighAlarm",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_FLOAT].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_HIGH_HIGH_ALARM]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         MODEL_NODE_MESSAGING_LAST_EVENT_SEQUENCE,
                                         "LastPubSubEventSequence",
                                         messaging_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_MESSAGING_EVENT_SEQUENCE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         MODEL_NODE_MESSAGING_LAST_EVENT_ID,
                                         "LastPubSubEventId",
                                         messaging_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_MESSAGING_EVENT_ID]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         MODEL_NODE_MESSAGING_LAST_EVENT_TYPE,
                                         "LastPubSubEventType",
                                         messaging_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_MESSAGING_EVENT_TYPE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         MODEL_NODE_MESSAGING_LAST_EVENT_SOURCE,
                                         "LastPubSubEventSource",
                                         messaging_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_MESSAGING_EVENT_SOURCE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         MODEL_NODE_MESSAGING_LAST_EVENT_MESSAGE,
                                         "LastPubSubEventMessage",
                                         messaging_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_MESSAGING_EVENT_MESSAGE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         MODEL_NODE_MESSAGING_LAST_EVENT_SEVERITY,
                                         "LastPubSubEventSeverity",
                                         messaging_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_MESSAGING_EVENT_SEVERITY]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         MODEL_NODE_MESSAGING_LAST_EVENT_TIME,
                                         "LastPubSubEventTime",
                                         messaging_id,
                                         UA_TYPES[UA_TYPES_DATETIME].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_MESSAGING_EVENT_TIME]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (g_model_config.enable_alarm_methods)
    {
        status = model_add_alarm_method_node(server, maintenance_diagnostics_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_reboot_method_node(server, maintenance_system_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_factory_reset_method_node(server, maintenance_system_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_sync_time_now_method_node(server, maintenance_diagnostics_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_blink_identify_method_node(server, maintenance_diagnostics_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_apply_config_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_save_config_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_set_mqtt_pubsub_enabled_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_set_device_identity_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_set_network_config_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_set_channel_config_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_get_channel_config_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_clear_channel_config_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_list_drivers_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_get_config_state_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_export_config_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_begin_config_import_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_write_config_chunk_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_abort_config_import_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_validate_imported_config_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_apply_imported_config_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_get_firmware_update_status_method_node(server, maintenance_firmware_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_begin_firmware_update_method_node(server, maintenance_firmware_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_write_firmware_chunk_method_node(server, maintenance_firmware_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_finalize_firmware_update_method_node(server, maintenance_firmware_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_abort_firmware_update_method_node(server, maintenance_firmware_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_self_test_method_node(server, maintenance_diagnostics_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_clear_diagnostics_method_node(server, maintenance_diagnostics_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_set_trust_list_method_node(server, security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_enter_commissioning_method_node(server, security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_exit_commissioning_method_node(server, security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_get_trust_state_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_export_trust_list_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_begin_trust_list_import_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_write_trust_list_chunk_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_abort_trust_list_import_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_validate_imported_trust_list_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_apply_imported_trust_list_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_get_certificate_state_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_export_certificate_metadata_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_begin_identity_rotation_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_write_identity_rotation_chunk_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_abort_identity_rotation_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_validate_staged_identity_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_apply_staged_identity_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

#ifdef UA_ENABLE_ENCRYPTION_MBEDTLS
        status = model_add_add_user_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_change_password_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_set_role_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_disable_user_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_delete_user_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
#endif
    }

    return UA_STATUSCODE_GOOD;
}

bool opcua_model_init(const opcua_model_config_t *config)
{
    if (config == NULL)
    {
        return false;
    }

    g_model_config = *config;
    g_pending_actions.reboot_pending = false;
    g_pending_actions.reboot_due_tick = 0;
    g_pending_actions.identify_blink_active = false;
    g_pending_actions.identify_next_tick = 0;
    g_pending_actions.identify_toggles_remaining = 0u;
    g_pending_actions.identify_led_on = false;
#ifdef UA_ENABLE_PUBSUB
    model_pubsub_reset_runtime();
#endif
    g_model_initialized = true;
    return true;
}

#ifdef UA_ENABLE_ENCRYPTION_MBEDTLS
static bool model_certificate_view_is_pem(const security_service_blob_view_t *certificate)
{
    static const char pem_prefix[] = "-----BEGIN CERTIFICATE-----";

    return certificate != NULL &&
           certificate->data != NULL &&
           certificate->length >= (sizeof(pem_prefix) - 1u) &&
           memcmp(certificate->data, pem_prefix, sizeof(pem_prefix) - 1u) == 0;
}

static void model_clear_runtime_security_config(UA_ServerConfig *config)
{
    if (config == NULL)
    {
        return;
    }

    if (config->accessControl.clear != NULL)
    {
        config->accessControl.clear(&config->accessControl);
        memset(&config->accessControl, 0, sizeof(config->accessControl));
    }

    if (config->secureChannelPKI.clear != NULL)
    {
        config->secureChannelPKI.clear(&config->secureChannelPKI);
        memset(&config->secureChannelPKI, 0, sizeof(config->secureChannelPKI));
    }

    if (config->sessionPKI.clear != NULL)
    {
        config->sessionPKI.clear(&config->sessionPKI);
        memset(&config->sessionPKI, 0, sizeof(config->sessionPKI));
    }

    for (size_t i = 0u; i < config->endpointsSize; ++i)
    {
        UA_EndpointDescription_clear(&config->endpoints[i]);
    }
    UA_free(config->endpoints);
    config->endpoints = NULL;
    config->endpointsSize = 0u;

    for (size_t i = 0u; i < config->securityPoliciesSize; ++i)
    {
        config->securityPolicies[i].clear(&config->securityPolicies[i]);
    }
    UA_free(config->securityPolicies);
    config->securityPolicies = NULL;
    config->securityPoliciesSize = 0u;

}

static void model_clear_server_urls(UA_ServerConfig *config)
{
    if (config == NULL)
    {
        return;
    }

    for (size_t i = 0u; i < config->serverUrlsSize; ++i)
    {
        UA_String_clear(&config->serverUrls[i]);
    }
    UA_free(config->serverUrls);
    config->serverUrls = NULL;
    config->serverUrlsSize = 0u;
}

static UA_StatusCode model_configure_server_url(UA_ServerConfig *config,
                                                const char *hostname,
                                                uint16_t port)
{
    char endpoint_url[160];
    UA_String url;
    UA_StatusCode status;

    if (config == NULL || hostname == NULL || hostname[0] == '\0')
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    snprintf(endpoint_url, sizeof(endpoint_url), "opc.tcp://%s:%u/", hostname, (unsigned)port);
    url = UA_STRING(endpoint_url);

    model_clear_server_urls(config);
    config->serverUrls = (UA_String *)UA_Array_new(1u, &UA_TYPES[UA_TYPES_STRING]);
    if (config->serverUrls == NULL)
    {
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }

    status = UA_String_copy(&url, &config->serverUrls[0]);
    if (status != UA_STATUSCODE_GOOD)
    {
        model_clear_server_urls(config);
        return status;
    }

    config->serverUrlsSize = 1u;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_configure_application_uri(UA_ServerConfig *config)
{
    const device_profile_config_t *device_config = config_service_get();
    const char *configured_uri = MODEL_BOOTSTRAP_APPLICATION_URI;
    UA_String application_uri;

    if (config == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    if (device_config != NULL && device_config->product_instance_uri[0] != '\0')
    {
        configured_uri = device_config->product_instance_uri;
    }

    application_uri = UA_STRING((char *)configured_uri);

    UA_String_clear(&config->applicationDescription.applicationUri);
    return UA_String_copy(&application_uri, &config->applicationDescription.applicationUri);
}

static UA_StatusCode model_seed_bootstrap_identity_if_needed(void)
{
    security_service_blob_view_t existing_certificate = {0};

    if (security_service_has_device_identity())
    {
        if (security_service_get_device_certificate(&existing_certificate) &&
            model_certificate_view_is_pem(&existing_certificate))
        {
            if (!security_service_set_device_identity(g_bootstrap_device_certificate,
                                                      sizeof(g_bootstrap_device_certificate),
                                                      g_bootstrap_device_private_key,
                                                      sizeof(g_bootstrap_device_private_key) - 1u))
            {
                return UA_STATUSCODE_BADINTERNALERROR;
            }

            printf("[OPC UA]\tMigrated legacy PEM bootstrap certificate to DER\n");
            fflush(stdout);
        }

        return UA_STATUSCODE_GOOD;
    }

    if (!security_service_seed_device_identity_if_missing(g_bootstrap_device_certificate,
                                                          sizeof(g_bootstrap_device_certificate),
                                                          g_bootstrap_device_private_key,
                                                          sizeof(g_bootstrap_device_private_key) - 1u))
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    printf("[OPC UA]\tSeeded bootstrap device certificate into security store\n");
    fflush(stdout);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_configure_secure_access_control(UA_ServerConfig *config)
{
    UA_UsernamePasswordLogin placeholder_login;
    UA_StatusCode status;
    const UA_String *username_policy_uri = &UA_SECURITY_POLICY_NONE_URI;

    if (config == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    placeholder_login.username = UA_STRING((char *)"bootstrap");
    placeholder_login.password = UA_STRING((char *)"bootstrap");

    status = UA_AccessControl_defaultWithLoginCallback(config,
                                                       true,
                                                       username_policy_uri,
                                                       1u,
                                                       &placeholder_login,
                                                       model_login_callback,
                                                       NULL);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    g_default_activate_session = config->accessControl.activateSession;
    g_default_close_session = config->accessControl.closeSession;
    g_default_get_user_executable = config->accessControl.getUserExecutable;
    g_default_get_user_executable_on_object = config->accessControl.getUserExecutableOnObject;
    g_default_get_user_access_level = config->accessControl.getUserAccessLevel;
    g_default_allow_browse_node = config->accessControl.allowBrowseNode;

    config->accessControl.activateSession = model_activate_session_secure_wrapper;
    config->accessControl.closeSession = model_close_session_secure_wrapper;
    config->accessControl.getUserExecutable = model_get_user_executable_wrapper;
    config->accessControl.getUserExecutableOnObject = model_get_user_executable_on_object_wrapper;
    config->accessControl.getUserAccessLevel = model_get_user_access_level_wrapper;
    config->accessControl.allowBrowseNode = model_allow_browse_node_wrapper;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_keep_only_none_and_signandencrypt_endpoints(UA_ServerConfig *config)
{
    UA_EndpointDescription *filtered = NULL;
    size_t keep_count = 0u;
    size_t out_index = 0u;

    if (config == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    for (size_t i = 0; i < config->endpointsSize; ++i)
    {
        const UA_EndpointDescription *endpoint = &config->endpoints[i];
        if (endpoint->securityMode == UA_MESSAGESECURITYMODE_NONE ||
            endpoint->securityMode == UA_MESSAGESECURITYMODE_SIGNANDENCRYPT)
        {
            ++keep_count;
        }
    }

    if (keep_count == 0u || keep_count == config->endpointsSize)
    {
        return UA_STATUSCODE_GOOD;
    }

    filtered = (UA_EndpointDescription *)UA_Array_new(keep_count,
                                                      &UA_TYPES[UA_TYPES_ENDPOINTDESCRIPTION]);
    if (filtered == NULL)
    {
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }

    for (size_t i = 0; i < config->endpointsSize; ++i)
    {
        const UA_EndpointDescription *endpoint = &config->endpoints[i];
        UA_StatusCode status;

        if (endpoint->securityMode != UA_MESSAGESECURITYMODE_NONE &&
            endpoint->securityMode != UA_MESSAGESECURITYMODE_SIGNANDENCRYPT)
        {
            continue;
        }

        status = UA_EndpointDescription_copy(endpoint, &filtered[out_index]);
        if (status != UA_STATUSCODE_GOOD)
        {
            UA_Array_delete(filtered, keep_count, &UA_TYPES[UA_TYPES_ENDPOINTDESCRIPTION]);
            return status;
        }
        ++out_index;
    }

    for (size_t i = 0; i < config->endpointsSize; ++i)
    {
        UA_EndpointDescription_clear(&config->endpoints[i]);
    }
    UA_free(config->endpoints);
    config->endpoints = filtered;
    config->endpointsSize = keep_count;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_keep_only_signandencrypt_endpoints(UA_ServerConfig *config)
{
    UA_EndpointDescription *filtered = NULL;
    size_t keep_count = 0u;
    size_t out_index = 0u;

    if (config == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    for (size_t i = 0; i < config->endpointsSize; ++i)
    {
        if (config->endpoints[i].securityMode == UA_MESSAGESECURITYMODE_SIGNANDENCRYPT)
        {
            ++keep_count;
        }
    }

    if (keep_count == 0u || keep_count == config->endpointsSize)
    {
        return UA_STATUSCODE_GOOD;
    }

    filtered = (UA_EndpointDescription *)UA_Array_new(keep_count,
                                                      &UA_TYPES[UA_TYPES_ENDPOINTDESCRIPTION]);
    if (filtered == NULL)
    {
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }

    for (size_t i = 0; i < config->endpointsSize; ++i)
    {
        UA_StatusCode status;

        if (config->endpoints[i].securityMode != UA_MESSAGESECURITYMODE_SIGNANDENCRYPT)
        {
            continue;
        }

        status = UA_EndpointDescription_copy(&config->endpoints[i], &filtered[out_index]);
        if (status != UA_STATUSCODE_GOOD)
        {
            UA_Array_delete(filtered, keep_count, &UA_TYPES[UA_TYPES_ENDPOINTDESCRIPTION]);
            return status;
        }
        ++out_index;
    }

    for (size_t i = 0; i < config->endpointsSize; ++i)
    {
        UA_EndpointDescription_clear(&config->endpoints[i]);
    }
    UA_free(config->endpoints);
    config->endpoints = filtered;
    config->endpointsSize = keep_count;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_limit_insecure_endpoints_to_anonymous(UA_ServerConfig *config)
{
    size_t endpoint_index;

    if (config == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    for (endpoint_index = 0; endpoint_index < config->endpointsSize; ++endpoint_index)
    {
        UA_EndpointDescription *endpoint = &config->endpoints[endpoint_index];
        size_t token_index;
        UA_UserTokenPolicy *anonymous_only = NULL;
        UA_StatusCode status;

        if (endpoint->securityMode != UA_MESSAGESECURITYMODE_NONE)
        {
            continue;
        }

        for (token_index = 0; token_index < endpoint->userIdentityTokensSize; ++token_index)
        {
            if (endpoint->userIdentityTokens[token_index].tokenType == UA_USERTOKENTYPE_ANONYMOUS)
            {
                break;
            }
        }

        if (token_index >= endpoint->userIdentityTokensSize || endpoint->userIdentityTokensSize == 1u)
        {
            continue;
        }

        anonymous_only = (UA_UserTokenPolicy *)UA_Array_new(1u, &UA_TYPES[UA_TYPES_USERTOKENPOLICY]);
        if (anonymous_only == NULL)
        {
            return UA_STATUSCODE_BADOUTOFMEMORY;
        }

        status = UA_UserTokenPolicy_copy(&endpoint->userIdentityTokens[token_index], &anonymous_only[0]);
        if (status != UA_STATUSCODE_GOOD)
        {
            UA_Array_delete(anonymous_only, 1u, &UA_TYPES[UA_TYPES_USERTOKENPOLICY]);
            return status;
        }

        UA_Array_delete(endpoint->userIdentityTokens,
                        endpoint->userIdentityTokensSize,
                        &UA_TYPES[UA_TYPES_USERTOKENPOLICY]);
        endpoint->userIdentityTokens = anonymous_only;
        endpoint->userIdentityTokensSize = 1u;

        /* Keep the compatibility discovery endpoint strictly "None":
           some clients are sensitive to a non-empty serverCertificate
           on SecurityPolicy#None / MessageSecurityMode=None. */
        UA_ByteString_clear(&endpoint->serverCertificate);
    }

    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_configure_secure_server(UA_ServerConfig *config)
{
    security_service_blob_view_t certificate = {0};
    security_service_blob_view_t private_key = {0};
    security_service_blob_view_t trust_list = {0};
    const bool commissioning_mode = security_service_is_commissioning_mode();
    const bool have_trust_list = security_service_get_trust_list(&trust_list);
    UA_ByteString local_certificate = UA_BYTESTRING_NULL;
    UA_ByteString local_private_key = UA_BYTESTRING_NULL;
    UA_NodeId default_application_group =
        UA_NS0ID(SERVERCONFIGURATION_CERTIFICATEGROUPS_DEFAULTAPPLICATIONGROUP);
    UA_NodeId default_user_token_group =
        UA_NS0ID(SERVERCONFIGURATION_CERTIFICATEGROUPS_DEFAULTUSERTOKENGROUP);
    UA_StatusCode status;

    if (config == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    status = model_seed_bootstrap_identity_if_needed();
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (!security_service_get_device_certificate(&certificate) ||
        !security_service_get_device_private_key(&private_key))
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    local_certificate.data = (UA_Byte *)certificate.data;
    local_certificate.length = certificate.length;
    local_private_key.data = (UA_Byte *)private_key.data;
    local_private_key.length = private_key.length;

    model_clear_runtime_security_config(config);

    if (!commissioning_mode && !have_trust_list)
    {
        printf("[OPC UA]\tStrict security mode requires a provisioned trust list\n");
        fflush(stdout);
        return UA_STATUSCODE_BADCONFIGURATIONERROR;
    }

    if (!commissioning_mode && have_trust_list)
    {
        status = security_certificate_group_service_configure(&config->secureChannelPKI,
                                                              &default_application_group,
                                                              config->logging);
        if (status == UA_STATUSCODE_GOOD)
        {
            status = security_certificate_group_service_configure(&config->sessionPKI,
                                                                  &default_user_token_group,
                                                                  config->logging);
        }

        if (status == UA_STATUSCODE_GOOD)
        {
            printf("[OPC UA]\tStrict mode enabled: using flash-backed trust list backend\n");
            fflush(stdout);
        }
    }
    else
    {
        UA_CertificateGroup_AcceptAll(&config->secureChannelPKI);
        UA_CertificateGroup_AcceptAll(&config->sessionPKI);
        status = UA_STATUSCODE_GOOD;
        printf("[OPC UA]\tCommissioning mode enabled: using AcceptAll client certificate verification\n");
        fflush(stdout);
    }

    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = UA_ServerConfig_addSecurityPolicyBasic256Sha256(config,
                                                             &local_certificate,
                                                             &local_private_key);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_configure_secure_access_control(config);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (commissioning_mode)
    {
        status = UA_ServerConfig_addSecurityPolicyNone(config, &local_certificate);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
    }

    status = UA_ServerConfig_addAllEndpoints(config);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (commissioning_mode)
    {
        status = model_keep_only_none_and_signandencrypt_endpoints(config);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_limit_insecure_endpoints_to_anonymous(config);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
    }
    else
    {
        status = model_keep_only_signandencrypt_endpoints(config);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
    }

    printf("[OPC UA]\tSecurity mode: %s\n",
           commissioning_mode ? "Commissioning" : "Strict");
    fflush(stdout);
    return UA_STATUSCODE_GOOD;
}
#endif

static void model_bind_open62541_allocators(void)
{
#ifdef UA_ENABLE_MALLOC_SINGLETON
    UA_mallocSingleton = ua_port_malloc;
    UA_callocSingleton = ua_port_calloc;
    UA_reallocSingleton = ua_port_realloc;
    UA_freeSingleton = ua_port_free;
#endif
}

void opcua_model_task(void *argument)
{
    UA_Server *server = NULL;
    UA_ServerConfig *config = NULL;
    UA_StatusCode status = UA_STATUSCODE_GOOD;
    bool wait_for_network_logged = false;
    model_event_runtime_state_t event_runtime_state = {0};
    (void)argument;

    if (!g_model_initialized)
    {
        printf("[OPC UA]\tModel is not initialized\n");
        fflush(stdout);
        vTaskDelete(NULL);
    }

    log_opcua_heap("Task started");
    while (!network_manager_is_ready())
    {
        if (!wait_for_network_logged)
        {
            printf("[OPC UA]\tWaiting for IPv4 before server startup\n");
            fflush(stdout);
            wait_for_network_logged = true;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    printf("[OPC UA]\tIPv4 is ready, continuing startup\n");
    fflush(stdout);
    model_bind_open62541_allocators();

    UA_ServerConfig server_config;
    memset(&server_config, 0, sizeof(server_config));
    config = &server_config;
    config->logging = &g_model_ua_logger;

    status = UA_ServerConfig_setMinimalCustomBuffer(config,
                                                    g_model_config.port,
                                                    0,
                                                    OPCUA_SEND_BUFFER_SIZE,
                                                    OPCUA_RECV_BUFFER_SIZE);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tUA_ServerConfig_setMinimalCustomBuffer failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
        vTaskDelete(NULL);
    }

    log_opcua_heap("Network layer configured");

    status = model_configure_application_uri(config);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tFailed to configure ApplicationUri: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
        UA_ServerConfig_clear(config);
        vTaskDelete(NULL);
    }

    /* Embedded profile: keep the default server configuration modest.
       Allow one extra secure channel for discovery / tooling while keeping
       only a single active session for maintenance operations. */
    config->maxSecureChannels = 2;
    config->maxSessions = 2;
    config->maxSubscriptions = 2;
    config->maxSubscriptionsPerSession = 2;
    config->maxMonitoredItems = 8;
    config->maxMonitoredItemsPerSubscription = 4;
    config->maxNotificationsPerPublish = 4;
    config->enableRetransmissionQueue = false;
    config->queueSizeLimits.min = 1;
    config->queueSizeLimits.max = 4;
    config->maxPublishReqPerSession = 2;
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    config->maxEventsPerNode = 8;
#endif
#ifdef UA_ENABLE_PUBSUB
    config->pubsubEnabled = model_pubsub_requested();
    config->pubSubConfig.enableDeltaFrames = false;
    if (config->pubsubEnabled)
    {
        config->pubSubConfig.stateChangeCallback = model_pubsub_state_change_callback;
        status = model_pubsub_register_mqtt_manager(config);
        if (status != UA_STATUSCODE_GOOD)
        {
            printf("[OPC UA]\tFailed to register MQTT connection manager: 0x%x (%s)\n",
                   status,
                   UA_StatusCode_name(status));
            fflush(stdout);
            UA_ServerConfig_clear(config);
            vTaskDelete(NULL);
        }
    }
#endif

    if (g_model_config.hostname != NULL && g_model_config.hostname[0] != '\0')
    {
        network_manager_status_t network_status = {0};
        const char *hostname_text = g_model_config.hostname;

        network_manager_get_status(&network_status);
        if (network_status.has_ipv4 && network_status.ipv4_address[0] != '\0')
        {
            hostname_text = network_status.ipv4_address;
        }

        status = model_configure_server_url(config, hostname_text, g_model_config.port);
        if (status != UA_STATUSCODE_GOOD)
        {
            printf("[OPC UA]\tFailed to configure endpoint URL: 0x%x (%s)\n",
                   status,
                   UA_StatusCode_name(status));
            fflush(stdout);
            UA_ServerConfig_clear(config);
            vTaskDelete(NULL);
        }

        printf("[OPC UA]\tUsing endpoint host %s\n", hostname_text);
        fflush(stdout);
    }

#ifdef UA_ENABLE_ENCRYPTION_MBEDTLS
    status = model_configure_secure_server(config);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tSecure server configuration failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
        UA_ServerConfig_clear(config);
        vTaskDelete(NULL);
    }
    log_opcua_heap("Secure policies and access control configured");
#else
    if (config->accessControl.clear != NULL)
    {
        config->accessControl.clear(&config->accessControl);
        memset(&config->accessControl, 0, sizeof(config->accessControl));
    }

    status = UA_AccessControl_default(config,
                                      true,
                                      NULL,
                                      &UA_SECURITY_POLICY_NONE_URI,
                                      0,
                                      NULL);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tUA_AccessControl_default failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
        UA_ServerConfig_clear(config);
        vTaskDelete(NULL);
    }

    log_opcua_heap("Access control configured");
#endif

#ifdef UA_ENABLE_HISTORIZING
    if (config->historyDatabase.clear != NULL)
    {
        config->historyDatabase.clear(&config->historyDatabase);
    }
    memset(&config->historyDatabase, 0, sizeof(config->historyDatabase));
    config->historyDatabase.context = &g_history_backend_context;
    config->historyDatabase.clear = model_history_database_clear;
    config->historyDatabase.readRaw = model_history_read_raw;
    config->accessHistoryDataCapability = true;
    config->maxReturnDataValues = HISTORY_SERVICE_CHANNEL_CAPACITY;
    log_opcua_heap("Historizing configured");
#endif

    server = UA_Server_newWithConfig(config);
    if (server == NULL)
    {
        printf("[OPC UA]\tUA_Server_newWithConfig returned NULL\n");
        fflush(stdout);
        vTaskDelete(NULL);
    }

    log_opcua_heap("UA_Server_newWithConfig completed");
    config = UA_Server_getConfig(server);

    status = model_build_address_space(server);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tFailed to build address space: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
        UA_Server_delete(server);
        vTaskDelete(NULL);
    }

    {
        const device_profile_config_t *running_config = config_service_get();

        if (running_config != NULL)
        {
            g_model_address_space_config_snapshot = *running_config;
            g_model_address_space_config_snapshot_valid = true;
        }
    }

    log_opcua_heap("Address space ready");

#ifdef UA_ENABLE_PUBSUB
    if (config->pubsubEnabled)
    {
        status = model_configure_pubsub_publishers(server, config);
        if (status != UA_STATUSCODE_GOOD)
        {
            printf("[OPC UA]\tFailed to configure PubSub publishers: 0x%x (%s)\n",
                   status,
                   UA_StatusCode_name(status));
            fflush(stdout);
            UA_Server_delete(server);
            vTaskDelete(NULL);
        }

        log_opcua_heap("PubSub configured");
    }
#endif

    printf("[OPC UA]\tStarting server on port %u\n", (unsigned)g_model_config.port);
    fflush(stdout);

    status = UA_Server_run_startup(server);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tUA_Server_run_startup failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
        UA_Server_delete(server);
        vTaskDelete(NULL);
    }

    model_finalize_identity_generation_after_startup();

    log_opcua_heap("Server startup complete");

#ifdef UA_ENABLE_PUBSUB
    if (g_model_pubsub.configured)
    {
        status = UA_Server_enableAllPubSubComponents(server);
        if (status != UA_STATUSCODE_GOOD)
        {
            printf("[OPC UA]\tFailed to enable PubSub components: 0x%x (%s)\n",
                   status,
                   UA_StatusCode_name(status));
            fflush(stdout);
            UA_Server_delete(server);
            vTaskDelete(NULL);
        }

        log_opcua_heap("PubSub enabled");
    }
#endif

#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    security_service_set_audit_callback(model_security_audit_callback, NULL);
    model_event_runtime_state_init(&event_runtime_state);
    {
        char startup_message[128];
        const device_profile_config_t *device_config = config_service_get();

        snprintf(startup_message,
                 sizeof(startup_message),
                 "Firmware startup complete (%s)",
                 device_config != NULL ? device_config->fw_revision : "unknown");
        model_emit_firmware_event(server, startup_message, 200u);
    }
#endif

    while (1)
    {
        if (g_pending_actions.reboot_pending &&
            xTaskGetTickCount() >= g_pending_actions.reboot_due_tick)
        {
            break;
        }

        if (g_pending_actions.identify_blink_active &&
            xTaskGetTickCount() >= g_pending_actions.identify_next_tick)
        {
            g_pending_actions.identify_led_on = !g_pending_actions.identify_led_on;
            led_driver_set(g_pending_actions.identify_led_on);

            if (g_pending_actions.identify_toggles_remaining > 0u)
            {
                g_pending_actions.identify_toggles_remaining--;
            }

            if (g_pending_actions.identify_toggles_remaining == 0u)
            {
                g_pending_actions.identify_blink_active = false;
                g_pending_actions.identify_led_on = false;
                led_driver_set(false);
            }
            else
            {
                g_pending_actions.identify_next_tick = xTaskGetTickCount() + pdMS_TO_TICKS(200u);
            }
        }

        model_process_runtime_events(server, &event_runtime_state);

        UA_UInt16 wait_ms = UA_Server_run_iterate(server, false);
        if (wait_ms == 0u)
        {
            wait_ms = 10u;
        }
        else if (wait_ms > 100u)
        {
            wait_ms = 100u;
        }

        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }

    if (g_pending_actions.reboot_pending)
    {
        printf("[OPC UA]\tExecuting scheduled reboot\n");
        fflush(stdout);
        diag_service_reboot(10u);
        while (1)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    status = UA_Server_run_shutdown(server);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tUA_Server_run_shutdown failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
    }

    UA_Server_delete(server);

#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    security_service_set_audit_callback(NULL, NULL);
#endif

    vTaskDelete(NULL);
}
