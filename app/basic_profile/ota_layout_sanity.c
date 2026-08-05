#include "ota_layout.h"
#include "ota/ota_state.h"

_Static_assert(OTA_LAYOUT_BOOTLOADER_SIZE_BYTES % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES == 0u,
               "Bootloader region must be erase aligned");
_Static_assert(OTA_LAYOUT_BANK_A_SIZE_BYTES % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES == 0u,
               "Bank A must be erase aligned");
_Static_assert(OTA_LAYOUT_BANK_B_SIZE_BYTES % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES == 0u,
               "Bank B must be erase aligned");
_Static_assert(OTA_LAYOUT_MAINTENANCE_JOURNAL_SIZE_BYTES % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES == 0u,
               "Maintenance journal must be erase aligned");
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
_Static_assert(OTA_LAYOUT_MAINTENANCE_JOURNAL_ORIGIN ==
                   (OTA_LAYOUT_FLASH_XIP_BASE + OTA_LAYOUT_MAINTENANCE_JOURNAL_OFFSET_BYTES),
               "Maintenance journal origin/offset mismatch");
_Static_assert(OTA_LAYOUT_BANK_A_OFFSET_BYTES == 0x010000u &&
                   OTA_LAYOUT_BANK_A_SIZE_BYTES == 0x7B4000u &&
                   OTA_LAYOUT_BANK_B_OFFSET_BYTES == 0x7C4000u &&
                   OTA_LAYOUT_BANK_B_SIZE_BYTES == 0x7B4000u &&
                   OTA_LAYOUT_MAINTENANCE_JOURNAL_OFFSET_BYTES == 0xF78000u &&
                   OTA_LAYOUT_MAINTENANCE_JOURNAL_SIZE_BYTES == 0x8000u,
               "Reference Flash bank/journal geometry changed");
