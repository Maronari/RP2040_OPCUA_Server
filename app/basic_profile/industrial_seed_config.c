/**
 * @file
 * @brief The industrial seed configuration itself; see
 *        app/basic_profile/industrial_seed_config.h for when it applies.
 *
 * Channel budget: 4 AI + 2 AO + 4 DI + 2 DO + 4 UIC is exactly
 * `PLATFORM_CHANNEL_LIMIT` (16), which the platform caps at 16. The board's
 * I/O therefore fills the channel table completely, leaving no slot for the
 * MCU's internal temperature sensor -- board-level diagnostics live in
 * `board_diagnostics_service` rather than in channels, so nothing is lost.
 *
 * Per-channel selector codes, all from `HARDWARE_MVP.md`/the datasheet notes
 * rather than invented:
 * - AI `source_flags = 0x05` -- `Range_CHn` "0 to 2.5 x VREF", i.e. 0-10.24 V
 *   with the internal 4.096 V reference.
 * - AO `source_flags = 0x01` -- `R[2:0]` "0 V to 10 V"; the register notes
 *   record 0-10 V and 4-20 mA as the only electrically validated ranges.
 * - DI `source_flags = 0x04` -- `FLTn` with `FBP=0`, `DELAY=100b`, `WBE=0`:
 *   the 1.6 ms filtering with wire-break disabled that `HARDWARE_MVP.md`
 *   specifies as this board's default.
 * - DO `source_flags = 0x00` -- no per-channel configuration exists.
 * - UIC `source_flags = 0x00` -- digital-input mode. The counter/frequency/
 *   period modes are equally available per channel, but which channel should
 *   count rather than read a level is a commissioning decision, so the seed
 *   picks the mode that claims no PIO state machine.
 */
#include "industrial_seed_config.h"

#if APP_INDUSTRIAL_SEED_AVAILABLE

#include "services/driver_catalog.h"
#include "services/telemetry_channel.h"

#define SEED_INPUT_FLAGS (TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_HISTORIZED)
#define SEED_OUTPUT_FLAGS (TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_WRITABLE)

/** @brief Sampling/publishing cadence shared by every seeded channel. */
#define SEED_SETTINGS                     \
    {                                     \
        .enabled = true,                  \
        .sample_period_ms = 1000u,        \
        .publish_period_ms = 1000u,       \
        .history_interval_ms = 1000u,     \
        .scale = 1.0,                     \
        .offset = 0.0,                    \
    }

