#include "services/sensor_manager.h"

#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "drivers/temp_sensor_driver.h"
#include "services/channel_config.h"
#include "services/channel_manager.h"
#include "services/device_channels.h"
#include "services/history_service.h"
#include "services/telemetry_driver.h"
#include "services/time_service.h"

#define SENSOR_MANAGER_STATIC_CHANNEL_COUNT 1u

static sensor_manager_snapshot_t g_snapshot;
static bool g_initialized = false;
static telemetry_channel_t g_channel_storage[SENSOR_MANAGER_STATIC_CHANNEL_COUNT];

static bool sensor_manager_temperature_channel_init(telemetry_channel_t *channel);
static bool sensor_manager_temperature_channel_poll(telemetry_channel_t *channel, uint32_t now_ms);
static bool sensor_manager_temperature_channel_configure(telemetry_channel_t *channel, const void *config);
static bool sensor_manager_temperature_channel_self_test(telemetry_channel_t *channel);
static void sensor_manager_snapshot_from_channel(const telemetry_channel_t *channel,
                                                sensor_manager_snapshot_t *snapshot);
static telemetry_channel_t *sensor_manager_get_temperature_channel(void);

static const telemetry_driver_vtbl_t g_temperature_channel_driver = {
    .init = sensor_manager_temperature_channel_init,
    .poll = sensor_manager_temperature_channel_poll,
    .configure = sensor_manager_temperature_channel_configure,
    .self_test = sensor_manager_temperature_channel_self_test};

static const channel_config_t g_channel_configs[] = {
    {
        .metadata =
            {
                .id = DEVICE_CHANNEL_ID_TEMPERATURE_1,
                .name = DEVICE_CHANNEL_NAME_TEMPERATURE_1,
                .description = "Internal temperature telemetry channel",
                .kind = TELEMETRY_CHANNEL_KIND_ANALOG,
                .value_type = TELEMETRY_CHANNEL_VALUE_FLOAT,
                .unit = "degC",
                .range = {false, false, 0.0, 0.0},
                .flags = TELEMETRY_CHANNEL_FLAG_ENABLED |
                         TELEMETRY_CHANNEL_FLAG_HISTORIZED |
                         TELEMETRY_CHANNEL_FLAG_ALARM_ENABLED,
            },
        .settings =
            {
                .enabled = true,
                .sample_period_ms = 1000u,
                .publish_period_ms = 1000u,
                .history_interval_ms = 1000u,
                .scale = 1.0,
                .offset = 0.0,
                .warning_threshold = 60.0,
                .critical_threshold = 75.0,
            },
        .driver = &g_temperature_channel_driver,
        .driver_context = NULL}};

static telemetry_channel_t *sensor_manager_get_temperature_channel(void)
{
    return channel_manager_find_by_id(DEVICE_CHANNEL_ID_TEMPERATURE_1);
}

static void sensor_manager_set_snapshot(const sensor_manager_snapshot_t *snapshot)
{
    taskENTER_CRITICAL();
    g_snapshot = *snapshot;
    taskEXIT_CRITICAL();
}

static bool sensor_manager_temperature_channel_init(telemetry_channel_t *channel)
{
    if (channel == NULL)
    {
        return false;
    }

    if (!channel->settings.enabled)
    {
        return true;
    }

    return temp_sensor_driver_init();
}

static bool sensor_manager_temperature_channel_poll(telemetry_channel_t *channel, uint32_t now_ms)
{
    telemetry_channel_t updated_channel;
    float raw_temperature_c = 0.0f;

    if (channel == NULL)
    {
        return false;
    }

    updated_channel = *channel;

    if (!updated_channel.settings.enabled)
    {
        updated_channel.runtime.quality = SERVICE_QUALITY_BAD;
        taskENTER_CRITICAL();
        *channel = updated_channel;
        taskEXIT_CRITICAL();
        return true;
    }

    if (temp_sensor_driver_read_celsius(&raw_temperature_c))
    {
        updated_channel.runtime.value.f32 =
            (raw_temperature_c * (float)updated_channel.settings.scale) + (float)updated_channel.settings.offset;
        updated_channel.runtime.source_timestamp_epoch_s = get_system_time();
        updated_channel.runtime.sample_count++;
        updated_channel.runtime.quality = SERVICE_QUALITY_GOOD;
        updated_channel.runtime.last_update_ms = now_ms;
        history_service_record_channel_sample(updated_channel.metadata.id,
                                              updated_channel.runtime.value.f32,
                                              updated_channel.runtime.quality,
                                              updated_channel.runtime.source_timestamp_epoch_s,
                                              updated_channel.settings.history_interval_ms);
    }
    else
    {
        updated_channel.runtime.error_count++;
        updated_channel.runtime.quality = SERVICE_QUALITY_BAD;
    }

    taskENTER_CRITICAL();
    *channel = updated_channel;
    taskEXIT_CRITICAL();
    return updated_channel.runtime.quality == SERVICE_QUALITY_GOOD;
}