#define OTA_LAYOUT_END(_offset, _size) ((_offset) + (_size))
#define OTA_LAYOUT_ASSERT_OFFSET_ALIGNED(_name) \
    _Static_assert((_name##_OFFSET_BYTES % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES) == 0u, \
                   #_name " offset must be erase aligned")
OTA_LAYOUT_ASSERT_OFFSET_ALIGNED(OTA_LAYOUT_BOOTLOADER);
OTA_LAYOUT_ASSERT_OFFSET_ALIGNED(OTA_LAYOUT_BANK_A);
OTA_LAYOUT_ASSERT_OFFSET_ALIGNED(OTA_LAYOUT_BANK_B);
OTA_LAYOUT_ASSERT_OFFSET_ALIGNED(OTA_LAYOUT_MAINTENANCE_JOURNAL);
OTA_LAYOUT_ASSERT_OFFSET_ALIGNED(OTA_LAYOUT_CONFIG_SLOT_0);
OTA_LAYOUT_ASSERT_OFFSET_ALIGNED(OTA_LAYOUT_CONFIG_SLOT_1);
OTA_LAYOUT_ASSERT_OFFSET_ALIGNED(OTA_LAYOUT_SECURITY_IDENTITY_SLOT_0);
OTA_LAYOUT_ASSERT_OFFSET_ALIGNED(OTA_LAYOUT_SECURITY_IDENTITY_SLOT_1);
OTA_LAYOUT_ASSERT_OFFSET_ALIGNED(OTA_LAYOUT_SECURITY_TRUST_SLOT_0);
OTA_LAYOUT_ASSERT_OFFSET_ALIGNED(OTA_LAYOUT_SECURITY_TRUST_SLOT_1);
OTA_LAYOUT_ASSERT_OFFSET_ALIGNED(OTA_LAYOUT_SECURITY_STATE);
OTA_LAYOUT_ASSERT_OFFSET_ALIGNED(OTA_LAYOUT_STATE);
_Static_assert(OTA_LAYOUT_END(OTA_LAYOUT_BOOTLOADER_OFFSET_BYTES, OTA_LAYOUT_BOOTLOADER_SIZE_BYTES) <=
                   OTA_LAYOUT_BANK_A_OFFSET_BYTES,
               "Bootloader overlaps Bank A");
_Static_assert(OTA_LAYOUT_END(OTA_LAYOUT_BANK_A_OFFSET_BYTES, OTA_LAYOUT_BANK_A_SIZE_BYTES) <=
                   OTA_LAYOUT_BANK_B_OFFSET_BYTES,
               "Bank A overlaps Bank B");
_Static_assert(OTA_LAYOUT_END(OTA_LAYOUT_BANK_B_OFFSET_BYTES, OTA_LAYOUT_BANK_B_SIZE_BYTES) <=
                   OTA_LAYOUT_MAINTENANCE_JOURNAL_OFFSET_BYTES,
               "Bank B overlaps maintenance journal");
_Static_assert(OTA_LAYOUT_END(OTA_LAYOUT_MAINTENANCE_JOURNAL_OFFSET_BYTES,
                             OTA_LAYOUT_MAINTENANCE_JOURNAL_SIZE_BYTES) <=
                   OTA_LAYOUT_CONFIG_SLOT_0_OFFSET_BYTES,
               "Maintenance journal overlaps configuration");
_Static_assert(OTA_LAYOUT_END(OTA_LAYOUT_CONFIG_SLOT_0_OFFSET_BYTES, OTA_LAYOUT_CONFIG_SLOT_SIZE_BYTES) <=
                   OTA_LAYOUT_CONFIG_SLOT_1_OFFSET_BYTES,
               "Configuration slots overlap");
_Static_assert(OTA_LAYOUT_END(OTA_LAYOUT_CONFIG_SLOT_1_OFFSET_BYTES, OTA_LAYOUT_CONFIG_SLOT_SIZE_BYTES) <=
                   OTA_LAYOUT_SECURITY_IDENTITY_SLOT_0_OFFSET_BYTES,
               "Configuration overlaps identity");
_Static_assert(OTA_LAYOUT_END(OTA_LAYOUT_SECURITY_IDENTITY_SLOT_0_OFFSET_BYTES,
                             OTA_LAYOUT_SECURITY_IDENTITY_SLOT_SIZE_BYTES) <=
                   OTA_LAYOUT_SECURITY_IDENTITY_SLOT_1_OFFSET_BYTES,
               "Identity slots overlap");
_Static_assert(OTA_LAYOUT_END(OTA_LAYOUT_SECURITY_IDENTITY_SLOT_1_OFFSET_BYTES,
                             OTA_LAYOUT_SECURITY_IDENTITY_SLOT_SIZE_BYTES) <=
                   OTA_LAYOUT_SECURITY_TRUST_SLOT_0_OFFSET_BYTES,
               "Identity overlaps trust");
_Static_assert(OTA_LAYOUT_END(OTA_LAYOUT_SECURITY_TRUST_SLOT_0_OFFSET_BYTES,
                             OTA_LAYOUT_SECURITY_TRUST_SLOT_SIZE_BYTES) <=
                   OTA_LAYOUT_SECURITY_TRUST_SLOT_1_OFFSET_BYTES,
               "Trust slots overlap");
_Static_assert(OTA_LAYOUT_END(OTA_LAYOUT_SECURITY_TRUST_SLOT_1_OFFSET_BYTES,
                             OTA_LAYOUT_SECURITY_TRUST_SLOT_SIZE_BYTES) <=
                   OTA_LAYOUT_SECURITY_STATE_OFFSET_BYTES,
               "Trust overlaps security state");
_Static_assert(OTA_LAYOUT_END(OTA_LAYOUT_SECURITY_STATE_OFFSET_BYTES,
                             OTA_LAYOUT_SECURITY_STATE_SIZE_BYTES) <=
                   OTA_LAYOUT_STATE_OFFSET_BYTES,
               "Security state overlaps OTA state");
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
