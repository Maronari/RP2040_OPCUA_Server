#include "services/channel_manager.h"

#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "services/telemetry_driver.h"
#include "services/time_service.h"

#include "psram_allocator.h"

typedef struct
{
    bool in_use;
    uint32_t generation;
    uint8_t operation_count;
    telemetry_channel_t channel;
    char id[TELEMETRY_CHANNEL_ID_MAX_LEN];
    char name[TELEMETRY_CHANNEL_NAME_MAX_LEN];
    char description[TELEMETRY_CHANNEL_DESCRIPTION_MAX_LEN];
    char unit[TELEMETRY_CHANNEL_UNIT_MAX_LEN];
} channel_manager_slot_t;

static channel_manager_slot_t *g_slots;
#if !defined(PICO_ON_DEVICE)
/* Host suites inject a fixed backing store; production never emits this BSS. */
static channel_manager_slot_t g_host_slots[PLATFORM_CHANNEL_LIMIT];
#endif
static size_t g_count;

static bool strings_equal(const char *lhs, const char *rhs)
{
    return lhs != NULL && rhs != NULL && strcmp(lhs, rhs) == 0;
}

static bool copy_string(char *destination, size_t destination_size, const char *source)
{
    size_t length = 0u;

    if (destination == NULL || destination_size == 0u || source == NULL)
    {
        return false;
    }

    while (length < destination_size && source[length] != '\0')
    {
        ++length;
    }
    if (length == 0u || length == destination_size)
    {
        return false;
    }
    memcpy(destination, source, length + 1u);
    return true;
}

static bool copy_optional_string(char *destination, size_t destination_size, const char *source)
{
    size_t length = 0u;

    if (source == NULL)
    {
        destination[0] = '\0';
        return true;
    }
    while (length < destination_size && source[length] != '\0')
    {
        ++length;
    }
    if (length == destination_size)
    {
        return false;
    }
    memcpy(destination, source, length + 1u);
    return true;
}

static void snapshot_from_slot(const channel_manager_slot_t *slot,
                               telemetry_channel_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->channel = slot->channel;
    (void)memcpy(snapshot->id, slot->id, sizeof(snapshot->id));
    (void)memcpy(snapshot->name, slot->name, sizeof(snapshot->name));
    (void)memcpy(snapshot->description, slot->description, sizeof(snapshot->description));
    (void)memcpy(snapshot->unit, slot->unit, sizeof(snapshot->unit));
    snapshot->channel.metadata.id = snapshot->id;
    snapshot->channel.metadata.name = snapshot->name;
    snapshot->channel.metadata.description = snapshot->description;
    snapshot->channel.metadata.unit = snapshot->unit;
}

void telemetry_channel_snapshot_clone(telemetry_channel_snapshot_t *destination,
                                      const telemetry_channel_snapshot_t *source)
{
    if (destination == NULL || source == NULL)
    {
        return;
    }
    memcpy(destination, source, sizeof(*destination));
    destination->channel.metadata.id = destination->id;
    destination->channel.metadata.name = destination->name;
    destination->channel.metadata.description = destination->description;
    destination->channel.metadata.unit = destination->unit;
}

static bool find_slot_by_id_locked(const char *id, size_t *out_index)
{
    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        if (g_slots[index].in_use && strings_equal(g_slots[index].id, id))
        {
            if (out_index != NULL)
            {
                *out_index = index;
            }
            return true;
        }
    }
    return false;
}

bool channel_manager_init(void)
{
    if (g_slots == NULL)
    {
#if defined(PICO_ON_DEVICE)
        if (!psram_allocator_is_ready())
        {
            return false;
        }
        g_slots = psram_calloc(PLATFORM_CHANNEL_LIMIT, sizeof(*g_slots));
        if (g_slots == NULL)
        {
            return false;
        }
#else
        g_slots = g_host_slots;
#endif
    }
    return channel_manager_reset();
}

bool channel_manager_is_ready(void)
{
    return g_slots != NULL;
}

bool channel_manager_reset(void)
{
    bool may_reset = true;

    if (g_slots == NULL)
    {
        return false;
    }
    taskENTER_CRITICAL();
    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        if (g_slots[index].operation_count != 0u)
        {
            may_reset = false;
            break;
        }
    }
    if (may_reset)
    {
        memset(g_slots, 0, PLATFORM_CHANNEL_LIMIT * sizeof(*g_slots));
        g_count = 0u;
    }
    taskEXIT_CRITICAL();
    return may_reset;
}

