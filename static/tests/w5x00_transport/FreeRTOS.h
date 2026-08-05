#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H

#include <stdint.h>

#define configSUPPORT_STATIC_ALLOCATION 1
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(value) (value)

typedef uint32_t TickType_t;
typedef uint32_t StackType_t;

#endif
