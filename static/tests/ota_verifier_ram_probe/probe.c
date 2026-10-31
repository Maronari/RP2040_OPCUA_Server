/* RAM-only T1 verifier measurement. It never erases or programs Flash. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "ota/ota_verifier.h"
#include "ota_verifier_measurement.h"
#include "pico/bootrom.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "psram_allocator.h"

#include "ota_verifier_fixture.h"

#if !defined(PICO_NO_FLASH) || !PICO_NO_FLASH
#error "The T1 verifier probe must be a PICO_NO_FLASH RAM binary"
#endif

enum {
    USB_WAIT_MS = 5000u,
    PROBE_WATCHDOG_MS = 8000u,
    MSP_STACK_BYTES = 4096u,
    STACK_CANARY_BYTES = 16u,
    STACK_GUARD_BYTES = 128u,
};

_Static_assert(PICO_STACK_SIZE == MSP_STACK_BYTES, "probe stack reservation changed");

typedef struct {
    bool mutate_image;
    unsigned calls;
    size_t largest_call;
    psram_allocator_stats_t during_stats;
    bool during_stats_valid;
} reader_context_t;

static uint8_t g_manifest[OTA_MANIFEST_BYTES];
static bool g_stack_measurement_available;
static uintptr_t g_stack_paint_start;
static uintptr_t g_stack_paint_end;

extern char __StackBottom;
extern char __StackTop;

static uintptr_t read_msp(void) {
    uintptr_t value = 0u;
#if defined(__arm__) || defined(__thumb__)
    __asm volatile("mrs %0, msp" : "=r"(value));
#endif
    return value;
}

static uintptr_t read_msplim(void) {
    uintptr_t value = 0u;
#if defined(__arm__) || defined(__thumb__)
    __asm volatile("mrs %0, msplim" : "=r"(value));
#endif
    return value;
}

static void stack_begin(void) {
    const uintptr_t bottom = (uintptr_t)&__StackBottom;
    const uintptr_t top = (uintptr_t)&__StackTop;
    const uint32_t irq = save_and_disable_interrupts();
    const uintptr_t current = read_msp();
    const uintptr_t end = current > STACK_GUARD_BYTES ? current - STACK_GUARD_BYTES : 0u;

    g_stack_measurement_available = false;
    if (top - bottom == MSP_STACK_BYTES && current > bottom + STACK_CANARY_BYTES + STACK_GUARD_BYTES &&
        current <= top && end > bottom + STACK_CANARY_BYTES &&
        (read_msplim() == 0u || read_msplim() <= bottom)) {
        volatile uint8_t *cursor;
        for (cursor = (volatile uint8_t *)bottom; cursor < (volatile uint8_t *)(bottom + STACK_CANARY_BYTES); ++cursor) {
            *cursor = 0x3cu;
        }
        for (cursor = (volatile uint8_t *)(bottom + STACK_CANARY_BYTES); cursor < (volatile uint8_t *)end; ++cursor) {
            *cursor = 0xa5u;
        }
        g_stack_paint_start = bottom + STACK_CANARY_BYTES;
        g_stack_paint_end = end;
        g_stack_measurement_available = true;
    }
    restore_interrupts(irq);
}

static bool stack_report(void) {
    uintptr_t cursor;
    const uint32_t irq = save_and_disable_interrupts();
    bool available = g_stack_measurement_available && read_msp() >= g_stack_paint_end;
    bool canary_ok = false;
    bool measured = false;

    if (available) {
        for (cursor = (uintptr_t)&__StackBottom; cursor < (uintptr_t)&__StackBottom + STACK_CANARY_BYTES &&
             *(const volatile uint8_t *)cursor == 0x3cu; ++cursor) {
        }
        canary_ok = cursor == (uintptr_t)&__StackBottom + STACK_CANARY_BYTES;
        if (canary_ok) {
            for (cursor = g_stack_paint_start; cursor < g_stack_paint_end &&
                 *(const volatile uint8_t *)cursor == 0xa5u; ++cursor) {
            }
            measured = cursor != g_stack_paint_start && cursor != g_stack_paint_end;
        }
    }
    restore_interrupts(irq);
    if (!available) {
        printf("[T1_VERIFIER_RAM] msp_wrapper=unavailable reserved_bytes=%u scope=bare_metal_probe_not_freertos\n",
               (unsigned)MSP_STACK_BYTES);
    } else if (!canary_ok) {
        printf("[T1_VERIFIER_RAM] msp_wrapper=canary_failed reserved_bytes=%u scope=bare_metal_probe_not_freertos\n",
               (unsigned)MSP_STACK_BYTES);
    } else if (!measured) {
        printf("[T1_VERIFIER_RAM] msp_wrapper=indeterminate reserved_bytes=%u scope=bare_metal_probe_not_freertos\n",
               (unsigned)MSP_STACK_BYTES);
    } else {
        printf("[T1_VERIFIER_RAM] msp_wrapper=observed used_bytes=%lu reserved_bytes=%u scope=bare_metal_probe_not_freertos\n",
               (unsigned long)((uintptr_t)&__StackTop - cursor), (unsigned)MSP_STACK_BYTES);
    }
    return available && canary_ok && measured;
}

static void wait_for_usb(void) {
    const absolute_time_t deadline = make_timeout_time_ms(USB_WAIT_MS);
    while (!stdio_usb_connected() && !time_reached(deadline)) sleep_ms(10u);
}

static bool fixture_reader(void *opaque, uint32_t offset, uint8_t *destination, size_t length) {
    reader_context_t *context = opaque;
    ++context->calls;
    if (length > context->largest_call) context->largest_call = length;
    size_t index;
    if ((size_t)offset > V060_FIXTURE_IMAGE_BYTES || length > V060_FIXTURE_IMAGE_BYTES - (size_t)offset) return false;
    for (index = 0u; index < length; ++index) destination[index] = (uint8_t)(((offset + index) * 19u + 7u) & 0xffu);
    if (context->mutate_image && length != 0u) destination[0] ^= 1u;
    watchdog_update(); /* Diagnostic liveness only; no OTA boot-window policy is exercised here. */
    context->during_stats_valid = psram_allocator_get_stats(&context->during_stats);
    return context->during_stats_valid;
}

