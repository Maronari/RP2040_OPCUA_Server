#include "services/config_service.h"

#include <stdio.h>
#include <string.h>

#include "drivers/flash_storage_driver.h"
#include "pico/unique_id.h"

static device_profile_config_t g_config;

static void config_service_populate_serial_number(device_profile_config_t *config)
{
    pico_unique_board_id_t board_id;
    size_t offset = 0;

    if (config == NULL || config->serial_number[0] != '\0')
    {
        return;
    }

    pico_get_unique_board_id(&board_id);

    for (size_t index = 0; index < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; ++index)
    {
        if ((offset + 2u) >= sizeof(config->serial_number))
        {
            break;
        }

        offset += (size_t)snprintf(&config->serial_number[offset],
                                   sizeof(config->serial_number) - offset,
                                   "%02X",
                                   board_id.id[index]);
    }
}

static void config_service_normalize(device_profile_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    if (config->sampling_period_ms == 0u)
    {
        config->sampling_period_ms = 1000u;
    }

    if (config->publish_period_ms == 0u)
    {
        config->publish_period_ms = 1000u;
    }

    if (config->temp_scale == 0.0f)
    {
        config->temp_scale = 1.0f;
    }

    if (config->device_name[0] == '\0' && config->opcua_hostname[0] != '\0')
    {
        strncpy(config->device_name, config->opcua_hostname, sizeof(config->device_name));
        config->device_name[sizeof(config->device_name) - 1] = '\0';
    }

    if (config->opcua_hostname[0] == '\0' && config->device_name[0] != '\0')
    {
        strncpy(config->opcua_hostname, config->device_name, sizeof(config->opcua_hostname));
        config->opcua_hostname[sizeof(config->opcua_hostname) - 1] = '\0';
    }

    if (config->ntp_server[0] == '\0')
    {
        strncpy(config->ntp_server, "pool.ntp.org", sizeof(config->ntp_server));
        config->ntp_server[sizeof(config->ntp_server) - 1] = '\0';
    }

    config_service_populate_serial_number(config);
}

void config_service_init(const device_profile_config_t *defaults)
{
    memset(&g_config, 0, sizeof(g_config));

    if (defaults != NULL)
    {
        g_config = *defaults;
    }

    device_profile_config_t persisted = {0};
    if (flash_storage_driver_load(&persisted, sizeof(persisted)))
    {
        g_config = persisted;
    }

    config_service_normalize(&g_config);
}

const device_profile_config_t *config_service_get(void)
{
    return &g_config;
}

bool config_service_save(void)
{
    return flash_storage_driver_save(&g_config, sizeof(g_config));
}
