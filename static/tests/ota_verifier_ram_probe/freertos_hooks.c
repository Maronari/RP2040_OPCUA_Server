/* Minimal fail-closed hooks required by the real PSRAM allocator's FreeRTOS dependency.
 * The RAM probe never starts the scheduler or creates tasks. */
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

void app_assert_handler(const char *file, unsigned line) {
    (void)file;
    (void)line;
    while (true) tight_loop_contents();
}

void vApplicationMallocFailedHook(void) {
    while (true) tight_loop_contents();
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name) {
    (void)task;
    (void)task_name;
    while (true) tight_loop_contents();
}

void vApplicationGetIdleTaskMemory(StaticTask_t **task_buffer, StackType_t **stack_buffer,
                                   configSTACK_DEPTH_TYPE *stack_size) {
    static StaticTask_t task;
    static StackType_t stack[configMINIMAL_STACK_SIZE];
    *task_buffer = &task;
    *stack_buffer = stack;
    *stack_size = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory(StaticTask_t **task_buffer, StackType_t **stack_buffer,
                                    configSTACK_DEPTH_TYPE *stack_size) {
    static StaticTask_t task;
    static StackType_t stack[configTIMER_TASK_STACK_DEPTH];
    *task_buffer = &task;
    *stack_buffer = stack;
    *stack_size = configTIMER_TASK_STACK_DEPTH;
}

void diag_service_trace_task_create(void *task_identity, const char *task_name,
                                    uint32_t task_number) {
    (void)task_identity;
    (void)task_name;
    (void)task_number;
}

void diag_service_trace_task_delete(void *task_identity, const char *task_name) {
    (void)task_identity;
    (void)task_name;
}
