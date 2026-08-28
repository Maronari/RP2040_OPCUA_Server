/**
 * @file
 * @brief First-stage bootloader entry point. Decides, on every reset,
 *        whether to apply a staged OTA update (copy Bank B -> Bank A),
 *        detects a failed post-update boot via the watchdog, and finally
 *        jumps into the application image in Bank A (or hangs blinking an
 *        LED if no valid image is found).
 *
 * Responsibilities:
 * - Own the bootloader-side implementation of the `ota_flash_backend.h`
 *   interface, driving Flash erase/program directly through
 *   `pico/flash.h`'s `flash_safe_execute()` (no RTOS, no shared
 *   FreeRTOS-mutex flash-serialization layer).
 * - Apply any OTA update staged in Bank B: validate it, copy it into
 *   Bank A page-by-page, and re-verify the copy before marking it
 *   `READY_TO_BOOT`.
 * - Detect a boot that previously reached `READY_TO_BOOT` but rebooted via
 *   watchdog before the application could acknowledge it, and mark that
 *   attempt `FAILED`.
 * - Validate Bank A's vector table and unconditionally jump to it if valid,
 *   independent of the OTA state record (see `bootloader_is_valid_bank_a_image()`).
 *
 * Ownership:
 * This is a single, bare-metal, pre-RTOS translation unit — every symbol
 * below except the two `ota_flash_backend_*` entry points is file-static.
 * `g_bootloader_copy_page` is a single reusable page-sized Flash staging
 * buffer for the Bank B -> Bank A copy loop.
 *
 * Concurrency:
 * Single-threaded, runs before any RTOS or interrupt-driven subsystem is
 * initialized (`multicore_reset_core1()` is called early to guarantee core 1
 * is quiesced). No mutexes/queues are used or needed. Flash operations use
 * `flash_safe_execute()` for the flash-critical-section discipline the Pico
 * SDK requires (disables interrupts / holds off the other core for the
 * duration), not for concurrency between application tasks.
 *
 * Safety-critical: bugs here can render the device unbootable (no field
 * recovery path other than reflashing via BOOTSEL/`picotool` — see
 * `docs/code/ota-bootloader.md`). See `docs/code/ota-bootloader.md` for the
 * full boot-decision flowchart and the documented gap that Bank A is
 * overwritten in place during a copy (no "last known good" fallback bank).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/regs/resets.h"
#include "hardware/resets.h"
#include "hardware/structs/nvic.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "hardware/watchdog.h"
#include "ota/ota_flash.h"
#include "ota/ota_state.h"
#include "ota_flash_backend.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "ota_layout.h"

#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

/** Valid range for a Cortex-M initial stack pointer read from an image's
 * vector table, used by `bootloader_is_valid_stack_pointer()` as a sanity
 * bound before trusting an image enough to jump to it. Covers the RP2350
 * SRAM address range. */
#define BOOTLOADER_RAM_START 0x20000000u
#define BOOTLOADER_RAM_END   0x20082000u

/** Parameter block passed through `flash_safe_execute()` to
 * `bootloader_flash_erase_range()`, since `flash_safe_execute()`'s callback
 * takes a single `void *` argument. */
typedef struct {
    uint32_t flash_offset;
    size_t byte_count;
} bootloader_flash_erase_context_t;

/** Parameter block passed through `flash_safe_execute()` to
 * `bootloader_flash_program_range()`; see
 * `bootloader_flash_erase_context_t`. */
typedef struct {
    uint32_t flash_offset;
    const uint8_t *data;
    size_t byte_count;
} bootloader_flash_program_context_t;

/** Single reusable page-sized staging buffer for
 * `bootloader_copy_bank_b_to_bank_a()`: each iteration copies one Bank B
 * page (padded with `0xFF` past the image's actual size) here before
 * programming it into Bank A. Not reentrant; safe only because the
 * bootloader is single-threaded. */
static uint8_t g_bootloader_copy_page[OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES];

/** Function-pointer type for an application's reset handler, invoked by
 * `bootloader_jump_to_bank_a()` to transfer control. The target address is
 * validated (`bootloader_is_valid_reset_handler()`) before being cast to
 * this type and called. */
typedef void (*bootloader_entry_fn_t)(void);

