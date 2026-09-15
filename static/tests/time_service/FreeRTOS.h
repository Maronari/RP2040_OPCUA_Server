#ifndef TIME_SERVICE_TEST_FREERTOS_H
#define TIME_SERVICE_TEST_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(value) (value)

void time_service_test_enter_critical(void);
void time_service_test_exit_critical(void);
#define taskENTER_CRITICAL() time_service_test_enter_critical()
#define taskEXIT_CRITICAL() time_service_test_exit_critical()

#endif
