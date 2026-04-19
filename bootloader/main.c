#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "hardware/watchdog.h"
#include "ota/ota_flash.h"
#include "ota/ota_state.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "ota_layout.h"

#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

#define BOOTLOADER_RAM_START 0x20000000u
#define BOOTLOADER_RAM_END   0x20082000u

typedef struct {
    uint32_t flash_offset;
    size_t byte_count;
} bootloader_flash_erase_context_t;

typedef struct {
    uint32_t flash_offset;
    const uint8_t *data;
    size_t byte_count;
} bootloader_flash_program_context_t;

static uint8_t g_bootloader_copy_page[OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES];

typedef void (*bootloader_entry_fn_t)(void);

static inline void bootloader_disable_interrupts(void)
{
    __asm volatile("cpsid i" : : : "memory");
}

static inline void bootloader_data_sync_barrier(void)
{
    __asm volatile("dsb 0xF" : : : "memory");
}

static inline void bootloader_instruction_sync_barrier(void)
{
    __asm volatile("isb 0xF" : : : "memory");
}

static inline void bootloader_set_main_stack_pointer(uint32_t stack_pointer)
{
    __asm volatile("msr msp, %0" : : "r"(stack_pointer) : "memory");
}

static bool bootloader_is_valid_stack_pointer(uint32_t stack_pointer)
{
    return stack_pointer >= BOOTLOADER_RAM_START && stack_pointer <= BOOTLOADER_RAM_END && (stack_pointer & 0x7u) == 0u;
}

static bool bootloader_is_valid_reset_handler(uint32_t reset_handler)
{
    if ((reset_handler & 0x1u) == 0u)
    {
        return false;
    }

    reset_handler &= ~0x1u;
    return reset_handler >= OTA_LAYOUT_BANK_A_ORIGIN &&
           reset_handler < (OTA_LAYOUT_BANK_A_ORIGIN + OTA_LAYOUT_BANK_A_SIZE_BYTES);
}

static uint32_t bootloader_round_up(uint32_t value, uint32_t alignment)
{
    if (alignment == 0u)
    {
        return 0u;
    }

    return (uint32_t)(((value + alignment - 1u) / alignment) * alignment);
}

static bool bootloader_is_valid_image_vectors(const uint32_t *vectors,
                                              uint32_t *out_stack_pointer,
                                              uint32_t *out_reset_handler)
{
    const uint32_t stack_pointer = vectors[0];
    const uint32_t reset_handler = vectors[1];

    if (out_stack_pointer != NULL)
    {
        *out_stack_pointer = stack_pointer;
    }
    if (out_reset_handler != NULL)
    {
        *out_reset_handler = reset_handler;
    }

    return bootloader_is_valid_stack_pointer(stack_pointer) &&
           bootloader_is_valid_reset_handler(reset_handler);
}

static bool bootloader_is_valid_bank_a_image(uint32_t *out_stack_pointer, uint32_t *out_reset_handler)
{
    const uint32_t *const vectors = (const uint32_t *)OTA_LAYOUT_BANK_A_ORIGIN;

    return bootloader_is_valid_image_vectors(vectors, out_stack_pointer, out_reset_handler);
}

static void __not_in_flash_func(bootloader_flash_erase_range)(void *param)
{
    const bootloader_flash_erase_context_t *context = (const bootloader_flash_erase_context_t *)param;

    flash_range_erase(context->flash_offset, context->byte_count);
}

static void __not_in_flash_func(bootloader_flash_program_range)(void *param)
{
    const bootloader_flash_program_context_t *context = (const bootloader_flash_program_context_t *)param;

    flash_range_program(context->flash_offset, context->data, context->byte_count);
}

static ota_flash_status_t bootloader_execute_erase(uint32_t flash_offset, size_t byte_count)
{
    bootloader_flash_erase_context_t context = {
        .flash_offset = flash_offset,
        .byte_count = byte_count,
    };

    if (byte_count == 0u || (byte_count % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES) != 0u)
    {
        return OTA_FLASH_STATUS_INVALID_ARGUMENT;
    }

    if (flash_safe_execute(bootloader_flash_erase_range, &context, 1000u) != PICO_OK)
    {
        return OTA_FLASH_STATUS_FLASH_OP_FAILED;
    }

    return OTA_FLASH_STATUS_OK;
}

