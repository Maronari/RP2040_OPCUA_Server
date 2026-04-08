# Telemetry Channel Migration

## Current temperature coupling

The current runtime is organized around a single hard-coded temperature source.
The tightest temperature-specific points are:

- `drivers/temp_sensor_driver.*`: a single physical source that reads internal temperature in Celsius.
- `services/sensor_manager.*`: a singleton snapshot with `temperature_c`, quality, counters, and timestamp.
- `services/history_service.*`: history still records one analog telemetry channel, even though the API is now channel-based.
- `services/alarm_service.*`: threshold logic only for temperature (`HIGH` / `HIGH_HIGH`).
- `services/include/services/config_service.h`: device-wide settings only. Channel-specific sampling/scaling/alarm parameters have been moved out.
- `app/basic_profile/main.c`: startup profile assumes one temperature sensor task.
- `opcua_model/src/opcua_model.c`: namespace, reads, writes, events, historizing and alarms are centered on `Device.ProcessValues.Temperature1`.

This makes the current implementation workable for one sensor, but brittle for reuse with humidity, current, discrete inputs, counters, derived values, or multiple sensors of the same kind.

## Target direction

The target architecture is:

- data sources plug into telemetry channels
- a temperature measurement becomes one instance of `telemetry_channel_t`
- channel drivers/adapters own source-specific polling and conversion
- OPC UA is built from `Device.Channels/*`, not from special-case nodes per signal type

The new base abstractions added in this step are:

- `services/include/services/telemetry_channel.h`
- `services/include/services/telemetry_driver.h`
- `services/include/services/channel_config.h`
- `services/include/services/channel_manager.h`
- `services/src/channel_manager.c`

These files started as lightweight scaffolding. The current firmware already uses them for the first telemetry channel and exposes temperature under `Device.Channels.Temperature1`.

## Core model

`telemetry_channel_t` represents one channel instance and now contains:

- `metadata`
- `settings`
- `driver`
- `driver_context`
- `runtime`

`metadata` currently holds:

- `id`
- `name`
- `description`
- `kind`
- `value_type`
- `unit`
- `range`
- `flags`

`settings` currently holds:

- `enabled`
- `sample_period_ms`
- `publish_period_ms`
- `history_interval_ms`
- `scale`
- `offset`
- `warning_threshold`
- `critical_threshold`

`runtime` currently holds:

- `value`
- `quality`
- `source_timestamp_epoch_s`
- `last_update_ms`
- `sample_count`
- `error_count`
- `runtime_flags`
- `alarm_state`

## Driver contract

`telemetry_driver_vtbl_t` is the generic adapter surface for any source:

- `init`
- `poll`
- `configure`
- `self_test`

That is enough for the first migration phase.

Examples of future adapters:

- internal temperature ADC adapter
- Modbus register adapter
- GPIO/discrete input adapter
- computed/virtual channel adapter
- derived channel that consumes another channel

## Channel manager

`channel_manager` is a registry, not a rewrite of runtime behavior.
It now supports:

- register channel
- register channels from static config
- enumerate channels
- find channel by id
- find channel by name
- poll all registered drivers

The current implementation is static and embedded-friendly:

- fixed registry size
- no heap allocation
- channel storage is provided by the caller

## Static configuration

`channel_config_t` is the first step away from hard-coded namespace nodes.
It describes the static shape of a channel and can later be made persistent.

At this stage the recommended migration model is:

1. Keep `device_profile_config_t` for existing device/network/security settings.
2. Introduce a static table of `channel_config_t` entries per firmware profile.
3. Instantiate runtime channels from that table with `channel_manager_register_from_config(...)`.
4. Move temperature-specific config fields gradually into per-channel configuration.

## Suggested migration path

### Phase 1: Introduce channel abstractions without behavior change

- keep `sensor_manager`, `history_service`, `alarm_service`, `opcua_model` working as they are
- add channel types and manager
- define one static channel config for the existing temperature source

Result:

- no user-visible breakage
- a stable contract exists for future migration

### Phase 2: Wrap temperature as a legacy channel

- implement a temperature adapter that fills one `telemetry_channel_t`
- keep `sensor_manager` as a compatibility shim reading channel runtime
- keep existing history/alarm behavior while moving runtime state behind the channel registry

Result:

- temperature is already a channel internally
- existing code can still ask for the old snapshot while the rest of the system migrates

### Phase 3: Generalize history and alarm evaluation

- introduce channel-aware history storage instead of temperature-only samples
- move threshold/alarm metadata behind per-channel descriptors
- make `alarm_service` evaluate channels instead of temperature snapshot only

Result:

- alarms/history stop being temperature-specific services

### Phase 4: Rebuild OPC UA around `Device.Channels/*`

- generate channel nodes from the channel registry
- move current `Temperature1` data to `Device.Channels.Temperature1`
- retire `Device.ProcessValues.Temperature1`

Recommended target shape:

- `Device.Channels`
- `Device.Channels.Temperature1`
- `Device.Channels.Temperature1.Value`
- `Device.Channels.Temperature1.Quality`
- `Device.Channels.Temperature1.SourceTimestamp`
- `Device.Channels.Temperature1.Unit`
- `Device.Channels.Temperature1.SamplePeriodMs`
- `Device.Channels.Temperature1.PublishPeriodMs`
- `Device.Channels.Temperature1.RangeMin`
- `Device.Channels.Temperature1.RangeMax`
- `Device.Channels.Temperature1.AlarmState`

### Phase 5: Remove temperature-only services

- retire `sensor_manager_snapshot_t`
- replace temperature-only historian and threshold logic
- keep `temp_sensor_driver` as just one driver backend

## Temperature mapping into the new model

The current temperature source maps naturally:

- `sensor_manager_snapshot_t.temperature_c` -> `channel.runtime.value.f32`
- `sensor_manager_snapshot_t.quality` -> `channel.runtime.quality`
- `sensor_manager_snapshot_t.source_timestamp_epoch_s` -> `channel.runtime.source_timestamp_epoch_s`
- legacy temperature config -> `channel.settings`
- `temp_scale` -> `channel.settings.scale`
- `temp_offset_c` -> `channel.settings.offset`
- `sampling_period_ms` -> `channel.settings.sample_period_ms`
- `publish_period_ms` -> `channel.settings.publish_period_ms`
- `temperature_history_interval_ms` -> `channel.settings.history_interval_ms`
- `high_temperature_threshold_c` / `high_high_temperature_threshold_c` -> `channel.settings.warning_threshold` / `channel.settings.critical_threshold`

## Recommended next implementation step

The next practical step should stay narrow:

- move persistent channel settings out of the legacy device profile config into a dedicated per-channel persistence model
- generalize history storage from "first analog channel" to a true multi-channel policy
- generalize alarm evaluation from the first channel to configurable per-channel alarm capabilities

That keeps the migration incremental while moving the remaining temperature-specific services behind channel metadata.
