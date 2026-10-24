#ifndef DIAG_SERVICE_TEST_LWIP_PSRAM_HEAP_H
#define DIAG_SERVICE_TEST_LWIP_PSRAM_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    bool supported;
    size_t reserved_bytes;
    size_t used_bytes;
    size_t peak_used_bytes;
    size_t largest_free_block_bytes;
    uint64_t failed_allocations;
    uint64_t failed_allocations_wrap_count;
} lwip_psram_heap_stats_t;

bool lwip_psram_heap_get_stats(lwip_psram_heap_stats_t *stats);

#endif
