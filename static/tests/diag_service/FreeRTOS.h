#ifndef DIAG_SERVICE_TEST_FREERTOS_H
#define DIAG_SERVICE_TEST_FREERTOS_H

#include <stddef.h>
#include <stdint.h>

typedef int BaseType_t;
typedef uint32_t UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef void *TaskHandle_t;
typedef uint32_t configRUN_TIME_COUNTER_TYPE;

typedef enum
{
    eRunning = 0,
    eReady,
    eBlocked,
    eSuspended,
    eDeleted,
    eInvalid
} eTaskState;

typedef struct
{
    TaskHandle_t xHandle;
    const char *pcTaskName;
    UBaseType_t xTaskNumber;
    eTaskState eCurrentState;
    UBaseType_t uxCurrentPriority;
    UBaseType_t uxBasePriority;
    configRUN_TIME_COUNTER_TYPE ulRunTimeCounter;
    uint16_t usStackHighWaterMark;
} TaskStatus_t;

typedef struct
{
    size_t xAvailableHeapSpaceInBytes;
    size_t xSizeOfLargestFreeBlockInBytes;
    size_t xSizeOfSmallestFreeBlockInBytes;
    size_t xNumberOfFreeBlocks;
    size_t xMinimumEverFreeBytesRemaining;
    size_t xNumberOfSuccessfulAllocations;
    size_t xNumberOfSuccessfulFrees;
} HeapStats_t;

#define configMAX_TASK_NAME_LEN 16u
#define configTOTAL_HEAP_SIZE 4096u
#define configTICK_RATE_HZ 1000u
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(value) ((TickType_t)(value))
#define taskSCHEDULER_RUNNING 2

void diag_test_enter_critical(void);
void diag_test_exit_critical(void);
UBaseType_t diag_test_enter_critical_from_isr(void);
void diag_test_exit_critical_from_isr(UBaseType_t mask);

#define taskENTER_CRITICAL() diag_test_enter_critical()
#define taskEXIT_CRITICAL() diag_test_exit_critical()
#define taskENTER_CRITICAL_FROM_ISR() diag_test_enter_critical_from_isr()
#define taskEXIT_CRITICAL_FROM_ISR(mask) diag_test_exit_critical_from_isr(mask)

#endif
