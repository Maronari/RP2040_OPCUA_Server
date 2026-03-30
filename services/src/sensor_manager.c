#include "services/sensor_manager.h"

#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "drivers/temp_sensor_driver.h"
#include "services/config_service.h"
#include "services/history_service.h"
#include "services/time_service.h"

static sensor_manager_snapshot_t g_snapshot;
static bool g_initialized = false;

static void sensor_manager_set_snapshot(const sensor_manager_snapshot_t *snapshot)
{
    taskENTER_CRITICAL();
    g_snapshot = *snapshot;
    taskEXIT_CRITICAL();
}

bool sensor_manager_init(void)
{
    const device_profile_config_t *config = config_service_get();

    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.enabled = config->enable_temperature_sensor;
    g_snapshot.quality = config->enable_temperature_sensor ? SERVICE_QUALITY_UNCERTAIN : SERVICE_QUALITY_BAD;

    if (!config->enable_temperature_sensor)
    {
        g_initialized = true;
        return true;
    }

    g_initialized = temp_sensor_driver_init();
    if (!g_initialized)
    {
        g_snapshot.quality = SERVICE_QUALITY_BAD;
    }

    return g_initialized;
}

void sensor_manager_task(void *argument)
{
    (void)argument;

    TickType_t last_wake_time = xTaskGetTickCount();

    while (1)
    {
        const device_profile_config_t *config = config_service_get();
        const uint32_t sample_period_ms = config->sampling_period_ms > 0u ? config->sampling_period_ms : 1000u;
        const TickType_t sample_delay_ticks = pdMS_TO_TICKS(sample_period_ms);
        sensor_manager_snapshot_t snapshot = {0};
        sensor_manager_get_snapshot(&snapshot);

        if (!g_initialized || !snapshot.enabled)
        {
            vTaskDelayUntil(&last_wake_time, sample_delay_ticks > 0 ? sample_delay_ticks : 1u);
            continue;
        }

        if (temp_sensor_driver_read_celsius(&snapshot.temperature_c))
        {
            snapshot.temperature_c = (snapshot.temperature_c * config->temp_scale) + config->temp_offset_c;
            snapshot.source_timestamp_epoch_s = get_system_time();
            snapshot.sample_count++;
            snapshot.quality = SERVICE_QUALITY_GOOD;
            snapshot.last_update_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            history_service_record_temperature_sample(snapshot.temperature_c,
                                                      snapshot.quality,
                                                      snapshot.source_timestamp_epoch_s);
        }
        else
        {
            snapshot.error_count++;
            snapshot.quality = SERVICE_QUALITY_BAD;
        }

        sensor_manager_set_snapshot(&snapshot);
        vTaskDelayUntil(&last_wake_time, sample_delay_ticks > 0 ? sample_delay_ticks : 1u);
    }
}

void sensor_manager_get_snapshot(sensor_manager_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL)
    {
        return;
    }

    taskENTER_CRITICAL();
    *out_snapshot = g_snapshot;
    taskEXIT_CRITICAL();
}
