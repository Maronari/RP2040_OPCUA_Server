#include "ua_port_alloc.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include "pico/stdio.h"

#define UA_PORT_ALLOC_MAGIC 0x55414c4cu

#define fflush(_stream) stdio_flush()

typedef union
{
    struct
    {
        size_t size;
        uint32_t magic;
    } meta;
    max_align_t alignment;
} ua_port_alloc_header_t;

static ua_port_alloc_header_t *ua_port_header_from_payload(void *mem)
{
    if (mem == NULL)
    {
        return NULL;
    }

    return ((ua_port_alloc_header_t *)mem) - 1;
}

static void *ua_port_payload_from_header(ua_port_alloc_header_t *header)
{
    if (header == NULL)
    {
        return NULL;
    }

    return (void *)(header + 1);
}

static void *ua_port_alloc_raw(size_t size, bool zero_init, const void *caller_pc)
{
    ua_port_alloc_header_t *header = NULL;
    const size_t total_size = sizeof(*header) + size;

    if (size == 0u || total_size < size)
    {
        return NULL;
    }

    header = (ua_port_alloc_header_t *)pvPortMalloc(total_size);
    if (header == NULL)
    {
        printf("[ALLOC]\t\tallocation failed, size=%u, caller=%p\n",
               (unsigned)size,
               caller_pc);
        fflush(stdout);
        return NULL;
    }

    if (size >= 50000u)
    {
        printf("[ALLOC]\t\tlarge allocation, size=%u, caller=%p\n",
               (unsigned)size,
               caller_pc);
        fflush(stdout);
    }

    header->meta.size = size;
    header->meta.magic = UA_PORT_ALLOC_MAGIC;

    if (zero_init)
    {
        memset(ua_port_payload_from_header(header), 0, size);
    }

    return ua_port_payload_from_header(header);
}

void *ua_port_malloc(size_t size)
{
    return ua_port_alloc_raw(size, false, __builtin_return_address(0));
}

void *ua_port_calloc(size_t num, size_t size)
{
    size_t total_size = 0;

    if (num == 0u || size == 0u)
    {
        return NULL;
    }

    if (size > (SIZE_MAX / num))
    {
        printf("[ALLOC]\t\tcalloc overflow, count=%u, size=%u\n",
               (unsigned)num,
               (unsigned)size);
        fflush(stdout);
        return NULL;
    }

    total_size = num * size;
    return ua_port_alloc_raw(total_size, true, __builtin_return_address(0));
}

void ua_port_free(void *mem)
{
    ua_port_alloc_header_t *header = ua_port_header_from_payload(mem);

    if (header == NULL)
    {
        return;
    }

    configASSERT(header->meta.magic == UA_PORT_ALLOC_MAGIC);
    header->meta.magic = 0u;
    vPortFree(header);
}

void *ua_port_realloc(void *mem, size_t new_size)
{
    ua_port_alloc_header_t *header = ua_port_header_from_payload(mem);
    void *new_mem = NULL;
    size_t copy_size = 0;

    if (new_size == 0u)
    {
        ua_port_free(mem);
        return NULL;
    }

    if (mem == NULL)
    {
        return ua_port_malloc(new_size);
    }

    configASSERT(header->meta.magic == UA_PORT_ALLOC_MAGIC);

    new_mem = ua_port_alloc_raw(new_size, false, __builtin_return_address(0));
    if (new_mem == NULL)
    {
        return NULL;
    }

    copy_size = header->meta.size < new_size ? header->meta.size : new_size;
    memcpy(new_mem, mem, copy_size);
    ua_port_free(mem);

    return new_mem;
}
