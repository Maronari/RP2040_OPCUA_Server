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
