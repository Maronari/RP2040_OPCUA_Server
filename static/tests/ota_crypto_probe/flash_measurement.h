#ifndef V060_T0_FLASH_MEASUREMENT_H
#define V060_T0_FLASH_MEASUREMENT_H

#include <stdbool.h>

/* Fixed legacy staging-sector diagnostic; no caller-configurable offset exists. */
bool ota_probe_flash_measurement_canonical_idle(void);
bool ota_probe_flash_measurement_target_is_erased(void);
bool ota_probe_flash_measurement_run(void);

#endif /* V060_T0_FLASH_MEASUREMENT_H */
