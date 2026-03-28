#include "services/alarm_service.h"

#include <FreeRTOS.h>
#include <task.h>

#include "services/config_service.h"
#include "services/sensor_manager.h"

static alarm_service_status_t g_status;

static void alarm_service_set_status(const alarm_service_status_t *status)
{
    taskENTER_CRITICAL();
    g_status = *status;
    taskEXIT_CRITICAL();
}

bool alarm_service_init(void)
{
    g_status.high_temperature_active = false;
    g_status.high_high_temperature_active = false;
    g_status.acknowledged = true;
    g_status.quality = SERVICE_QUALITY_UNCERTAIN;
    g_status.high_threshold_c = config_service_get()->high_temperature_threshold_c;
    g_status.high_high_threshold_c = config_service_get()->high_high_temperature_threshold_c;
    g_status.alarm_state = TEMPERATURE_ALARM_STATE_NORMAL;
    g_status.evaluation_count = 0;
    g_status.trip_count = 0;
    return true;
}

void alarm_service_task(void *argument)
{
    (void)argument;

    TickType_t last_wake_time = xTaskGetTickCount();

    while (1)
    {
        const device_profile_config_t *config = config_service_get();
        sensor_manager_snapshot_t sensor_snapshot = {0};
        alarm_service_status_t status = {0};
        temperature_alarm_state_t previous_alarm_state = TEMPERATURE_ALARM_STATE_NORMAL;

        sensor_manager_get_snapshot(&sensor_snapshot);
        alarm_service_get_status(&status);

        previous_alarm_state = status.alarm_state;
        status.high_threshold_c = config->high_temperature_threshold_c;
        status.high_high_threshold_c = config->high_high_temperature_threshold_c;
        status.evaluation_count++;

        if (sensor_snapshot.quality == SERVICE_QUALITY_GOOD)
        {
            status.quality = SERVICE_QUALITY_GOOD;
            status.high_temperature_active = false;
            status.high_high_temperature_active = false;
            status.alarm_state = TEMPERATURE_ALARM_STATE_NORMAL;

            if (sensor_snapshot.temperature_c >= status.high_high_threshold_c)
            {
                status.high_temperature_active = true;
                status.high_high_temperature_active = true;
                status.alarm_state = TEMPERATURE_ALARM_STATE_HIGH_HIGH;
            }
            else if (sensor_snapshot.temperature_c >= status.high_threshold_c)
            {
                status.high_temperature_active = true;
                status.alarm_state = TEMPERATURE_ALARM_STATE_HIGH;
            }

            if (status.alarm_state != previous_alarm_state &&
                status.alarm_state >= TEMPERATURE_ALARM_STATE_HIGH)
            {
                status.trip_count++;
                status.acknowledged = false;
            }
        }
        else
        {
            status.high_temperature_active = false;
            status.high_high_temperature_active = false;
            status.alarm_state = TEMPERATURE_ALARM_STATE_INVALID;
            status.quality = sensor_snapshot.quality;
        }

        alarm_service_set_status(&status);
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(500));
    }
}

void alarm_service_get_status(alarm_service_status_t *out_status)
{
    if (out_status == NULL)
    {
        return;
    }

    taskENTER_CRITICAL();
    *out_status = g_status;
    taskEXIT_CRITICAL();
}

void alarm_service_acknowledge_all(void)
{
    taskENTER_CRITICAL();
    g_status.acknowledged = true;
    taskEXIT_CRITICAL();
}
