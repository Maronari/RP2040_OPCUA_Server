#include "services/sensor_manager.h"

#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "services/channel_config.h"
#include "services/channel_manager.h"
#include "services/config_service.h"
#include "services/device_channels.h"
#include "services/driver_catalog.h"
#include "services/history_service.h"
#include "services/telemetry_driver.h"

#define SENSOR_MANAGER_MAX_CHANNELS CONFIG_SERVICE_MAX_CHANNELS
#define SENSOR_MANAGER_DEFAULT_POLL_PERIOD_MS 1000u

typedef struct
{
    const driver_catalog_entry_t *driver_entry;
    uint8_t source_index;
    uint8_t source_flags;
} sensor_manager_channel_context_t;

static sensor_manager_snapshot_t g_snapshot;
static bool g_initialized = false;
static uint32_t g_applied_config_revision = 0u;
static telemetry_channel_t g_channel_storage[SENSOR_MANAGER_MAX_CHANNELS];
static channel_config_t g_runtime_channel_configs[SENSOR_MANAGER_MAX_CHANNELS];
static sensor_manager_channel_context_t g_channel_contexts[SENSOR_MANAGER_MAX_CHANNELS];

static bool sensor_manager_channel_init(telemetry_channel_t *channel);
static bool sensor_manager_channel_poll(telemetry_channel_t *channel, uint32_t now_ms);
static bool sensor_manager_channel_configure(telemetry_channel_t *channel, const void *config);
static bool sensor_manager_channel_self_test(telemetry_channel_t *channel);
static void sensor_manager_snapshot_from_channel(const telemetry_channel_t *channel,
                                                 sensor_manager_snapshot_t *snapshot);
static telemetry_channel_t *sensor_manager_get_primary_channel(void);
static bool sensor_manager_activate_running_config(void);

static const telemetry_driver_vtbl_t g_builtin_channel_driver = {
    .init = sensor_manager_channel_init,
    .poll = sensor_manager_channel_poll,
    .configure = sensor_manager_channel_configure,
    .self_test = sensor_manager_channel_self_test};

static void sensor_manager_set_snapshot(const sensor_manager_snapshot_t *snapshot)
{
    taskENTER_CRITICAL();
    g_snapshot = *snapshot;
    taskEXIT_CRITICAL();
}

static sensor_manager_channel_context_t *sensor_manager_channel_context(const telemetry_channel_t *channel)
{
    return channel != NULL ? (sensor_manager_channel_context_t *)channel->driver_context : NULL;
}

static bool sensor_manager_should_sample(const telemetry_channel_t *channel, uint32_t now_ms)
{
    uint32_t period_ms = SENSOR_MANAGER_DEFAULT_POLL_PERIOD_MS;

    if (channel == NULL)
    {
        return false;
    }

    period_ms = channel->settings.sample_period_ms > 0u ? channel->settings.sample_period_ms : SENSOR_MANAGER_DEFAULT_POLL_PERIOD_MS;
    if (channel->runtime.last_update_ms == 0u)
    {
        return true;
    }

    return (uint32_t)(now_ms - channel->runtime.last_update_ms) >= period_ms;
}

static float sensor_manager_runtime_value_as_float(const telemetry_channel_t *channel)
{
    if (channel == NULL)
    {
        return 0.0f;
    }

    switch (channel->metadata.value_type)
    {
    case TELEMETRY_CHANNEL_VALUE_DOUBLE:
        return (float)channel->runtime.value.f64;
    case TELEMETRY_CHANNEL_VALUE_INT32:
        return (float)channel->runtime.value.i32;
    case TELEMETRY_CHANNEL_VALUE_UINT32:
        return (float)channel->runtime.value.u32;
    case TELEMETRY_CHANNEL_VALUE_BOOLEAN:
        return channel->runtime.value.boolean ? 1.0f : 0.0f;
    case TELEMETRY_CHANNEL_VALUE_FLOAT:
    default:
        return channel->runtime.value.f32;
    }
}

static bool sensor_manager_channel_init(telemetry_channel_t *channel)
{
    sensor_manager_channel_context_t *context = sensor_manager_channel_context(channel);

    if (channel == NULL || context == NULL || context->driver_entry == NULL)
    {
        return false;
    }

    if (!channel->settings.enabled)
    {
        channel->runtime.quality = SERVICE_QUALITY_BAD;
        return true;
    }

    if (context->driver_entry->init == NULL)
    {
        return true;
    }

    return context->driver_entry->init(channel, context->source_index, context->source_flags);
}

