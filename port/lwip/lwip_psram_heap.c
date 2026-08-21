#include "lwip_psram_heap.h"

#include <stddef.h>

#include "lwipopts.h"
#include "psram_allocator.h"

/* lwIP aligns the supplied pointer and uses MEM_SIZE plus two allocator
 * headers. Keep explicit padding so this remains valid if header alignment
 * grows while the externally visible arena bound stays unchanged. */
#define LWIP_PSRAM_HEAP_PADDING_BYTES 64u
#define LWIP_PSRAM_HEAP_ALLOCATION_BYTES \
    (LWIP_OPCUA_SECURE_SEND_MIN + LWIP_PSRAM_HEAP_PADDING_BYTES)

unsigned char *g_lwip_ram_heap = NULL;

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
