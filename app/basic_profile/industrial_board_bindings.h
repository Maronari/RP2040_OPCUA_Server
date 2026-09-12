/**
 * @file
 * @brief Board wiring for `RP2350_INDUSTRIAL_MVP_V1`: hands each industrial
 *        driver the physical backing it needs (the shared SPI bus, the PIO
 *        edge counter) before any channel is activated.
 *
 * Without this step the drivers are present in the image but unbound, and
 * every SPI-attached channel plus every UIC counter mode fails `start()`
 * with `TELEMETRY_DRIVER_RESULT_INVALID_STATE` -- which is exactly what
 * shipped through v0.5.1's driver work, since nothing outside host tests
 * ever called `native_*_bind_bus()`.
 *
 * Compiled-out drivers bind nothing: every block below is gated on the same
 * `FIRMWARE_ENABLE_NATIVE_*` option that decides whether the driver is in
 * the image at all. In a `FIRMWARE_DRIVER_PACK=base` build (the default)
 * this is therefore a no-op, so the reference RP2350 + W5500 board never
 * initializes `spi1` or claims PIO for chips it does not have --
 * `ROADMAP.md:552`'s profile separation, enforced at runtime rather than
 * only at compile time.
 *
 * Pre-hardware: the pin map is frozen (`HARDWARE_MVP.md` section 6.3) but
 * no assembled board exists, so none of this is hardware qualified.
 */
#ifndef APP_INDUSTRIAL_BOARD_BINDINGS_H
#define APP_INDUSTRIAL_BOARD_BINDINGS_H

#include <stdbool.h>

/**
 * @brief Bind every compiled-in industrial driver to its board backing.
 *
 * Call once during startup, before `sensor_manager_init()` -- that function
 * activates the running configuration, which starts channel drivers.
 *
 * @return true if every compiled-in binding succeeded, including the
 *         vacuous case where no industrial driver is compiled in; false if
 *         any bus initialization failed, in which case the channels using
 *         that driver will fail activation with a loud driver result rather
 *         than reading wrong values.
 */
bool industrial_board_bindings_apply(void);

#endif
