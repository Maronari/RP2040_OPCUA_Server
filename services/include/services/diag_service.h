#ifndef SERVICES_DIAG_SERVICE_H
#define SERVICES_DIAG_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <FreeRTOS.h>
#include <task.h>

/** @brief Maximum number of FreeRTOS tasks this service can register by name (diag_service_register_task()). */
#define DIAG_SERVICE_MAX_TASKS 8
/** @brief Maximum number of tasks sampled by uxTaskGetSystemState() when approximating CPU load; excess tasks are silently dropped from the sample. */
#define DIAG_SERVICE_MAX_SYSTEM_TASKS 16
#define DIAG_SERVICE_RESET_CAUSE_MAX_LEN 32

/* T2 snapshot contract.  These values are deliberately independent from
 * open62541 StatusCode values: the OPC UA adapter is the only layer which
 * translates them to DataValue status codes. */
#define DIAG_SERVICE_SNAPSHOT_PERIOD_MS 1000u
#define DIAG_SERVICE_MAX_SNAPSHOT_TASKS 16u

typedef enum
{
    DIAG_SERVICE_QUALITY_GOOD = 0,
    DIAG_SERVICE_QUALITY_UNCERTAIN_LAST_USABLE = 1,
    DIAG_SERVICE_QUALITY_BAD_NO_DATA = 2,
    DIAG_SERVICE_QUALITY_BAD_NOT_SUPPORTED = 3,
    DIAG_SERVICE_QUALITY_BAD_NOT_CONNECTED = 4
} diag_service_quality_t;

typedef enum
{
    DIAG_SERVICE_ALLOC_FAILURE_NONE = 0,
    DIAG_SERVICE_ALLOC_FAILURE_OUT_OF_MEMORY = 1,
    DIAG_SERVICE_ALLOC_FAILURE_LOCK_TIMEOUT = 2,
    DIAG_SERVICE_ALLOC_FAILURE_INVALID_CONTEXT = 3,
    DIAG_SERVICE_ALLOC_FAILURE_SIZE_OVERFLOW = 4
} diag_service_alloc_failure_reason_t;

typedef enum
{
    DIAG_SERVICE_TASK_STATE_RUNNING = 0,
    DIAG_SERVICE_TASK_STATE_READY = 1,
    DIAG_SERVICE_TASK_STATE_BLOCKED = 2,
    DIAG_SERVICE_TASK_STATE_SUSPENDED = 3,
    DIAG_SERVICE_TASK_STATE_DELETED = 4,
    DIAG_SERVICE_TASK_STATE_INVALID = 5
} diag_service_task_state_t;

typedef struct
{
    uint64_t value;
    uint64_t wrap_count;
} diag_service_counter_t;

/* A native task record never retains a TCB-owned pointer.  `name` is copied
 * by the create trace hook after FreeRTOS truncation. */
typedef struct
{
    char name[configMAX_TASK_NAME_LEN];
    bool present;
    uint32_t task_number;
    uint64_t generation;
    uint64_t generation_wrap_count;
    diag_service_task_state_t state;
    uint32_t current_priority;
    uint32_t base_priority;
    uint64_t runtime_counter_ms;
    uint64_t runtime_counter_wrap_count;
    diag_service_quality_t runtime_counter_quality;
    double runtime_percent;
    diag_service_quality_t runtime_percent_quality;
    uint64_t stack_high_watermark_bytes;
    diag_service_quality_t quality;
} diag_service_snapshot_task_t;

typedef struct
{
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t peak_used_bytes;
    uint64_t free_bytes;
    uint64_t minimum_ever_free_bytes;
    uint64_t largest_free_block_bytes;
    uint64_t smallest_free_block_bytes;
    uint64_t free_block_count;
    diag_service_counter_t successful_allocations;
    diag_service_counter_t successful_frees;
    diag_service_counter_t failed_allocations;
    diag_service_counter_t invalid_context_failures;
    uint64_t last_failed_requested_bytes;
    uint64_t last_failed_adjusted_bytes;
    diag_service_alloc_failure_reason_t last_failure_reason;
    uint64_t last_failure_uptime_ms;
    bool last_failure_timestamp_valid;
    uint64_t last_failure_timestamp_unix_us;
    diag_service_quality_t quality;
} diag_service_heap_snapshot_t;