/**
 * @brief Masks all CPU interrupts (`cpsid i`) ahead of the irrevocable jump
 *        into the application.
 *
 * @note Never re-enabled by the bootloader itself; the application is
 *       expected to re-enable interrupts during its own startup.
 */
static inline void bootloader_disable_interrupts(void)
{
    __asm volatile("cpsid i" : : : "memory");
}

/**
 * @brief Full system data synchronization barrier (`dsb`), ensuring prior
 *        memory/peripheral-register writes (e.g. NVIC masking, VTOR update)
 *        have completed before subsequent instructions execute.
 */
static inline void bootloader_data_sync_barrier(void)
{
    __asm volatile("dsb 0xF" : : : "memory");
}

/**
 * @brief Instruction synchronization barrier (`isb`), flushing the pipeline
 *        so subsequently fetched instructions see the effects of preceding
 *        barriered writes (e.g. the updated VTOR/stack pointer before
 *        jumping to the application).
 */
static inline void bootloader_instruction_sync_barrier(void)
{
    __asm volatile("isb 0xF" : : : "memory");
}

/**
 * @brief Masks and clears all pending NVIC interrupts and holds USB
 *        (USBCTRL) in reset, so no bootloader-owned interrupt can fire
 *        against the application's not-yet-installed handlers after the
 *        jump.
 *
 * Called immediately before `bootloader_jump_to_bank_a()` hands off control.
 * Leaves USBCTRL in reset deliberately — the application's TinyUSB
 * initialization resets and releases the block itself before installing its
 * own handler, so the bootloader must not leave USB running unowned.
 *
 * @note Directly manipulates NVIC (`nvic_hw->icer`/`icpr`) and RESETS
 *       hardware registers; not safe to call after interrupts matter to any
 *       other in-flight bootloader activity (there is none at this point in
 *       `main()`).
 */
static void bootloader_quiesce_interrupt_sources(void)
{
    const size_t nvic_bank_count = (NUM_IRQS + 31u) / 32u;

    /* The application re-enables interrupts during its pre-main runtime
     * initialization. Do not let a bootloader USB or timer interrupt run
     * against the application's not-yet-installed peripheral handlers. */
    for (size_t bank = 0u; bank < nvic_bank_count; ++bank)
    {
        nvic_hw->icer[bank] = UINT32_MAX;
    }

    /* USB stdio is the only continuously active interrupt source used by the
     * bootloader. Leave USBCTRL in reset; application TinyUSB initialization
     * resets and releases the block before installing its handler. */
    reset_block(RESETS_RESET_USBCTRL_BITS);

    for (size_t bank = 0u; bank < nvic_bank_count; ++bank)
    {
        nvic_hw->icpr[bank] = UINT32_MAX;
    }

    bootloader_data_sync_barrier();
    bootloader_instruction_sync_barrier();
}

/**
 * @brief Loads the Cortex-M main stack pointer (MSP) register, switching
 *        the CPU to the application's initial stack immediately before the
 *        jump to its reset handler.
 *
 * @param stack_pointer New MSP value; caller must have already validated it
 *        via `bootloader_is_valid_stack_pointer()`.
 * @note Irreversible from the bootloader's point of view — the bootloader's
 *       own stack is abandoned by this call.
 */
static inline void bootloader_set_main_stack_pointer(uint32_t stack_pointer)
{
    __asm volatile("msr msp, %0" : : "r"(stack_pointer) : "memory");
}

/**
 * @brief Checks that a candidate initial stack pointer lies within SRAM and
 *        is 8-byte aligned, as the AAPCS/Cortex-M reset sequence requires.
 *
 * Part of the safety-critical vector-table validation gate: a corrupt or
 * garbage stack pointer here would crash immediately on jump, so this is
 * checked before ever transferring control (see
 * `bootloader_is_valid_image_vectors()`).
 *
 * @param stack_pointer Candidate value read from `vectors[0]` of a
 *        candidate image.
 * @return true if within `[BOOTLOADER_RAM_START, BOOTLOADER_RAM_END]` and
 *         8-byte aligned; false otherwise.
 */
static bool bootloader_is_valid_stack_pointer(uint32_t stack_pointer)
{
    return stack_pointer >= BOOTLOADER_RAM_START && stack_pointer <= BOOTLOADER_RAM_END && (stack_pointer & 0x7u) == 0u;
}