const device_profile_config_t g_industrial_profile = {
    .manufacturer = "RP2 OPC UA",
    .model = "IndustrialFirmwareProfile",
    .serial_number = "",
    .hw_revision = DEVICE_PROFILE_HW_REVISION,
    .fw_revision = DEVICE_PROFILE_FW_REVISION,
    .build_time = DEVICE_PROFILE_BUILD_TIME,
    .git_hash = DEVICE_PROFILE_GIT_HASH,
    .device_name = "RP2 Industrial Device",
    .opcua_hostname = "rp2-industrial-profile",
    .ntp_server = "pool.ntp.org",
    .mqtt_broker_url = "opc.mqtt://127.0.0.1:1883",
    .mqtt_topic_root = "devices/rp2-industrial-profile",
    .enable_heap_diagnostics = true,
    .enable_task_diagnostics = true,
    .enable_alarm_ack_method = true,
    .enable_mqtt_pubsub = false,
    .mqtt_pubsub_use_json = true,
    .opcua_port = 4840,
    .timezone_offset_minutes = 0,
    /* 80 s. Repeated rather than shared: the reference profile's own value
     * lives in app/basic_profile/main.c as APP_DEFAULT_WATCHDOG_TIMEOUT_MS,
     * which is file-local there. Keep the two in step. */
    .watchdog_timeout_ms = 80000u,
    .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56},
    .network_mode = NETWORK_MODE_DHCP,
    .static_ip = {192, 168, 1, 42},
    .static_netmask = {255, 255, 255, 0},
    .static_gateway = {192, 168, 1, 0},
    .static_dns = {8, 8, 8, 8},
    .channels = {
        /* AI0..AI3 -- ADS8688 mux inputs, 0-10.24 V. */
        {
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_ANALOG,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_FLOAT,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_ADS8688,
            .source_index = 0u,
            .source_flags = 0x05u,
            .has_minimum = 1u,
            .has_maximum = 1u,
            .id = "channel.ai.1",
            .name = "AnalogInput1",
            .description = "Industrial analog input AI0",
            .unit = "V",
            .flags = SEED_INPUT_FLAGS,
            .minimum = 0.0,
            .maximum = 10.24,
            .settings = SEED_SETTINGS,
        },
        {
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_ANALOG,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_FLOAT,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_ADS8688,
            .source_index = 1u,
            .source_flags = 0x05u,
            .has_minimum = 1u,
            .has_maximum = 1u,
            .id = "channel.ai.2",
            .name = "AnalogInput2",
            .description = "Industrial analog input AI1",
            .unit = "V",
            .flags = SEED_INPUT_FLAGS,
            .minimum = 0.0,
            .maximum = 10.24,
            .settings = SEED_SETTINGS,
        },
        {
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_ANALOG,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_FLOAT,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_ADS8688,
            .source_index = 2u,
            .source_flags = 0x05u,
            .has_minimum = 1u,
            .has_maximum = 1u,
            .id = "channel.ai.3",
            .name = "AnalogInput3",
            .description = "Industrial analog input AI2",
            .unit = "V",
            .flags = SEED_INPUT_FLAGS,
            .minimum = 0.0,
            .maximum = 10.24,
            .settings = SEED_SETTINGS,
        },
        {
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_ANALOG,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_FLOAT,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_ADS8688,
            .source_index = 3u,
            .source_flags = 0x05u,
            .has_minimum = 1u,
            .has_maximum = 1u,
            .id = "channel.ai.4",
            .name = "AnalogInput4",
            .description = "Industrial analog input AI3",
            .unit = "V",
            .flags = SEED_INPUT_FLAGS,
            .minimum = 0.0,
            .maximum = 10.24,
            .settings = SEED_SETTINGS,
        },
        /* AO0/AO1 -- AD5422 daisy chain, 0-10 V. */
        {
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_ANALOG,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_FLOAT,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_AD5422,
            .source_index = 0u,
            .source_flags = 0x01u,
            .has_minimum = 1u,
            .has_maximum = 1u,
            .id = "channel.ao.1",
            .name = "AnalogOutput1",
            .description = "Industrial analog output AO0",
            .unit = "V",
            .flags = SEED_OUTPUT_FLAGS,
            .minimum = 0.0,
            .maximum = 10.0,
            .settings = SEED_SETTINGS,
        },
        {
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_ANALOG,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_FLOAT,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_AD5422,
            .source_index = 1u,
            .source_flags = 0x01u,
            .has_minimum = 1u,
            .has_maximum = 1u,
            .id = "channel.ao.2",
            .name = "AnalogOutput2",
            .description = "Industrial analog output AO1",
            .unit = "V",
            .flags = SEED_OUTPUT_FLAGS,
            .minimum = 0.0,
            .maximum = 10.0,
            .settings = SEED_SETTINGS,
        },
        /* DI0..DI3 -- MAX22190, 1.6 ms filtering. */
        {
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_DISCRETE,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_BOOLEAN,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_MAX22190,
            .source_index = 0u,
            .source_flags = 0x04u,
            .id = "channel.di.1",
            .name = "DigitalInput1",
            .description = "Industrial digital input DI0",
            .unit = "",
            .flags = SEED_INPUT_FLAGS,
            .settings = SEED_SETTINGS,
        },
        {
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_DISCRETE,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_BOOLEAN,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_MAX22190,
            .source_index = 1u,
            .source_flags = 0x04u,
            .id = "channel.di.2",
            .name = "DigitalInput2",
            .description = "Industrial digital input DI1",
            .unit = "",
            .flags = SEED_INPUT_FLAGS,
            .settings = SEED_SETTINGS,
        },
        {
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_DISCRETE,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_BOOLEAN,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_MAX22190,
            .source_index = 2u,
            .source_flags = 0x04u,
            .id = "channel.di.3",
            .name = "DigitalInput3",
            .description = "Industrial digital input DI2",
            .unit = "",
            .flags = SEED_INPUT_FLAGS,
            .settings = SEED_SETTINGS,
        },
        {
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_DISCRETE,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_BOOLEAN,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_MAX22190,
            .source_index = 3u,
            .source_flags = 0x04u,
            .id = "channel.di.4",
            .name = "DigitalInput4",
            .description = "Industrial digital input DI3",
            .unit = "",
            .flags = SEED_INPUT_FLAGS,
            .settings = SEED_SETTINGS,
        },
        /* DO0/DO1 -- IPS2050 protected high-side outputs. */
        {
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_DISCRETE,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_BOOLEAN,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_IPS2050,
            .source_index = 0u,
            .source_flags = 0x00u,
            .id = "channel.do.1",
            .name = "DigitalOutput1",
            .description = "Industrial digital output DO0",
            .unit = "",
            .flags = SEED_OUTPUT_FLAGS,
            .settings = SEED_SETTINGS,
        },
        {
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_DISCRETE,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_BOOLEAN,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_IPS2050,
            .source_index = 1u,
            .source_flags = 0x00u,
            .id = "channel.do.2",
            .name = "DigitalOutput2",
            .description = "Industrial digital output DO1",
            .unit = "",
            .flags = SEED_OUTPUT_FLAGS,
            .settings = SEED_SETTINGS,
        },
        /* UIC0..UIC3 -- universal inputs, seeded in digital-input mode. */
        {
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_DISCRETE,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_BOOLEAN,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_UIC,
            .source_index = 0u,
            .source_flags = 0x00u,
            .id = "channel.uic.1",
            .name = "UniversalInput1",
            .description = "Universal protected input UIC0",
            .unit = "",
            .flags = SEED_INPUT_FLAGS,
            .settings = SEED_SETTINGS,
        },
        {
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_DISCRETE,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_BOOLEAN,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_UIC,
            .source_index = 1u,
            .source_flags = 0x00u,
            .id = "channel.uic.2",
            .name = "UniversalInput2",
            .description = "Universal protected input UIC1",
            .unit = "",
            .flags = SEED_INPUT_FLAGS,
            .settings = SEED_SETTINGS,
        },
        {
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_DISCRETE,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_BOOLEAN,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_UIC,
            .source_index = 2u,
            .source_flags = 0x00u,
            .id = "channel.uic.3",
            .name = "UniversalInput3",
            .description = "Universal protected input UIC2",
            .unit = "",
            .flags = SEED_INPUT_FLAGS,
            .settings = SEED_SETTINGS,
        },
        {
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_DISCRETE,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_BOOLEAN,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_UIC,
            .source_index = 3u,
            .source_flags = 0x00u,
            .id = "channel.uic.4",
            .name = "UniversalInput4",
            .description = "Universal protected input UIC3",
            .unit = "",
            .flags = SEED_INPUT_FLAGS,
            .settings = SEED_SETTINGS,
        },
    }};

#endif
