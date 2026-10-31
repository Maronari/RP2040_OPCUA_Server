#ifndef OTA_VERIFIER_PLATFORM_H
#define OTA_VERIFIER_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The application/bootloader supplies this small ownership boundary.  It must
 * acquire without waiting, return a single exclusive arena, and wipe/free it
 * only after ota_verifier_platform_release().
 */
typedef enum {
    OTA_VERIFIER_PLATFORM_READY = 0,
    OTA_VERIFIER_PLATFORM_BUSY,
    OTA_VERIFIER_PLATFORM_ALLOCATION_FAILED,
} ota_verifier_platform_result_t;

ota_verifier_platform_result_t ota_verifier_platform_acquire(uint8_t **out_arena,
                                                              size_t *out_arena_bytes);
void ota_verifier_platform_release(uint8_t *arena, size_t arena_bytes);

#endif /* OTA_VERIFIER_PLATFORM_H */
