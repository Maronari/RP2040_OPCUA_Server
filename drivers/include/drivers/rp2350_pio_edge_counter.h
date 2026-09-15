/**
 * @file
 * @brief Real RP2350 PIO-backed implementation of drivers/pio_edge_counter.h,
 *        used by native_uic_bind_counter() in real firmware (a host test
 *        binds a fake pio_edge_counter_t directly instead).
 *
 * Counts pulse edges on a GPIO using one PIO state machine per active
 * channel, running drivers/src/uic_pulse_counter.pio -- see that file and
 * drivers/include/drivers/uic_driver.h for the counting technique and its
 * standing pre-hardware correctness caveat (no PIO simulator or real
 * silicon available to verify the hand-written state machine against).
 *
 * Ownership: process-wide file-static slot table, one entry per
 * concurrently-active GPIO (up to 4, matching UIC0..UIC3); a PIO program
 * is loaded into each of the two PIO blocks at most once and shared by
 * every state machine claimed from that block.
 */
#ifndef DRIVERS_RP2350_PIO_EDGE_COUNTER_H
#define DRIVERS_RP2350_PIO_EDGE_COUNTER_H

#include "drivers/pio_edge_counter.h"

/** @brief The real RP2350 PIO-backed vtbl; pass to native_uic_bind_counter() with context = NULL (this backing needs no external context, all state is file-static). */
extern const pio_edge_counter_vtbl_t rp2350_pio_edge_counter_vtbl;

#endif
