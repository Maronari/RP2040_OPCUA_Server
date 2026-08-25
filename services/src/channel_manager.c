/**
 * @file
 * @brief Bounded, PSRAM-backed channel registry: lifecycle (register/reset),
 *        snapshot reads, settings updates, and driver dispatch.
 *
 * Responsibilities:
 * - Owns the fixed PLATFORM_CHANNEL_LIMIT-slot channel registry
 *   (g_slots/g_host_slots) and all its string storage (id/name/description/
 *   unit are copied in, never borrowed).
 * - Implements the acquire/publish/release driver-operation lease pattern:
 *   a generation+token snapshot is taken under a critical section, the
 *   driver call happens outside any lock, and the result is published back
 *   only if the generation/token still match -- concurrent reconfiguration
 *   causes a silent, safe rejection rather than a torn write.
 * - Drives channel_manager_poll_all() from sensor_manager_task() and
 *   channel_manager_write_value() from OPC UA Write callbacks.
 *
 * Ownership: every string/struct in g_slots is exclusively owned here;
 * callers only ever get an owned telemetry_channel_snapshot_t copy.
 *
 * Concurrency: taskENTER_CRITICAL()/taskEXIT_CRITICAL() (global critical
 * section, not a mutex) around every registry access; drivers are never
 * called while this lock is held. See docs/code/state-ownership.md for the
 * full lease-pattern description.
 */

#include "services/channel_manager.h"

#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "services/telemetry_driver.h"
#include "services/time_service.h"

#include "psram_allocator.h"

/**
 * @brief Internal registry slot: owned string storage plus lease/generation
 *        bookkeeping for one channel.
 *
 * @c generation increments on register and on channel_manager_update_settings();
 * @c operation_active/@c operation_token implement the driver-operation
 * lease (see channel_manager_acquire_driver_operation()). @c channel's
 * metadata id/name/description/unit pointers always point back into this
 * slot's own id/name/description/unit arrays (never external storage),
 * fixed up by snapshot_from_slot() on every copy-out.
 */
