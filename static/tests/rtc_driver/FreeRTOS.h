#ifndef RTC_DRIVER_TEST_FREERTOS_H
#define RTC_DRIVER_TEST_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(value) ((TickType_t)(value))

#endif
