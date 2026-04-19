#include "ota_layout.h"
#include "ota/ota_state.h"

_Static_assert(OTA_LAYOUT_BOOTLOADER_SIZE_BYTES % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES == 0u,
               "Bootloader region must be erase aligned");
_Static_assert(OTA_LAYOUT_BANK_A_SIZE_BYTES % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES == 0u,
               "Bank A must be erase aligned");
_Static_assert(OTA_LAYOUT_BANK_B_SIZE_BYTES % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES == 0u,
               "Bank B must be erase aligned");
_Static_assert(OTA_LAYOUT_CONFIG_SLOT_SIZE_BYTES % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES == 0u,
               "Config slots must be erase aligned");
_Static_assert(OTA_LAYOUT_SECURITY_IDENTITY_SLOT_SIZE_BYTES % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES == 0u,
               "Security identity slots must be erase aligned");
_Static_assert(OTA_LAYOUT_SECURITY_TRUST_SLOT_SIZE_BYTES % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES == 0u,
               "Security trust slots must be erase aligned");
_Static_assert(OTA_LAYOUT_SECURITY_STATE_SIZE_BYTES % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES == 0u,
               "Security state region must be erase aligned");
_Static_assert(OTA_LAYOUT_STATE_SIZE_BYTES % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES == 0u,
               "State region must be erase aligned");
_Static_assert(OTA_LAYOUT_BANK_A_ORIGIN == (OTA_LAYOUT_FLASH_XIP_BASE + OTA_LAYOUT_BANK_A_OFFSET_BYTES),
               "Bank A origin/offset mismatch");
_Static_assert(OTA_LAYOUT_BANK_B_ORIGIN == (OTA_LAYOUT_FLASH_XIP_BASE + OTA_LAYOUT_BANK_B_OFFSET_BYTES),
               "Bank B origin/offset mismatch");
_Static_assert(OTA_LAYOUT_CONFIG_SLOT_0_ORIGIN == (OTA_LAYOUT_FLASH_XIP_BASE + OTA_LAYOUT_CONFIG_SLOT_0_OFFSET_BYTES),
               "Config slot 0 origin/offset mismatch");
_Static_assert(OTA_LAYOUT_CONFIG_SLOT_1_ORIGIN == (OTA_LAYOUT_FLASH_XIP_BASE + OTA_LAYOUT_CONFIG_SLOT_1_OFFSET_BYTES),
               "Config slot 1 origin/offset mismatch");
_Static_assert(OTA_LAYOUT_SECURITY_IDENTITY_SLOT_0_ORIGIN ==
                   (OTA_LAYOUT_FLASH_XIP_BASE + OTA_LAYOUT_SECURITY_IDENTITY_SLOT_0_OFFSET_BYTES),
               "Security identity slot 0 origin/offset mismatch");
_Static_assert(OTA_LAYOUT_SECURITY_IDENTITY_SLOT_1_ORIGIN ==
                   (OTA_LAYOUT_FLASH_XIP_BASE + OTA_LAYOUT_SECURITY_IDENTITY_SLOT_1_OFFSET_BYTES),
               "Security identity slot 1 origin/offset mismatch");
_Static_assert(OTA_LAYOUT_SECURITY_TRUST_SLOT_0_ORIGIN ==
                   (OTA_LAYOUT_FLASH_XIP_BASE + OTA_LAYOUT_SECURITY_TRUST_SLOT_0_OFFSET_BYTES),
               "Security trust slot 0 origin/offset mismatch");
_Static_assert(OTA_LAYOUT_SECURITY_TRUST_SLOT_1_ORIGIN ==
                   (OTA_LAYOUT_FLASH_XIP_BASE + OTA_LAYOUT_SECURITY_TRUST_SLOT_1_OFFSET_BYTES),
               "Security trust slot 1 origin/offset mismatch");
_Static_assert(OTA_LAYOUT_SECURITY_STATE_ORIGIN ==
                   (OTA_LAYOUT_FLASH_XIP_BASE + OTA_LAYOUT_SECURITY_STATE_OFFSET_BYTES),
               "Security state origin/offset mismatch");
_Static_assert(OTA_LAYOUT_STATE_ORIGIN == (OTA_LAYOUT_FLASH_XIP_BASE + OTA_LAYOUT_STATE_OFFSET_BYTES),
               "State origin/offset mismatch");
_Static_assert((OTA_LAYOUT_STATE_OFFSET_BYTES + OTA_LAYOUT_STATE_SIZE_BYTES) == OTA_LAYOUT_FLASH_TOTAL_SIZE_BYTES,
               "OTA layout must span the configured flash size exactly");
_Static_assert(OTA_LAYOUT_SECURITY_STORE_SIZE_BYTES >= sizeof(uint32_t),
               "Legacy security store region must remain available during migration");
_Static_assert(sizeof(ota_state_record_t) <= OTA_LAYOUT_STATE_SIZE_BYTES,
               "OTA state record must fit into the reserved state region");