/**
 * @brief Checks that a candidate reset-handler address has the Thumb bit
 *        set and falls within Bank A's Flash address range.
 *
 * Part of the safety-critical vector-table validation gate (see
 * `bootloader_is_valid_image_vectors()`); this is what ultimately gates
 * `main()`'s decision to jump into Bank A at all.
 *
 * @param reset_handler Candidate value read from `vectors[1]` of a
 *        candidate image (still with the Thumb bit set, as stored in a
 *        vector table).
 * @return true if bit 0 (Thumb bit) is set and the (bit-0-cleared) address
 *         lies within `[OTA_LAYOUT_BANK_A_ORIGIN, OTA_LAYOUT_BANK_A_ORIGIN +
 *         OTA_LAYOUT_BANK_A_SIZE_BYTES)`; false otherwise.
 */
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

/**
 * @brief Rounds @p value up to the next multiple of @p alignment.
 *
 * Used to compute Flash erase/program byte counts covering an arbitrary
 * image size (mirrors `ota_flash_round_up()` in `ota/src/ota_flash.c`; this
 * bootloader translation unit does not link against that file).
 *
 * @param value Value to round up.
 * @param alignment Alignment to round to; must be non-zero.
 * @return @p value rounded up to a multiple of @p alignment, or 0 if
 *         @p alignment is 0.
 */
static uint32_t bootloader_round_up(uint32_t value, uint32_t alignment)
{
    if (alignment == 0u)
    {
        return 0u;
    }

    return (uint32_t)(((value + alignment - 1u) / alignment) * alignment);
}

/**
 * @brief Reads and validates the first two words (initial SP, reset
 *        handler) of a candidate image's vector table.
 *
 * The sole authority for whether an image is safe to jump to: composes
 * `bootloader_is_valid_stack_pointer()` and
 * `bootloader_is_valid_reset_handler()`. Always populates the out-params
 * (even on failure) so callers can log the observed values.
 *
 * @param vectors Pointer to the candidate image's vector table (Flash
 *        XIP-mapped memory), read directly — no bounds checking of
 *        @p vectors itself beyond what the caller guarantees.
 * @param out_stack_pointer Receives `vectors[0]`. May be NULL.
 * @param out_reset_handler Receives `vectors[1]`. May be NULL.
 * @return true if both the stack pointer and reset handler pass validation;
 *         false otherwise.
 */
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

/**
 * @brief Validates Bank A's vector table — the final authority for whether
 *        the bootloader will jump into the application.
 *
 * @param out_stack_pointer Receives Bank A's initial SP. May be NULL.
 * @param out_reset_handler Receives Bank A's reset handler address. May be
 *        NULL.
 * @return true if Bank A contains a structurally valid vector table; false
 *         otherwise. Deliberately independent of the OTA state record's
 *         `state` field — see `docs/code/ota-bootloader.md` boot-decision
 *         notes.
 */
static bool bootloader_is_valid_bank_a_image(uint32_t *out_stack_pointer, uint32_t *out_reset_handler)
{
    const uint32_t *const vectors = (const uint32_t *)OTA_LAYOUT_BANK_A_ORIGIN;

    return bootloader_is_valid_image_vectors(vectors, out_stack_pointer, out_reset_handler);
}

/**
 * @brief `flash_safe_execute()` callback that performs the actual Flash
 *        sector erase.
 *
 * Runs inside the Pico SDK's flash-critical section (interrupts disabled,
 * other core held off, XIP unavailable), hence `__not_in_flash_func` —
 * this function's own code must not be fetched from Flash while erasing it.
 *
 * @param param Pointer to a `bootloader_flash_erase_context_t` describing
 *        the erase range.
 */
static void __not_in_flash_func(bootloader_flash_erase_range)(void *param)
{
    const bootloader_flash_erase_context_t *context = (const bootloader_flash_erase_context_t *)param;

    flash_range_erase(context->flash_offset, context->byte_count);
}

/**
 * @brief `flash_safe_execute()` callback that performs the actual Flash
 *        page program.
 *
 * Same flash-critical-section / `__not_in_flash_func` constraint as
 * `bootloader_flash_erase_range()`.
 *
 * @param param Pointer to a `bootloader_flash_program_context_t` describing
 *        the destination and source data.
 */
static void __not_in_flash_func(bootloader_flash_program_range)(void *param)
{
    const bootloader_flash_program_context_t *context = (const bootloader_flash_program_context_t *)param;

    flash_range_program(context->flash_offset, context->data, context->byte_count);
}

