/**
 * @file
 * @brief RTC/time synchronization service: RTC bring-up, SNTP sync
 *        orchestration, and system-time accessors used across services.
 *
 * Pairs with `drivers/rtc_driver` (RP2350 Always-On timer). Runs its own
 * "TimeTask" FreeRTOS task (time_service_task()) that starts SNTP once the
 * network is ready and periodically re-derives status/quality from the RTC.
 */
#ifndef SERVICES_TIME_SERVICE_H
#define SERVICES_TIME_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "services/service_types.h"

#define TIME_SERVICE_NTP_SERVER_MAX_LEN 64

/**
 * @brief Point-in-time snapshot of RTC/SNTP synchronization status.
 *
 * Owned by `time_service.c`'s single static `g_status`; published/read
 * under `taskENTER_CRITICAL`/`taskEXIT_CRITICAL`
 * (`time_service_set_status()`/time_service_get_status()).
 * `synchronized` is sticky: once `last_sync_epoch_s > 0`, it never reverts
 * to false in time_service_task() (`time_service.c:112`).
 */
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

/**
 * @brief Initializes the RTC driver and seeds status from `config_service_get()`.
 *
 * Must be called once at boot before time_service_task() is started. Sets
 * quality to UNCERTAIN if the RTC reports a valid time, BAD otherwise (SNTP
 * has not run yet at this point, so GOOD is never reached here).
 *
 * @return true if `rtc_driver_init()` succeeded; false otherwise. On
 *         failure, status quality is left as SERVICE_QUALITY_BAD.
 */
bool time_service_init(void);

/**
 * @brief FreeRTOS task entry point ("TimeTask"). Starts SNTP once the
 *        network is ready and refreshes RTC-derived status every 1000 ms.
 *
 * Runs forever; never returns. Starts SNTP (`sntp_setservername`/
 * `sntp_init`) exactly once, gated on `network_manager_is_ready()` and
 * `!status.sntp_started`. Recomputes `quality` each iteration: GOOD once
 * `synchronized`, UNCERTAIN if only the RTC is valid, BAD otherwise.
 *
 * @param argument Unused (FreeRTOS task signature).
 */
void time_service_task(void *argument);

/**
 * @brief Copies the current time/sync status snapshot to @p out_status.
 *
 * Thread-safe: reads under `taskENTER_CRITICAL`/`taskEXIT_CRITICAL`. No-op
 * if @p out_status is NULL.
 */
void time_service_get_status(time_service_status_t *out_status);

/**
 * @brief Forces an immediate SNTP re-sync against the configured NTP server.
 *
 * Restarts SNTP (`sntp_stop()` + `sntp_setservername()` + `sntp_init()`).
 *
 * @return false without side effects if the service is not initialized or
 *         the network is not ready (`network_manager_is_ready()` false);
 *         true otherwise (does not itself confirm sync completion - actual
 *         time application happens later via the `SNTP_SET_SYSTEM_TIME`
 *         lwIP callback, set_system_time()).
 */
bool time_service_request_sync_now(void);

/** @brief Returns the current RTC-backed Unix time in whole seconds. Thin wrapper over `rtc_driver_get_unix_time()`. */
uint32_t get_system_time(void);

/** @brief Returns the current RTC-backed Unix time in microseconds. Thin wrapper over `rtc_driver_get_unix_time_us()`. */
uint64_t get_system_time_us(void);

/**
 * @brief Reads the RTC-backed wall clock for consumers that require an
 *        absolute UTC timestamp.
 *
 * Returns false without modifying @p out_unix_time_us when the RTC has not
 * been initialized or its time is invalid. Unix epoch zero is a valid
 * reading and must be distinguished from an invalid clock by this
 * function's boolean result, not by comparing the output to zero.
 *
 * @param out_unix_time_us Destination for the UTC microsecond timestamp;
 *        must be non-NULL (NULL returns false).
 * @return true if a valid RTC-backed timestamp was written.
 */
bool time_service_get_utc_time_us(uint64_t *out_unix_time_us);

/** @brief Returns the configured local timezone offset from UTC in minutes (from `config_service_get()`). */
int16_t get_system_timezone_offset_minutes(void);

/**
 * @brief Applies a new absolute Unix time to the RTC and marks the service synchronized.
 *
 * Registered as the lwIP `SNTP_SET_SYSTEM_TIME` callback
 * (`port/lwip/lwipopts.h:143`), so in normal operation this is invoked from
 * lwIP's SNTP processing context (the lwIP/tcpip task), not called directly
 * by application code. Writes the RTC via `rtc_driver_set_unix_time()`,
 * then marks status synchronized/GOOD and increments `sync_count`.
 *
 * @param seconds New absolute Unix time, in seconds.
 */
void set_system_time(uint32_t seconds);

#endif