static ota_verifier_policy_t fixture_policy(void) {
    ota_verifier_policy_t policy = {0};
    policy.key_id[0] = 7u;
    memcpy(policy.public_key_sec1[0], v060_fixture_public_key, 65u);
    policy.trusted_key_count = 1u;
    memcpy(policy.hardware_profile_id, "waveshare_core2350b_16m8m", 25u);
    memcpy(policy.firmware_profile_id, "device_profile_basic", 21u);
    memcpy(policy.driver_pack_id, "base", 5u);
    policy.driver_pack_contract_version = 1u;
    policy.board_revision = 7u; /* Test-only fixture; never a production provisioned revision. */
    policy.board_revision_present = true;
    policy.recovery_layout_id = 1u;
    policy.execution_origin = 0x10040000u;
    policy.policy_id = 0x86000001u;
    policy.maximum_image_size = 5271552u;
    policy.test_only_allow_downgrade = true;
    return policy;
}

static bool auth_expect(const ota_verifier_policy_t *policy, const uint8_t *manifest,
                        ota_verify_result_t expected) {
    ota_authenticated_manifest_t output;
    const ota_verify_result_t result = ota_authenticate_manifest(policy, manifest, OTA_MANIFEST_BYTES, &output);
    printf("[T1_VERIFIER_RAM] auth expected=%u got=%u\n", (unsigned)expected, (unsigned)result);
    return result == expected;
}

static void print_stats(const char *phase, const psram_allocator_stats_t *stats) {
    if (!stats->ready) {
        printf("[T1_VERIFIER_RAM] psram phase=%s unavailable\n", phase);
        return;
    }
    printf("[T1_VERIFIER_RAM] psram phase=%s used=%u peak=%u requested=%u peak_requested=%u free=%u largest=%u\n",
           phase, (unsigned)stats->used_bytes, (unsigned)stats->peak_used_bytes,
           (unsigned)stats->requested_bytes_in_use, (unsigned)stats->peak_requested_bytes_in_use,
           (unsigned)stats->free_bytes, (unsigned)stats->largest_free_block_bytes);
}

static void subtract_be(uint8_t destination[32], const uint8_t left[32], const uint8_t right[32]) {
    int index;
    unsigned borrow = 0u;

    for (index = 31; index >= 0; --index) {
        const unsigned minuend = left[index];
        const unsigned subtrahend = right[index] + borrow;
        destination[index] = (uint8_t)(minuend - subtrahend);
        borrow = minuend < subtrahend ? 1u : 0u;
    }
}

