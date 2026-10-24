#ifndef LWIP_PSRAM_HEAP_H
#define LWIP_PSRAM_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reserve the bounded lwIP arena after the board PSRAM allocator is ready and
 * before any lwIP initialization. The allocation is permanent by design. */
bool lwip_psram_heap_init(void);

typedef struct
{
    bool supported;
    bool ready;
    size_t reserved_bytes;
    size_t used_bytes;
    size_t peak_used_bytes;
    size_t largest_free_block_bytes;
    uint64_t failed_allocations;
    uint64_t failed_allocations_wrap_count;
} lwip_psram_heap_stats_t;

bool lwip_psram_heap_get_stats(lwip_psram_heap_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif
