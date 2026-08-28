#ifndef SERVICES_DEVICE_CHANNELS_H
#define SERVICES_DEVICE_CHANNELS_H

/**
 * @file
 * @brief String constants naming the single legacy "Temperature1" channel
 *        (config id, display name, and OPC UA NodeId path segments).
 *
 * DEVICE_CHANNEL_ID_TEMPERATURE_1 is used as the default seeded channel id
 * (app/basic_profile/main.c) and as the lookup key for the legacy
 * alarm_service_get_status() singleton shim (alarm_service.c). The
 * DEVICE_CHANNEL_NODE_TEMPERATURE_1* constants are OPC UA NodeId string path
 * segments consumed by opcua_model/src/model_channels.c and related model_*
 * files to address this one channel's nodes directly, independent of the
 * general per-channel node addressing used for every other configured
 * channel. Not used to enumerate or address any other channel.
 */

#define DEVICE_CHANNELS_NODE_ID "Device.Channels"
#define DEVICE_CHANNEL_ID_TEMPERATURE_1 "channel.temperature.1"
#define DEVICE_CHANNEL_NAME_TEMPERATURE_1 "Temperature1"
#define DEVICE_CHANNEL_NODE_TEMPERATURE_1 "Device.Channels.Temperature1"
#define DEVICE_CHANNEL_NODE_TEMPERATURE_1_VALUE "Device.Channels.Temperature1.Value"
#define DEVICE_CHANNEL_NODE_TEMPERATURE_1_STATUS "Device.Channels.Temperature1.Status"
#define DEVICE_CHANNEL_NODE_TEMPERATURE_1_SOURCE_TIMESTAMP "Device.Channels.Temperature1.SourceTimestamp"
#define DEVICE_CHANNEL_NODE_TEMPERATURE_1_UNITS "Device.Channels.Temperature1.Units"
#define DEVICE_CHANNEL_NODE_TEMPERATURE_1_UPDATE_PERIOD_MS "Device.Channels.Temperature1.UpdatePeriodMs"
#define DEVICE_CHANNEL_NODE_TEMPERATURE_1_HISTORY_INTERVAL_MS "Device.Channels.Temperature1.HistoryIntervalMs"
#define DEVICE_CHANNEL_NODE_TEMPERATURE_1_ALARM_STATE "Device.Channels.Temperature1.AlarmState"

#endif
