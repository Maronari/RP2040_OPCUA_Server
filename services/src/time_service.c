#include "services/time_service.h"

#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "drivers/rtc_driver.h"
#include "lwip/apps/sntp.h"
#include "services/config_service.h"
#include "services/network_manager.h"

static time_service_status_t g_status;
static bool g_initialized = false;

static void time_service_set_status(const time_service_status_t *status)
{
    taskENTER_CRITICAL();
    g_status = *status;
    taskEXIT_CRITICAL();
}

bool time_service_init(void)
{
    const device_profile_config_t *config = config_service_get();

    memset(&g_status, 0, sizeof(g_status));
    g_initialized = rtc_driver_init();
    strncpy(g_status.ntp_server, config->ntp_server, sizeof(g_status.ntp_server));
    g_status.ntp_server[sizeof(g_status.ntp_server) - 1] = '\0';
    g_status.quality = g_initialized ? SERVICE_QUALITY_UNCERTAIN : SERVICE_QUALITY_BAD;
    return g_initialized;
}

void set_system_time(uint32_t seconds)
{
    time_service_status_t status = {0};

    rtc_driver_set_unix_time(seconds);
    time_service_get_status(&status);
    status.unix_time_s = seconds;
    status.last_sync_epoch_s = seconds;
    status.rtc_valid = true;
    status.sntp_started = true;
    status.synchronized = true;
    status.sync_count++;
    status.quality = SERVICE_QUALITY_GOOD;
    time_service_set_status(&status);
}

uint32_t get_system_time(void)
{
    return rtc_driver_get_unix_time();
}

int16_t get_system_timezone_offset_minutes(void)
{
    return config_service_get()->timezone_offset_minutes;
}

void time_service_task(void *argument)
{
    (void)argument;

    TickType_t last_wake_time = xTaskGetTickCount();

    while (1)
    {
        time_service_status_t status = {0};
        time_service_get_status(&status);

        if (network_manager_is_ready() && !status.sntp_started)
        {
            sntp_setservername(0, status.ntp_server);
            sntp_init();
            status.sntp_started = true;
        }

        status.rtc_valid = rtc_driver_is_valid();
        status.unix_time_s = rtc_driver_get_unix_time();
        status.synchronized = status.synchronized || status.last_sync_epoch_s > 0u;

        if (status.synchronized)
        {
            status.quality = SERVICE_QUALITY_GOOD;
        }
        else if (status.rtc_valid)
        {
            status.quality = SERVICE_QUALITY_UNCERTAIN;
        }
        else
        {
            status.quality = SERVICE_QUALITY_BAD;
        }

        time_service_set_status(&status);
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(1000));
    }
}

void time_service_get_status(time_service_status_t *out_status)
{
    if (out_status == NULL)
    {
        return;
    }

    taskENTER_CRITICAL();
    *out_status = g_status;
    taskEXIT_CRITICAL();
}

bool time_service_request_sync_now(void)
{
    time_service_status_t status = {0};

    if (!g_initialized || !network_manager_is_ready())
    {
        return false;
    }

    time_service_get_status(&status);
    sntp_stop();
    sntp_setservername(0, status.ntp_server);
    sntp_init();
    status.sntp_started = true;
    time_service_set_status(&status);
    return true;
}
