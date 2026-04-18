#ifndef SERVICES_CONFIG_SERVICE_H
#define SERVICES_CONFIG_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/driver_catalog.h"
#include "services/service_types.h"
#include "services/telemetry_channel.h"

#define CONFIG_SERVICE_TEXT_MAX_LEN 64
#define CONFIG_SERVICE_URI_MAX_LEN 64
#define CONFIG_SERVICE_ASSET_ID_MAX_LEN 32
#define CONFIG_SERVICE_LOCATION_MAX_LEN 48
#define CONFIG_SERVICE_ASSET_CLASS_MAX_LEN 32
#define CONFIG_SERVICE_MAX_CHANNELS 4u
#define CONFIG_SERVICE_CHANNEL_ID_MAX_LEN 32
#define CONFIG_SERVICE_CHANNEL_NAME_MAX_LEN 32
#define CONFIG_SERVICE_CHANNEL_DESCRIPTION_MAX_LEN 64
#define CONFIG_SERVICE_CHANNEL_UNIT_MAX_LEN 16

typedef struct
{
    uint8_t in_use;
    uint8_t kind;
    uint8_t value_type;
    uint8_t driver_id;
    uint8_t source_index;
    uint8_t source_flags;
    uint8_t has_minimum;
    uint8_t has_maximum;
    char id[CONFIG_SERVICE_CHANNEL_ID_MAX_LEN];
    char name[CONFIG_SERVICE_CHANNEL_NAME_MAX_LEN];
    char description[CONFIG_SERVICE_CHANNEL_DESCRIPTION_MAX_LEN];
    char unit[CONFIG_SERVICE_CHANNEL_UNIT_MAX_LEN];
    uint32_t flags;
    double minimum;
    double maximum;
    telemetry_channel_settings_t settings;
} config_channel_definition_t;

typedef struct
{
    char device_name[CONFIG_SERVICE_TEXT_MAX_LEN];
    char opcua_hostname[CONFIG_SERVICE_TEXT_MAX_LEN];
    char product_instance_uri[CONFIG_SERVICE_URI_MAX_LEN];
    char serial_number[CONFIG_SERVICE_TEXT_MAX_LEN];
    char asset_id[CONFIG_SERVICE_ASSET_ID_MAX_LEN];
    char hw_revision[CONFIG_SERVICE_TEXT_MAX_LEN];
    char fw_revision[CONFIG_SERVICE_TEXT_MAX_LEN];
    int64_t manufacture_date_utc;
    int64_t calibration_date_utc;
    char location[CONFIG_SERVICE_LOCATION_MAX_LEN];
    char asset_class[CONFIG_SERVICE_ASSET_CLASS_MAX_LEN];
} device_identity_config_t;

typedef struct
{
    bool use_dhcp;
    uint8_t mac[6];
    char opcua_hostname[CONFIG_SERVICE_TEXT_MAX_LEN];
    uint16_t opcua_port;
    char ntp_server[CONFIG_SERVICE_TEXT_MAX_LEN];
    uint8_t static_ip[4];
    uint8_t static_netmask[4];
    uint8_t static_gateway[4];
    uint8_t static_dns[4];
} device_network_config_t;

typedef struct
{
    char manufacturer[CONFIG_SERVICE_TEXT_MAX_LEN];
    char model[CONFIG_SERVICE_TEXT_MAX_LEN];
    char serial_number[CONFIG_SERVICE_TEXT_MAX_LEN];
    char hw_revision[CONFIG_SERVICE_TEXT_MAX_LEN];
    char fw_revision[CONFIG_SERVICE_TEXT_MAX_LEN];
    char build_time[CONFIG_SERVICE_TEXT_MAX_LEN];
    char git_hash[CONFIG_SERVICE_TEXT_MAX_LEN];
    char device_name[CONFIG_SERVICE_TEXT_MAX_LEN];
    char opcua_hostname[CONFIG_SERVICE_TEXT_MAX_LEN];
    char ntp_server[CONFIG_SERVICE_TEXT_MAX_LEN];
    char mqtt_broker_url[CONFIG_SERVICE_TEXT_MAX_LEN];
    char mqtt_topic_root[CONFIG_SERVICE_TEXT_MAX_LEN];
    bool enable_heap_diagnostics;
    bool enable_task_diagnostics;
    bool enable_alarm_ack_method;
    bool enable_mqtt_pubsub;
    bool mqtt_pubsub_use_json;
    uint16_t opcua_port;
    int16_t timezone_offset_minutes;
    uint32_t watchdog_timeout_ms;
    uint8_t mac[6];
    network_mode_t network_mode;
    uint8_t static_ip[4];
    uint8_t static_netmask[4];
    uint8_t static_gateway[4];
    uint8_t static_dns[4];
    config_channel_definition_t channels[CONFIG_SERVICE_MAX_CHANNELS];
    char product_instance_uri[CONFIG_SERVICE_URI_MAX_LEN];
    char asset_id[CONFIG_SERVICE_ASSET_ID_MAX_LEN];
    int64_t manufacture_date_utc;
    int64_t calibration_date_utc;
    char location[CONFIG_SERVICE_LOCATION_MAX_LEN];
    char asset_class[CONFIG_SERVICE_ASSET_CLASS_MAX_LEN];
} device_profile_config_t;

void config_service_init(const device_profile_config_t *defaults);
const device_profile_config_t *config_service_get(void);
const device_profile_config_t *config_service_get_staged(void);
bool config_service_reset_to_defaults(void);
bool config_service_apply(void);
bool config_service_set_timezone_offset_minutes(int16_t value);
bool config_service_stage_set_mqtt_pubsub_enabled(bool enabled);
bool config_service_stage_set_identity(const device_identity_config_t *identity);
bool config_service_stage_set_network(const device_network_config_t *network);
bool config_service_stage_copy_channel(size_t index, config_channel_definition_t *out_channel);
bool config_service_stage_set_channel(size_t index, const config_channel_definition_t *channel);
bool config_service_stage_clear_channel(size_t index);
bool config_service_replace_and_save(const device_profile_config_t *config, bool apply_running);
uint32_t config_service_get_running_revision(void);
uint32_t config_service_get_staged_revision(void);
bool config_service_save(void);

#endif
