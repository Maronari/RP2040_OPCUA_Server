#include "services/diag_service.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip_psram_heap.h"
#include "psram_allocator.h"
#include "semphr.h"

static TickType_t g_tick;
static TaskStatus_t g_status[20];
static UBaseType_t g_status_count;
static configRUN_TIME_COUNTER_TYPE g_total_runtime;
static HeapStats_t g_heap_stats;
static bool g_fail_psram_allocation;
static bool g_fail_heap_allocation;
static unsigned g_task_critical_depth;
static unsigned g_isr_critical_depth;
static bool g_interleave_task_replacement;
static void *g_interleave_old_identity;
static void *g_interleave_new_identity;
static const char *g_interleave_name;

void diag_test_enter_critical(void)
{
    ++g_task_critical_depth;
}

void diag_test_exit_critical(void)
{
    assert(g_task_critical_depth > 0u);
    --g_task_critical_depth;
}

UBaseType_t diag_test_enter_critical_from_isr(void)
{
    ++g_isr_critical_depth;
    return 0x5au;
}

void diag_test_exit_critical_from_isr(UBaseType_t mask)
{
    assert(mask == 0x5au);
    assert(g_isr_critical_depth > 0u);
    --g_isr_critical_depth;
}

UBaseType_t uxTaskGetNumberOfTasks(void)
{
    return g_status_count;
}

UBaseType_t uxTaskGetSystemState(TaskStatus_t *status,
                                UBaseType_t capacity,
                                configRUN_TIME_COUNTER_TYPE *total_runtime)
{
    if (capacity < g_status_count)
    {
        return 0u;
    }
    if (g_interleave_task_replacement)
    {
        g_interleave_task_replacement = false;
        diag_service_trace_task_delete(g_interleave_old_identity, g_interleave_name);
        diag_service_trace_task_create(g_interleave_new_identity, g_interleave_name, 91u);
    }
    memcpy(status, g_status, g_status_count * sizeof(g_status[0]));
    *total_runtime = g_total_runtime;
    return g_status_count;
}

TickType_t xTaskGetTickCount(void)
{
    return g_tick;
}

TickType_t xTaskGetTickCountFromISR(void)
{
    return g_tick;
}

BaseType_t xTaskGetSchedulerState(void)
{
    return taskSCHEDULER_RUNNING;
}

void vPortGetHeapStats(HeapStats_t *stats)
{
    *stats = g_heap_stats;
}

void *pvPortMalloc(size_t size)
{
    return g_fail_heap_allocation ? NULL : calloc(1u, size);
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    storage->marker = 1u;
    return storage;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t timeout)
{
    (void)timeout;
    return handle != NULL ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    return handle != NULL ? pdTRUE : pdFALSE;
}

bool watchdog_caused_reboot(void)
{
    return false;
}

void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms)
{
    (void)pc;
    (void)sp;
    (void)delay_ms;
}

void *psram_calloc(size_t count, size_t size)
{
    return g_fail_psram_allocation ? NULL : calloc(count, size);
}

bool psram_allocator_get_stats(psram_allocator_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));
    stats->compiled = true;
    stats->ready = true;
    stats->total_bytes = 8192u;
    stats->free_bytes = 6144u;
    stats->used_bytes = 2048u;
    stats->peak_used_bytes = 3072u;
    stats->largest_free_block_bytes = 4096u;
    return true;
}

bool lwip_psram_heap_get_stats(lwip_psram_heap_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));
    stats->supported = false;
    return false;
}

static void set_status(size_t index,
                       void *identity,
                       const char *name,
                       uint32_t task_number,
                       uint32_t runtime)
{
    g_status[index].xHandle = identity;
    g_status[index].pcTaskName = name;
    g_status[index].xTaskNumber = task_number;
    g_status[index].eCurrentState = eReady;
    g_status[index].uxCurrentPriority = 3u;
    g_status[index].uxBasePriority = 2u;
    g_status[index].ulRunTimeCounter = runtime;
    g_status[index].usStackHighWaterMark = 32u;
}

