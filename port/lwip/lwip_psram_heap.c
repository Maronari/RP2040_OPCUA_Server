#include "lwip_psram_heap.h"

#include <stddef.h>
#include <string.h>

#include "lwip/stats.h"
#include "lwipopts.h"
#include "psram_allocator.h"

/* lwIP aligns the supplied pointer and uses MEM_SIZE plus two allocator
 * headers. Keep explicit padding so this remains valid if header alignment
 * grows while the externally visible arena bound stays unchanged. */
#define LWIP_PSRAM_HEAP_PADDING_BYTES 64u
#define LWIP_PSRAM_HEAP_ALLOCATION_BYTES \
    (LWIP_OPCUA_SECURE_SEND_MIN + LWIP_PSRAM_HEAP_PADDING_BYTES)

unsigned char *g_lwip_ram_heap = NULL;

static uint16_t g_lwip_failed_allocations_raw;
static uint64_t g_lwip_failed_allocations;
static uint64_t g_lwip_failed_allocations_wrap_count;

static void lwip_extend_failure_counter(uint16_t raw)
{
    const uint16_t delta = (uint16_t)(raw - g_lwip_failed_allocations_raw);
    if (UINT64_MAX - g_lwip_failed_allocations < delta)
    {
        g_lwip_failed_allocations =
            delta - (UINT64_MAX - g_lwip_failed_allocations) - 1u;
        if (g_lwip_failed_allocations_wrap_count != UINT64_MAX)
        {
            ++g_lwip_failed_allocations_wrap_count;
        }
    }
    else
    {
        g_lwip_failed_allocations += delta;
    }
    g_lwip_failed_allocations_raw = raw;
}

bool lwip_psram_heap_init(void)
{
    if (g_lwip_ram_heap != NULL)
    {
        return true;
    }
    if (!psram_allocator_is_ready())
    {
        return false;
    }

    g_lwip_ram_heap = (unsigned char *)psram_calloc(1u, LWIP_PSRAM_HEAP_ALLOCATION_BYTES);
    return g_lwip_ram_heap != NULL;
}

bool lwip_psram_heap_get_stats(lwip_psram_heap_stats_t *out_stats)
{
    if (out_stats == NULL)
    {
        return false;
    }
    memset(out_stats, 0, sizeof(*out_stats));
#if LWIP_STATS && MEM_STATS
    /* Aggregate lwIP stats are useful internally, but the bundled allocator
     * does not publish its free-list extents.  Keep the provider unsupported
     * until a tracked upstream/port extension can return the exact largest
     * block; never fabricate zero for the public contract. */
    out_stats->supported = false;
    out_stats->ready = g_lwip_ram_heap != NULL;
    out_stats->reserved_bytes = g_lwip_ram_heap != NULL ? LWIP_PSRAM_HEAP_ALLOCATION_BYTES : 0u;
    out_stats->used_bytes = lwip_stats.mem.used;
    out_stats->peak_used_bytes = lwip_stats.mem.max;
    if (!out_stats->ready)
    {
        return false;
    }
    lwip_extend_failure_counter((uint16_t)lwip_stats.mem.err);
    out_stats->failed_allocations = g_lwip_failed_allocations;
    out_stats->failed_allocations_wrap_count =
        g_lwip_failed_allocations_wrap_count;
    return false;
#else
    return false;
#endif
}
