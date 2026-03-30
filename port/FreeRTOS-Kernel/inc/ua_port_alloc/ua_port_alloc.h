#ifndef UA_PORT_ALLOC_H
#define UA_PORT_ALLOC_H

#include <stddef.h>

void *ua_port_malloc(size_t size);
void *ua_port_calloc(size_t num, size_t size);
void ua_port_free(void *mem);
void *ua_port_realloc(void *mem, size_t new_size);

#endif /* UA_PORT_ALLOC_H */
