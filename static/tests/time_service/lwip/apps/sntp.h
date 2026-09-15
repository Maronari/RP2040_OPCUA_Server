#ifndef TIME_SERVICE_TEST_SNTP_H
#define TIME_SERVICE_TEST_SNTP_H

#include <stdint.h>

void sntp_setservername(uint8_t idx, const char *server);
void sntp_init(void);
void sntp_stop(void);

#endif
