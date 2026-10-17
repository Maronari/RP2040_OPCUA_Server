/**
 * @file
 * @brief Bounded resource snapshots and compatibility diagnostics.
 *
 * The FreeRTOS trace ingress is independent from service initialization so
 * Idle, Timer and InitTask lifecycle events are retained. Trace callbacks
 * only touch the fixed ingress table while the kernel already holds its task
 * critical section. The sampler copies that table before querying task state.
 */

#include "services/diag_service.h"
#include "services/benchmark_timing.h"

#include <string.h>

#include <semphr.h>

#include "hardware/watchdog.h"
#include "lwip_psram_heap.h"
#include "psram_allocator.h"

#if defined(PICO_ON_DEVICE)
#include "pico/platform/sections.h"
#define DIAG_SAMPLING_SCRATCH_SECTION __scratch_y("diag_sampling")
#else
#define DIAG_SAMPLING_SCRATCH_SECTION
#endif

#define DIAG_SERVICE_LOCK_TIMEOUT_TICKS pdMS_TO_TICKS(10u)
#define DIAG_SERVICE_NATIVE_RUNTIME_MODULUS_MS (UINT64_C(1) << 32u)

typedef struct
{
    char name[configMAX_TASK_NAME_LEN];
    void *identity;
    uint32_t task_number;
    uint64_t generation;
    uint64_t generation_wrap_count;
    bool present;
    bool canonical;
} diag_task_ingress_t;

typedef struct
{
    bool initialized;
    uint64_t generation;
    uint32_t last_raw_runtime_ms;
    uint64_t last_sample_uptime_ms;
    diag_service_counter_t extended_runtime_ms;
} diag_task_runtime_state_t;

typedef struct
{
    diag_service_counter_t failed_allocations;
    diag_service_counter_t invalid_context_failures;
    uint64_t last_failed_requested_bytes;
    uint64_t last_failed_adjusted_bytes;
    diag_service_alloc_failure_reason_t last_failure_reason;
    uint64_t last_failure_uptime_ms;
    bool last_failure_timestamp_valid;
    uint64_t last_failure_timestamp_unix_us;
} diag_heap_failure_state_t;

typedef struct
{
    diag_service_snapshot_t snapshots[2];
} diag_snapshot_storage_t;

/* uxTaskGetSystemState() runs while the scheduler is suspended. Keep its
 * fixed work buffers in internal SRAM rather than touching external PSRAM in
 * that bounded kernel path. Only OpcUaTask calls diag_service_sample(), and
 * readers never access these work buffers. */
typedef struct
{
    diag_task_ingress_t ingress_sample[DIAG_SERVICE_MAX_SNAPSHOT_TASKS];
    TaskStatus_t task_status_sample[DIAG_SERVICE_MAX_SNAPSHOT_TASKS];
} diag_sampling_scratch_t;

static struct
{
    char name[configMAX_TASK_NAME_LEN];
} g_legacy_tasks[DIAG_SERVICE_MAX_TASKS];

static size_t g_legacy_task_count;
static diag_task_ingress_t g_task_ingress[DIAG_SERVICE_MAX_SNAPSHOT_TASKS];
static diag_task_runtime_state_t g_task_runtime[DIAG_SERVICE_MAX_SNAPSHOT_TASKS];
static bool g_task_capacity_exceeded;

static char g_reset_cause[DIAG_SERVICE_RESET_CAUSE_MAX_LEN];
static uint32_t g_last_fault_code;
static diag_heap_failure_state_t g_heap_failures;

static diag_snapshot_storage_t *g_snapshot_storage;
static diag_sampling_scratch_t g_sampling_scratch DIAG_SAMPLING_SCRATCH_SECTION;
static uint8_t g_published_snapshot_index;
static bool g_snapshot_valid;
static TickType_t g_snapshot_tick;
static StaticSemaphore_t g_snapshot_lock_storage;
static StaticSemaphore_t g_snapshot_sample_lock_storage;

_Static_assert(sizeof(diag_sampling_scratch_t) <= 2048u,
               "task reconciliation scratch must fit below the Core0 Scratch-Y stack");
static SemaphoreHandle_t g_snapshot_lock;
static SemaphoreHandle_t g_snapshot_sample_lock;

static uint32_t g_heap_allocations_raw;
static uint32_t g_heap_frees_raw;
static bool g_total_runtime_initialized;
static uint32_t g_total_runtime_raw;
static uint64_t g_total_runtime_sample_uptime_ms;
static bool g_previous_percent_sample_complete;

static void diag_copy_name(char *destination, const char *source)
{
    if (source == NULL)
    {
        destination[0] = '\0';
        return;
    }

    strncpy(destination, source, configMAX_TASK_NAME_LEN);
    destination[configMAX_TASK_NAME_LEN - 1u] = '\0';
}

static void diag_counter_add(diag_service_counter_t *counter, uint64_t amount)
{
    if (UINT64_MAX - counter->value < amount)
    {
        counter->value = amount - (UINT64_MAX - counter->value) - 1u;
        if (counter->wrap_count != UINT64_MAX)
        {
            ++counter->wrap_count;
        }
        return;
    }
    counter->value += amount;
}

static void diag_increment_generation(diag_task_ingress_t *record)
{
    ++record->generation;
    if (record->generation == 0u && record->generation_wrap_count != UINT64_MAX)
    {
        ++record->generation_wrap_count;
    }
}

static int diag_find_ingress_by_name(const char *name)
{
    for (size_t index = 0u; index < DIAG_SERVICE_MAX_SNAPSHOT_TASKS; ++index)
    {
        if (g_task_ingress[index].canonical &&
            strcmp(g_task_ingress[index].name, name) == 0)
        {
            return (int)index;
        }
    }
    return -1;
}

