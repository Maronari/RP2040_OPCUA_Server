#include "ota_verifier_platform.h"

#include "pico/mutex.h"
#include "psram_allocator.h"

#define OTA_VERIFIER_OPERATION_BYTES 65536u

auto_init_mutex(g_ota_verifier_mutex);

ota_verifier_platform_result_t ota_verifier_platform_acquire(
    uint8_t **out_arena, size_t *out_arena_bytes) {
    *out_arena = NULL;
    *out_arena_bytes = 0u;
    if (!mutex_try_enter(&g_ota_verifier_mutex, NULL)) {
        return OTA_VERIFIER_PLATFORM_BUSY;
    }
    /* Never borrow internal SRAM when PSRAM is unavailable. */
    if (!psram_allocator_is_ready()) {
        mutex_exit(&g_ota_verifier_mutex);
        return OTA_VERIFIER_PLATFORM_ALLOCATION_FAILED;
    }
    uint8_t *arena = psram_malloc(OTA_VERIFIER_OPERATION_BYTES);
    if (arena == NULL) {
        mutex_exit(&g_ota_verifier_mutex);
        return OTA_VERIFIER_PLATFORM_ALLOCATION_FAILED;
    }
    *out_arena = arena;
    *out_arena_bytes = OTA_VERIFIER_OPERATION_BYTES;
    return OTA_VERIFIER_PLATFORM_READY;
}

void ota_verifier_platform_release(uint8_t *arena, size_t arena_bytes) {
    /* Volatile stores preserve the wipe even under link-time optimization. */
    volatile uint8_t *bytes = arena;
    for (size_t i = 0u; i < arena_bytes; ++i) {
        bytes[i] = 0u;
    }
    psram_free(arena);
    mutex_exit(&g_ota_verifier_mutex);
}