typedef struct
{
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t peak_used_bytes;
    uint64_t free_bytes;
    uint64_t largest_free_block_bytes;
    uint64_t requested_bytes_in_use;
    uint64_t peak_requested_bytes_in_use;
    diag_service_counter_t successful_allocations;
    diag_service_counter_t successful_frees;
    diag_service_counter_t failed_allocations;
    diag_service_counter_t lock_timeouts;
    diag_service_counter_t invalid_context_failures;
    uint64_t last_failed_requested_bytes;
    diag_service_alloc_failure_reason_t last_failure_reason;
    uint64_t last_failure_uptime_ms;
    bool last_failure_timestamp_valid;
    uint64_t last_failure_timestamp_unix_us;
    bool ready;
    bool compiled;
    diag_service_quality_t quality;
} diag_service_psram_snapshot_t;

typedef struct
{
    uint64_t reserved_bytes;
    uint64_t used_bytes;
    uint64_t peak_used_bytes;
    uint64_t largest_free_block_bytes;
    diag_service_counter_t failed_allocations;
    diag_service_quality_t quality;
} diag_service_lwip_snapshot_t;

/* The one-second coherent publication consumed by the OPC UA adapter.  Wall
 * time is valid only when `timestamp_valid` is true; zero is otherwise not a
 * substitute for an unavailable timestamp. */
typedef struct
{
    uint64_t sequence;
    uint64_t sequence_wrap_count;
    uint64_t timestamp_unix_us;
    bool timestamp_valid;
    uint64_t uptime_ms;
    uint64_t age_ms;
    uint32_t task_count;
    bool task_list_truncated;
    bool task_name_conflict;
    bool task_model_incomplete;
    diag_service_quality_t quality;
    diag_service_heap_snapshot_t heap;
    diag_service_psram_snapshot_t psram;
    diag_service_lwip_snapshot_t lwip;
    uint32_t retained_task_count;
    diag_service_snapshot_task_t tasks[DIAG_SERVICE_MAX_SNAPSHOT_TASKS];
} diag_service_snapshot_t;

/**
 * @brief Firmware-defined fault codes recorded via
 *        diag_service_set_last_fault_code() and surfaced through
 *        diag_service_system_stats_t::last_fault_code /
 *        device_health_report_t::last_fault_code.
 */
typedef enum
{
    DIAG_FAULT_CODE_NONE = 0,
    DIAG_FAULT_CODE_MALLOC_FAILED = 1,
    DIAG_FAULT_CODE_STACK_OVERFLOW = 2,
    DIAG_FAULT_CODE_SELF_TEST_FAILED = 3
} diag_fault_code_t;

/** @brief Point-in-time snapshot of one registered FreeRTOS task, as reported by vTaskGetInfo(). */
typedef struct
{
    char name[configMAX_TASK_NAME_LEN];
    eTaskState state;
    uint32_t task_number;
    uint32_t runtime_counter;
    uint32_t current_priority;
    uint32_t base_priority;
    uint32_t stack_high_water_mark;
} diag_service_task_info_t;

/** @brief FreeRTOS heap allocator statistics, as reported by vPortGetHeapStats(). */
typedef struct
{
    uint32_t available_heap_bytes;
    uint32_t largest_free_block_bytes;
    uint32_t smallest_free_block_bytes;
    uint32_t free_block_count;
    uint32_t minimum_ever_free_bytes;
    uint32_t successful_allocations;
    uint32_t successful_frees;
} diag_service_heap_stats_t;

/** @brief Aggregate system-level diagnostic snapshot (uptime, approximate load, last fault, reset cause). */
typedef struct
{
    uint32_t uptime_sec;
    uint32_t cpu_like_load_approx;
    uint32_t last_fault_code;
    char reset_cause[DIAG_SERVICE_RESET_CAUSE_MAX_LEN];
} diag_service_system_stats_t;

/**
 * @brief Initialize the task registry and record the boot reset cause.
 *
 * Reads the watchdog-caused-reboot flag via `hardware/watchdog.h`
 * (`watchdog_caused_reboot()`) directly -- this service is a documented
 * exception to the services-don't-touch-hardware layering rule (see
 * docs/code/architecture.md#confirmed-layering-observations). Call once
 * during platform init, before any task registers itself.
 */
void diag_service_init(void);
/**
 * @brief Register a FreeRTOS task handle under a display name for later
 *        diag_service_get_task_info()/_by_name() lookup.
 *
 * @param name Display name to record (independent of the FreeRTOS task's
 *        own pcName); copied, not retained.
 * @param handle Task handle to register; must not be NULL.
 * @return true on success; false on NULL arguments or if
 *         DIAG_SERVICE_MAX_TASKS registered slots are already used.
 * @note Thread-safe (taskENTER_CRITICAL()-protected).
 */
