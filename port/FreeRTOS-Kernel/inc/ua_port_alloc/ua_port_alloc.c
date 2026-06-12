#include "ua_port_alloc.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include "pico/stdio.h"
#include "psram_allocator.h"

#define UA_PORT_ALLOC_MAGIC 0x55414c4cu
#define UA_PORT_ALLOC_SOURCE_FREERTOS 0x4652544fu
#define UA_PORT_ALLOC_SOURCE_PSRAM 0x50535241u

#define fflush(_stream) stdio_flush()

typedef union
{
    struct
    {
        size_t size;
        uint32_t magic;
        uint32_t source;
    } meta;
    max_align_t alignment;
} ua_port_alloc_header_t;

static bool g_ua_port_psram_alloc_enabled = true;

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
    bool from_psram = false;

    if (size == 0u || total_size < size)
    {
        return NULL;
    }

    if (g_ua_port_psram_alloc_enabled)
    {
        header = (ua_port_alloc_header_t *)psram_malloc(total_size);
        if (header != NULL)
        {
            from_psram = true;
        }
    }

    if (header == NULL)
    {
        header = (ua_port_alloc_header_t *)pvPortMalloc(total_size);
    }

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
    header->meta.source = from_psram ? UA_PORT_ALLOC_SOURCE_PSRAM : UA_PORT_ALLOC_SOURCE_FREERTOS;

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
    configASSERT(header->meta.source == UA_PORT_ALLOC_SOURCE_PSRAM ||
                 header->meta.source == UA_PORT_ALLOC_SOURCE_FREERTOS);

    const uint32_t source = header->meta.source;
    header->meta.magic = 0u;
    header->meta.source = 0u;

    if (source == UA_PORT_ALLOC_SOURCE_PSRAM)
    {
        psram_free(header);
    }
    else
    {
        vPortFree(header);
    }
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

void ua_port_alloc_set_psram_enabled(bool enabled)
{
    g_ua_port_psram_alloc_enabled = enabled;
    printf("[ALLOC]\t\tPSRAM allocation %s for new UA allocations\n",
           enabled ? "enabled" : "disabled");
    fflush(stdout);
}

void ua_port_alloc_log_stats(const char *stage)
{
    printf("[ALLOC]\t\t%s: psram=%s mode=%s total=%u free=%u largest=%u\n",
           stage != NULL ? stage : "stats",
           psram_allocator_is_ready() ? "ready" : "unavailable",
           g_ua_port_psram_alloc_enabled ? "psram-first" : "sram-runtime",
           (unsigned)psram_total_size(),
           (unsigned)psram_free_size(),
           (unsigned)psram_largest_free_block());
    fflush(stdout);
}
