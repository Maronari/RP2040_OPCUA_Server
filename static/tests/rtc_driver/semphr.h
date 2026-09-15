#ifndef RTC_DRIVER_TEST_SEMPHR_H
#define RTC_DRIVER_TEST_SEMPHR_H

#include <stdbool.h>

#include "FreeRTOS.h"

typedef struct
{
    bool held;
} StaticSemaphore_t;

typedef StaticSemaphore_t *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);

#endif