static ota_flash_status_t bootloader_execute_program(uint32_t flash_offset,
                                                     const uint8_t *data,
                                                     size_t byte_count)
{
    bootloader_flash_program_context_t context = {
        .flash_offset = flash_offset,
        .data = data,
        .byte_count = byte_count,
    };

    if (data == NULL || byte_count == 0u || (byte_count % OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES) != 0u)
    {
        return OTA_FLASH_STATUS_INVALID_ARGUMENT;
    }

    if (flash_safe_execute(bootloader_flash_program_range, &context, 1000u) != PICO_OK)
    {
        return OTA_FLASH_STATUS_FLASH_OP_FAILED;
    }

    return OTA_FLASH_STATUS_OK;
}

static ota_image_error_t bootloader_map_flash_status_to_error(ota_flash_status_t status)
{
    switch (status)
    {
    case OTA_FLASH_STATUS_VERIFY_FAILED:
        return OTA_IMAGE_ERROR_VERIFY_FAILED;
    case OTA_FLASH_STATUS_OK:
        return OTA_IMAGE_ERROR_NONE;
    case OTA_FLASH_STATUS_INVALID_ARGUMENT:
    case OTA_FLASH_STATUS_OUT_OF_RANGE:
        return OTA_IMAGE_ERROR_INVALID_STAGED_IMAGE;
    case OTA_FLASH_STATUS_FLASH_OP_FAILED:
    case OTA_FLASH_STATUS_WRITER_STATE_ERROR:
    default:
        return OTA_IMAGE_ERROR_COPY_FAILED;
    }
}

static bool bootloader_state_needs_copy(uint16_t state)
{
    return state == OTA_IMAGE_STATE_STAGED_IN_BANK_B ||
           state == OTA_IMAGE_STATE_COPYING_TO_BANK_A;
}

static bool bootloader_state_ready_to_boot(uint16_t state)
{
    return state == OTA_IMAGE_STATE_READY_TO_BOOT;
}

static bool bootloader_validate_state_record_layout(const ota_state_record_t *record)
{
    return record != NULL &&
           record->image_size > 0u &&
           record->image_size <= OTA_LAYOUT_BANK_B_SIZE_BYTES &&
           record->staged_bank_offset == OTA_LAYOUT_BANK_B_OFFSET_BYTES &&
           record->staged_bank_size == OTA_LAYOUT_BANK_B_SIZE_BYTES;
}

static bool bootloader_validate_staged_image(const ota_state_record_t *record)
{
    const uint32_t *const staged_vectors = (const uint32_t *)OTA_LAYOUT_BANK_B_ORIGIN;

    return bootloader_validate_state_record_layout(record) &&
           bootloader_is_valid_image_vectors(staged_vectors, NULL, NULL) &&
           ota_flash_verify_bank_b(record->image_size, record->image_crc32) == OTA_FLASH_STATUS_OK;
}

static ota_flash_status_t bootloader_write_state_record(ota_state_record_t *record,
                                                        ota_image_state_t state,
                                                        ota_image_error_t error)
{
    if (record == NULL)
    {
        return OTA_FLASH_STATUS_INVALID_ARGUMENT;
    }

    record->state = (uint16_t)state;
    record->last_error = (uint32_t)error;
    return ota_flash_write_state(record);
}

static ota_flash_status_t bootloader_copy_bank_b_to_bank_a(const ota_state_record_t *record)
{
    const uint8_t *const staged_base = (const uint8_t *)OTA_LAYOUT_BANK_B_ORIGIN;
    const uint32_t erase_bytes = bootloader_round_up(record->image_size, OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES);
    const uint32_t program_bytes = bootloader_round_up(record->image_size, OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES);
    ota_flash_status_t status;

    status = bootloader_execute_erase(OTA_LAYOUT_BANK_A_OFFSET_BYTES, erase_bytes);
    if (status != OTA_FLASH_STATUS_OK)
    {
        return status;
    }

    for (uint32_t offset = 0u; offset < program_bytes; offset += OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES)
    {
        size_t chunk_size = OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES;

        if (offset + chunk_size > record->image_size)
        {
            chunk_size = (offset < record->image_size) ? (size_t)(record->image_size - offset) : 0u;
        }

        memset(g_bootloader_copy_page, 0xFF, sizeof(g_bootloader_copy_page));
        if (chunk_size > 0u)
        {
            memcpy(g_bootloader_copy_page, staged_base + offset, chunk_size);
        }

        status = bootloader_execute_program(OTA_LAYOUT_BANK_A_OFFSET_BYTES + offset,
                                            g_bootloader_copy_page,
                                            sizeof(g_bootloader_copy_page));
        if (status != OTA_FLASH_STATUS_OK)
        {
            return status;
        }
    }

    if (ota_crc32_compute((const void *)OTA_LAYOUT_BANK_A_ORIGIN, record->image_size) != record->image_crc32)
    {
        return OTA_FLASH_STATUS_VERIFY_FAILED;
    }

    if (!bootloader_is_valid_bank_a_image(NULL, NULL))
    {
        return OTA_FLASH_STATUS_VERIFY_FAILED;
    }

    return OTA_FLASH_STATUS_OK;
}

