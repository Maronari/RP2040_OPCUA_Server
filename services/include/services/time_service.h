#ifndef SERVICES_TIME_SERVICE_H
#define SERVICES_TIME_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "services/service_types.h"

#define TIME_SERVICE_NTP_SERVER_MAX_LEN 64

typedef struct
{
    uint32_t unix_time_s;
    uint32_t last_sync_epoch_s;
    bool rtc_valid;
    bool sntp_started;
    bool synchronized;
    service_quality_t quality;
    uint32_t sync_count;
    uint32_t sync_error_count;
    char ntp_server[TIME_SERVICE_NTP_SERVER_MAX_LEN];
} time_service_status_t;

bool time_service_init(void);
void time_service_task(void *argument);
void time_service_get_status(time_service_status_t *out_status);
bool time_service_request_sync_now(void);
uint32_t get_system_time(void);
uint64_t get_system_time_us(void);
int16_t get_system_timezone_offset_minutes(void);
void set_system_time(uint32_t seconds);

#endif
