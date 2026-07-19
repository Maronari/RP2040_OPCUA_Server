#ifndef UA_PORT_ALLOC_H
#define UA_PORT_ALLOC_H

#include <stdbool.h>
#include <stddef.h>

typedef struct
{
    bool psram_ready;
    bool psram_enabled;
    size_t total_bytes;
    size_t free_bytes;
    size_t largest_free_block_bytes;
} ua_port_alloc_stats_t;

void *ua_port_malloc(size_t size);
void *ua_port_calloc(size_t num, size_t size);
void ua_port_free(void *mem);
void *ua_port_realloc(void *mem, size_t new_size);
void ua_port_alloc_set_psram_enabled(bool enabled);
bool ua_port_alloc_is_psram_enabled(void);
void ua_port_alloc_get_stats(ua_port_alloc_stats_t *stats);
void ua_port_alloc_log_stats(const char *stage);

#endif /* UA_PORT_ALLOC_H */
