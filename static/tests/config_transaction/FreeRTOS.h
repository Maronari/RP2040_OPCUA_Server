#ifndef CONFIG_TRANSACTION_TEST_FREERTOS_H
#define CONFIG_TRANSACTION_TEST_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
#define portTICK_PERIOD_MS 1u
#define pdMS_TO_TICKS(value) (value)
#define pdTRUE 1
#define pdFALSE 0
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)

#endif