static bool sensor_manager_channel_poll(telemetry_channel_t *channel, uint32_t now_ms)
{
    telemetry_channel_t updated_channel;
    sensor_manager_channel_context_t *context = sensor_manager_channel_context(channel);

    if (channel == NULL || context == NULL || context->driver_entry == NULL)
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

    if (!sensor_manager_should_sample(&updated_channel, now_ms))
    {
        return true;
    }

    if (context->driver_entry->poll == NULL)
    {
        updated_channel.runtime.error_count++;
        updated_channel.runtime.quality = SERVICE_QUALITY_BAD;
    }
    else
    {
        (void)context->driver_entry->poll(&updated_channel,
                                          context->source_index,
                                          context->source_flags,
                                          now_ms);
    }

    taskENTER_CRITICAL();
    *channel = updated_channel;
    taskEXIT_CRITICAL();
    return updated_channel.runtime.quality == SERVICE_QUALITY_GOOD;
}

static bool sensor_manager_channel_configure(telemetry_channel_t *channel, const void *config)
{
    (void)channel;
    (void)config;
    return true;
}

static bool sensor_manager_channel_self_test(telemetry_channel_t *channel)
{
    telemetry_channel_t copy = {0};

    if (channel == NULL)
    {
        return false;
    }

    copy = *channel;
    copy.runtime.last_update_ms = 0u;
    return sensor_manager_channel_poll(&copy, (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
}

static telemetry_channel_t *sensor_manager_get_primary_channel(void)
{
    telemetry_channel_t *channel = channel_manager_find_by_id(DEVICE_CHANNEL_ID_TEMPERATURE_1);

    if (channel != NULL)
    {
        return channel;
    }

    return channel_manager_count() > 0u ? channel_manager_get_by_index(0u) : NULL;
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
    snapshot->temperature_c = sensor_manager_runtime_value_as_float(channel);
}

static size_t sensor_manager_build_channels_from_config(const device_profile_config_t *config)
{
    size_t prepared_count = 0u;

    if (config == NULL)
    {
        return 0u;
    }

    memset(g_runtime_channel_configs, 0, sizeof(g_runtime_channel_configs));
    memset(g_channel_contexts, 0, sizeof(g_channel_contexts));
    memset(g_channel_storage, 0, sizeof(g_channel_storage));

    for (size_t index = 0u; index < CONFIG_SERVICE_MAX_CHANNELS && prepared_count < SENSOR_MANAGER_MAX_CHANNELS; ++index)
    {
        const config_channel_definition_t *stored_channel = &config->channels[index];
        const driver_catalog_entry_t *driver_entry = driver_catalog_get_by_id(stored_channel->driver_id);
        channel_config_t *runtime_channel = &g_runtime_channel_configs[prepared_count];
        sensor_manager_channel_context_t *context = &g_channel_contexts[prepared_count];

        if (stored_channel->in_use == 0u || driver_entry == NULL)
        {
            continue;
        }

        context->driver_entry = driver_entry;
        context->source_index = stored_channel->source_index;
        context->source_flags = stored_channel->source_flags;

        runtime_channel->metadata.id = stored_channel->id;
        runtime_channel->metadata.name = stored_channel->name;
        runtime_channel->metadata.description = stored_channel->description;
        runtime_channel->metadata.kind = (telemetry_channel_kind_t)stored_channel->kind;
        runtime_channel->metadata.value_type = (telemetry_channel_value_type_t)stored_channel->value_type;
        runtime_channel->metadata.unit = stored_channel->unit;
        runtime_channel->metadata.range.has_minimum = stored_channel->has_minimum != 0u;
        runtime_channel->metadata.range.has_maximum = stored_channel->has_maximum != 0u;
        runtime_channel->metadata.range.minimum = stored_channel->minimum;
        runtime_channel->metadata.range.maximum = stored_channel->maximum;
        runtime_channel->metadata.flags = stored_channel->flags;
        runtime_channel->settings = stored_channel->settings;
        runtime_channel->driver = &g_builtin_channel_driver;
        runtime_channel->driver_context = context;
        ++prepared_count;
    }

    return prepared_count;
}

static uint32_t sensor_manager_resolve_poll_period_ms(void)
{
    uint32_t min_period_ms = 0u;

    for (size_t index = 0u; index < channel_manager_count(); ++index)
    {
        const telemetry_channel_t *channel = channel_manager_get_by_index(index);
        const uint32_t candidate =
            (channel != NULL && channel->settings.enabled && channel->settings.sample_period_ms > 0u)
                ? channel->settings.sample_period_ms
                : SENSOR_MANAGER_DEFAULT_POLL_PERIOD_MS;

        if (min_period_ms == 0u || candidate < min_period_ms)
        {
            min_period_ms = candidate;
        }
    }

    return min_period_ms > 0u ? min_period_ms : SENSOR_MANAGER_DEFAULT_POLL_PERIOD_MS;
}

static bool sensor_manager_activate_running_config(void)
{
    bool have_pollable_channel = false;
    telemetry_channel_t *primary_channel = NULL;
    const device_profile_config_t *config = config_service_get();
    const size_t prepared_count = sensor_manager_build_channels_from_config(config);

    if (prepared_count == 0u)
    {
        sensor_manager_snapshot_t empty_snapshot = {0};

        empty_snapshot.enabled = true;
        empty_snapshot.quality = SERVICE_QUALITY_BAD;
        sensor_manager_set_snapshot(&empty_snapshot);
        g_initialized = false;
        return false;
    }

    vTaskSuspendAll();
    channel_manager_reset();
    (void)channel_manager_register_from_config(g_runtime_channel_configs,
                                               prepared_count,
                                               g_channel_storage,
                                               SENSOR_MANAGER_MAX_CHANNELS);

    g_initialized = channel_manager_count() > 0u;
    for (size_t index = 0u; index < channel_manager_count(); ++index)
    {
        telemetry_channel_t *channel = channel_manager_get_by_index(index);

        if (channel == NULL)
        {
            continue;
        }

        if (channel->driver != NULL && channel->driver->init != NULL && !channel->driver->init(channel))
        {
            channel->runtime.quality = SERVICE_QUALITY_BAD;
            channel->runtime.error_count++;
        }
        else
        {
            channel->runtime.quality = SERVICE_QUALITY_UNCERTAIN;
            if (channel->settings.enabled)
            {
                have_pollable_channel = true;
            }
        }
    }

    g_initialized = g_initialized && have_pollable_channel;
    g_applied_config_revision = config_service_get_running_revision();

    primary_channel = sensor_manager_get_primary_channel();
    {
        sensor_manager_snapshot_t snapshot = {0};
        sensor_manager_snapshot_from_channel(primary_channel, &snapshot);
        sensor_manager_set_snapshot(&snapshot);
    }
    xTaskResumeAll();
    return g_initialized;
}

bool sensor_manager_init(void)
{
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.enabled = true;
    g_snapshot.quality = SERVICE_QUALITY_UNCERTAIN;
    g_applied_config_revision = 0u;
    return sensor_manager_activate_running_config();
}

void sensor_manager_task(void *argument)
{
    (void)argument;

    TickType_t last_wake_time = xTaskGetTickCount();

    while (1)
    {
        sensor_manager_snapshot_t snapshot = {0};
        telemetry_channel_t primary_copy = {0};
        telemetry_channel_t *primary_channel = NULL;
        uint32_t poll_period_ms = SENSOR_MANAGER_DEFAULT_POLL_PERIOD_MS;
        const uint32_t running_revision = config_service_get_running_revision();

        if (running_revision != g_applied_config_revision)
        {
            (void)sensor_manager_activate_running_config();
        }

        if (!g_initialized)
        {
            snapshot.quality = SERVICE_QUALITY_BAD;
            sensor_manager_set_snapshot(&snapshot);
            vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(SENSOR_MANAGER_DEFAULT_POLL_PERIOD_MS));
            continue;
        }

        (void)channel_manager_poll_all((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));

        primary_channel = sensor_manager_get_primary_channel();
        if (primary_channel != NULL &&
            primary_channel->metadata.id != NULL &&
            channel_manager_copy_channel(primary_channel->metadata.id, &primary_copy))
        {
            sensor_manager_snapshot_from_channel(&primary_copy, &snapshot);
        }
        else
        {
            sensor_manager_snapshot_from_channel(NULL, &snapshot);
        }

        sensor_manager_set_snapshot(&snapshot);
        poll_period_ms = sensor_manager_resolve_poll_period_ms();
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(poll_period_ms > 0u ? poll_period_ms : 1u));
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
