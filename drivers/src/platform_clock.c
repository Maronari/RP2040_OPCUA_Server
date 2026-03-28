#include "drivers/platform_clock.h"

#include "hardware/clocks.h"
#include "pico/stdlib.h"

#define PLATFORM_PLL_SYS_KHZ (133 * 1000)

void platform_clock_init(void)
{
    set_sys_clock_khz(PLATFORM_PLL_SYS_KHZ, true);

    clock_configure(
        clk_peri,
        0,
        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
        PLATFORM_PLL_SYS_KHZ * 1000,
        PLATFORM_PLL_SYS_KHZ * 1000);
}