/**
 * @brief Validates and executes a Flash sector erase via
 *        `flash_safe_execute()`.
 *
 * This is the bootloader's half of the `ota_flash_backend.h` contract for
 * erase (see `ota_flash_backend_erase()` below); unlike the application-side
 * backend (`ota_flash_backend_application.c`), there is no shared
 * FreeRTOS-mutex flash-serialization layer here — the bootloader is
 * single-threaded, so `flash_safe_execute()`'s own critical-section handling
 * is sufficient.
 *
 * @param flash_offset Flash offset to erase; must be erase-block aligned
 *        (checked by the underlying `flash_range_erase()`, not here).
 * @param byte_count Number of bytes to erase; must be a non-zero multiple
 *        of the Flash erase-block size.
 * @return `OTA_FLASH_STATUS_OK` on success; `OTA_FLASH_STATUS_INVALID_ARGUMENT`
 *         if @p byte_count is 0 or not erase-block aligned;
 *         `OTA_FLASH_STATUS_FLASH_OP_FAILED` if `flash_safe_execute()` does
 *         not return `PICO_OK` (e.g. the 1000 ms timeout waiting for the
 *         other core/interrupts to quiesce elapses).
 */
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

/**
 * @brief Validates and executes a Flash page program via
 *        `flash_safe_execute()`. Bootloader half of the
 *        `ota_flash_backend.h` program contract; see
 *        `bootloader_execute_erase()` for the erase half and its
 *        concurrency notes.
 *
 * @param flash_offset Destination Flash offset; must be program-page
 *        aligned. The target range must already be erased.
 * @param data Source bytes. Must not be NULL.
 * @param byte_count Number of bytes to program; must be a non-zero multiple
 *        of the Flash program-page size.
 * @return `OTA_FLASH_STATUS_OK` on success; `OTA_FLASH_STATUS_INVALID_ARGUMENT`
 *         if @p data is NULL or @p byte_count is 0/misaligned;
 *         `OTA_FLASH_STATUS_FLASH_OP_FAILED` on a `flash_safe_execute()`
 *         failure/timeout.
 */
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

/* Bootloader's implementation of the ota_flash_backend.h contract (see that
 * header for @param/@return semantics); delegates directly to
 * bootloader_execute_erase(), which uses flash_safe_execute() rather than
 * the application's shared FreeRTOS-mutex flash-serialization layer. */
ota_flash_status_t ota_flash_backend_erase(uint32_t flash_offset, size_t byte_count)
{
    return bootloader_execute_erase(flash_offset, byte_count);
}

/* See ota_flash_backend_erase() above; delegates to
 * bootloader_execute_program(). */
ota_flash_status_t ota_flash_backend_program(uint32_t flash_offset,
                                             const uint8_t *data,
                                             size_t byte_count)
{
    return bootloader_execute_program(flash_offset, data, byte_count);
}

/**
 * @brief Translates an `ota_flash_status_t` (low-level Flash result) into
 *        an `ota_image_error_t` (persisted OTA lifecycle error) for storage
 *        in `ota_state_record_t::last_error`.
 *
 * @param status Flash-layer status to translate.
 * @return `OTA_IMAGE_ERROR_NONE` for `OTA_FLASH_STATUS_OK`;
 *         `OTA_IMAGE_ERROR_VERIFY_FAILED` for a CRC/vector-table verify
 *         failure; `OTA_IMAGE_ERROR_INVALID_STAGED_IMAGE` for an
 *         invalid-argument/out-of-range condition; every other status
 *         (including unrecognized values) maps to
 *         `OTA_IMAGE_ERROR_COPY_FAILED`.
 */
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

/**
 * @brief True if @p state indicates a Bank B -> Bank A copy has not yet
 *        completed (staged but not yet copied, or interrupted mid-copy).
 *
 * `COPYING_TO_BANK_A` is included deliberately: if the bootloader reset
 * while a previous copy was in progress, the copy is simply retried from
 * the start on the next boot (Bank B is still intact; the copy is
 * idempotent).
 *
 * @param state Raw `ota_state_record_t::state` value.
 * @return true for `OTA_IMAGE_STATE_STAGED_IN_BANK_B` or
 *         `OTA_IMAGE_STATE_COPYING_TO_BANK_A`.
 */
