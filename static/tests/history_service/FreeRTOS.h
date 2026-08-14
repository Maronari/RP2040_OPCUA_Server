#ifndef HISTORY_SERVICE_TEST_FREERTOS_H
#define HISTORY_SERVICE_TEST_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
#define portTICK_PERIOD_MS 1u
#define pdMS_TO_TICKS(value) (value)
#define pdTRUE 1
#define pdFALSE 0
void history_service_test_enter_critical(void);
void history_service_test_exit_critical(void);
#define taskENTER_CRITICAL() history_service_test_enter_critical()
#define taskEXIT_CRITICAL() history_service_test_exit_critical()

#endif
