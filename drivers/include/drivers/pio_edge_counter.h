/**
 * @file
 * @brief Generic edge-counter abstraction used by the native `UIC` driver
 *        (drivers/uic_driver.h) so its counter/frequency/period mode logic
 *        can be host-tested against a fake backing, exactly the same
 *        "linked-seam" pattern as drivers/spi_transport.h -- see
 *        docs/releases/v0.5.1.md T2's retrospective for why this pattern
 *        was adopted as the preferred default over T1's Pico-SDK-header-
 *        substitution approach.
 *
 * The real backing (drivers/rp2350_pio_edge_counter.[ch]) counts pulse
 * edges on an RP2350 GPIO using a PIO state machine's X scratch register as
 * a free-running down-counter, read on demand via pio_sm_exec()-injected
 * instructions -- no DMA or continuous FIFO streaming, since a channel
 * read() is a low-rate periodic SensorTask poll, not a high-rate capture.
 * A host test binds a fake counter directly.
 */
#ifndef DRIVERS_PIO_EDGE_COUNTER_H
#define DRIVERS_PIO_EDGE_COUNTER_H

#include <stdbool.h>
#include <stdint.h>

#include "services/telemetry_driver.h"

/**
 * @brief Operations a pulse-edge-counter backing must implement.
 *
 * @c start claims whatever hardware resource this GPIO's counting needs
 * (a PIO state machine on real hardware) and begins counting from zero.
 * @c stop releases it. @c read_edges returns the cumulative edge count
 * observed since @c start, as a value that wraps modulo 2^32 -- callers
 * (native_uic_driver.c) must compute deltas with unsigned (wrapping)
 * subtraction, never assume a bounded absolute count.
 */
typedef struct
{
    telemetry_driver_result_t (*start)(void *context, uint8_t gpio);
    void (*stop)(void *context, uint8_t gpio);
    uint32_t (*read_edges)(void *context, uint8_t gpio);
} pio_edge_counter_vtbl_t;

/** @brief A bound edge-counter backing: vtbl + opaque context, mirroring spi_bus_t's shape. */
typedef struct
{
    const pio_edge_counter_vtbl_t *vtbl;
    void *context;
} pio_edge_counter_t;

#endif
