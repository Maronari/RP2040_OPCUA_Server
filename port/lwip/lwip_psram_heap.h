#ifndef LWIP_PSRAM_HEAP_H
#define LWIP_PSRAM_HEAP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reserve the bounded lwIP arena after the board PSRAM allocator is ready and
 * before any lwIP initialization. The allocation is permanent by design. */
bool lwip_psram_heap_init(void);

#ifdef __cplusplus
}
#endif

#endif
