#ifndef SERVICES_DIAG_SERVICE_H
#define SERVICES_DIAG_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <FreeRTOS.h>
#include <task.h>

#define DIAG_SERVICE_MAX_TASKS 8
#define DIAG_SERVICE_MAX_SYSTEM_TASKS 16
#define DIAG_SERVICE_RESET_CAUSE_MAX_LEN 32

typedef enum
{
    DIAG_FAULT_CODE_NONE = 0,
    DIAG_FAULT_CODE_MALLOC_FAILED = 1,
    DIAG_FAULT_CODE_STACK_OVERFLOW = 2,
    DIAG_FAULT_CODE_SELF_TEST_FAILED = 3
} diag_fault_code_t;

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

typedef struct
{
    uint32_t uptime_sec;
    uint32_t cpu_like_load_approx;
    uint32_t last_fault_code;
    char reset_cause[DIAG_SERVICE_RESET_CAUSE_MAX_LEN];
} diag_service_system_stats_t;

void diag_service_init(void);
bool diag_service_register_task(const char *name, TaskHandle_t handle);
size_t diag_service_get_task_count(void);
bool diag_service_get_task_info(size_t index, diag_service_task_info_t *out_info);
bool diag_service_get_task_info_by_name(const char *name, diag_service_task_info_t *out_info);
void diag_service_get_heap_stats(diag_service_heap_stats_t *out_stats);
void diag_service_get_system_stats(diag_service_system_stats_t *out_stats);
void diag_service_set_last_fault_code(diag_fault_code_t fault_code);
void diag_service_clear(void);
void diag_service_reboot(uint32_t delay_ms);

#endif
