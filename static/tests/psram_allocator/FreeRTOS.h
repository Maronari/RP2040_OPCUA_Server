#ifndef PSRAM_ALLOCATOR_TEST_FREERTOS_H
#define PSRAM_ALLOCATOR_TEST_FREERTOS_H

#include <stdint.h>

typedef int BaseType_t;
typedef uint32_t UBaseType_t;
typedef uint32_t TickType_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(value) ((TickType_t)(value))
#define configTICK_RATE_HZ 1000u
#define taskSCHEDULER_RUNNING 2

void psram_test_enter_critical(void);
void psram_test_exit_critical(void);
UBaseType_t psram_test_enter_critical_from_isr(void);
void psram_test_exit_critical_from_isr(UBaseType_t mask);
BaseType_t xPortIsInsideInterrupt(void);
TickType_t xTaskGetTickCount(void);
TickType_t xTaskGetTickCountFromISR(void);
BaseType_t xTaskGetSchedulerState(void);

#define taskENTER_CRITICAL() psram_test_enter_critical()
#define taskEXIT_CRITICAL() psram_test_exit_critical()
#define taskENTER_CRITICAL_FROM_ISR() psram_test_enter_critical_from_isr()
#define taskEXIT_CRITICAL_FROM_ISR(mask) psram_test_exit_critical_from_isr(mask)
#define configASSERT(value) ((void)(value))

#endif
