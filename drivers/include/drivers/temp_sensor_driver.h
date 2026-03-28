#ifndef DRIVERS_TEMP_SENSOR_DRIVER_H
#define DRIVERS_TEMP_SENSOR_DRIVER_H

#include <stdbool.h>

bool temp_sensor_driver_init(void);
bool temp_sensor_driver_read_celsius(float *out_temperature_c);

#endif
