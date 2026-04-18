#ifndef SERVICES_CONFIG_EXPORT_SERVICE_H
#define SERVICES_CONFIG_EXPORT_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

bool config_export_service_write_running_json(char *buffer, size_t buffer_size);
bool config_export_service_write_staged_json(char *buffer, size_t buffer_size);
bool config_export_service_write_state_json(char *buffer, size_t buffer_size, bool reboot_required);

#endif