static bool bootloader_state_needs_copy(uint16_t state)
{
    return state == OTA_IMAGE_STATE_STAGED_IN_BANK_B ||
           state == OTA_IMAGE_STATE_COPYING_TO_BANK_A;
}

/**
 * @brief True if @p state is `OTA_IMAGE_STATE_READY_TO_BOOT`.
 *
 * @param state Raw `ota_state_record_t::state` value.
 */
static bool bootloader_state_ready_to_boot(uint16_t state)
{
    return state == OTA_IMAGE_STATE_READY_TO_BOOT;
}

/**
 * @brief Sanity-checks a staged-image record's size/bank fields before any
 *        Flash content is trusted.
 *
 * Guards against a corrupted or stale record pointing at an
 * out-of-range/zero image size, or a `staged_bank_offset`/`staged_bank_size`
 * that no longer matches the current Bank B layout (e.g. after a firmware
 * rebuild changed the Flash partitioning).
 *
 * @param record Record to check.
 * @return true if @p record is non-NULL, `image_size` is in
 *         `(0, OTA_LAYOUT_BANK_B_SIZE_BYTES]`, and the staged bank
 *         offset/size exactly match the current Bank B region; false
 *         otherwise.
 */
static bool bootloader_validate_state_record_layout(const ota_state_record_t *record)
{
    return record != NULL &&
           record->image_size > 0u &&
           record->image_size <= OTA_LAYOUT_BANK_B_SIZE_BYTES &&
           record->staged_bank_offset == OTA_LAYOUT_BANK_B_OFFSET_BYTES &&
           record->staged_bank_size == OTA_LAYOUT_BANK_B_SIZE_BYTES;
}

/**
 * @brief Full integrity check of the image staged in Bank B before copying
 *        it into Bank A.
 *
 * Three checks, all of which must pass: record layout sanity
 * (`bootloader_validate_state_record_layout()`), a structurally valid Bank B
 * vector table (`bootloader_is_valid_image_vectors()`), and a CRC32 match
 * against `record->image_crc32` (`ota_flash_verify_bank_b()`, from
 * `ota/src/ota_flash.c`). This is the sole integrity gate before Bank A —
 * the currently bootable image — is overwritten; there is no signature
 * check (see `docs/code/ota-bootloader.md#integrity-checking`).
 *
 * @param record OTA state record describing the staged image.
 * @return true only if all three checks pass.
 */
static bool bootloader_validate_staged_image(const ota_state_record_t *record)
{
    const uint32_t *const staged_vectors = (const uint32_t *)OTA_LAYOUT_BANK_B_ORIGIN;

    return bootloader_validate_state_record_layout(record) &&
           bootloader_is_valid_image_vectors(staged_vectors, NULL, NULL) &&
           ota_flash_verify_bank_b(record->image_size, record->image_crc32) == OTA_FLASH_STATUS_OK;
}

/**
 * @brief Sets @p record's `state`/`last_error` and persists it via
 *        `ota_flash_write_state()`.
 *
 * Central choke point for every OTA state transition made by the
 * bootloader; mutates @p record in place so the caller's in-memory copy
 * stays in sync with what was (attempted to be) written to Flash.
 *
 * @param record Record to update and persist. Must not be NULL.
 * @param state New lifecycle state to store.
 * @param error New last-error value to store.
 * @return `OTA_FLASH_STATUS_OK` on success; `OTA_FLASH_STATUS_INVALID_ARGUMENT`
 *         if @p record is NULL; otherwise propagates
 *         `ota_flash_write_state()` failures. On a Flash write failure,
 *         @p record's in-memory fields are still updated even though the
 *         persisted copy is now stale — callers should treat a non-OK
 *         return as "state change did not durably happen".
 */
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