int main(void) {
    ota_verifier_policy_t policy;
    ota_verified_candidate_snapshot_t candidate;
    ota_verifier_allocator_measurement_t measurement;
    psram_allocator_stats_t before;
    psram_allocator_stats_t after;
    reader_context_t reader = {0};
    bool ok = true;
    uint64_t started;
    void *exhaustion = NULL;

    watchdog_enable(PROBE_WATCHDOG_MS, true);
    {
        const uint32_t irq = save_and_disable_interrupts();
        flash_start_xip(); /* Required only to read Flash-backed fixture code safely in PICO_NO_FLASH. */
        restore_interrupts(irq);
    }
    stdio_init_all();
    wait_for_usb();
    printf("[T1_VERIFIER_RAM] start mode=ram_only flash_write=none fixture_image_bytes=%u policy=test_only\n",
           (unsigned)V060_FIXTURE_IMAGE_BYTES);
    if (!psram_allocator_init() || !psram_allocator_get_stats(&before)) {
        printf("[T1_VERIFIER_RAM] FAIL psram_init\n");
        sleep_ms(100u);
        reset_usb_boot(0u, 0u);
    }
    print_stats("before", &before);
    policy = fixture_policy();
    memcpy(g_manifest, v060_fixture_manifest, sizeof(g_manifest));
    stack_begin();
    ota_verifier_allocator_measurement_reset();
    started = time_us_64();
    if (ota_verify_candidate(&policy, g_manifest, sizeof(g_manifest), fixture_reader, &reader,
                             V060_FIXTURE_IMAGE_BYTES, &candidate) != OTA_VERIFY_OK ||
        reader.largest_call > OTA_VERIFY_STREAM_CHUNK_BYTES || reader.calls == 0u) ok = false;
    ota_verifier_allocator_measurement_get(&measurement);
    printf("[T1_VERIFIER_RAM] candidate positive=%s elapsed_us=%llu reader_calls=%u max_chunk=%u\n",
           ok ? "pass" : "fail", (unsigned long long)(time_us_64() - started), reader.calls,
           (unsigned)reader.largest_call);
    printf("[T1_VERIFIER_RAM] crypto payload_peak=%u header_peak=%u current_payload=%u current_header=%u\n",
           (unsigned)measurement.peak_payload_bytes, (unsigned)measurement.peak_header_bytes,
           (unsigned)measurement.current_payload_bytes, (unsigned)measurement.current_header_bytes);
    if (measurement.current_payload_bytes != 0u || measurement.current_header_bytes != 0u ||
        !reader.during_stats_valid || reader.during_stats.used_bytes <= before.used_bytes ||
        reader.during_stats.requested_bytes_in_use != before.requested_bytes_in_use + 65536u) ok = false;
    print_stats("during_reader", &reader.during_stats);
    watchdog_update();
    reader.mutate_image = true;
    if (ota_verify_candidate(&policy, g_manifest, sizeof(g_manifest), fixture_reader, &reader,
                             V060_FIXTURE_IMAGE_BYTES, &candidate) != OTA_VERIFY_IMAGE_HASH_MISMATCH) ok = false;
    reader.mutate_image = false;
    memcpy(g_manifest, v060_fixture_manifest, sizeof(g_manifest));
    memset(g_manifest + 256u, 0, 32u);
    if (!auth_expect(&policy, g_manifest, OTA_VERIFY_SIGNATURE_INVALID)) ok = false;
    memcpy(g_manifest, v060_fixture_manifest, sizeof(g_manifest));
    memset(g_manifest + 288u, 0xff, 32u);
    if (!auth_expect(&policy, g_manifest, OTA_VERIFY_SIGNATURE_INVALID)) ok = false;
    memcpy(g_manifest, v060_fixture_manifest, sizeof(g_manifest));
    {
        static const uint8_t p256_order[32] = {
            0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84,
            0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
        };
        subtract_be(g_manifest + 288u, p256_order, v060_fixture_manifest + 288u);
    }
    if (!auth_expect(&policy, g_manifest, OTA_VERIFY_SIGNATURE_INVALID)) ok = false;
    memcpy(g_manifest, v060_fixture_manifest, sizeof(g_manifest));
    g_manifest[20] = 8u;
    if (!auth_expect(&policy, g_manifest, OTA_VERIFY_UNKNOWN_KEY)) ok = false;
    memcpy(policy.hardware_profile_id, "wrong_profile", 14u);
    if (!auth_expect(&policy, v060_fixture_manifest, OTA_VERIFY_INCOMPATIBLE)) ok = false;
    policy = fixture_policy();
    watchdog_update();
    if (before.largest_free_block_bytes > 65536u + 512u) {
        exhaustion = psram_malloc(before.largest_free_block_bytes - 256u);
        if (exhaustion == NULL || !auth_expect(&policy, v060_fixture_manifest, OTA_VERIFY_ALLOCATION_FAILED)) ok = false;
        if (exhaustion != NULL) psram_free(exhaustion);
        if (!auth_expect(&policy, v060_fixture_manifest, OTA_VERIFY_OK)) ok = false;
    } else {
        printf("[T1_VERIFIER_RAM] FAIL insufficient_psram_for_exhaustion_test\n");
        ok = false;
    }
    watchdog_update();
    if (!psram_allocator_get_stats(&after)) ok = false;
    print_stats("after", &after);
    if (after.used_bytes != before.used_bytes || after.requested_bytes_in_use != before.requested_bytes_in_use) ok = false;
    if (!stack_report()) ok = false;
    printf("[T1_VERIFIER_RAM] %s returning_bootsel=1\n", ok ? "PASS" : "FAIL");
    fflush(stdout);
    sleep_ms(250u);
    reset_usb_boot(0u, 0u);
    while (true) tight_loop_contents();
}