bool channel_manager_register(const telemetry_channel_t *channel,
                              channel_manager_handle_t *out_handle)
{
    size_t free_index = PLATFORM_CHANNEL_LIMIT;

    if (channel == NULL || channel->metadata.id == NULL || channel->metadata.name == NULL ||
        channel->metadata.id[0] == '\0' || channel->metadata.name[0] == '\0' || g_slots == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();
    if (g_count >= PLATFORM_CHANNEL_LIMIT || find_slot_by_id_locked(channel->metadata.id, NULL))
    {
        taskEXIT_CRITICAL();
        return false;
    }
    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        if (!g_slots[index].in_use)
        {
            free_index = index;
            break;
        }
    }
    if (free_index == PLATFORM_CHANNEL_LIMIT ||
        !copy_string(g_slots[free_index].id, sizeof(g_slots[free_index].id), channel->metadata.id) ||
        !copy_string(g_slots[free_index].name, sizeof(g_slots[free_index].name), channel->metadata.name) ||
        !copy_optional_string(g_slots[free_index].description, sizeof(g_slots[free_index].description), channel->metadata.description) ||
        !copy_optional_string(g_slots[free_index].unit, sizeof(g_slots[free_index].unit), channel->metadata.unit))
    {
        memset(&g_slots[free_index], 0, sizeof(g_slots[free_index]));
        taskEXIT_CRITICAL();
        return false;
    }

    g_slots[free_index].channel = *channel;
    g_slots[free_index].channel.metadata.id = g_slots[free_index].id;
    g_slots[free_index].channel.metadata.name = g_slots[free_index].name;
    g_slots[free_index].channel.metadata.description = g_slots[free_index].description;
    g_slots[free_index].channel.metadata.unit = g_slots[free_index].unit;
    g_slots[free_index].channel.runtime.source_timestamp_unix_us = TELEMETRY_CHANNEL_SOURCE_TIMESTAMP_INVALID;
    g_slots[free_index].channel.runtime.quality = SERVICE_QUALITY_UNCERTAIN;
    g_slots[free_index].generation++;
    if (g_slots[free_index].generation == 0u)
    {
        g_slots[free_index].generation = 1u;
    }
    g_slots[free_index].in_use = true;
    ++g_count;
    if (out_handle != NULL)
    {
        out_handle->index = (uint8_t)free_index;
        out_handle->generation = g_slots[free_index].generation;
    }
    taskEXIT_CRITICAL();
    return true;
}

bool channel_manager_copy_snapshot_by_id(const char *id, telemetry_channel_snapshot_t *out_snapshot)
{
    size_t index;
    bool found;

    if (id == NULL || out_snapshot == NULL || g_slots == NULL)
    {
        return false;
    }
    taskENTER_CRITICAL();
    found = find_slot_by_id_locked(id, &index);
    if (found)
    {
        snapshot_from_slot(&g_slots[index], out_snapshot);
    }
    taskEXIT_CRITICAL();
    return found;
}

bool channel_manager_copy_snapshot_by_index(size_t index, telemetry_channel_snapshot_t *out_snapshot)
{
    bool found;

    if (out_snapshot == NULL || index >= PLATFORM_CHANNEL_LIMIT || g_slots == NULL)
    {
        return false;
    }
    taskENTER_CRITICAL();
    found = g_slots[index].in_use;
    if (found)
    {
        snapshot_from_slot(&g_slots[index], out_snapshot);
    }
    taskEXIT_CRITICAL();
    return found;
}

bool channel_manager_update_settings(const char *id, const telemetry_channel_settings_t *settings)
{
    size_t index;
    bool found;

    if (id == NULL || settings == NULL || g_slots == NULL)
    {
        return false;
    }
    taskENTER_CRITICAL();
    found = find_slot_by_id_locked(id, &index);
    if (found)
    {
        g_slots[index].channel.settings = *settings;
        ++g_slots[index].generation;
        if (g_slots[index].generation == 0u)
        {
            g_slots[index].generation = 1u;
        }
    }
    taskEXIT_CRITICAL();
    return found;
}

