#include "services/config_service.h"

#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "drivers/flash_storage_driver.h"
#include "pico/unique_id.h"

static device_profile_config_t g_config;
static device_profile_config_t g_default_config;
static bool g_default_config_valid = false;

static bool config_service_is_valid_float(float value)
{
    return value == value;
}

static bool config_service_is_valid_timezone_offset_minutes(int16_t value)
{
    return value >= -720 && value <= 840;
}

static bool config_service_is_valid_temperature_history_interval_ms(uint32_t value)
{
    return value <= 86400000u;
}

static void config_service_populate_serial_number(device_profile_config_t *config)
{
    pico_unique_board_id_t board_id;
    size_t offset = 0;

    if (config == NULL || config->serial_number[0] != '\0')
    {
        return;
    }

    pico_get_unique_board_id(&board_id);

    for (size_t index = 0; index < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; ++index)
    {
        if ((offset + 2u) >= sizeof(config->serial_number))
        {
            break;
        }

        offset += (size_t)snprintf(&config->serial_number[offset],
                                   sizeof(config->serial_number) - offset,
                                   "%02X",
                                   board_id.id[index]);
    }
}

static void config_service_normalize(device_profile_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    if (config->sampling_period_ms == 0u)
    {
        config->sampling_period_ms = 1000u;
    }

    if (config->publish_period_ms == 0u)
    {
        config->publish_period_ms = 1000u;
    }

    if (!config_service_is_valid_temperature_history_interval_ms(config->temperature_history_interval_ms))
    {
        config->temperature_history_interval_ms = config->sampling_period_ms;
    }

    if (!config_service_is_valid_timezone_offset_minutes(config->timezone_offset_minutes))
    {
        config->timezone_offset_minutes = 0;
    }

    if (config->temp_scale == 0.0f)
    {
        config->temp_scale = 1.0f;
    }

    if (config->device_name[0] == '\0' && config->opcua_hostname[0] != '\0')
    {
        strncpy(config->device_name, config->opcua_hostname, sizeof(config->device_name));
        config->device_name[sizeof(config->device_name) - 1] = '\0';
    }

    if (config->opcua_hostname[0] == '\0' && config->device_name[0] != '\0')
    {
        strncpy(config->opcua_hostname, config->device_name, sizeof(config->opcua_hostname));
        config->opcua_hostname[sizeof(config->opcua_hostname) - 1] = '\0';
    }

    if (config->ntp_server[0] == '\0')
    {
        strncpy(config->ntp_server, "pool.ntp.org", sizeof(config->ntp_server));
        config->ntp_server[sizeof(config->ntp_server) - 1] = '\0';
    }

    config_service_populate_serial_number(config);
}

void config_service_init(const device_profile_config_t *defaults)
{
    memset(&g_config, 0, sizeof(g_config));
    memset(&g_default_config, 0, sizeof(g_default_config));
    g_default_config_valid = false;

    if (defaults != NULL)
    {
        g_config = *defaults;
        g_default_config = *defaults;
        g_default_config_valid = true;
    }

    device_profile_config_t persisted = {0};
    if (flash_storage_driver_load(&persisted, sizeof(persisted)))
    {
        g_config = persisted;
    }

    config_service_normalize(&g_config);
    if (g_default_config_valid)
    {
        config_service_normalize(&g_default_config);
    }
}

const device_profile_config_t *config_service_get(void)
{
    return &g_config;
}

bool config_service_reset_to_defaults(void)
{
    if (!g_default_config_valid)
    {
        return false;
    }

    taskENTER_CRITICAL();
    g_config = g_default_config;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_set_sampling_period_ms(uint32_t value)
{
    if (value == 0u)
    {
        return false;
    }

    taskENTER_CRITICAL();
    g_config.sampling_period_ms = value;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_set_temperature_history_interval_ms(uint32_t value)
{
    if (!config_service_is_valid_temperature_history_interval_ms(value))
    {
        return false;
    }

    taskENTER_CRITICAL();
    g_config.temperature_history_interval_ms = value;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_set_publish_period_ms(uint32_t value)
{
    if (value == 0u)
    {
        return false;
    }

    taskENTER_CRITICAL();
    g_config.publish_period_ms = value;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_set_timezone_offset_minutes(int16_t value)
{
    if (!config_service_is_valid_timezone_offset_minutes(value))
    {
        return false;
    }

    taskENTER_CRITICAL();
    g_config.timezone_offset_minutes = value;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_set_temp_offset_c(float value)
{
    if (!config_service_is_valid_float(value))
    {
        return false;
    }

    taskENTER_CRITICAL();
    g_config.temp_offset_c = value;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_set_temp_scale(float value)
{
    if (!config_service_is_valid_float(value) || value == 0.0f)
    {
        return false;
    }

    taskENTER_CRITICAL();
    g_config.temp_scale = value;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_set_high_temperature_threshold_c(float value)
{
    if (!config_service_is_valid_float(value) || value > g_config.high_high_temperature_threshold_c)
    {
        return false;
    }

    taskENTER_CRITICAL();
    g_config.high_temperature_threshold_c = value;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_set_high_high_temperature_threshold_c(float value)
{
    if (!config_service_is_valid_float(value) || value < g_config.high_temperature_threshold_c)
    {
        return false;
    }

    taskENTER_CRITICAL();
    g_config.high_high_temperature_threshold_c = value;
    taskEXIT_CRITICAL();
    return true;
}

bool config_service_save(void)
{
    return flash_storage_driver_save(&g_config, sizeof(g_config));
}
