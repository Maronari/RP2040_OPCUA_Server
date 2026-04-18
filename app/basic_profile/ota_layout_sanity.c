#include "ota_layout.h"
#include "ota/ota_state.h"

_Static_assert(OTA_LAYOUT_BOOTLOADER_SIZE_BYTES % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES == 0u,
               "Bootloader region must be erase aligned");
_Static_assert(OTA_LAYOUT_BANK_A_SIZE_BYTES % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES == 0u,
               "Bank A must be erase aligned");
_Static_assert(OTA_LAYOUT_BANK_B_SIZE_BYTES % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES == 0u,
               "Bank B must be erase aligned");
_Static_assert(OTA_LAYOUT_STATE_SIZE_BYTES % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES == 0u,
               "State region must be erase aligned");
_Static_assert(OTA_LAYOUT_BANK_A_ORIGIN == (OTA_LAYOUT_FLASH_XIP_BASE + OTA_LAYOUT_BANK_A_OFFSET_BYTES),
               "Bank A origin/offset mismatch");
_Static_assert(OTA_LAYOUT_BANK_B_ORIGIN == (OTA_LAYOUT_FLASH_XIP_BASE + OTA_LAYOUT_BANK_B_OFFSET_BYTES),
               "Bank B origin/offset mismatch");
_Static_assert(OTA_LAYOUT_STATE_ORIGIN == (OTA_LAYOUT_FLASH_XIP_BASE + OTA_LAYOUT_STATE_OFFSET_BYTES),
               "State origin/offset mismatch");
_Static_assert((OTA_LAYOUT_STATE_OFFSET_BYTES + OTA_LAYOUT_STATE_SIZE_BYTES) == OTA_LAYOUT_FLASH_TOTAL_SIZE_BYTES,
               "OTA layout must span the configured flash size exactly");
_Static_assert(sizeof(ota_state_record_t) <= OTA_LAYOUT_STATE_SIZE_BYTES,
               "OTA state record must fit into the reserved state region");