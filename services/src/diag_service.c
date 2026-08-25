/**
 * @file
 * @brief Task registry, heap/system diagnostics, reset-cause reporting, and
 *        watchdog-driven reboot.
 *
 * Responsibilities:
 * - Owns a bounded (DIAG_SERVICE_MAX_TASKS) name->TaskHandle_t registry
 *   populated by each task at startup (diag_service_register_task()).
 * - Reports FreeRTOS heap and per-task diagnostics on demand.
 * - Records the boot reset cause and any runtime fault code.
 * - Drives a hardware watchdog reboot (diag_service_reboot()).
 *
 * Ownership: g_registered_tasks/g_registered_task_count, g_reset_cause,
 * g_last_fault_code are exclusively owned by this file.
 *
 * Concurrency: mutating/reading g_registered_task_count,
 * g_reset_cause, g_last_fault_code go through
 * taskENTER_CRITICAL()/taskEXIT_CRITICAL(). This is one of the documented
 * exceptions to the services-layer no-direct-hardware rule: it calls
 * `hardware/watchdog.h`'s watchdog_caused_reboot()/watchdog_reboot()
 * directly (see docs/code/architecture.md#confirmed-layering-observations).
 */

#include "services/diag_service.h"

#include <string.h>

#include "hardware/watchdog.h"

/* Bounded task registry, reset-cause string, and last fault code; see the
 * file-level @c Ownership/@c Concurrency notes above for lifetime and
 * synchronization. */
static struct
{
    TaskHandle_t handle;
    char name[configMAX_TASK_NAME_LEN];
} g_registered_tasks[DIAG_SERVICE_MAX_TASKS];
static size_t g_registered_task_count = 0;
static char g_reset_cause[DIAG_SERVICE_RESET_CAUSE_MAX_LEN];
static uint32_t g_last_fault_code = DIAG_FAULT_CODE_NONE;

/**
 * @brief Copy one registered task's handle and name out by index.
 * @pre Caller must hold the diag_service critical section (all call sites
 *      do) -- this helper does not lock itself.
 * @param index Registration-order index.
 * @param out_handle Receives the task handle.
 * @param out_name Destination buffer for the name.
 * @param out_name_size Size of @p out_name.
 * @return true if @p index was in range and the copied handle is non-NULL;
 *         false otherwise.
 */
static bool diag_service_copy_registered_task(size_t index, TaskHandle_t *out_handle, char *out_name, size_t out_name_size)
{
    if (out_handle == NULL || out_name == NULL || out_name_size == 0u)
    {
        return false;
    }

    if (index >= g_registered_task_count)
    {
        return false;
    }

    *out_handle = g_registered_tasks[index].handle;
    strncpy(out_name, g_registered_tasks[index].name, out_name_size);
    out_name[out_name_size - 1] = '\0';
    return *out_handle != NULL;
}

/**
 * @brief Approximate CPU load as 100 minus the IDLE task's share of total
 *        FreeRTOS runtime-counter ticks.
 *
 * Samples up to DIAG_SERVICE_MAX_SYSTEM_TASKS tasks via
 * uxTaskGetSystemState() -- if more tasks exist than that, the excess are
 * silently excluded from the sample (this is an approximation, not an exact
 * measurement, as the name implies).
 *
 * @return Approximate load percentage in [0, 100]; 0 if no tasks, no
 *         runtime accounted, or the IDLE task could not be found.
 */
static uint32_t diag_service_calculate_cpu_like_load(void)
{
    TaskStatus_t task_status_array[DIAG_SERVICE_MAX_SYSTEM_TASKS];
    configRUN_TIME_COUNTER_TYPE total_runtime = 0;
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    uint64_t idle_runtime = 0;

    memset(task_status_array, 0, sizeof(task_status_array));

    if (task_count == 0u)
    {
        return 0u;
    }

    if (task_count > DIAG_SERVICE_MAX_SYSTEM_TASKS)
    {
        task_count = DIAG_SERVICE_MAX_SYSTEM_TASKS;
    }

    task_count = uxTaskGetSystemState(task_status_array, task_count, &total_runtime);
    if (task_count == 0u || total_runtime == 0u)
    {
        return 0u;
    }

    for (UBaseType_t index = 0; index < task_count; ++index)
    {
        if (strcmp(task_status_array[index].pcTaskName, "IDLE") == 0)
        {
            idle_runtime = task_status_array[index].ulRunTimeCounter;
            break;
        }
    }

    if (idle_runtime >= (uint64_t)total_runtime)
    {
        return 0u;
    }

    return (uint32_t)(100u - ((idle_runtime * 100u) / (uint64_t)total_runtime));
}

