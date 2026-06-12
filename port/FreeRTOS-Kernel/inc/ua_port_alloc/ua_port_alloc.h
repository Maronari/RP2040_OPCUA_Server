#ifndef UA_PORT_ALLOC_H
#define UA_PORT_ALLOC_H

#include <stdbool.h>
#include <stddef.h>

void *ua_port_malloc(size_t size);
void *ua_port_calloc(size_t num, size_t size);
void ua_port_free(void *mem);
void *ua_port_realloc(void *mem, size_t new_size);
void ua_port_alloc_set_psram_enabled(bool enabled);
void ua_port_alloc_log_stats(const char *stage);

#endif /* UA_PORT_ALLOC_H */
