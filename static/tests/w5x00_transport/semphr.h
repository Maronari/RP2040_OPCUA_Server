#ifndef TEST_SEMPHR_H
#define TEST_SEMPHR_H

#include "FreeRTOS.h"

typedef struct { int locked; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);
int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks);
int xSemaphoreGive(SemaphoreHandle_t semaphore);

#endif