static int diag_find_ingress_by_identity(const void *identity)
{
    for (size_t index = 0u; index < DIAG_SERVICE_MAX_SNAPSHOT_TASKS; ++index)
    {
        if (g_task_ingress[index].present && g_task_ingress[index].identity == identity)
        {
            return (int)index;
        }
    }
    return -1;
}

static int diag_find_live_duplicate(const diag_task_ingress_t *records,
                                    const char *name,
                                    size_t canonical_slot)
{
    for (size_t index = 0u; index < DIAG_SERVICE_MAX_SNAPSHOT_TASKS; ++index)
    {
        if (index != canonical_slot &&
            !records[index].canonical &&
            records[index].present &&
            strcmp(records[index].name, name) == 0)
        {
            return (int)index;
        }
    }
    return -1;
}

static int diag_find_unused_ingress(void)
{
    for (size_t index = 0u; index < DIAG_SERVICE_MAX_SNAPSHOT_TASKS; ++index)
    {
        if (g_task_ingress[index].name[0] == '\0')
        {
            return (int)index;
        }
    }
    return -1;
}

static bool diag_ingress_has_conflict(const diag_task_ingress_t *records)
{
    for (size_t canonical = 0u;
         canonical < DIAG_SERVICE_MAX_SNAPSHOT_TASKS;
         ++canonical)
    {
        if (!records[canonical].canonical || !records[canonical].present)
        {
            continue;
        }
        for (size_t candidate = 0u;
             candidate < DIAG_SERVICE_MAX_SNAPSHOT_TASKS;
             ++candidate)
        {
            if (candidate != canonical && records[candidate].present &&
                strcmp(records[candidate].name, records[canonical].name) == 0)
            {
                return true;
            }
        }
    }
    return false;
}

static diag_service_task_state_t diag_map_task_state(eTaskState state)
{
    switch (state)
    {
        case eRunning:
            return DIAG_SERVICE_TASK_STATE_RUNNING;
        case eReady:
            return DIAG_SERVICE_TASK_STATE_READY;
        case eBlocked:
            return DIAG_SERVICE_TASK_STATE_BLOCKED;
        case eSuspended:
            return DIAG_SERVICE_TASK_STATE_SUSPENDED;
        case eDeleted:
            return DIAG_SERVICE_TASK_STATE_DELETED;
        default:
            return DIAG_SERVICE_TASK_STATE_INVALID;
    }
}

static bool diag_snapshot_lock(void)
{
    return g_snapshot_lock != NULL &&
           xSemaphoreTake(g_snapshot_lock, DIAG_SERVICE_LOCK_TIMEOUT_TICKS) == pdTRUE;
}

/* Readers only hold g_snapshot_lock for an owned-copy operation. A separate
 * writer gate permits collection into the unpublished double-buffer slot
 * without allowing two callers to select and mutate the same slot. */
static bool diag_snapshot_sample_lock(void)
{
    return g_snapshot_sample_lock != NULL &&
           xSemaphoreTake(g_snapshot_sample_lock, DIAG_SERVICE_LOCK_TIMEOUT_TICKS) == pdTRUE;
}

static bool diag_try_allocate_snapshot_storage(void)
{
    if (g_snapshot_storage != NULL)
    {
        return true;
    }

    g_snapshot_storage =
        (diag_snapshot_storage_t *)psram_calloc(1u, sizeof(*g_snapshot_storage));
    if (g_snapshot_storage == NULL)
    {
        g_snapshot_storage =
            (diag_snapshot_storage_t *)pvPortMalloc(sizeof(*g_snapshot_storage));
        if (g_snapshot_storage != NULL)
        {
            memset(g_snapshot_storage, 0, sizeof(*g_snapshot_storage));
        }
    }
    return g_snapshot_storage != NULL;
}

static void diag_copy_heap_failure_state(diag_heap_failure_state_t *out_state)
{
    taskENTER_CRITICAL();
    *out_state = g_heap_failures;
    taskEXIT_CRITICAL();
}

static uint32_t diag_calculate_cpu_like_load(void)
{
    TaskStatus_t task_status[DIAG_SERVICE_MAX_SYSTEM_TASKS];
    configRUN_TIME_COUNTER_TYPE total_runtime = 0u;
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    uint64_t idle_runtime = 0u;

    if (task_count == 0u || task_count > DIAG_SERVICE_MAX_SYSTEM_TASKS)
    {
        return 0u;
    }

    memset(task_status, 0, sizeof(task_status));
    task_count = uxTaskGetSystemState(task_status, task_count, &total_runtime);
    if (task_count == 0u || total_runtime == 0u)
    {
        return 0u;
    }

    for (UBaseType_t index = 0u; index < task_count; ++index)
    {
        if (strcmp(task_status[index].pcTaskName, "IDLE") == 0)
        {
            idle_runtime = task_status[index].ulRunTimeCounter;
            break;
        }
    }

    if (idle_runtime >= (uint64_t)total_runtime)
    {
        return 0u;
    }
    return (uint32_t)(100u - ((idle_runtime * 100u) / (uint64_t)total_runtime));
}

