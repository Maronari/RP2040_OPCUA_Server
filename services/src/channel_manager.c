#include "services/channel_manager.h"

#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "services/telemetry_driver.h"

static telemetry_channel_t *g_registered_channels[CHANNEL_MANAGER_MAX_CHANNELS];
static size_t g_registered_channel_count = 0u;

static bool channel_manager_strings_equal(const char *lhs, const char *rhs)
{
    if (lhs == NULL || rhs == NULL)
    {
        return false;
    }

    return strcmp(lhs, rhs) == 0;
}

void channel_manager_init(void)
{
    channel_manager_reset();
}

void channel_manager_reset(void)
{
    memset(g_registered_channels, 0, sizeof(g_registered_channels));
    g_registered_channel_count = 0u;
}

bool channel_manager_register(telemetry_channel_t *channel)
{
    if (channel == NULL ||
        channel->metadata.id == NULL || channel->metadata.id[0] == '\0' ||
        channel->metadata.name == NULL || channel->metadata.name[0] == '\0' ||
        g_registered_channel_count >= CHANNEL_MANAGER_MAX_CHANNELS)
    {
        return false;
    }

    if (channel_manager_find_by_id(channel->metadata.id) != NULL)
    {
        return false;
    }

    g_registered_channels[g_registered_channel_count++] = channel;
    return true;
}

bool channel_manager_copy_channel(const char *id, telemetry_channel_t *out_channel)
{
    if (id == NULL || out_channel == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();
    for (size_t index = 0u; index < g_registered_channel_count; ++index)
    {
        telemetry_channel_t *channel = g_registered_channels[index];

        if (channel != NULL && channel_manager_strings_equal(channel->metadata.id, id))
        {
            *out_channel = *channel;
            taskEXIT_CRITICAL();
            return true;
        }
    }
    taskEXIT_CRITICAL();

    return false;
}

bool channel_manager_update_settings(const char *id, const telemetry_channel_settings_t *settings)
{
    if (id == NULL || settings == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();
    for (size_t index = 0u; index < g_registered_channel_count; ++index)
    {
        telemetry_channel_t *channel = g_registered_channels[index];

        if (channel != NULL && channel_manager_strings_equal(channel->metadata.id, id))
        {
            channel->settings = *settings;
            taskEXIT_CRITICAL();
            return true;
        }
    }
    taskEXIT_CRITICAL();

    return false;
}

size_t channel_manager_register_from_config(const channel_config_t *configs,
                                            size_t config_count,
                                            telemetry_channel_t *channel_storage,
                                            size_t channel_capacity)
{
    size_t registered_count = 0u;

    if (configs == NULL || channel_storage == NULL || channel_capacity == 0u)
    {
        return 0u;
    }

    for (size_t index = 0u; index < config_count && registered_count < channel_capacity; ++index)
    {
        telemetry_channel_t *channel = &channel_storage[registered_count];

        memset(channel, 0, sizeof(*channel));
        channel->metadata = configs[index].metadata;
        channel->settings = configs[index].settings;
        channel->driver = configs[index].driver;
        channel->driver_context = configs[index].driver_context;
        channel->runtime.quality = SERVICE_QUALITY_UNCERTAIN;

        if (!channel_manager_register(channel))
        {
            continue;
        }

        ++registered_count;
    }

    return registered_count;
}

size_t channel_manager_count(void)
{
    return g_registered_channel_count;
}

telemetry_channel_t *channel_manager_get_by_index(size_t index)
{
    if (index >= g_registered_channel_count)
    {
        return NULL;
    }

    return g_registered_channels[index];
}

telemetry_channel_t *channel_manager_find_by_id(const char *id)
{
    for (size_t index = 0u; index < g_registered_channel_count; ++index)
    {
        if (channel_manager_strings_equal(g_registered_channels[index]->metadata.id, id))
        {
            return g_registered_channels[index];
        }
    }

    return NULL;
}

telemetry_channel_t *channel_manager_find_by_name(const char *name)
{
    for (size_t index = 0u; index < g_registered_channel_count; ++index)
    {
        if (channel_manager_strings_equal(g_registered_channels[index]->metadata.name, name))
        {
            return g_registered_channels[index];
        }
    }

    return NULL;
}

bool channel_manager_poll_all(uint32_t now_ms)
{
    bool all_ok = true;

    for (size_t index = 0u; index < g_registered_channel_count; ++index)
    {
        telemetry_channel_t *channel = g_registered_channels[index];

        if (channel == NULL || channel->driver == NULL || channel->driver->poll == NULL)
        {
            continue;
        }

        if (!channel->driver->poll(channel, now_ms))
        {
            all_ok = false;
        }
    }

    return all_ok;
}
