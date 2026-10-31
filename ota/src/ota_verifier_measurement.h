#ifndef OTA_VERIFIER_MEASUREMENT_H
#define OTA_VERIFIER_MEASUREMENT_H

#include <stddef.h>

typedef struct {
    size_t current_payload_bytes;
    size_t peak_payload_bytes;
    size_t current_header_bytes;
    size_t peak_header_bytes;
} ota_verifier_allocator_measurement_t;

#if defined(OTA_VERIFIER_MEASUREMENT)
void ota_verifier_allocator_measurement_reset(void);
void ota_verifier_allocator_measurement_get(ota_verifier_allocator_measurement_t *out);
#endif

#endif /* OTA_VERIFIER_MEASUREMENT_H */