void diag_service_trace_task_create(void *task_identity,
                                    const char *name,
                                    uint32_t task_number)
{
    char truncated_name[configMAX_TASK_NAME_LEN];

    if (task_identity == NULL || name == NULL)
    {
        return;
    }

    diag_copy_name(truncated_name, name);
    if (diag_find_ingress_by_identity(task_identity) >= 0)
    {
        return;
    }

    int slot = diag_find_ingress_by_name(truncated_name);
    if (slot >= 0)
    {
        diag_task_ingress_t *record = &g_task_ingress[slot];
        if (!record->present)
        {
            record->identity = task_identity;
            record->task_number = task_number;
            record->present = true;
            diag_increment_generation(record);
        }
        else
        {
            const int duplicate_slot = diag_find_unused_ingress();
            if (duplicate_slot < 0)
            {
                g_task_capacity_exceeded = true;
                return;
            }
            diag_task_ingress_t *duplicate = &g_task_ingress[duplicate_slot];
            memset(duplicate, 0, sizeof(*duplicate));
            diag_copy_name(duplicate->name, truncated_name);
            duplicate->identity = task_identity;
            duplicate->task_number = task_number;
            duplicate->generation = record->generation;
            duplicate->generation_wrap_count = record->generation_wrap_count;
            duplicate->present = true;
        }
        return;
    }

    slot = diag_find_unused_ingress();
    if (slot < 0)
    {
        g_task_capacity_exceeded = true;
        return;
    }

    diag_task_ingress_t *record = &g_task_ingress[slot];
    memset(record, 0, sizeof(*record));
    diag_copy_name(record->name, truncated_name);
    record->identity = task_identity;
    record->task_number = task_number;
    record->generation = 1u;
    record->present = true;
    record->canonical = true;
}

void diag_service_trace_task_delete(void *task_identity, const char *name)
{
    const int slot = diag_find_ingress_by_identity(task_identity);
    (void)name;
    if (slot < 0)
    {
        return;
    }

    diag_task_ingress_t *record = &g_task_ingress[slot];
    if (!record->canonical)
    {
        memset(record, 0, sizeof(*record));
        return;
    }

    const int promoted_duplicate =
        diag_find_live_duplicate(g_task_ingress, record->name, (size_t)slot);
    if (promoted_duplicate >= 0)
    {
        record->identity = g_task_ingress[promoted_duplicate].identity;
        record->task_number = g_task_ingress[promoted_duplicate].task_number;
        memset(&g_task_ingress[promoted_duplicate],
               0,
               sizeof(g_task_ingress[promoted_duplicate]));
        diag_increment_generation(record);
        return;
    }

    record->identity = NULL;
    record->task_number = 0u;
    record->present = false;
}

void diag_service_init(void)
{
    (void)diag_try_allocate_snapshot_storage();

    if (g_snapshot_lock == NULL)
    {
        g_snapshot_lock = xSemaphoreCreateMutexStatic(&g_snapshot_lock_storage);
    }
    if (g_snapshot_sample_lock == NULL)
    {
        g_snapshot_sample_lock =
            xSemaphoreCreateMutexStatic(&g_snapshot_sample_lock_storage);
    }

    taskENTER_CRITICAL();
    memset(g_legacy_tasks, 0, sizeof(g_legacy_tasks));
    g_legacy_task_count = 0u;
    g_last_fault_code = DIAG_FAULT_CODE_NONE;
    diag_copy_name(g_reset_cause,
                   watchdog_caused_reboot() ? "Watchdog" : "PowerOnOrExternal");
    taskEXIT_CRITICAL();
}

bool diag_service_register_task(const char *name, TaskHandle_t handle)
{
    if (name == NULL || handle == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();
    if (g_legacy_task_count >= DIAG_SERVICE_MAX_TASKS)
    {
        taskEXIT_CRITICAL();
        return false;
    }
    diag_copy_name(g_legacy_tasks[g_legacy_task_count].name, name);
    ++g_legacy_task_count;
    taskEXIT_CRITICAL();
    return true;
}

size_t diag_service_get_task_count(void)
{
    size_t count;
    taskENTER_CRITICAL();
    count = g_legacy_task_count;
    taskEXIT_CRITICAL();
    return count;
}

bool diag_service_get_snapshot(diag_service_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL || g_snapshot_storage == NULL || !diag_snapshot_lock())
    {
        return false;
    }

    const bool valid = g_snapshot_valid;
    if (valid)
    {
        *out_snapshot = g_snapshot_storage->snapshots[g_published_snapshot_index];
        const TickType_t age_ticks = xTaskGetTickCount() - g_snapshot_tick;
        out_snapshot->age_ms = ((uint64_t)age_ticks * 1000u) / configTICK_RATE_HZ;
    }
    (void)xSemaphoreGive(g_snapshot_lock);
    return valid;
}

