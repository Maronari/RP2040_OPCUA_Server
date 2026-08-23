#ifndef ALARM_SERVICE_TEST_TASK_H
#define ALARM_SERVICE_TEST_TASK_H

#include "FreeRTOS.h"

TickType_t xTaskGetTickCount(void);
void vTaskDelayUntil(TickType_t *previous_wake_time, TickType_t time_increment);

#endif
