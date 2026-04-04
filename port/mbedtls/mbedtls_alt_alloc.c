#include <stdint.h>

#include "FreeRTOS.h"
#include "portable.h"

void *mbedtls_port_calloc(size_t count, size_t size) {
    if(count != 0 && size > (SIZE_MAX / count)) {
        return NULL;
    }

    return pvPortCalloc(count, size);
}

void mbedtls_port_free(void *ptr) {
    if(ptr) {
        vPortFree(ptr);
    }
}
