#include "rp2_ua_datetime_clock.h"

#include <limits.h>

#include "services/time_service.h"

bool rp2_ua_datetime_now(int64_t *out_datetime_100ns)
{
    uint64_t unix_time_us;
    uint64_t unix_time_100ns;

    if (out_datetime_100ns == NULL ||
        !time_service_get_utc_time_us(&unix_time_us))
    {
        return false;
    }

    if (unix_time_us > (UINT64_MAX / UINT64_C(10)))
    {
        return false;
    }

    unix_time_100ns = unix_time_us * UINT64_C(10);
    if (unix_time_100ns >
        (uint64_t)(INT64_MAX - RP2_UA_DATETIME_UNIX_EPOCH_100NS))
    {
        return false;
    }

    *out_datetime_100ns = RP2_UA_DATETIME_UNIX_EPOCH_100NS +
                           (int64_t)unix_time_100ns;
    return true;
}
