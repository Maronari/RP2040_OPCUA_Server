/**
 * @file
 * @brief Explicit, bounded device-monotonic sample-timing capture for v0.5.2
 *        synthetic benchmark firmware.
 *
 * This service is deliberately inert in a normal firmware image.  A
 * benchmark image allocates its record store from PSRAM at initialization;
 * it never falls back to internal SRAM.  Only SensorTask supplies samples,
 * through the observer returned by benchmark_timing_sensor_poll_begin().
 */
#ifndef SERVICES_BENCHMARK_TIMING_H
#define SERVICES_BENCHMARK_TIMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/channel_manager.h"

#ifndef RP2_BENCHMARK_PROFILE_ENABLED
#define RP2_BENCHMARK_PROFILE_ENABLED 0
#endif

/** Hard cap for a single native-evidence JSON document. */
#define BENCHMARK_TIMING_MAX_RECORDS 8192u
#define BENCHMARK_TIMING_MAX_JSON_BYTES (1024u * 1024u)
/* T2 reference-board budget gate: exactly one hundred one-second snapshots. */
#define BENCHMARK_TIMING_DIAG_BUDGET_SAMPLE_COUNT 100u
#define BENCHMARK_TIMING_DIAG_BUDGET_MAX_JSON_BYTES 2048u
#define BENCHMARK_TIMING_DIAG_BUDGET_MAX_CAPTURE_SECONDS 120u

typedef enum
{
    BENCHMARK_TIMING_DIAG_PHASE_ACQUIRE = 0,
    BENCHMARK_TIMING_DIAG_PHASE_COPY,
    BENCHMARK_TIMING_DIAG_PHASE_HEAP,
    BENCHMARK_TIMING_DIAG_PHASE_PSRAM,
    BENCHMARK_TIMING_DIAG_PHASE_LWIP,
    BENCHMARK_TIMING_DIAG_PHASE_TASKS,
    BENCHMARK_TIMING_DIAG_PHASE_PUBLISH,
    BENCHMARK_TIMING_DIAG_PHASE_COUNT
} benchmark_timing_diag_phase_t;

#ifndef RP2_BENCHMARK_TIMING_WARMUP_SECONDS
#define RP2_BENCHMARK_TIMING_WARMUP_SECONDS 10u
#endif

#ifndef RP2_BENCHMARK_TIMING_CAPTURE_SECONDS
#define RP2_BENCHMARK_TIMING_CAPTURE_SECONDS 30u
#endif

/** One app-console request observed without blocking SensorTask. */
typedef enum
{
    BENCHMARK_TIMING_CONTROL_NONE = 0,
    BENCHMARK_TIMING_CONTROL_START,
    BENCHMARK_TIMING_CONTROL_DUMP
} benchmark_timing_control_t;

/** App-owned, nonblocking input seam for `benchmark start`/`benchmark dump`. */
typedef benchmark_timing_control_t (*benchmark_timing_control_poll_fn)(void *context);
/** App-owned serial-output seam.  It is called only after capture has ended. */
typedef bool (*benchmark_timing_output_fn)(const char *bytes, size_t length, void *context);

/** Records the device-monotonic boot marker before service initialization. */
void benchmark_timing_note_boot_started(void);
/** Allocates the benchmark-only PSRAM record store.  Failure remains observable. */
bool benchmark_timing_init(void);
/** Installs app-owned nonblocking control and output seams. */
void benchmark_timing_set_control(benchmark_timing_control_poll_fn poll, void *context);
void benchmark_timing_set_output(benchmark_timing_output_fn output, void *context);

/** Processes an explicit serial start or post-run dump request. */
void benchmark_timing_sensor_task_process_control(void);
/**
 * Starts/advances the SensorTask-only capture phase and returns the exact
 * post-publication observer to pass to channel_manager_poll_all_observed().
 * Returns NULL outside the requested capture interval.
 */
channel_manager_sample_observer_t benchmark_timing_sensor_poll_begin(void);
/** Ends the SensorTask poll boundary and transitions a reached deadline. */
void benchmark_timing_sensor_poll_end(void);
/**
 * Emits one bounded JSON document after an explicit dump request.  A failed
 * writer retains the capture but consumes the request, so a retry also needs
 * an explicit command and cannot perturb subsequent idle polls.
 */
bool benchmark_timing_dump_if_ready(void);

/*
 * Synthetic-profile-only ingress around one coherent diag_service snapshot.
 * OpcUaTask ends the measured sample immediately after publication, then
 * supplies its stack HWM. The completed evidence record becomes visible only
 * after that final value is atomically stored. Both functions compile to
 * no-ops in production firmware.
 */
uint64_t benchmark_timing_diag_sample_begin(void);
/** Returns the same device-monotonic clock used for a bounded phase split. */
uint64_t benchmark_timing_diag_phase_now_us(void);
/** Supplies all phase durations for the current sample before its end hook. */
void benchmark_timing_diag_sample_set_phase_durations(
    const uint64_t durations_us[BENCHMARK_TIMING_DIAG_PHASE_COUNT]);
void benchmark_timing_diag_sample_end(uint64_t started_monotonic_us,
                                      bool snapshot_published);
/** Supply the current OpcUaTask stack HWM after ending the snapshot sample. */
void benchmark_timing_diag_sample_set_opcua_task_stack_high_water(
    uint64_t opcua_task_stack_high_water_bytes);

#endif