static bool bootloader_prepare_ready_state_for_boot(ota_state_record_t *record)
{
    ota_flash_status_t status;

    if (record == NULL || !bootloader_state_ready_to_boot(record->state))
    {
        return false;
    }

    record->boot_attempts++;
    status = ota_flash_write_state(record);
    if (status != OTA_FLASH_STATUS_OK)
    {
        return false;
    }

    return true;
}

static bool bootloader_record_pending_boot_failure(ota_state_record_t *record, bool watchdog_reboot)
{
    ota_flash_status_t status;

    if (record == NULL || !watchdog_reboot || !bootloader_state_ready_to_boot(record->state) || record->boot_attempts == 0u)
    {
        return false;
    }

    status = bootloader_write_state_record(record,
                                           OTA_IMAGE_STATE_FAILED,
                                           OTA_IMAGE_ERROR_BOOT_FAILED);
    return status == OTA_FLASH_STATUS_OK;
}

static bool bootloader_apply_pending_update(ota_state_record_t *record)
{
    ota_flash_status_t status;

    if (record == NULL || !bootloader_state_needs_copy(record->state))
    {
        return false;
    }

    if (!bootloader_validate_staged_image(record))
    {
        record->boot_attempts = 0u;
        (void)bootloader_write_state_record(record,
                                            OTA_IMAGE_STATE_FAILED,
                                            OTA_IMAGE_ERROR_INVALID_STAGED_IMAGE);
        return false;
    }

    record->boot_attempts = 0u;
    status = bootloader_write_state_record(record,
                                           OTA_IMAGE_STATE_COPYING_TO_BANK_A,
                                           OTA_IMAGE_ERROR_NONE);
    if (status != OTA_FLASH_STATUS_OK)
    {
        return false;
    }

    status = bootloader_copy_bank_b_to_bank_a(record);
    if (status != OTA_FLASH_STATUS_OK)
    {
        (void)bootloader_write_state_record(record,
                                            OTA_IMAGE_STATE_FAILED,
                                            bootloader_map_flash_status_to_error(status));
        return false;
    }

    status = bootloader_write_state_record(record,
                                           OTA_IMAGE_STATE_READY_TO_BOOT,
                                           OTA_IMAGE_ERROR_NONE);
    return status == OTA_FLASH_STATUS_OK;
}

static void bootloader_print_ota_state(void)
{
    
    ota_state_record_t current_record;
    const ota_state_record_t *record = &current_record;

    if (!ota_flash_read_state(&current_record))
    {
        printf("[BOOT] ota state: invalid or empty\n");
        fflush(stdout);
        return;
    }

    printf("[BOOT] ota state: %s\n", ota_state_to_string(record->state));
    printf("[BOOT] image size: 0x%08lx crc32=0x%08lx\n",
           (unsigned long)record->image_size,
           (unsigned long)record->image_crc32);
    printf("[BOOT] staged bank: offset=0x%08lx size=0x%08lx\n",
           (unsigned long)record->staged_bank_offset,
           (unsigned long)record->staged_bank_size);
    printf("[BOOT] attempts=%lu version=%lu last_error=%s\n",
           (unsigned long)record->boot_attempts,
           (unsigned long)record->version_number,
           ota_error_to_string(record->last_error));
    fflush(stdout);
}

