#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "hardware/watchdog.h"
#include "ota/ota_state.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "ota_layout.h"

#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

#define BOOTLOADER_RAM_START 0x20000000u
#define BOOTLOADER_RAM_END   0x20082000u

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

static bool bootloader_is_valid_bank_a_image(uint32_t *out_stack_pointer, uint32_t *out_reset_handler)
{
    const uint32_t *const vectors = (const uint32_t *)OTA_LAYOUT_BANK_A_ORIGIN;
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

static void bootloader_print_ota_state(void)
{
    const ota_state_record_t *record = (const ota_state_record_t *)OTA_LAYOUT_STATE_ORIGIN;

    if (!ota_state_validate(record))
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
    printf("[BOOT] state region:    0x%08lx + 0x%08lx\n",
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
    uint32_t stack_pointer = 0u;
    uint32_t reset_handler = 0u;
    const bool has_valid_bank_a = bootloader_is_valid_bank_a_image(&stack_pointer, &reset_handler);

    stdio_init_all();
    sleep_ms(250);
    multicore_reset_core1();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 0);

    if (watchdog_caused_reboot())
    {
        printf("[BOOT] reboot reason: watchdog\n");
        fflush(stdout);
    }

    bootloader_print_banner();
    bootloader_print_ota_state();

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