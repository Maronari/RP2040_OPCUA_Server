#ifndef PSRAM_ALLOCATOR_TEST_SEMPHR_H
#define PSRAM_ALLOCATOR_TEST_SEMPHR_H

#include "FreeRTOS.h"

typedef struct
{
    unsigned marker;
} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t timeout);
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle);

#endif
