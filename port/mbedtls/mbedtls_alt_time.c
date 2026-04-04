#include <stdint.h>

#include "mbedtls/platform_time.h"
#include "FreeRTOS.h"
#include "task.h"

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    TickType_t ticks = xTaskGetTickCount();
    return (mbedtls_ms_time_t)(((uint64_t)ticks * 1000u) / configTICK_RATE_HZ);
}