size_t channel_manager_register_from_config(const channel_config_t *configs, size_t config_count)
{
    size_t registered = 0u;

    if (configs == NULL)
    {
        return 0u;
    }
    for (size_t index = 0u; index < config_count && registered < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        if (channel_manager_register((const telemetry_channel_t *)&(telemetry_channel_t){
                                         .metadata = configs[index].metadata,
                                         .settings = configs[index].settings,
                                         .driver = configs[index].driver,
                                         .driver_context = configs[index].driver_context,
                                     }, NULL))
        {
            ++registered;
        }
    }
    return registered;
}

size_t channel_manager_count(void)
{
    size_t count;
    if (g_slots == NULL)
    {
        return 0u;
    }
    taskENTER_CRITICAL();
    count = g_count;
    taskEXIT_CRITICAL();
    return count;
}

bool channel_manager_acquire_driver_operation(size_t index,
                                              channel_manager_driver_operation_t *out_operation)
{
    if (out_operation == NULL || index >= PLATFORM_CHANNEL_LIMIT || g_slots == NULL)
    {
        return false;
    }
    taskENTER_CRITICAL();
    if (!g_slots[index].in_use || g_slots[index].operation_count == UINT8_MAX)
    {
        taskEXIT_CRITICAL();
        return false;
    }
    ++g_slots[index].operation_count;
    out_operation->handle.index = (uint8_t)index;
    out_operation->handle.generation = g_slots[index].generation;
    snapshot_from_slot(&g_slots[index], &out_operation->snapshot);
    taskEXIT_CRITICAL();
    return true;
}

bool channel_manager_publish_driver_operation(const channel_manager_driver_operation_t *operation)
{
    channel_manager_slot_t *slot;

    if (operation == NULL || operation->handle.index >= PLATFORM_CHANNEL_LIMIT || g_slots == NULL)
    {
        return false;
    }
    taskENTER_CRITICAL();
    slot = &g_slots[operation->handle.index];
    if (!slot->in_use || slot->generation != operation->handle.generation || slot->operation_count == 0u)
    {
        taskEXIT_CRITICAL();
        return false;
    }
    slot->channel.runtime = operation->snapshot.channel.runtime;
    taskEXIT_CRITICAL();
    return true;
}

void channel_manager_release_driver_operation(const channel_manager_driver_operation_t *operation)
{
    if (operation == NULL || operation->handle.index >= PLATFORM_CHANNEL_LIMIT || g_slots == NULL)
    {
        return;
    }
    taskENTER_CRITICAL();
    if (g_slots[operation->handle.index].in_use &&
        g_slots[operation->handle.index].operation_count > 0u)
    {
        --g_slots[operation->handle.index].operation_count;
    }
    taskEXIT_CRITICAL();
}

bool channel_manager_poll_all(uint32_t now_ms)
{
    bool all_ok = true;

    if (g_slots == NULL)
    {
        return false;
    }

    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        channel_manager_driver_operation_t operation;
        telemetry_driver_result_t result;

        if (!channel_manager_acquire_driver_operation(index, &operation))
        {
            continue;
        }
        if (!operation.snapshot.channel.settings.enabled || operation.snapshot.channel.driver == NULL ||
            operation.snapshot.channel.driver->read == NULL)
        {
            channel_manager_release_driver_operation(&operation);
            continue;
        }

        /* The driver executes without the registry critical section held. */
        result = operation.snapshot.channel.driver->read(&operation.snapshot.channel, now_ms);
        if (result != TELEMETRY_DRIVER_RESULT_OK)
        {
            ++operation.snapshot.channel.runtime.error_count;
            operation.snapshot.channel.runtime.quality = SERVICE_QUALITY_BAD;
            all_ok = false;
        }
        else
        {
            uint64_t source_timestamp = TELEMETRY_CHANNEL_SOURCE_TIMESTAMP_INVALID;

            (void)time_service_get_utc_time_us(&source_timestamp);
            operation.snapshot.channel.runtime.source_timestamp_unix_us = source_timestamp;
            operation.snapshot.channel.runtime.last_update_ms = now_ms;
            ++operation.snapshot.channel.runtime.sample_count;
            operation.snapshot.channel.runtime.quality = SERVICE_QUALITY_GOOD;
        }
        if (!channel_manager_publish_driver_operation(&operation))
        {
            all_ok = false;
        }
        channel_manager_release_driver_operation(&operation);
    }
    return all_ok;
}
