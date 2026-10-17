#ifndef PSRAM_ALLOCATOR_H
#define PSRAM_ALLOCATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSRAM_ALLOCATOR_BASE_ADDR ((uintptr_t)0x11000000u)

typedef enum
{
    PSRAM_ALLOCATOR_FAILURE_NONE = 0,
    PSRAM_ALLOCATOR_FAILURE_OUT_OF_MEMORY = 1,
    PSRAM_ALLOCATOR_FAILURE_LOCK_TIMEOUT = 2,
    PSRAM_ALLOCATOR_FAILURE_INVALID_CONTEXT = 3,
    PSRAM_ALLOCATOR_FAILURE_SIZE_OVERFLOW = 4
} psram_allocator_failure_reason_t;

typedef struct
{
    /* Byte gauges are coherent at psram_allocator_get_stats() return. */
    bool compiled;
    bool ready;
    size_t total_bytes;
    size_t free_bytes;
    size_t largest_free_block_bytes;
    size_t used_bytes;
    size_t peak_used_bytes;
    size_t requested_bytes_in_use;
    size_t peak_requested_bytes_in_use;
    uint64_t successful_allocations;
    uint64_t successful_allocations_wrap_count;
    uint64_t successful_frees;
    uint64_t successful_frees_wrap_count;
    uint64_t failed_allocations;
    uint64_t failed_allocations_wrap_count;
    uint64_t lock_timeouts;
    uint64_t lock_timeouts_wrap_count;
    uint64_t invalid_context_failures;
    uint64_t invalid_context_failures_wrap_count;
    size_t last_failed_requested_bytes;
    psram_allocator_failure_reason_t last_failure_reason;
    uint64_t last_failure_uptime_ms;
    bool last_failure_timestamp_valid;
    uint64_t last_failure_timestamp_unix_us;
} psram_allocator_stats_t;

/* Counters have reboot lifetime. UInt64 values wrap to zero and their wrap
 * counters increment; wrap counters saturate at UINT64_MAX. A false stats
 * return means the current arena view is unavailable and callers must not
 * represent zeroed fields as measurements. */

bool psram_allocator_init(void);
bool psram_allocator_is_ready(void);
bool psram_allocator_self_test(void);

void *psram_malloc(size_t size);
void *psram_calloc(size_t count, size_t size);
void *psram_realloc(void *ptr, size_t size);
void psram_free(void *ptr);

bool psram_contains_ptr(const void *ptr);
size_t psram_total_size(void);
size_t psram_free_size(void);
size_t psram_largest_free_block(void);
bool psram_allocator_get_stats(psram_allocator_stats_t *out_stats);

#if defined(PSRAM_ALLOCATOR_TEST_HOOKS)
bool psram_allocator_test_init(void *arena, size_t arena_size);
#endif

#ifdef __cplusplus
}
#endif

#endif
