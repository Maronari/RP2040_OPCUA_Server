#ifndef DRIVERS_PLATFORM_CLOCK_H
#define DRIVERS_PLATFORM_CLOCK_H

/**
 * @brief Configure the system/peripheral clock tree at early boot.
 *
 * RP2040 only: sets the system clock to SYS_CLK_HZ and reconfigures the
 * peripheral clock off the system PLL. On RP2350 this is an explicit no-op
 * (compiled out via `#if PICO_RP2040`) -- reconfiguring clocks there before
 * stdio starts can break USB enumeration, so RP2350 stays on the SDK-managed
 * default clock tree.
 *
 * @pre Must run very early in boot, before stdio/USB initialization.
 * @note Not safe to call more than once or after other clock-dependent
 *       peripherals (UART, USB, ADC) have been initialized.
 */
void platform_clock_init(void);

#endif