static bool sensor_manager_temperature_channel_configure(telemetry_channel_t *channel, const void *config)
{
    (void)channel;
    (void)config;
    return true;
}

static bool sensor_manager_temperature_channel_self_test(telemetry_channel_t *channel)
{
    float sample = 0.0f;
    (void)channel;
    return temp_sensor_driver_read_celsius(&sample);
}

static void sensor_manager_snapshot_from_channel(const telemetry_channel_t *channel,
                                                sensor_manager_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    if (channel == NULL)
    {
        snapshot->quality = SERVICE_QUALITY_BAD;
        return;
    }

    snapshot->enabled = channel->settings.enabled;
    snapshot->quality = channel->runtime.quality;
    snapshot->source_timestamp_epoch_s = channel->runtime.source_timestamp_epoch_s;
    snapshot->sample_count = channel->runtime.sample_count;
    snapshot->error_count = channel->runtime.error_count;
    snapshot->last_update_ms = channel->runtime.last_update_ms;

    switch (channel->metadata.value_type)
    {
    case TELEMETRY_CHANNEL_VALUE_DOUBLE:
        snapshot->temperature_c = (float)channel->runtime.value.f64;
        break;
    case TELEMETRY_CHANNEL_VALUE_FLOAT:
    default:
        snapshot->temperature_c = channel->runtime.value.f32;
        break;
    }
}

bool sensor_manager_init(void)
{
    telemetry_channel_t *temperature_channel = NULL;

    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.enabled = true;
    g_snapshot.quality = SERVICE_QUALITY_UNCERTAIN;

    temperature_channel = sensor_manager_get_temperature_channel();
    if (temperature_channel == NULL)
    {
        (void)channel_manager_register_from_config(g_channel_configs,
                                                   SENSOR_MANAGER_STATIC_CHANNEL_COUNT,
                                                   g_channel_storage,
                                                   SENSOR_MANAGER_STATIC_CHANNEL_COUNT);
        temperature_channel = sensor_manager_get_temperature_channel();
    }

    if (temperature_channel == NULL)
    {
        g_snapshot.quality = SERVICE_QUALITY_BAD;
        return false;
    }

    {
        telemetry_channel_t channel_copy = {0};
        if (channel_manager_copy_channel(DEVICE_CHANNEL_ID_TEMPERATURE_1, &channel_copy))
        {
            sensor_manager_snapshot_from_channel(&channel_copy, &g_snapshot);
        }
    }

    if (!temperature_channel->settings.enabled)
    {
        g_initialized = true;
        return true;
    }

    g_initialized = temperature_channel->driver != NULL && temperature_channel->driver->init != NULL
                        ? temperature_channel->driver->init(temperature_channel)
                        : true;
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
        telemetry_channel_t channel_copy = {0};
        sensor_manager_snapshot_t snapshot = {0};
        uint32_t sample_period_ms = 1000u;

        if (!channel_manager_copy_channel(DEVICE_CHANNEL_ID_TEMPERATURE_1, &channel_copy))
        {
            memset(&channel_copy, 0, sizeof(channel_copy));
            channel_copy.runtime.quality = SERVICE_QUALITY_BAD;
        }

        sample_period_ms = channel_copy.settings.sample_period_ms > 0u ? channel_copy.settings.sample_period_ms : 1000u;
        if (!g_initialized || !channel_copy.settings.enabled)
        {
            sensor_manager_snapshot_from_channel(&channel_copy, &snapshot);
            sensor_manager_set_snapshot(&snapshot);
            vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(sample_period_ms > 0u ? sample_period_ms : 1u));
            continue;
        }

        (void)channel_manager_poll_all((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        if (!channel_manager_copy_channel(DEVICE_CHANNEL_ID_TEMPERATURE_1, &channel_copy))
        {
            memset(&channel_copy, 0, sizeof(channel_copy));
            channel_copy.runtime.quality = SERVICE_QUALITY_BAD;
        }

        sensor_manager_snapshot_from_channel(&channel_copy, &snapshot);
        sensor_manager_set_snapshot(&snapshot);
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(sample_period_ms > 0u ? sample_period_ms : 1u));
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
