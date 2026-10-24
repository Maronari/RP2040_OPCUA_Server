#ifndef DIAG_SERVICE_TEST_PSRAM_ALLOCATOR_H
#define DIAG_SERVICE_TEST_PSRAM_ALLOCATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    bool compiled;
    bool ready;
    size_t total_bytes;
    size_t used_bytes;
    size_t peak_used_bytes;
    size_t free_bytes;
    size_t largest_free_block_bytes;
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
    int last_failure_reason;
    uint64_t last_failure_uptime_ms;
    bool last_failure_timestamp_valid;
    uint64_t last_failure_timestamp_unix_us;
} psram_allocator_stats_t;

void *psram_calloc(size_t count, size_t size);
bool psram_allocator_get_stats(psram_allocator_stats_t *stats);

#endif
