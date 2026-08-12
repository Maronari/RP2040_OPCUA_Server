#ifndef SENSOR_MANAGER_TEST_FREERTOS_H
#define SENSOR_MANAGER_TEST_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
#define portTICK_PERIOD_MS 1u
#define pdMS_TO_TICKS(value) (value)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)

#endif
