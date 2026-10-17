#ifndef SERVICES_RESOURCE_EXHAUSTION_TEST_H
#define SERVICES_RESOURCE_EXHAUSTION_TEST_H

/*
 * Test-only, compile-time allocator exhaustion runner.  It has no OPC UA
 * surface and is linked only by the resource-exhaustion firmware profile.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef RP2_RESOURCE_EXHAUSTION_TEST_ENABLED
#define RP2_RESOURCE_EXHAUSTION_TEST_ENABLED 0
#endif

#ifndef RP2_RESOURCE_EXHAUSTION_PROFILE_ID
#define RP2_RESOURCE_EXHAUSTION_PROFILE_ID "disabled"
#endif

#define RESOURCE_EXHAUSTION_TEST_CYCLES 100u
#define RESOURCE_EXHAUSTION_TEST_POINTER_CAPACITY 512u
#define RESOURCE_EXHAUSTION_TEST_HEAP_REQUEST_BYTES 1024u
#define RESOURCE_EXHAUSTION_TEST_PSRAM_REQUEST_BYTES 16384u
#define RESOURCE_EXHAUSTION_TEST_JSON_MAX_BYTES 4096u

typedef enum
{
    RESOURCE_EXHAUSTION_TEST_STATUS_NOT_RUN = 0,
    RESOURCE_EXHAUSTION_TEST_STATUS_PASS,
    RESOURCE_EXHAUSTION_TEST_STATUS_STATS_UNAVAILABLE,
    RESOURCE_EXHAUSTION_TEST_STATUS_PSRAM_UNAVAILABLE,
    RESOURCE_EXHAUSTION_TEST_STATUS_POINTER_CAPACITY,
    RESOURCE_EXHAUSTION_TEST_STATUS_OOM_REASON,
    RESOURCE_EXHAUSTION_TEST_STATUS_COUNTER_MISMATCH,
    RESOURCE_EXHAUSTION_TEST_STATUS_COUNTER_WRAP_AMBIGUOUS,
    RESOURCE_EXHAUSTION_TEST_STATUS_CORRUPTION,
    RESOURCE_EXHAUSTION_TEST_STATUS_BASELINE_MISMATCH,
    RESOURCE_EXHAUSTION_TEST_STATUS_MINIMUM_EVER_UNSTABLE,
    RESOURCE_EXHAUSTION_TEST_STATUS_INVALID_ARGUMENT
} resource_exhaustion_test_status_t;

typedef struct
{
    uint64_t value;
    uint64_t wrap_count;
} resource_exhaustion_test_counter_t;

/* Every gauge is a byte count. `available` means free/used and counter
 * values are coherent; `requested_bytes_available` is false when an
 * allocator has no caller-requested-byte gauge. PSRAM also requires
 * `ready` to be true. */
typedef struct
{
    bool available;
    bool ready;
    bool requested_bytes_available;
    bool minimum_ever_free_bytes_available;
    uint64_t free_bytes;
    uint64_t used_bytes;
    uint64_t peak_used_bytes;
    uint64_t requested_bytes;
    uint64_t minimum_ever_free_bytes;
    resource_exhaustion_test_counter_t failed_allocations;
    uint32_t last_failure_reason;
} resource_exhaustion_test_stats_t;

typedef struct
{
    resource_exhaustion_test_status_t status;
    uint32_t requested_cycles;
    uint32_t completed_cycles;
    uint32_t min_block_count;
    uint32_t max_block_count;
    uint32_t first_failure_cycle; /* one-based, zero when all passed */
    bool baseline_returned;
    bool counter_exact;
    bool oom_reason_exact;
    bool payload_verified;
    bool minimum_ever_stable;
    resource_exhaustion_test_stats_t initial_stats;
    resource_exhaustion_test_stats_t final_stats;
} resource_exhaustion_test_allocator_result_t;

typedef struct
{
    const char *profile_id;
    uint64_t start_monotonic_ms;
    uint64_t end_monotonic_ms;
    uint64_t init_task_stack_high_water_start_bytes;
    uint64_t init_task_stack_high_water_end_bytes;
    resource_exhaustion_test_allocator_result_t heap;
    resource_exhaustion_test_allocator_result_t psram;
    bool all_passed;
} resource_exhaustion_test_result_t;

typedef void *(*resource_exhaustion_test_allocate_fn)(size_t size, void *context);
typedef void (*resource_exhaustion_test_free_fn)(void *pointer, void *context);
typedef bool (*resource_exhaustion_test_stats_fn)(resource_exhaustion_test_stats_t *out_stats,
                                                   void *context);
typedef uint64_t (*resource_exhaustion_test_now_fn)(void *context);
/* Host-only fault seam, invoked after the deterministic write and before the
 * mandatory full readback.  Native profile leaves it NULL. */
typedef void (*resource_exhaustion_test_after_fill_fn)(void *pointer,
                                                        size_t size,
                                                        uint32_t cycle,
                                                        uint32_t block,
                                                        void *context);

typedef struct
{
    resource_exhaustion_test_allocate_fn allocate;
    resource_exhaustion_test_free_fn release;
    resource_exhaustion_test_stats_fn get_stats;
    resource_exhaustion_test_now_fn now_monotonic_ms;
    resource_exhaustion_test_after_fill_fn after_fill;
    void *context;
} resource_exhaustion_test_allocator_ops_t;

typedef bool (*resource_exhaustion_test_write_fn)(const char *bytes,
                                                   size_t length,
                                                   void *context);

/* Runs the production test using pvPortMalloc/vPortFree and psram_malloc/free.
 * Caller must have initialized diag_service first, so the initial snapshot
 * allocation is outside the measured pressure window. */
bool resource_exhaustion_test_run(resource_exhaustion_test_result_t *out_result);

/* Test seam; production never supplies non-native allocator operations. */
bool resource_exhaustion_test_run_with_ops(
    const resource_exhaustion_test_allocator_ops_t *heap_ops,
    const resource_exhaustion_test_allocator_ops_t *psram_ops,
    const char *profile_id,
    resource_exhaustion_test_result_t *out_result);

/* Bounded, allocation-free JSON emission.  Called only after all retained
 * pointers have been released. */
bool resource_exhaustion_test_write_json(const resource_exhaustion_test_result_t *result,
                                         resource_exhaustion_test_write_fn write,
                                         void *context);

#endif
