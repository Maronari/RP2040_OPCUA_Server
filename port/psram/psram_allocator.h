#ifndef PSRAM_ALLOCATOR_H
#define PSRAM_ALLOCATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSRAM_ALLOCATOR_BASE_ADDR ((uintptr_t)0x11000000u)

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

#ifdef __cplusplus
}
#endif

#endif