/**
 * @brief Erases Bank A and copies the already-validated Bank B image into
 *        it, page by page, then re-verifies the copy.
 *
 * Copy order: erase all of Bank A up front (rounded up to an erase-block
 * boundary), then program Bank A page-by-page from `g_bootloader_copy_page`
 * (each page freshly `0xFF`-padded and `memcpy`'d from Bank B, so a
 * short final page beyond `record->image_size` is padded rather than
 * copying stale Bank B bytes). After all pages are programmed, the function
 * independently recomputes the CRC32 of the copied Bank A range and
 * re-validates Bank A's vector table before declaring success — this is a
 * second, independent verification beyond the pre-copy check in
 * `bootloader_validate_staged_image()`.
 *
 * @param record OTA state record describing the image being copied
 *        (`image_size`, `image_crc32`). Caller must have already validated
 *        it via `bootloader_validate_staged_image()`.
 * @return `OTA_FLASH_STATUS_OK` only if erase, every page program, the
 *         post-copy CRC32, and the post-copy vector-table check all
 *         succeed; `OTA_FLASH_STATUS_VERIFY_FAILED` if the post-copy CRC32
 *         or vector table check fails; otherwise the first Flash operation
 *         failure encountered.
 * @note **No rollback**: Bank A is erased and partially/fully overwritten
 *       as a side effect regardless of whether this function ultimately
 *       returns success. There is no "last known good" backup bank — if
 *       this fails partway through, Bank A is left in a partially-copied,
 *       generally non-bootable state until a subsequent successful OTA
 *       update replaces it (see
 *       `docs/code/ota-bootloader.md#health-confirmation-and-rollback--current-gap`).
 */
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

/**
 * @brief If @p record is `READY_TO_BOOT`, increments `boot_attempts` and
 *        persists it before this boot proceeds.
 *
 * `boot_attempts` is the mechanism `bootloader_record_pending_boot_failure()`
 * uses on the *next* boot to tell "this READY_TO_BOOT image was actually
 * handed off to at least once" apart from "never handed off yet". Called
 * unconditionally in `main()` after `bootloader_apply_pending_update()`, so
 * it also covers an image that just transitioned to `READY_TO_BOOT` in this
 * same boot.
 *
 * @param record Current OTA state record; mutated in place on success.
 * @return true if @p record was `READY_TO_BOOT` and the incremented count
 *         was persisted; false if @p record is NULL, not `READY_TO_BOOT`,
 *         or the Flash write failed (in which case `boot_attempts` was
 *         still incremented in memory but the persisted value is stale).
 * @note There is no upper bound enforced on `boot_attempts` — it does not
 *       by itself prevent an indefinitely crash-looping image from
 *       continuing to be booted (see
 *       `docs/code/ota-bootloader.md#health-confirmation-and-rollback--current-gap`).
 */
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

/**
 * @brief Detects that the *previous* boot reached `READY_TO_BOOT`, was
 *        handed off to at least once (`boot_attempts > 0`), and this boot
 *        was caused by the hardware watchdog — implying the application
 *        never reached `app_acknowledge_ota_boot_if_needed()` — and if so,
 *        marks the record `FAILED`.
 *
 * This is the bootloader's only failure-detection mechanism for a bad OTA
 * update: it does not restore a previous image, it only stops treating the
 * current one as pending acknowledgement (see
 * `bootloader_copy_bank_b_to_bank_a()` for why there is nothing to restore
 * to). Must be called before `bootloader_apply_pending_update()` in
 * `main()`'s sequence so a stale `READY_TO_BOOT` failure is recorded before
 * any new update is considered.
 *
 * @param record Current OTA state record; mutated (state/last_error) in
 *        place on success.
 * @param watchdog_reboot true if this boot was caused by the RP2350
 *        hardware watchdog (`watchdog_caused_reboot()`).
 * @return true if the failure was detected and persisted as `FAILED`;
 *         false if any precondition does not hold (not a watchdog reboot,
 *         not `READY_TO_BOOT`, `boot_attempts == 0`, or @p record is NULL)
 *         or the Flash write failed.
 */
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

/**
 * @brief Top-level "apply a staged OTA update" orchestration: validates,
 *        copies Bank B into Bank A, and transitions the OTA state record
 *        through `COPYING_TO_BANK_A` to `READY_TO_BOOT` or `FAILED`.
 *
 * Sequence: no-op unless @p record's state is `STAGED_IN_BANK_B` or
 * `COPYING_TO_BANK_A` (`bootloader_state_needs_copy()`). Validates the
 * staged image (`bootloader_validate_staged_image()`); on failure, resets
 * `boot_attempts` and marks the record `FAILED` with
 * `OTA_IMAGE_ERROR_INVALID_STAGED_IMAGE` (Bank A is left untouched in this
 * path — the copy never starts). On success, transitions to
 * `COPYING_TO_BANK_A` *before* touching Bank A (so a reset mid-copy is
 * detected as `COPYING_TO_BANK_A` and retried on the next boot — see
 * `bootloader_state_needs_copy()`), performs the copy
 * (`bootloader_copy_bank_b_to_bank_a()`), and on success transitions to
 * `READY_TO_BOOT`; on copy failure, transitions to `FAILED` with an error
 * mapped from the Flash status (`bootloader_map_flash_status_to_error()`).
 *
 * @param record Current OTA state record; mutated and persisted in place as
 *        the state machine progresses.
 * @return true only if the record ends this call as `READY_TO_BOOT` and
 *         that transition was durably persisted; false in every other case
 *         (no-op, validation failure, copy failure, or a Flash write
 *         failure at any step).
 */
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

