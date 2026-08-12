#ifndef HISTORY_SERVICE_TEST_FREERTOS_H
#define HISTORY_SERVICE_TEST_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
#define portTICK_PERIOD_MS 1u
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)

#endif
