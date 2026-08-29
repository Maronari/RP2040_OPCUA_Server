#ifndef SENSOR_MANAGER_TEST_TASK_H
#define SENSOR_MANAGER_TEST_TASK_H

#include "FreeRTOS.h"
TickType_t xTaskGetTickCount(void);
void vTaskDelayUntil(TickType_t *last_wake_time, TickType_t period);
void vTaskDelay(TickType_t ticks);

#endif
