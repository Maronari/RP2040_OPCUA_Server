/**
 * @file
 * @brief Real RP2350 PIO-backed implementation of pio_edge_counter_vtbl_t.
 *
 * See drivers/include/drivers/rp2350_pio_edge_counter.h and
 * drivers/src/uic_pulse_counter.pio for the counting technique and its
 * standing pre-hardware correctness caveat.
 *
 * Ownership: g_slots/g_program_offset/g_program_loaded are process-wide
 * file-static state, claimed lazily and never released back to the pool
 * beyond a per-channel stop() (mirrors adc_voltage_channel_driver.c's
 * g_adc_ready/rp2350_spi_bus.c's lazy shared-init convention for the
 * PIO-program-load half of this state; the per-channel SM claim is
 * genuinely released on stop(), unlike those drivers' singleton
 * peripherals, since a PIO SM is a scarcer, per-channel resource).
 */
#include "drivers/rp2350_pio_edge_counter.h"

#include "hardware/gpio.h"
#include "hardware/pio.h"

#include "uic_pulse_counter.pio.h"

#include <stddef.h>

#define RP2350_PIO_EDGE_COUNTER_MAX_SLOTS 4u

typedef struct
{
    bool in_use;
    uint8_t gpio;
    PIO pio;
    uint sm;
} rp2350_pio_edge_counter_slot_t;

static rp2350_pio_edge_counter_slot_t g_slots[RP2350_PIO_EDGE_COUNTER_MAX_SLOTS];
static int g_program_offset[NUM_PIOS];
static bool g_program_loaded[NUM_PIOS];

static PIO rp2350_pio_instance(uint index)
{
    static PIO const instances[NUM_PIOS] = {
        pio0,
        pio1,
#if NUM_PIOS > 2
        pio2,
#endif
    };
    return instances[index];
}

static rp2350_pio_edge_counter_slot_t *rp2350_find_free_slot(void)
{
    size_t index;

    for (index = 0u; index < RP2350_PIO_EDGE_COUNTER_MAX_SLOTS; ++index)
    {
        if (!g_slots[index].in_use)
        {
            return &g_slots[index];
        }
    }
    return NULL;
}

static rp2350_pio_edge_counter_slot_t *rp2350_find_slot_by_gpio(uint8_t gpio)
{
    size_t index;

    for (index = 0u; index < RP2350_PIO_EDGE_COUNTER_MAX_SLOTS; ++index)
    {
        if (g_slots[index].in_use && g_slots[index].gpio == gpio)
        {
            return &g_slots[index];
        }
    }
    return NULL;
}

static telemetry_driver_result_t rp2350_pio_edge_counter_start(void *context, uint8_t gpio)
{
    rp2350_pio_edge_counter_slot_t *slot = rp2350_find_free_slot();
    uint pio_index;

    (void)context;
    if (slot == NULL)
    {
        return TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
    }
    for (pio_index = 0u; pio_index < NUM_PIOS; ++pio_index)
    {
        PIO pio = rp2350_pio_instance(pio_index);
        pio_sm_config config;
        int sm;

        if (!g_program_loaded[pio_index])
        {
            int offset;

            if (!pio_can_add_program(pio, &uic_pulse_counter_program))
            {
                continue;
            }
            offset = pio_add_program(pio, &uic_pulse_counter_program);
            if (offset < 0)
            {
                continue;
            }
            g_program_offset[pio_index] = offset;
            g_program_loaded[pio_index] = true;
        }
        sm = pio_claim_unused_sm(pio, false);
        if (sm < 0)
        {
            continue;
        }
        pio_gpio_init(pio, gpio);
        gpio_disable_pulls(gpio);
        config = uic_pulse_counter_program_get_default_config((uint)g_program_offset[pio_index]);
        sm_config_set_in_pins(&config, gpio);
        pio_sm_init(pio, (uint)sm, (uint)g_program_offset[pio_index], &config);
        /* X = 0xFFFFFFFF: a full-range down-counter, so the running edge
         * total is always 0xFFFFFFFF - X (see uic_pulse_counter.pio). */
        pio_sm_exec(pio, (uint)sm, pio_encode_mov_not(pio_x, pio_null));
        pio_sm_set_enabled(pio, (uint)sm, true);
        slot->in_use = true;
        slot->gpio = gpio;
        slot->pio = pio;
        slot->sm = (uint)sm;
        return TELEMETRY_DRIVER_RESULT_OK;
    }
    return TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
}

static void rp2350_pio_edge_counter_stop(void *context, uint8_t gpio)
{
    rp2350_pio_edge_counter_slot_t *slot = rp2350_find_slot_by_gpio(gpio);

    (void)context;
    if (slot == NULL)
    {
        return;
    }
    pio_sm_set_enabled(slot->pio, slot->sm, false);
    pio_sm_unclaim(slot->pio, slot->sm);
    slot->in_use = false;
}

static uint32_t rp2350_pio_edge_counter_read_edges(void *context, uint8_t gpio)
{
    rp2350_pio_edge_counter_slot_t *slot = rp2350_find_slot_by_gpio(gpio);
    uint32_t x_snapshot;

    (void)context;
    if (slot == NULL)
    {
        return 0u;
    }
    /* On-demand snapshot: inject "mov isr, x" then "push" so the SM's
     * free-running X becomes readable from the RX FIFO without disturbing
     * its counting loop -- see this file's header doc and
     * uic_pulse_counter.pio for why no DMA/continuous streaming is used. */
    pio_sm_exec_wait_blocking(slot->pio, slot->sm, pio_encode_mov(pio_isr, pio_x));
    pio_sm_exec_wait_blocking(slot->pio, slot->sm, pio_encode_push(false, false));
    x_snapshot = pio_sm_get_blocking(slot->pio, slot->sm);
    return 0xFFFFFFFFu - x_snapshot;
}

const pio_edge_counter_vtbl_t rp2350_pio_edge_counter_vtbl = {
    .start = rp2350_pio_edge_counter_start,
    .stop = rp2350_pio_edge_counter_stop,
    .read_edges = rp2350_pio_edge_counter_read_edges,
};
