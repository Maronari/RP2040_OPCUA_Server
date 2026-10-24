#ifndef DIAG_SERVICE_TEST_TASK_H
#define DIAG_SERVICE_TEST_TASK_H

#include "FreeRTOS.h"

UBaseType_t uxTaskGetNumberOfTasks(void);
UBaseType_t uxTaskGetSystemState(TaskStatus_t *status,
                                UBaseType_t capacity,
                                configRUN_TIME_COUNTER_TYPE *total_runtime);
TickType_t xTaskGetTickCount(void);
TickType_t xTaskGetTickCountFromISR(void);
BaseType_t xTaskGetSchedulerState(void);
void vPortGetHeapStats(HeapStats_t *stats);
void *pvPortMalloc(size_t size);

#endif
