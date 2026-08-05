#ifndef RP2_UA_DATETIME_CLOCK_H
#define RP2_UA_DATETIME_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* OPC UA DateTime ticks are 100 ns since 1601-01-01T00:00:00Z. */
#define RP2_UA_DATETIME_UNIX_EPOCH_100NS INT64_C(116444736000000000)

/*
 * Converts the time-service UTC reading to an OPC UA DateTime.  It never
 * substitutes a monotonic/boot-relative value: false denotes unavailable or
 * out-of-range wall time, and leaves out_datetime_100ns unchanged.
 */
bool rp2_ua_datetime_now(int64_t *out_datetime_100ns);

#endif