/**
 * @brief Logs the current OTA state record over stdio (USB/UART console),
 *        or a fixed "invalid or empty" message if none is present.
 *
 * Diagnostic only, no side effects on the record itself.
 */
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

/**
 * @brief Logs the resolved Flash region layout (bootloader, both banks,
 *        config/security/OTA-state slots) over stdio, once at startup.
 *
 * Diagnostic only; values are the `OTA_LAYOUT_*` constants generated by
 * `ota/cmake/ota_layout.cmake` for this build, not read back from Flash.
 */
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

/**
 * @brief Irrevocably hands off execution to the application in Bank A.
 *
 * Sequence (order matters, safety-critical): log and briefly delay to flush
 * the console (`sleep_ms(50)`), disable interrupts, quiesce all NVIC/USB
 * interrupt sources (`bootloader_quiesce_interrupt_sources()`), stop and
 * clear SysTick (so the application's own SysTick configuration starts from
 * a known state), set VTOR to Bank A's vector table base, execute a
 * data+instruction barrier pair, load the application's initial stack
 * pointer, another barrier pair, then call through @p reset_handler. The
 * function never returns from that call in practice; the trailing
 * `tight_loop_contents()` loop exists only as a defensive catch-all should
 * the callee somehow return.
 *
 * @param stack_pointer Bank A's initial MSP value, already validated by the
 *        caller via `bootloader_is_valid_bank_a_image()`.
 * @param reset_handler Bank A's reset handler address (Thumb bit set),
 *        already validated by the caller.
 * @pre Caller must have already confirmed Bank A's vector table is valid;
 *      this function performs no further validation.
 * @note `noreturn`: the bootloader's entire runtime state (stack, vector
 *       table, interrupt configuration) is being handed to the application;
 *       there is no path back into bootloader code after this point.
 */
static void __attribute__((noreturn)) bootloader_jump_to_bank_a(uint32_t stack_pointer, uint32_t reset_handler)
{
    printf("[BOOT] handing off to bank A reset=0x%08lx sp=0x%08lx\n",
           (unsigned long)reset_handler,
           (unsigned long)stack_pointer);
    fflush(stdout);
    sleep_ms(50);

    bootloader_disable_interrupts();
    bootloader_quiesce_interrupt_sources();
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

/**
 * @brief Bootloader entry point. Runs once per device reset before any
 *        application code; either jumps into Bank A or hangs blinking the
 *        onboard LED.
 *
 * Sequence: init stdio/GPIO LED, reset core 1 (`multicore_reset_core1()`,
 * guaranteeing a clean state for whichever image ends up running), log the
 * reboot reason/Flash layout/OTA state, then read the OTA state record and
 * — if present — run it through the three-step state machine in order:
 * `bootloader_record_pending_boot_failure()` (detect a previous failed
 * hand-off), `bootloader_apply_pending_update()` (copy a staged update),
 * `bootloader_prepare_ready_state_for_boot()` (bump `boot_attempts` for
 * this hand-off). Finally, validates Bank A's vector table
 * (`bootloader_is_valid_bank_a_image()`) — independent of whatever the OTA
 * state record says — and either jumps to it
 * (`bootloader_jump_to_bank_a()`, never returns) or falls into an infinite
 * LED-blink loop with no further recovery path.
 *
 * @return Never returns on the success path (jumps away); on the failure
 *         path, loops forever and this declared `int` return is unreachable
 *         in practice.
 * @note This is the actual bootloader executable's entry point (see
 *       `bootloader/CMakeLists.txt`), only linked when
 *       `OTA_BUILD_BOOTLOADER=ON`.
 */
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
