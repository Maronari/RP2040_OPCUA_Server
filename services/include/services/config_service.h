#ifndef SERVICES_CONFIG_SERVICE_H
#define SERVICES_CONFIG_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "services/service_types.h"

#define CONFIG_SERVICE_TEXT_MAX_LEN 64

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
    bool enable_temperature_sensor;
    bool enable_heap_diagnostics;
    bool enable_task_diagnostics;
    bool enable_alarm_ack_method;
    uint16_t opcua_port;
    int16_t timezone_offset_minutes;
    uint32_t watchdog_timeout_ms;
    uint32_t sampling_period_ms;
    uint32_t temperature_history_interval_ms;
    uint32_t publish_period_ms;
    float temp_offset_c;
    float temp_scale;
    float high_temperature_threshold_c;
    float high_high_temperature_threshold_c;
    uint8_t mac[6];
    network_mode_t network_mode;
    uint8_t static_ip[4];
    uint8_t static_netmask[4];
    uint8_t static_gateway[4];
    uint8_t static_dns[4];
} device_profile_config_t;

void config_service_init(const device_profile_config_t *defaults);
const device_profile_config_t *config_service_get(void);
bool config_service_reset_to_defaults(void);
bool config_service_set_sampling_period_ms(uint32_t value);
bool config_service_set_temperature_history_interval_ms(uint32_t value);
bool config_service_set_publish_period_ms(uint32_t value);
bool config_service_set_timezone_offset_minutes(int16_t value);
bool config_service_set_temp_offset_c(float value);
bool config_service_set_temp_scale(float value);
bool config_service_set_high_temperature_threshold_c(float value);
bool config_service_set_high_high_temperature_threshold_c(float value);
bool config_service_save(void);

#endif
