/**
 * @file
 * @brief Early-boot system/peripheral clock configuration (RP2040 only).
 *
 * Responsibilities:
 * - On RP2040, set the system clock and re-derive the peripheral clock from
 *   the system PLL before any clock-dependent peripheral initializes.
 * - On RP2350, do nothing: leave the SDK-managed default clock tree in
 *   place so USB enumeration during early boot is not disturbed.
 *
 * Ownership: no persistent state -- this file only issues one-shot SDK
 * clock-tree calls.
 *
 * Concurrency: none; must run once from the boot/init task before any other
 * task touches clock-dependent peripherals.
 */
#include "drivers/platform_clock.h"

#include "hardware/clocks.h"
#include "hardware/platform_defs.h"
#include "pico/stdlib.h"

#define PLATFORM_SYS_CLOCK_HZ  SYS_CLK_HZ
#define PLATFORM_SYS_CLOCK_KHZ (PLATFORM_SYS_CLOCK_HZ / 1000u)

void platform_clock_init(void)
{
#if PICO_RP2040
    set_sys_clock_khz(PLATFORM_SYS_CLOCK_KHZ, true);

    clock_configure(
        clk_peri,
        0,
        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
        PLATFORM_SYS_CLOCK_HZ,
        PLATFORM_SYS_CLOCK_HZ);
#else
    /* Leave RP2350 on the SDK-managed default clock tree during early boot.
     * Reconfiguring clocks here can break USB enumeration before stdio starts. */
    (void)PLATFORM_SYS_CLOCK_HZ;
    (void)PLATFORM_SYS_CLOCK_KHZ;
#endif
}