static void bootloader_print_banner(void)
{
    printf("[BOOT] bootloader online\n");
    printf("[BOOT] bootloader:      0x%08lx + 0x%08lx\n",
           (unsigned long)OTA_LAYOUT_BOOTLOADER_ORIGIN,
           (unsigned long)OTA_LAYOUT_BOOTLOADER_SIZE_BYTES);
    printf("[BOOT] bank A:          0x%08lx + 0x%08lx\n",
           (unsigned long)OTA_LAYOUT_BANK_A_ORIGIN,
           (unsigned long)OTA_LAYOUT_BANK_A_SIZE_BYTES);
    printf("[BOOT] bank B:          0x%08lx + 0x%08lx\n",
           (unsigned long)OTA_LAYOUT_BANK_B_ORIGIN,
           (unsigned long)OTA_LAYOUT_BANK_B_SIZE_BYTES);
    printf("[BOOT] config slot 0:   0x%08lx + 0x%08lx\n",
           (unsigned long)OTA_LAYOUT_CONFIG_SLOT_0_ORIGIN,
           (unsigned long)OTA_LAYOUT_CONFIG_SLOT_SIZE_BYTES);
    printf("[BOOT] config slot 1:   0x%08lx + 0x%08lx\n",
           (unsigned long)OTA_LAYOUT_CONFIG_SLOT_1_ORIGIN,
           (unsigned long)OTA_LAYOUT_CONFIG_SLOT_SIZE_BYTES);
    printf("[BOOT] sec ident slot0: 0x%08lx + 0x%08lx\n",
           (unsigned long)OTA_LAYOUT_SECURITY_IDENTITY_SLOT_0_ORIGIN,
           (unsigned long)OTA_LAYOUT_SECURITY_IDENTITY_SLOT_SIZE_BYTES);
    printf("[BOOT] sec ident slot1: 0x%08lx + 0x%08lx\n",
           (unsigned long)OTA_LAYOUT_SECURITY_IDENTITY_SLOT_1_ORIGIN,
           (unsigned long)OTA_LAYOUT_SECURITY_IDENTITY_SLOT_SIZE_BYTES);
    printf("[BOOT] sec trust slot0: 0x%08lx + 0x%08lx\n",
           (unsigned long)OTA_LAYOUT_SECURITY_TRUST_SLOT_0_ORIGIN,
           (unsigned long)OTA_LAYOUT_SECURITY_TRUST_SLOT_SIZE_BYTES);
    printf("[BOOT] sec trust slot1: 0x%08lx + 0x%08lx\n",
           (unsigned long)OTA_LAYOUT_SECURITY_TRUST_SLOT_1_ORIGIN,
           (unsigned long)OTA_LAYOUT_SECURITY_TRUST_SLOT_SIZE_BYTES);
    printf("[BOOT] sec state:       0x%08lx + 0x%08lx\n",
           (unsigned long)OTA_LAYOUT_SECURITY_STATE_ORIGIN,
           (unsigned long)OTA_LAYOUT_SECURITY_STATE_SIZE_BYTES);
    printf("[BOOT] ota state:       0x%08lx + 0x%08lx\n",
           (unsigned long)OTA_LAYOUT_STATE_ORIGIN,
           (unsigned long)OTA_LAYOUT_STATE_SIZE_BYTES);
    fflush(stdout);
}

static void __attribute__((noreturn)) bootloader_jump_to_bank_a(uint32_t stack_pointer, uint32_t reset_handler)
{
    printf("[BOOT] handing off to bank A reset=0x%08lx sp=0x%08lx\n",
           (unsigned long)reset_handler,
           (unsigned long)stack_pointer);
    fflush(stdout);
    sleep_ms(50);

    bootloader_disable_interrupts();
    multicore_reset_core1();
    systick_hw->csr = 0u;
    systick_hw->rvr = 0u;
    systick_hw->cvr = 0u;
    scb_hw->vtor = OTA_LAYOUT_BANK_A_ORIGIN;
    bootloader_data_sync_barrier();
    bootloader_instruction_sync_barrier();
    bootloader_set_main_stack_pointer(stack_pointer);
    bootloader_data_sync_barrier();
    bootloader_instruction_sync_barrier();

    ((bootloader_entry_fn_t)reset_handler)();
    while (true)
    {
        tight_loop_contents();
    }
}

int main(void)
{
    ota_state_record_t ota_record;
    bool has_ota_record = false;
    bool has_valid_bank_a = false;
    uint32_t stack_pointer = 0u;
    uint32_t reset_handler = 0u;
    const bool watchdog_reboot = watchdog_caused_reboot();

    stdio_init_all();
    sleep_ms(250);
    multicore_reset_core1();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 0);

    if (watchdog_reboot)
    {
        printf("[BOOT] reboot reason: watchdog\n");
        fflush(stdout);
    }

    bootloader_print_banner();
    bootloader_print_ota_state();

    has_ota_record = ota_flash_read_state(&ota_record);
    if (has_ota_record)
    {
        (void)bootloader_record_pending_boot_failure(&ota_record, watchdog_reboot);
        (void)bootloader_apply_pending_update(&ota_record);
        (void)bootloader_prepare_ready_state_for_boot(&ota_record);
    }

    has_valid_bank_a = bootloader_is_valid_bank_a_image(&stack_pointer, &reset_handler);

    if (has_valid_bank_a)
    {
        bootloader_jump_to_bank_a(stack_pointer, reset_handler);
    }

    printf("[BOOT] no valid bank A image, staying in bootloader\n");
    printf("[BOOT] observed reset=0x%08lx sp=0x%08lx\n",
           (unsigned long)reset_handler,
           (unsigned long)stack_pointer);
    fflush(stdout);

    while (true)
    {
        gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
        sleep_ms(250);
    }
}