static const diag_service_snapshot_task_t *find_task(
    const diag_service_snapshot_t *snapshot,
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

int main(void)
{
    diag_service_snapshot_t snapshot;
    static int task_a;
    static int task_b;
    static int task_c;
    static int interleave_old;
    static int interleave_new;

    g_heap_stats.xAvailableHeapSpaceInBytes = 3072u;
    g_heap_stats.xMinimumEverFreeBytesRemaining = 2048u;
    g_heap_stats.xSizeOfLargestFreeBlockInBytes = 1024u;
    g_heap_stats.xSizeOfSmallestFreeBlockInBytes = 32u;
    g_heap_stats.xNumberOfFreeBlocks = 4u;

    g_fail_psram_allocation = true;
    g_fail_heap_allocation = true;
    diag_service_init();
    assert(!diag_service_snapshot_is_available());
    assert(!diag_service_sample(1u, false, 0u));

    g_fail_heap_allocation = false;
    assert(diag_service_sample(2u, false, 0u));
    assert(diag_service_snapshot_is_available());

    diag_service_trace_task_create(&task_a, "Worker", 1u);
    diag_service_trace_task_create(&task_b, "Worker", 2u);
    set_status(0u, &task_a, "Worker", 1u, UINT32_MAX - 3u);
    set_status(1u, &task_b, "Worker", 2u, 10u);
    g_status_count = 2u;
    g_total_runtime = 100u;
    assert(diag_service_sample(100u, false, 0u));
    assert(diag_service_get_snapshot(&snapshot));
    assert(snapshot.task_name_conflict);
    assert(snapshot.task_model_incomplete);

    diag_service_trace_task_delete(&task_a, "Worker");
    set_status(0u, &task_b, "Worker", 2u, UINT32_MAX - 3u);
    g_status_count = 1u;
    g_total_runtime = UINT32_MAX - 50u;
    assert(diag_service_sample(200u, false, 0u));
    assert(diag_service_get_snapshot(&snapshot));
    assert(!snapshot.task_name_conflict);
    const diag_service_snapshot_task_t *worker = find_task(&snapshot, "Worker");
    assert(worker != NULL && worker->present && worker->task_number == 2u);
    assert(worker->runtime_percent_quality == DIAG_SERVICE_QUALITY_BAD_NO_DATA);

    set_status(0u, &task_b, "Worker", 2u, 4u);
    g_total_runtime = 49u;
    assert(diag_service_sample(201u, false, 0u));
    assert(diag_service_get_snapshot(&snapshot));
    worker = find_task(&snapshot, "Worker");
    assert(worker != NULL);
    assert(worker->runtime_counter_ms == ((uint64_t)UINT32_MAX - 3u) + 8u);
    assert(worker->runtime_counter_quality == DIAG_SERVICE_QUALITY_GOOD);
    assert(worker->runtime_percent_quality == DIAG_SERVICE_QUALITY_GOOD);
    assert(worker->runtime_percent == 8.0);

    diag_service_trace_task_delete(&task_b, "Worker");
    g_status_count = 0u;
    assert(diag_service_sample(300u, false, 0u));
    assert(diag_service_get_snapshot(&snapshot));
    worker = find_task(&snapshot, "Worker");
    assert(worker != NULL && !worker->present);
    assert(worker->state == DIAG_SERVICE_TASK_STATE_DELETED);
    assert(worker->quality == DIAG_SERVICE_QUALITY_BAD_NO_DATA);

    diag_service_trace_task_create(&task_c, "Worker", 3u);
    set_status(0u, &task_c, "Worker", 3u, 10u);
    g_status_count = 1u;
    g_total_runtime = 59u;
    assert(diag_service_sample(301u, false, 0u));
    assert(diag_service_get_snapshot(&snapshot));
    worker = find_task(&snapshot, "Worker");
    assert(worker != NULL && worker->present && worker->task_number == 3u);
    assert(worker->generation == 3u);
    assert(worker->runtime_percent_quality == DIAG_SERVICE_QUALITY_BAD_NO_DATA);

    diag_service_trace_task_delete(&task_c, "Worker");
    diag_service_trace_task_create(&task_c, "ReusedTCB", 4u);
    set_status(0u, &task_c, "ReusedTCB", 4u, 20u);
    g_total_runtime = 69u;
    assert(diag_service_sample(302u, false, 0u));
    assert(diag_service_get_snapshot(&snapshot));
    worker = find_task(&snapshot, "Worker");
    const diag_service_snapshot_task_t *reused =
        find_task(&snapshot, "ReusedTCB");
    assert(worker != NULL && !worker->present);
    assert(reused != NULL && reused->present && reused->generation == 1u);

    /* The sampler owns runtime-extension state. Replacing a task after the
     * ingress copy but before system-state capture must not let the trace hook
     * erase the captured generation's wrap calculation. */
    diag_service_trace_task_create(&interleave_old, "Interleave", 90u);
    set_status(0u, &task_c, "ReusedTCB", 4u, 20u);
    set_status(1u, &interleave_old, "Interleave", 90u, UINT32_MAX - 3u);
    g_status_count = 2u;
    g_total_runtime = UINT32_MAX - 20u;
    assert(diag_service_sample(400u, false, 0u));
    g_interleave_old_identity = &interleave_old;
    g_interleave_new_identity = &interleave_new;
    g_interleave_name = "Interleave";
    g_interleave_task_replacement = true;
    set_status(1u, &interleave_old, "Interleave", 90u, 4u);
    g_total_runtime = 29u;
    assert(diag_service_sample(401u, false, 0u));
    assert(diag_service_get_snapshot(&snapshot));
    const diag_service_snapshot_task_t *interleaved =
        find_task(&snapshot, "Interleave");
    assert(interleaved != NULL);
    assert(interleaved->runtime_counter_ms == ((uint64_t)UINT32_MAX - 3u) + 8u);
    assert(interleaved->runtime_counter_quality == DIAG_SERVICE_QUALITY_GOOD);

    set_status(1u, &interleave_new, "Interleave", 91u, 10u);

    diag_service_record_heap_failure_from_isr(
        DIAG_SERVICE_ALLOC_FAILURE_INVALID_CONTEXT, 64u, 72u, 500u);
    assert(g_isr_critical_depth == 0u);
    g_status_count = 2u;
    assert(diag_service_sample(500u, false, 0u));
    assert(diag_service_get_snapshot(&snapshot));
    assert(snapshot.heap.failed_allocations.value == 1u);
    assert(snapshot.heap.invalid_context_failures.value == 1u);
    assert(snapshot.heap.last_failure_reason ==
           DIAG_SERVICE_ALLOC_FAILURE_INVALID_CONTEXT);
    assert(!snapshot.heap.last_failure_timestamp_valid);
    reused = find_task(&snapshot, "ReusedTCB");
    assert(reused != NULL);
    assert(reused->runtime_percent_quality == DIAG_SERVICE_QUALITY_BAD_NO_DATA);

    static int extra_tasks[16];
    static char extra_names[16][configMAX_TASK_NAME_LEN];
    for (size_t index = 0u; index < 16u; ++index)
    {
        (void)snprintf(extra_names[index],
                       sizeof(extra_names[index]),
                       "Extra%u",
                       (unsigned)index);
        diag_service_trace_task_create(&extra_tasks[index],
                                       extra_names[index],
                                       (uint32_t)(10u + index));
    }
    g_status_count = 18u;
    assert(diag_service_sample(600u, false, 0u));
    assert(diag_service_get_snapshot(&snapshot));
    assert(snapshot.task_count == 18u);
    assert(snapshot.task_list_truncated);
    assert(snapshot.task_model_incomplete);

    assert(g_task_critical_depth == 0u);
    return 0;
}
