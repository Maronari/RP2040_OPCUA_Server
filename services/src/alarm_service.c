#include "services/alarm_service.h"

#include <FreeRTOS.h>
#include <task.h>

#include "services/channel_manager.h"
#include "services/device_channels.h"

static alarm_service_status_t g_status;

static void alarm_service_set_status(const alarm_service_status_t *status)
{
    taskENTER_CRITICAL();
    g_status = *status;
    taskEXIT_CRITICAL();
}

bool alarm_service_init(void)
{
    g_status.channel_id = DEVICE_CHANNEL_ID_TEMPERATURE_1;
    g_status.channel_name = DEVICE_CHANNEL_NAME_TEMPERATURE_1;
    g_status.warning_active = false;
    g_status.critical_active = false;
    g_status.acknowledged = true;
    g_status.quality = SERVICE_QUALITY_UNCERTAIN;
    g_status.warning_threshold = 60.0f;
    g_status.critical_threshold = 75.0f;
    g_status.state = TELEMETRY_ALARM_STATE_NORMAL;
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
        telemetry_channel_t channel = {0};
        alarm_service_status_t status = {0};
        telemetry_alarm_state_t previous_state = TELEMETRY_ALARM_STATE_NORMAL;
        float measured_value = 0.0f;

        (void)channel_manager_copy_channel(DEVICE_CHANNEL_ID_TEMPERATURE_1, &channel);
        alarm_service_get_status(&status);

        previous_state = status.state;
        status.channel_id = channel.metadata.id != NULL ? channel.metadata.id : DEVICE_CHANNEL_ID_TEMPERATURE_1;
        status.channel_name = channel.metadata.name != NULL ? channel.metadata.name : DEVICE_CHANNEL_NAME_TEMPERATURE_1;
        status.warning_threshold = (float)channel.settings.warning_threshold;
        status.critical_threshold = (float)channel.settings.critical_threshold;
        status.evaluation_count++;

        switch (channel.metadata.value_type)
        {
        case TELEMETRY_CHANNEL_VALUE_DOUBLE:
            measured_value = (float)channel.runtime.value.f64;
            break;
        case TELEMETRY_CHANNEL_VALUE_FLOAT:
        default:
            measured_value = channel.runtime.value.f32;
            break;
        }

        if ((channel.metadata.flags & TELEMETRY_CHANNEL_FLAG_ENABLED) != 0u &&
            channel.runtime.quality == SERVICE_QUALITY_GOOD)
        {
            status.quality = SERVICE_QUALITY_GOOD;
            status.warning_active = false;
            status.critical_active = false;
            status.state = TELEMETRY_ALARM_STATE_NORMAL;

            if (measured_value >= status.critical_threshold)
            {
                status.warning_active = true;
                status.critical_active = true;
                status.state = TELEMETRY_ALARM_STATE_CRITICAL;
            }
            else if (measured_value >= status.warning_threshold)
            {
                status.warning_active = true;
                status.state = TELEMETRY_ALARM_STATE_WARNING;
            }

            if (status.state != previous_state && status.state >= TELEMETRY_ALARM_STATE_WARNING)
            {
                status.trip_count++;
                status.acknowledged = false;
            }
        }
        else
        {
            status.warning_active = false;
            status.critical_active = false;
            status.state = TELEMETRY_ALARM_STATE_INVALID;
            status.quality = channel.runtime.quality;
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