bool diag_service_get_task_info(size_t index, diag_service_task_info_t *out_info)
{
    char registered_name[configMAX_TASK_NAME_LEN];

    if (out_info == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();
    if (index >= g_legacy_task_count)
    {
        taskEXIT_CRITICAL();
        return false;
    }
    diag_copy_name(registered_name, g_legacy_tasks[index].name);
    taskEXIT_CRITICAL();

    if (g_snapshot_storage == NULL || !diag_snapshot_lock())
    {
        return false;
    }

    bool found = false;
    const diag_service_snapshot_t *snapshot =
        &g_snapshot_storage->snapshots[g_published_snapshot_index];
    if (!g_snapshot_valid)
    {
        (void)xSemaphoreGive(g_snapshot_lock);
        return false;
    }

    for (uint32_t task_index = 0u;
         task_index < snapshot->retained_task_count;
         ++task_index)
    {
        const diag_service_snapshot_task_t *task = &snapshot->tasks[task_index];
        if (!task->present || strcmp(task->name, registered_name) != 0)
        {
            continue;
        }

        memset(out_info, 0, sizeof(*out_info));
        diag_copy_name(out_info->name, task->name);
        out_info->state = (eTaskState)task->state;
        out_info->task_number = task->task_number;
        out_info->runtime_counter = (uint32_t)task->runtime_counter_ms;
        out_info->current_priority = task->current_priority;
        out_info->base_priority = task->base_priority;
        out_info->stack_high_water_mark =
            (uint32_t)(task->stack_high_watermark_bytes / sizeof(StackType_t));
        found = true;
        break;
    }
    (void)xSemaphoreGive(g_snapshot_lock);
    return found;
}

bool diag_service_get_task_info_by_name(const char *name,
                                        diag_service_task_info_t *out_info)
{
    if (name == NULL || out_info == NULL)
    {
        return false;
    }

    const size_t count = diag_service_get_task_count();
    for (size_t index = 0u; index < count; ++index)
    {
        char registered_name[configMAX_TASK_NAME_LEN];
        taskENTER_CRITICAL();
        diag_copy_name(registered_name, g_legacy_tasks[index].name);
        taskEXIT_CRITICAL();
        if (strcmp(name, registered_name) == 0)
        {
            return diag_service_get_task_info(index, out_info);
        }
    }
    return false;
}

void diag_service_get_heap_stats(diag_service_heap_stats_t *out_stats)
{
    HeapStats_t heap_stats = {0};
    if (out_stats == NULL)
    {
        return;
    }

    vPortGetHeapStats(&heap_stats);
    out_stats->available_heap_bytes = heap_stats.xAvailableHeapSpaceInBytes;
    out_stats->largest_free_block_bytes = heap_stats.xSizeOfLargestFreeBlockInBytes;
    out_stats->smallest_free_block_bytes = heap_stats.xSizeOfSmallestFreeBlockInBytes;
    out_stats->free_block_count = heap_stats.xNumberOfFreeBlocks;
    out_stats->minimum_ever_free_bytes = heap_stats.xMinimumEverFreeBytesRemaining;
    out_stats->successful_allocations = heap_stats.xNumberOfSuccessfulAllocations;
    out_stats->successful_frees = heap_stats.xNumberOfSuccessfulFrees;
}

void diag_service_record_heap_failure(diag_service_alloc_failure_reason_t reason,
                                      uint64_t requested_bytes,
                                      uint64_t adjusted_bytes,
                                      uint64_t uptime_ms,
                                      bool timestamp_valid,
                                      uint64_t timestamp_unix_us)
{
    taskENTER_CRITICAL();
    diag_counter_add(&g_heap_failures.failed_allocations, 1u);
    if (reason == DIAG_SERVICE_ALLOC_FAILURE_INVALID_CONTEXT)
    {
        diag_counter_add(&g_heap_failures.invalid_context_failures, 1u);
    }
    g_heap_failures.last_failed_requested_bytes = requested_bytes;
    g_heap_failures.last_failed_adjusted_bytes = adjusted_bytes;
    g_heap_failures.last_failure_reason = reason;
    g_heap_failures.last_failure_uptime_ms = uptime_ms;
    g_heap_failures.last_failure_timestamp_valid = timestamp_valid;
    g_heap_failures.last_failure_timestamp_unix_us =
        timestamp_valid ? timestamp_unix_us : 0u;
    taskEXIT_CRITICAL();
}

void diag_service_record_heap_failure_from_isr(
    diag_service_alloc_failure_reason_t reason,
    uint64_t requested_bytes,
    uint64_t adjusted_bytes,
    uint64_t uptime_ms)
{
    const UBaseType_t interrupt_mask = taskENTER_CRITICAL_FROM_ISR();
    diag_counter_add(&g_heap_failures.failed_allocations, 1u);
    if (reason == DIAG_SERVICE_ALLOC_FAILURE_INVALID_CONTEXT)
    {
        diag_counter_add(&g_heap_failures.invalid_context_failures, 1u);
    }
    g_heap_failures.last_failed_requested_bytes = requested_bytes;
    g_heap_failures.last_failed_adjusted_bytes = adjusted_bytes;
    g_heap_failures.last_failure_reason = reason;
    g_heap_failures.last_failure_uptime_ms = uptime_ms;
    g_heap_failures.last_failure_timestamp_valid = false;
    g_heap_failures.last_failure_timestamp_unix_us = 0u;
    taskEXIT_CRITICAL_FROM_ISR(interrupt_mask);
}

bool diag_service_snapshot_is_available(void)
{
    if (g_snapshot_storage == NULL || !diag_snapshot_lock())
    {
        return false;
    }
    const bool available = g_snapshot_valid;
    (void)xSemaphoreGive(g_snapshot_lock);
    return available;
}

static void diag_sample_heap(diag_service_snapshot_t *snapshot)
{
    HeapStats_t stats = {0};
    diag_heap_failure_state_t failures;

    vPortGetHeapStats(&stats);
    diag_copy_heap_failure_state(&failures);

    snapshot->heap.total_bytes = configTOTAL_HEAP_SIZE;
    snapshot->heap.free_bytes = stats.xAvailableHeapSpaceInBytes;
    snapshot->heap.used_bytes = snapshot->heap.total_bytes - snapshot->heap.free_bytes;
    if (snapshot->heap.used_bytes > snapshot->heap.peak_used_bytes)
    {
        snapshot->heap.peak_used_bytes = snapshot->heap.used_bytes;
    }
    snapshot->heap.minimum_ever_free_bytes = stats.xMinimumEverFreeBytesRemaining;
    snapshot->heap.largest_free_block_bytes = stats.xSizeOfLargestFreeBlockInBytes;
    snapshot->heap.smallest_free_block_bytes = stats.xSizeOfSmallestFreeBlockInBytes;
    snapshot->heap.free_block_count = stats.xNumberOfFreeBlocks;

    diag_counter_add(&snapshot->heap.successful_allocations,
                     (uint32_t)stats.xNumberOfSuccessfulAllocations -
                         g_heap_allocations_raw);
    diag_counter_add(&snapshot->heap.successful_frees,
                     (uint32_t)stats.xNumberOfSuccessfulFrees - g_heap_frees_raw);
    g_heap_allocations_raw = (uint32_t)stats.xNumberOfSuccessfulAllocations;
    g_heap_frees_raw = (uint32_t)stats.xNumberOfSuccessfulFrees;

    snapshot->heap.failed_allocations = failures.failed_allocations;
    snapshot->heap.invalid_context_failures = failures.invalid_context_failures;
    snapshot->heap.last_failed_requested_bytes = failures.last_failed_requested_bytes;
    snapshot->heap.last_failed_adjusted_bytes = failures.last_failed_adjusted_bytes;
    snapshot->heap.last_failure_reason = failures.last_failure_reason;
    snapshot->heap.last_failure_uptime_ms = failures.last_failure_uptime_ms;
    snapshot->heap.last_failure_timestamp_valid = failures.last_failure_timestamp_valid;
    snapshot->heap.last_failure_timestamp_unix_us = failures.last_failure_timestamp_unix_us;
    snapshot->heap.quality = DIAG_SERVICE_QUALITY_GOOD;
}

static void diag_sample_psram(diag_service_snapshot_t *snapshot)
{
    psram_allocator_stats_t stats = {0};
    const bool refreshed = psram_allocator_get_stats(&stats);

    if (!refreshed && snapshot->psram.quality == DIAG_SERVICE_QUALITY_GOOD)
    {
        snapshot->psram.quality = DIAG_SERVICE_QUALITY_UNCERTAIN_LAST_USABLE;
        return;
    }

    snapshot->psram.compiled = stats.compiled;
    snapshot->psram.ready = stats.ready;
    snapshot->psram.total_bytes = stats.total_bytes;
    snapshot->psram.free_bytes = stats.free_bytes;
    snapshot->psram.used_bytes = stats.used_bytes;
    snapshot->psram.peak_used_bytes = stats.peak_used_bytes;
    snapshot->psram.largest_free_block_bytes = stats.largest_free_block_bytes;
    snapshot->psram.requested_bytes_in_use = stats.requested_bytes_in_use;
    snapshot->psram.peak_requested_bytes_in_use = stats.peak_requested_bytes_in_use;
    snapshot->psram.successful_allocations.value = stats.successful_allocations;
    snapshot->psram.successful_allocations.wrap_count =
        stats.successful_allocations_wrap_count;
    snapshot->psram.successful_frees.value = stats.successful_frees;
    snapshot->psram.successful_frees.wrap_count = stats.successful_frees_wrap_count;
    snapshot->psram.failed_allocations.value = stats.failed_allocations;
    snapshot->psram.failed_allocations.wrap_count = stats.failed_allocations_wrap_count;
    snapshot->psram.lock_timeouts.value = stats.lock_timeouts;
    snapshot->psram.lock_timeouts.wrap_count = stats.lock_timeouts_wrap_count;
    snapshot->psram.invalid_context_failures.value = stats.invalid_context_failures;
    snapshot->psram.invalid_context_failures.wrap_count =
        stats.invalid_context_failures_wrap_count;
    snapshot->psram.last_failed_requested_bytes = stats.last_failed_requested_bytes;
    snapshot->psram.last_failure_reason =
        (diag_service_alloc_failure_reason_t)stats.last_failure_reason;
    snapshot->psram.last_failure_uptime_ms = stats.last_failure_uptime_ms;
    snapshot->psram.last_failure_timestamp_valid = stats.last_failure_timestamp_valid;
    snapshot->psram.last_failure_timestamp_unix_us = stats.last_failure_timestamp_unix_us;
    snapshot->psram.quality = refreshed
                                  ? DIAG_SERVICE_QUALITY_GOOD
                                  : (stats.compiled
                                         ? DIAG_SERVICE_QUALITY_BAD_NOT_CONNECTED
                                         : DIAG_SERVICE_QUALITY_BAD_NOT_SUPPORTED);
}

static void diag_sample_lwip(diag_service_snapshot_t *snapshot)
{
    lwip_psram_heap_stats_t stats = {0};
    const bool refreshed = lwip_psram_heap_get_stats(&stats);

    if (!refreshed && snapshot->lwip.quality == DIAG_SERVICE_QUALITY_GOOD)
    {
        snapshot->lwip.quality = DIAG_SERVICE_QUALITY_UNCERTAIN_LAST_USABLE;
        return;
    }

    snapshot->lwip.reserved_bytes = stats.reserved_bytes;
    snapshot->lwip.used_bytes = stats.used_bytes;
    snapshot->lwip.peak_used_bytes = stats.peak_used_bytes;
    snapshot->lwip.largest_free_block_bytes = stats.largest_free_block_bytes;
    snapshot->lwip.failed_allocations.value = stats.failed_allocations;
    snapshot->lwip.failed_allocations.wrap_count = stats.failed_allocations_wrap_count;
    snapshot->lwip.quality = refreshed
                                ? DIAG_SERVICE_QUALITY_GOOD
                                : (stats.supported
                                       ? DIAG_SERVICE_QUALITY_BAD_NOT_CONNECTED
                                       : DIAG_SERVICE_QUALITY_BAD_NOT_SUPPORTED);
}

static int diag_find_status(const diag_task_ingress_t *record,
                            UBaseType_t status_count)
{
    for (UBaseType_t index = 0u; index < status_count; ++index)
    {
        if ((void *)g_sampling_scratch.task_status_sample[index].xHandle ==
                record->identity &&
            g_sampling_scratch.task_status_sample[index].xTaskNumber ==
                record->task_number)
        {
            return (int)index;
        }
    }
    return -1;
}

static void diag_mark_task_values_unavailable(diag_service_snapshot_task_t *task)
{
    task->task_number = 0u;
    task->current_priority = 0u;
    task->base_priority = 0u;
    task->runtime_counter_quality = DIAG_SERVICE_QUALITY_BAD_NO_DATA;
    task->runtime_percent = 0.0;
    task->runtime_percent_quality = DIAG_SERVICE_QUALITY_BAD_NO_DATA;
    task->stack_high_watermark_bytes = 0u;
}

static diag_service_snapshot_task_t *diag_find_snapshot_task(
    diag_service_snapshot_t *snapshot,
    const char *name)
{
    for (uint32_t index = 0u; index < snapshot->retained_task_count; ++index)
    {
        if (strcmp(snapshot->tasks[index].name, name) == 0)
        {
            return &snapshot->tasks[index];
        }
    }
    return NULL;
}

static void diag_sample_tasks(diag_service_snapshot_t *snapshot,
                              uint64_t monotonic_ms)
{
    configRUN_TIME_COUNTER_TYPE total_runtime = 0u;
    UBaseType_t sampled_count = 0u;
    bool capacity_exceeded = false;

    snapshot->task_count = uxTaskGetNumberOfTasks();
    taskENTER_CRITICAL();
    memcpy(g_sampling_scratch.ingress_sample,
           g_task_ingress,
           sizeof(g_sampling_scratch.ingress_sample));
    capacity_exceeded = g_task_capacity_exceeded;
    taskEXIT_CRITICAL();

    bool complete = snapshot->task_count <= DIAG_SERVICE_MAX_SNAPSHOT_TASKS;
    if (complete)
    {
        memset(g_sampling_scratch.task_status_sample,
               0,
               sizeof(g_sampling_scratch.task_status_sample));
        sampled_count = uxTaskGetSystemState(g_sampling_scratch.task_status_sample,
                                             DIAG_SERVICE_MAX_SNAPSHOT_TASKS,
                                             &total_runtime);
        complete = sampled_count == snapshot->task_count;
    }

    snapshot->task_list_truncated = !complete || capacity_exceeded;
    snapshot->task_name_conflict =
        diag_ingress_has_conflict(g_sampling_scratch.ingress_sample);
    snapshot->task_model_incomplete = snapshot->task_list_truncated ||
                                      snapshot->task_name_conflict;
    const bool percent_sample_complete =
        complete && !snapshot->task_model_incomplete;
    const bool percent_pair_valid =
        percent_sample_complete && g_previous_percent_sample_complete;

    bool total_delta_valid = false;
    uint32_t total_delta = 0u;
    if (complete)
    {
        const uint32_t total_raw = (uint32_t)total_runtime;
        if (g_total_runtime_initialized &&
            monotonic_ms >= g_total_runtime_sample_uptime_ms &&
            monotonic_ms - g_total_runtime_sample_uptime_ms <
                DIAG_SERVICE_NATIVE_RUNTIME_MODULUS_MS)
        {
            total_delta = total_raw - g_total_runtime_raw;
            total_delta_valid = total_delta != 0u;
        }
        g_total_runtime_initialized = true;
        g_total_runtime_raw = total_raw;
        g_total_runtime_sample_uptime_ms = monotonic_ms;
    }

    for (size_t slot = 0u; slot < DIAG_SERVICE_MAX_SNAPSHOT_TASKS; ++slot)
    {
        const diag_task_ingress_t *ingress =
            &g_sampling_scratch.ingress_sample[slot];
        if (!ingress->canonical)
        {
            continue;
        }

        diag_service_snapshot_task_t *task =
            diag_find_snapshot_task(snapshot, ingress->name);
        const bool had_previous = task != NULL;
        if (!had_previous)
        {
            if (snapshot->retained_task_count >= DIAG_SERVICE_MAX_SNAPSHOT_TASKS)
            {
                snapshot->task_list_truncated = true;
                snapshot->task_model_incomplete = true;
                continue;
            }
            task = &snapshot->tasks[snapshot->retained_task_count++];
            memset(task, 0, sizeof(*task));
        }

        diag_copy_name(task->name, ingress->name);
        task->present = ingress->present;
        task->generation = ingress->generation;
        task->generation_wrap_count = ingress->generation_wrap_count;

        if (!ingress->present)
        {
            task->state = DIAG_SERVICE_TASK_STATE_DELETED;
            task->quality = DIAG_SERVICE_QUALITY_BAD_NO_DATA;
            diag_mark_task_values_unavailable(task);
            memset(&g_task_runtime[slot], 0, sizeof(g_task_runtime[slot]));
            continue;
        }

        if (!complete)
        {
            task->quality = had_previous
                                ? DIAG_SERVICE_QUALITY_UNCERTAIN_LAST_USABLE
                                : DIAG_SERVICE_QUALITY_BAD_NO_DATA;
            task->runtime_counter_quality = task->quality;
            task->runtime_percent_quality = DIAG_SERVICE_QUALITY_BAD_NO_DATA;
            continue;
        }

        const int status_index = diag_find_status(ingress, sampled_count);
        if (status_index < 0 ||
            diag_find_live_duplicate(g_sampling_scratch.ingress_sample,
                                     ingress->name,
                                     slot) >= 0)
        {
            task->quality = had_previous
                                ? DIAG_SERVICE_QUALITY_UNCERTAIN_LAST_USABLE
                                : DIAG_SERVICE_QUALITY_BAD_NO_DATA;
            task->runtime_counter_quality = task->quality;
            task->runtime_percent_quality = DIAG_SERVICE_QUALITY_BAD_NO_DATA;
            continue;
        }

        const TaskStatus_t *status =
            &g_sampling_scratch.task_status_sample[status_index];
        diag_task_runtime_state_t *runtime = &g_task_runtime[slot];
        const uint32_t raw_runtime = (uint32_t)status->ulRunTimeCounter;
        bool runtime_delta_valid = false;
        bool runtime_gap_unprovable = false;
        uint32_t runtime_delta = 0u;

        if (!runtime->initialized || runtime->generation != ingress->generation)
        {
            memset(runtime, 0, sizeof(*runtime));
            runtime->initialized = true;
            runtime->generation = ingress->generation;
            runtime->extended_runtime_ms.value = raw_runtime;
        }
        else if (monotonic_ms >= runtime->last_sample_uptime_ms &&
                 monotonic_ms - runtime->last_sample_uptime_ms <
                     DIAG_SERVICE_NATIVE_RUNTIME_MODULUS_MS)
        {
            runtime_delta = raw_runtime - runtime->last_raw_runtime_ms;
            diag_counter_add(&runtime->extended_runtime_ms, runtime_delta);
            runtime_delta_valid = true;
        }
        else
        {
            runtime_gap_unprovable = true;
        }

        runtime->last_raw_runtime_ms = raw_runtime;
        runtime->last_sample_uptime_ms = monotonic_ms;

        task->task_number = status->xTaskNumber;
        task->state = diag_map_task_state(status->eCurrentState);
        task->current_priority = status->uxCurrentPriority;
        task->base_priority = status->uxBasePriority;
        task->runtime_counter_ms = runtime->extended_runtime_ms.value;
        task->runtime_counter_wrap_count = runtime->extended_runtime_ms.wrap_count;
        task->runtime_counter_quality = runtime_gap_unprovable
                                            ? DIAG_SERVICE_QUALITY_UNCERTAIN_LAST_USABLE
                                            : DIAG_SERVICE_QUALITY_GOOD;
        task->stack_high_watermark_bytes =
            (uint64_t)status->usStackHighWaterMark * sizeof(StackType_t);
        task->quality = DIAG_SERVICE_QUALITY_GOOD;

        if (runtime_delta_valid && total_delta_valid && percent_pair_valid)
        {
            task->runtime_percent =
                (100.0 * (double)runtime_delta) / (double)total_delta;
            task->runtime_percent_quality = DIAG_SERVICE_QUALITY_GOOD;
        }
        else
        {
            task->runtime_percent = 0.0;
            task->runtime_percent_quality = DIAG_SERVICE_QUALITY_BAD_NO_DATA;
        }
    }
    g_previous_percent_sample_complete = percent_sample_complete;
}

bool diag_service_sample(uint64_t monotonic_ms,
                         bool timestamp_valid,
                         uint64_t timestamp_unix_us)
{
#if RP2_BENCHMARK_PROFILE_ENABLED
    const uint64_t benchmark_started_monotonic_us = benchmark_timing_diag_sample_begin();
    uint64_t benchmark_phase_durations_us[BENCHMARK_TIMING_DIAG_PHASE_COUNT] = {0};
    uint64_t benchmark_phase_started_monotonic_us = benchmark_started_monotonic_us;
#endif
    if (!diag_try_allocate_snapshot_storage() || !diag_snapshot_sample_lock())
    {
#if RP2_BENCHMARK_PROFILE_ENABLED
        benchmark_phase_durations_us[BENCHMARK_TIMING_DIAG_PHASE_ACQUIRE] =
            benchmark_timing_diag_phase_now_us() - benchmark_phase_started_monotonic_us;
        benchmark_timing_diag_sample_set_phase_durations(benchmark_phase_durations_us);
        benchmark_timing_diag_sample_end(
            benchmark_started_monotonic_us, false);
        benchmark_timing_diag_sample_set_opcua_task_stack_high_water(
            (uint64_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
#endif
        return false;
    }

    if (!diag_snapshot_lock())
    {
#if RP2_BENCHMARK_PROFILE_ENABLED
        benchmark_phase_durations_us[BENCHMARK_TIMING_DIAG_PHASE_ACQUIRE] =
            benchmark_timing_diag_phase_now_us() - benchmark_phase_started_monotonic_us;
        benchmark_timing_diag_sample_set_phase_durations(benchmark_phase_durations_us);
        benchmark_timing_diag_sample_end(
            benchmark_started_monotonic_us, false);
        benchmark_timing_diag_sample_set_opcua_task_stack_high_water(
            (uint64_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
#endif
        (void)xSemaphoreGive(g_snapshot_sample_lock);
        return false;
    }

    const uint8_t next_index = g_published_snapshot_index ^ 1u;
    diag_service_snapshot_t *snapshot =
        &g_snapshot_storage->snapshots[next_index];
    const bool previous_snapshot_valid = g_snapshot_valid;
    const uint8_t previous_index = g_published_snapshot_index;
    (void)xSemaphoreGive(g_snapshot_lock);
#if RP2_BENCHMARK_PROFILE_ENABLED
    benchmark_phase_durations_us[BENCHMARK_TIMING_DIAG_PHASE_ACQUIRE] =
        benchmark_timing_diag_phase_now_us() - benchmark_phase_started_monotonic_us;
    benchmark_phase_started_monotonic_us = benchmark_timing_diag_phase_now_us();
#endif
    if (previous_snapshot_valid)
    {
        *snapshot = g_snapshot_storage->snapshots[previous_index];
    }
    else
    {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->psram.quality = DIAG_SERVICE_QUALITY_BAD_NO_DATA;
        snapshot->lwip.quality = DIAG_SERVICE_QUALITY_BAD_NO_DATA;
    }
#if RP2_BENCHMARK_PROFILE_ENABLED
    benchmark_phase_durations_us[BENCHMARK_TIMING_DIAG_PHASE_COPY] =
        benchmark_timing_diag_phase_now_us() - benchmark_phase_started_monotonic_us;
    benchmark_phase_started_monotonic_us = benchmark_timing_diag_phase_now_us();
#endif

    snapshot->uptime_ms = monotonic_ms;
    snapshot->age_ms = 0u;
    snapshot->timestamp_valid = timestamp_valid;
    snapshot->timestamp_unix_us = timestamp_valid ? timestamp_unix_us : 0u;
    snapshot->quality = DIAG_SERVICE_QUALITY_GOOD;

    diag_sample_heap(snapshot);
#if RP2_BENCHMARK_PROFILE_ENABLED
    benchmark_phase_durations_us[BENCHMARK_TIMING_DIAG_PHASE_HEAP] =
        benchmark_timing_diag_phase_now_us() - benchmark_phase_started_monotonic_us;
    benchmark_phase_started_monotonic_us = benchmark_timing_diag_phase_now_us();
#endif
    diag_sample_psram(snapshot);
#if RP2_BENCHMARK_PROFILE_ENABLED
    benchmark_phase_durations_us[BENCHMARK_TIMING_DIAG_PHASE_PSRAM] =
        benchmark_timing_diag_phase_now_us() - benchmark_phase_started_monotonic_us;
    benchmark_phase_started_monotonic_us = benchmark_timing_diag_phase_now_us();
#endif
    diag_sample_lwip(snapshot);
#if RP2_BENCHMARK_PROFILE_ENABLED
    benchmark_phase_durations_us[BENCHMARK_TIMING_DIAG_PHASE_LWIP] =
        benchmark_timing_diag_phase_now_us() - benchmark_phase_started_monotonic_us;
    benchmark_phase_started_monotonic_us = benchmark_timing_diag_phase_now_us();
#endif
    diag_sample_tasks(snapshot, monotonic_ms);
#if RP2_BENCHMARK_PROFILE_ENABLED
    benchmark_phase_durations_us[BENCHMARK_TIMING_DIAG_PHASE_TASKS] =
        benchmark_timing_diag_phase_now_us() - benchmark_phase_started_monotonic_us;
    benchmark_phase_started_monotonic_us = benchmark_timing_diag_phase_now_us();
#endif

    ++snapshot->sequence;
    if (snapshot->sequence == 0u && snapshot->sequence_wrap_count != UINT64_MAX)
    {
        ++snapshot->sequence_wrap_count;
    }

    if (!diag_snapshot_lock())
    {
#if RP2_BENCHMARK_PROFILE_ENABLED
        benchmark_phase_durations_us[BENCHMARK_TIMING_DIAG_PHASE_PUBLISH] =
            benchmark_timing_diag_phase_now_us() - benchmark_phase_started_monotonic_us;
        benchmark_timing_diag_sample_set_phase_durations(benchmark_phase_durations_us);
        benchmark_timing_diag_sample_end(
            benchmark_started_monotonic_us, false);
        benchmark_timing_diag_sample_set_opcua_task_stack_high_water(
            (uint64_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
#endif
        (void)xSemaphoreGive(g_snapshot_sample_lock);
        return false;
    }
    g_published_snapshot_index = next_index;
    g_snapshot_tick = xTaskGetTickCount();
    g_snapshot_valid = true;
    (void)xSemaphoreGive(g_snapshot_lock);
    (void)xSemaphoreGive(g_snapshot_sample_lock);
#if RP2_BENCHMARK_PROFILE_ENABLED
    benchmark_phase_durations_us[BENCHMARK_TIMING_DIAG_PHASE_PUBLISH] =
        benchmark_timing_diag_phase_now_us() - benchmark_phase_started_monotonic_us;
    benchmark_timing_diag_sample_set_phase_durations(benchmark_phase_durations_us);
    benchmark_timing_diag_sample_end(
        benchmark_started_monotonic_us, true);
    benchmark_timing_diag_sample_set_opcua_task_stack_high_water(
        (uint64_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
#endif
    return true;
}

void diag_service_get_system_stats(diag_service_system_stats_t *out_stats)
{
    if (out_stats == NULL)
    {
        return;
    }

    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->uptime_sec = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
    out_stats->cpu_like_load_approx = diag_calculate_cpu_like_load();

    taskENTER_CRITICAL();
    out_stats->last_fault_code = g_last_fault_code;
    strncpy(out_stats->reset_cause, g_reset_cause, sizeof(out_stats->reset_cause));
    out_stats->reset_cause[sizeof(out_stats->reset_cause) - 1u] = '\0';
    taskEXIT_CRITICAL();
}

void diag_service_set_last_fault_code(diag_fault_code_t fault_code)
{
    taskENTER_CRITICAL();
    g_last_fault_code = (uint32_t)fault_code;
    taskEXIT_CRITICAL();
}

void diag_service_clear(void)
{
    diag_service_set_last_fault_code(DIAG_FAULT_CODE_NONE);
}

void diag_service_reboot(uint32_t delay_ms)
{
    watchdog_reboot(0, 0, delay_ms);
}
