/**
 * @file
 * @brief Thin wrapper around the `TPS3851H33EDRBR` **external** supervisor/
 *        watchdog (`RP2350_INDUSTRIAL_MVP_V1`, `HARDWARE_MVP.md` section
 *        5.5, `GPIO2`/`GPIO3`). Pre-hardware: source-complete and
 *        host/mock-tested, but not hardware qualified -- see `ROADMAP.md`
 *        section 5.2.
 *
 * This is a **separate layer from the RP2350's own internal hardware
 * watchdog** (`drivers/watchdog_driver.h`) -- per `ROADMAP.md`, the
 * internal watchdog is retained "as a separate layer unless a later
 * design decision removes it." A future `WatchdogTask` integration (see
 * `app/basic_profile/main.c`, not touched by this release) would call
 * both this driver's `tps3851_watchdog_driver_kick()` and the internal
 * `watchdog_driver_kick()` together -- servicing one and not the other
 * would defeat the point of having two independent supervision layers.
 *
 * Board topology: `RESET_N`/`WDO_N` are wired-OR directly into
 * `SYS_RESET_N` in hardware -- a watchdog timeout is a full MCU reset,
 * not a software-observable event this driver could read back after the
 * fact (unlike `IPS2050`'s/`MAX22190`'s real fault-pin readbacks). Unlike
 * the internal watchdog's software-selectable timeout,
 * `TPS3851H33EDRBR`'s watchdog window is a fixed hardware property of the
 * selected part variant, not configurable from firmware -- this driver
 * has no timeout parameter.
 *
 * **Board-pin-conflict caveat, not silently ignored**: `HARDWARE_MVP.md`'s
 * frozen `RP2350_INDUSTRIAL_MVP_V1` GPIO map assigns `GPIO2`/`GPIO3` to
 * `WD_KICK`/`WD_ENABLE`. The *current* reference firmware's W5500 SPI
 * binding (`port/ioLibrary_Driver/inc/w5x00_spi.h`) still hardcodes
 * `PIN_SCK=2`/`PIN_MOSI=3` for a non-industrial board's Ethernet wiring --
 * a real conflict on real `RP2350_INDUSTRIAL_MVP_V1` hardware if both were
 * active simultaneously. This driver is inert until some future board-
 * bring-up code calls it (matching every other native driver added this
 * release -- none are wired into a live init path yet), so adding it does
 * not itself create a live conflict, but this dependency is exactly why
 * `ROADMAP.md`'s frozen Phase 1 order places the W5500 board-binding
 * rework (moving Ethernet to `GPIO4..GPIO8` per the same frozen map)
 * immediately after this item -- see `docs/releases/v0.5.1.md` T9/T10.
 *
 * Hardware-safe default: both `WD_ENABLE` and `WD_KICK` have `100k`
 * pull-downs (`HARDWARE_MVP.md`), so an uninitialized/reset MCU already
 * presents a hardware-safe (watchdog disabled) state before firmware
 * runs; `tps3851_watchdog_driver_init()` explicitly drives the same safe
 * level rather than relying on the external pull-downs alone, matching
 * every other board-safe-state convention this release established.
 *
 * Ownership: two file-static GPIO-state booleans (kick level, enabled
 * flag) -- there is exactly one `TPS3851` in scope for this board.
 *
 * Concurrency: none; intended to be called only from a single
 * watchdog-servicing task context (see the internal watchdog's own
 * concurrency note in `drivers/watchdog_driver.h`).
 */
#ifndef DRIVERS_TPS3851_WATCHDOG_DRIVER_H
#define DRIVERS_TPS3851_WATCHDOG_DRIVER_H

#include <stdbool.h>

/**
 * @brief Initializes `WD_ENABLE`/`WD_KICK` as outputs driven to their safe
 *        (disabled/low) level. Call once at startup, before
 *        tps3851_watchdog_driver_enable().
 */
void tps3851_watchdog_driver_init(void);
/**
 * @brief Asserts `WD_ENABLE` (arms the external supervisor's watchdog
 *        monitoring). Call only once firmware is fully initialized and
 *        ready to be supervised -- mirrors `DO_ENABLE_REQ`/`AO_ENABLE_REQ`'s
 *        "assert only after init complete" convention
 *        (`drivers/ips2050_driver.h`). Idempotent.
 */
void tps3851_watchdog_driver_enable(void);
/**
 * @brief Services the watchdog by toggling `WD_KICK`'s driven level (an
 *        edge-based service signal, not level-based -- each call flips
 *        it). Has no supervisory effect on real hardware until
 *        tps3851_watchdog_driver_enable() has been called, but is always
 *        safe to call (toggles the pin regardless).
 */
void tps3851_watchdog_driver_kick(void);
/** @brief Whether tps3851_watchdog_driver_enable() has been called. */
bool tps3851_watchdog_driver_is_enabled(void);

#endif
