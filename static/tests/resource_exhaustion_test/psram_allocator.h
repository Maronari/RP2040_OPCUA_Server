#ifndef TEST_PSRAM_ALLOCATOR_H
#define TEST_PSRAM_ALLOCATOR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef enum { PSRAM_ALLOCATOR_FAILURE_NONE = 0, PSRAM_ALLOCATOR_FAILURE_OUT_OF_MEMORY = 1 } psram_allocator_failure_reason_t;
typedef struct { bool compiled; bool ready; size_t free_bytes; size_t used_bytes; size_t peak_used_bytes; size_t requested_bytes_in_use; uint64_t failed_allocations; uint64_t failed_allocations_wrap_count; psram_allocator_failure_reason_t last_failure_reason; } psram_allocator_stats_t;
void *psram_malloc(size_t size);
void psram_free(void *pointer);
bool psram_allocator_get_stats(psram_allocator_stats_t *out_stats);
#endif
