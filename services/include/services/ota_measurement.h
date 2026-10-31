/**
 * @file
 * @brief Compile-time-only T0 startup/stack measurement console markers.
 *
 * The feature is disabled in ordinary firmware. It has no service state,
 * persistence or OPC UA representation. Enabled callers use fixed phase IDs.
 */
#ifndef SERVICES_OTA_MEASUREMENT_H
#define SERVICES_OTA_MEASUREMENT_H

#ifndef FIRMWARE_OTA_MEASUREMENT
#define FIRMWARE_OTA_MEASUREMENT 0
#endif

/* Fixed, non-public diagnostic vocabulary. Keep these out of OPC UA model
 * sources so public-inventory extraction remains independent of T0 console
 * instrumentation. */
#define OTA_MEASUREMENT_STATE_OK "ok"
#define OTA_MEASUREMENT_STATE_FAILED "failed"
#define OTA_MEASUREMENT_STATE_UNAVAILABLE "unavailable"

#define OTA_MEASUREMENT_PHASE_MAIN_ENTER "main_enter"
#define OTA_MEASUREMENT_PHASE_CONSOLE_READY "console_ready"
#define OTA_MEASUREMENT_PHASE_SCHEDULER_RUNNING "scheduler_running"
#define OTA_MEASUREMENT_PHASE_FLASH_COORDINATOR "flash_coordinator"
#define OTA_MEASUREMENT_PHASE_CONFIG_INIT "config_init"
#define OTA_MEASUREMENT_PHASE_CHANNEL_INIT "channel_init"
#define OTA_MEASUREMENT_PHASE_NETWORK_INIT "network_init"
#define OTA_MEASUREMENT_PHASE_HISTORY_INIT "history_init"
#define OTA_MEASUREMENT_PHASE_SECURITY_INIT "security_init"
#define OTA_MEASUREMENT_PHASE_INIT_COMPLETE "init_complete"
#define OTA_MEASUREMENT_PHASE_OPCUA_TASK "opcua_task"
#define OTA_MEASUREMENT_PHASE_WAITING_IPV4 "waiting_ipv4"
#define OTA_MEASUREMENT_PHASE_IPV4_READY "ipv4_ready"
#define OTA_MEASUREMENT_PHASE_SERVER_CREATED "server_created"
#define OTA_MEASUREMENT_PHASE_ADDRESS_SPACE "address_space"
#define OTA_MEASUREMENT_PHASE_SERVER_STARTUP "server_startup"
#define OTA_MEASUREMENT_PHASE_SERVER_ITERATE "server_iterate"
#define OTA_MEASUREMENT_PHASE_INIT_STACK "init_stack"
#define OTA_MEASUREMENT_PHASE_OPCUA_STACK "opcua_stack"

#if FIRMWARE_OTA_MEASUREMENT
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "pico/stdio.h"
#include "pico/time.h"

static inline void ota_measurement_event(const char *phase,
                                         const char *state)
{
    printf("[OTA_MEASURE] phase=%s t_us=%" PRIu64 " state=%s\n",
           phase, time_us_64(), state);
    stdio_flush();
}

/* The startup timestamp is captured before platform initialization but its
 * marker cannot be emitted until the USB console has become available. */
static inline void ota_measurement_event_at(const char *phase,
                                            const char *state,
                                            uint64_t observed_us)
{
    printf("[OTA_MEASURE] phase=%s t_us=%" PRIu64 " state=%s\n",
           phase, observed_us, state);
    stdio_flush();
}

static inline void ota_measurement_stack_event(const char *phase,
                                               uint64_t stack_high_water_bytes)
{
    printf("[OTA_MEASURE] phase=%s t_us=%" PRIu64
           " state=ok stack_hwm_bytes=%" PRIu64 "\n",
           phase, time_us_64(), stack_high_water_bytes);
    stdio_flush();
}

#define OTA_MEASUREMENT_EVENT(phase, state) \
    ota_measurement_event((phase), (state))
#define OTA_MEASUREMENT_EVENT_AT(phase, state, observed_us) \
    ota_measurement_event_at((phase), (state), (observed_us))
#define OTA_MEASUREMENT_STACK_EVENT(phase, stack_high_water_bytes) \
    ota_measurement_stack_event((phase), (stack_high_water_bytes))
#else
/* Empty variadic macros ensure disabled builds do not evaluate arguments. */
#define OTA_MEASUREMENT_EVENT(...) do { } while (0)
#define OTA_MEASUREMENT_EVENT_AT(...) do { } while (0)
#define OTA_MEASUREMENT_STACK_EVENT(...) do { } while (0)
#endif

#endif /* SERVICES_OTA_MEASUREMENT_H */