bool diag_service_register_task(const char *name, TaskHandle_t handle);
/** @brief Number of tasks registered via diag_service_register_task(). Thread-safe. */
size_t diag_service_get_task_count(void);
/**
 * @brief Fetch live FreeRTOS task info for a previously registered task by
 *        registration-order index.
 *
 * Calls vTaskGetInfo() on the registered handle, so the result reflects the
 * task's *current* state at call time, not registration time.
 *
 * @param index Registration-order index in [0, diag_service_get_task_count()).
 * @param out_info Must not be NULL; fully overwritten on success.
 * @return true if @p index named a registered task; false otherwise.
 */
bool diag_service_get_task_info(size_t index, diag_service_task_info_t *out_info);
/**
 * @brief Fetch live FreeRTOS task info for a previously registered task by
 *        its registered display name.
 *
 * @param name Name previously passed to diag_service_register_task().
 * @param out_info Must not be NULL; fully overwritten on success.
 * @return true if a task was registered under @p name; false otherwise.
 */
bool diag_service_get_task_info_by_name(const char *name, diag_service_task_info_t *out_info);
/** @brief Fill @p out_stats with current FreeRTOS heap statistics (vPortGetHeapStats()). @p out_stats must not be NULL. */
void diag_service_get_heap_stats(diag_service_heap_stats_t *out_stats);

/** Record a FreeRTOS heap allocation failure without changing device health.
 * All counters have reboot lifetime; values wrap and their wrap counters
 * saturate.  This hook-safe function allocates no memory. */
void diag_service_record_heap_failure(diag_service_alloc_failure_reason_t reason,
                                      uint64_t requested_bytes,
                                      uint64_t adjusted_bytes,
                                      uint64_t uptime_ms,
                                      bool timestamp_valid,
                                      uint64_t timestamp_unix_us);
/** ISR-safe variant used only by the FreeRTOS malloc-failed hook. */
void diag_service_record_heap_failure_from_isr(
    diag_service_alloc_failure_reason_t reason,
    uint64_t requested_bytes,
    uint64_t adjusted_bytes,
    uint64_t uptime_ms);

/** True only after a complete coherent sample has been published. */
bool diag_service_snapshot_is_available(void);

/** Publish one complete one-second diagnostics sample.  The caller supplies
 * monotonic/wall clock observations so this service remains hardware-free. */
bool diag_service_sample(uint64_t monotonic_ms,
                         bool timestamp_valid,
                         uint64_t timestamp_unix_us);

/** Copy the last complete coherent sample.  Returns false before the first
 * sample; @p out_snapshot is never partially written. */
bool diag_service_get_snapshot(diag_service_snapshot_t *out_snapshot);

/** Trace-hook ingress.  These functions have bounded, non-allocating,
 * critical-section-safe implementations and must be called only by the
 * FreeRTOS trace create/delete hooks. */
void diag_service_trace_task_create(void *task_identity,
                                    const char *name,
                                    uint32_t task_number);
void diag_service_trace_task_delete(void *task_identity,
                                    const char *name);
/**
 * @brief Fill @p out_stats with uptime, an approximate CPU load (100 minus
 *        the IDLE task's runtime-counter share), the last recorded fault
 *        code, and the boot reset cause recorded by diag_service_init().
 * @param out_stats Must not be NULL; fully overwritten.
 * @note Thread-safe for the fault-code/reset-cause fields (critical-section
 *       protected); uptime/load are read without synchronization.
 */
void diag_service_get_system_stats(diag_service_system_stats_t *out_stats);
/** @brief Record the most recent fault code, overwriting any previous value. Thread-safe (critical-section protected). */
void diag_service_set_last_fault_code(diag_fault_code_t fault_code);
/** @brief Reset the last fault code to DIAG_FAULT_CODE_NONE. */
void diag_service_clear(void);
/**
 * @brief Trigger a hardware watchdog reboot after @p delay_ms.
 *
 * Calls `watchdog_reboot()` from `hardware/watchdog.h` directly -- see the
 * note on diag_service_init(). Does not return before the reboot occurs (the
 * caller's remaining code, if any, still executes until the watchdog fires).
 *
 * @param delay_ms Delay before reboot, in milliseconds.
 */
void diag_service_reboot(uint32_t delay_ms);

#endif