typedef struct
{
    bool in_use;
    uint32_t generation;
    bool operation_active;
    uint32_t operation_token;
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

/**
 * @brief Copy a required, non-empty NUL-terminated string into a
 *        fixed-size buffer, rejecting anything that would truncate.
 *
 * @param destination Fixed-size destination buffer.
 * @param destination_size Size of @p destination in bytes.
 * @param source Source string; must be non-NULL and non-empty.
 * @return true if the whole string (with terminator) fit; false on NULL/
 *         empty/oversized input, in which case @p destination is left
 *         unmodified by this function (callers must not rely on it being
 *         zeroed).
 */
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

/**
 * @brief Copy an optional NUL-terminated string into a fixed-size buffer,
 *        or write an empty string if @p source is NULL.
 *
 * Unlike copy_string(), an empty/NULL @p source is a valid input (used for
 * optional metadata like description/unit) -- only an over-length @p source
 * is rejected.
 *
 * @param destination Fixed-size destination buffer.
 * @param destination_size Size of @p destination in bytes.
 * @param source Source string, or NULL for "no value".
 * @return true on success; false if @p source does not fit.
 */
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

/**
 * @brief Copy one registry slot out into a caller-owned, self-referential
 *        snapshot.
 *
 * Copies the slot's channel struct and its four string buffers by value,
 * then re-points @c snapshot->channel.metadata's id/name/description/unit
 * pointers at the *snapshot's own* embedded arrays -- never at @p slot's
 * storage -- so the returned snapshot remains valid after the registry
 * slot is later mutated or recycled. Must be called while holding the
 * registry critical section (all call sites do).
 *
 * @param slot Source registry slot; not retained.
 * @param snapshot Destination; fully overwritten.
 */
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

/* Implementation note: telemetry_channel_snapshot_t is self-referential
 * (channel.metadata's id/name/description/unit point back into this same
 * struct's embedded string arrays), so a plain struct-assignment/memcpy of
 * one snapshot into another would leave the destination's metadata pointers
 * dangling into the *source* object. This function copies the raw bytes and
 * then re-points every metadata pointer at the destination's own arrays --
 * always use this instead of `*dst = *src` when duplicating a snapshot.
 * Declared in services/telemetry_channel.h (outside this file's assigned
 * documentation scope). */
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

/**
 * @brief Linear-scan the registry for a slot bound to @p id.
 *
 * @pre Caller must already hold the registry critical section (name suffix
 *      `_locked` marks this as a lock-held-only helper; it does not acquire
 *      or release the lock itself).
 * @param id Channel id to find.
 * @param out_index Optional; receives the matching slot index on success.
 * @return true if a matching in-use slot was found.
 */
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

/**
 * @brief Validate a write value against its declared type before any driver
 *        call.
 *
 * For TELEMETRY_CHANNEL_VALUE_STRING, additionally requires a NUL terminator
 * within TELEMETRY_CHANNEL_STRING_VALUE_MAX_LEN bytes (rejects an unterminated
 * buffer rather than reading out of bounds downstream).
 *
 * @param type Declared value type.
 * @param value Value to validate; must be non-NULL.
 * @return true if @p value is well-formed for @p type.
 */
static bool channel_manager_value_is_valid(telemetry_channel_value_type_t type,
                                           const telemetry_channel_value_t *value)
{
    return value != NULL && type <= TELEMETRY_CHANNEL_VALUE_STRING &&
           (type != TELEMETRY_CHANNEL_VALUE_STRING ||
            memchr(value->string, '\0', TELEMETRY_CHANNEL_STRING_VALUE_MAX_LEN) != NULL);
}

/**
 * @brief Translate a driver-level telemetry_driver_result_t into the
 *        service-level channel_manager_write_result_t exposed to callers.
 *
 * @param result Result returned by a driver's write() call.
 * @return The corresponding channel_manager_write_result_t; an unrecognized
 *         driver result conservatively maps to
 *         CHANNEL_MANAGER_WRITE_RESULT_HARDWARE_FAILURE.
 */
static channel_manager_write_result_t channel_manager_map_driver_result(telemetry_driver_result_t result)
{
    switch (result)
    {
    case TELEMETRY_DRIVER_RESULT_OK: return CHANNEL_MANAGER_WRITE_RESULT_OK;
    case TELEMETRY_DRIVER_RESULT_UNSUPPORTED: return CHANNEL_MANAGER_WRITE_RESULT_UNSUPPORTED;
    case TELEMETRY_DRIVER_RESULT_INVALID_STATE: return CHANNEL_MANAGER_WRITE_RESULT_INVALID_LIFECYCLE;
    case TELEMETRY_DRIVER_RESULT_TIMEOUT: return CHANNEL_MANAGER_WRITE_RESULT_TIMEOUT;
    case TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE: return CHANNEL_MANAGER_WRITE_RESULT_HARDWARE_FAILURE;
    case TELEMETRY_DRIVER_RESULT_NO_MEMORY: return CHANNEL_MANAGER_WRITE_RESULT_RESOURCE_FAILURE;
    case TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT: return CHANNEL_MANAGER_WRITE_RESULT_TYPE_MISMATCH;
    default: return CHANNEL_MANAGER_WRITE_RESULT_HARDWARE_FAILURE;
    }
}

/**
 * @brief Current time in milliseconds, for stamping post-write runtime
 *        state.
 *
 * On device, reads the FreeRTOS tick count (xTaskGetTickCount() *
 * portTICK_PERIOD_MS); on host builds (no scheduler), always returns 0.
 */
static uint32_t channel_manager_current_time_ms(void)
{
#if defined(PICO_ON_DEVICE)
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
#else
    return 0u;
#endif
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
        if (g_slots[index].operation_active)
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

channel_manager_write_result_t channel_manager_write_value(const char *id,
                                                            telemetry_channel_value_type_t type,
                                                            const telemetry_channel_value_t *value,
                                                            telemetry_channel_snapshot_t *out_snapshot)
{
    channel_manager_driver_operation_t operation = {0};
    telemetry_driver_result_t driver_result;
    channel_manager_write_result_t result;
    size_t index;
    bool found;
    uint64_t source_timestamp = TELEMETRY_CHANNEL_SOURCE_TIMESTAMP_INVALID;

    if (id == NULL || value == NULL || !channel_manager_value_is_valid(type, value))
    {
        return CHANNEL_MANAGER_WRITE_RESULT_TYPE_MISMATCH;
    }
    if (g_slots == NULL)
    {
        return CHANNEL_MANAGER_WRITE_RESULT_INVALID_LIFECYCLE;
    }

    taskENTER_CRITICAL();
    found = find_slot_by_id_locked(id, &index);
    if (!found)
    {
        taskEXIT_CRITICAL();
        return CHANNEL_MANAGER_WRITE_RESULT_NOT_FOUND;
    }
    if (!g_slots[index].channel.settings.enabled ||
        (g_slots[index].channel.metadata.flags & TELEMETRY_CHANNEL_FLAG_ENABLED) == 0u)
    {
        taskEXIT_CRITICAL();
        return CHANNEL_MANAGER_WRITE_RESULT_DISABLED;
    }
    if ((g_slots[index].channel.metadata.flags & TELEMETRY_CHANNEL_FLAG_WRITABLE) == 0u)
    {
        taskEXIT_CRITICAL();
        return CHANNEL_MANAGER_WRITE_RESULT_READ_ONLY;
    }
    if (g_slots[index].channel.metadata.value_type != type)
    {
        taskEXIT_CRITICAL();
        return CHANNEL_MANAGER_WRITE_RESULT_TYPE_MISMATCH;
    }
    if (g_slots[index].channel.driver == NULL || g_slots[index].channel.driver->write == NULL)
    {
        taskEXIT_CRITICAL();
        return CHANNEL_MANAGER_WRITE_RESULT_UNSUPPORTED;
    }
    taskEXIT_CRITICAL();

    if (!channel_manager_acquire_driver_operation(index, &operation))
    {
        return CHANNEL_MANAGER_WRITE_RESULT_RESOURCE_FAILURE;
    }
    /* The settings/type checks above may have raced a reconfiguration.  Do
     * not invoke a driver with a lease that no longer describes this request. */
    if (!strings_equal(operation.snapshot.channel.metadata.id, id) ||
        !operation.snapshot.channel.settings.enabled ||
        (operation.snapshot.channel.metadata.flags &
         (TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_WRITABLE)) !=
            (TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_WRITABLE) ||
        operation.snapshot.channel.metadata.value_type != type ||
        operation.snapshot.channel.driver == NULL || operation.snapshot.channel.driver->write == NULL)
    {
        channel_manager_release_driver_operation(&operation);
        return CHANNEL_MANAGER_WRITE_RESULT_STALE;
    }

    driver_result = operation.snapshot.channel.driver->write(&operation.snapshot.channel, value);
    result = channel_manager_map_driver_result(driver_result);
    if (result == CHANNEL_MANAGER_WRITE_RESULT_OK)
    {
        (void)time_service_get_utc_time_us(&source_timestamp);
        operation.snapshot.channel.runtime.source_timestamp_unix_us = source_timestamp;
        operation.snapshot.channel.runtime.last_update_ms = channel_manager_current_time_ms();
        ++operation.snapshot.channel.runtime.sample_count;
        operation.snapshot.channel.runtime.quality = SERVICE_QUALITY_GOOD;
        if (!channel_manager_publish_driver_operation(&operation))
        {
            result = CHANNEL_MANAGER_WRITE_RESULT_STALE;
        }
        else if (out_snapshot != NULL)
        {
            telemetry_channel_snapshot_clone(out_snapshot, &operation.snapshot);
        }
    }
    channel_manager_release_driver_operation(&operation);
    return result;
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
    if (!g_slots[index].in_use || g_slots[index].operation_active)
    {
        taskEXIT_CRITICAL();
        return false;
    }
    ++g_slots[index].operation_token;
    if (g_slots[index].operation_token == 0u)
    {
        ++g_slots[index].operation_token;
    }
    g_slots[index].operation_active = true;
    out_operation->handle.index = (uint8_t)index;
    out_operation->handle.generation = g_slots[index].generation;
    out_operation->operation_token = g_slots[index].operation_token;
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
    if (!slot->in_use || slot->generation != operation->handle.generation || !slot->operation_active ||
        slot->operation_token != operation->operation_token)
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
    if (g_slots[operation->handle.index].operation_active &&
        g_slots[operation->handle.index].operation_token == operation->operation_token)
    {
        g_slots[operation->handle.index].operation_active = false;
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
