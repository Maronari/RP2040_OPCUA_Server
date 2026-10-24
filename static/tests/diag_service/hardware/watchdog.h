#ifndef DIAG_SERVICE_TEST_WATCHDOG_H
#define DIAG_SERVICE_TEST_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

bool watchdog_caused_reboot(void);
void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms);

#endif
