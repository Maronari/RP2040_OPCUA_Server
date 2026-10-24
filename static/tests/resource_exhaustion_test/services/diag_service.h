#ifndef TEST_DIAG_SERVICE_H
#define TEST_DIAG_SERVICE_H
#include <stdbool.h>
#include <stdint.h>
typedef enum { DIAG_SERVICE_QUALITY_GOOD = 0 } diag_service_quality_t;
typedef struct { uint64_t value; uint64_t wrap_count; } diag_service_counter_t;
typedef struct { uint64_t free_bytes; uint64_t used_bytes; uint64_t peak_used_bytes; uint64_t minimum_ever_free_bytes; diag_service_counter_t failed_allocations; unsigned last_failure_reason; diag_service_quality_t quality; } diag_service_heap_snapshot_t;
typedef struct { diag_service_heap_snapshot_t heap; } diag_service_snapshot_t;
bool diag_service_sample(uint64_t monotonic_ms, bool timestamp_valid, uint64_t timestamp_unix_us);
bool diag_service_get_snapshot(diag_service_snapshot_t *out_snapshot);
#endif