void diag_service_init(void)
{
    memset(g_registered_tasks, 0, sizeof(g_registered_tasks));
    g_registered_task_count = 0;
    strncpy(g_reset_cause,
            watchdog_caused_reboot() ? "Watchdog" : "PowerOnOrExternal",
            sizeof(g_reset_cause));
    g_reset_cause[sizeof(g_reset_cause) - 1] = '\0';
    g_last_fault_code = DIAG_FAULT_CODE_NONE;
}

bool diag_service_register_task(const char *name, TaskHandle_t handle)
{
    if (name == NULL || handle == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();

    if (g_registered_task_count >= DIAG_SERVICE_MAX_TASKS)
    {
        taskEXIT_CRITICAL();
        return false;
    }

    strncpy(g_registered_tasks[g_registered_task_count].name, name, configMAX_TASK_NAME_LEN);
    g_registered_tasks[g_registered_task_count].name[configMAX_TASK_NAME_LEN - 1] = '\0';
    g_registered_tasks[g_registered_task_count].handle = handle;
    g_registered_task_count++;

    taskEXIT_CRITICAL();
    return true;
}

size_t diag_service_get_task_count(void)
{
    size_t count = 0;

    taskENTER_CRITICAL();
    count = g_registered_task_count;
    taskEXIT_CRITICAL();

    return count;
}

bool diag_service_get_task_info(size_t index, diag_service_task_info_t *out_info)
{
    TaskHandle_t handle = NULL;
    char name[configMAX_TASK_NAME_LEN] = {0};
    TaskStatus_t task_status = {0};

    if (out_info == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();
    (void)diag_service_copy_registered_task(index, &handle, name, sizeof(name));
    taskEXIT_CRITICAL();

    if (handle == NULL)
    {
        return false;
    }

    vTaskGetInfo(handle, &task_status, pdTRUE, eInvalid);

    memset(out_info, 0, sizeof(*out_info));
    strncpy(out_info->name, name, sizeof(out_info->name));
    out_info->name[sizeof(out_info->name) - 1] = '\0';
    out_info->state = task_status.eCurrentState;
    out_info->task_number = task_status.xTaskNumber;
    out_info->runtime_counter = task_status.ulRunTimeCounter;
    out_info->current_priority = task_status.uxCurrentPriority;
    out_info->base_priority = task_status.uxBasePriority;
    out_info->stack_high_water_mark = task_status.usStackHighWaterMark;
    return true;
}

bool diag_service_get_task_info_by_name(const char *name, diag_service_task_info_t *out_info)
{
    if (name == NULL || out_info == NULL)
    {
        return false;
    }

    for (size_t index = 0; index < diag_service_get_task_count(); ++index)
    {
        diag_service_task_info_t info = {0};

        if (!diag_service_get_task_info(index, &info))
        {
            continue;
        }

        if (strcmp(info.name, name) == 0)
        {
            *out_info = info;
            return true;
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

void diag_service_get_system_stats(diag_service_system_stats_t *out_stats)
{
    if (out_stats == NULL)
    {
        return;
    }

    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->uptime_sec = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
    out_stats->cpu_like_load_approx = diag_service_calculate_cpu_like_load();

    taskENTER_CRITICAL();
    out_stats->last_fault_code = g_last_fault_code;
    strncpy(out_stats->reset_cause, g_reset_cause, sizeof(out_stats->reset_cause));
    out_stats->reset_cause[sizeof(out_stats->reset_cause) - 1] = '\0';
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
