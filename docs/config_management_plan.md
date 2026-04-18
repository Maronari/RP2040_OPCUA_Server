# Configuration Management Plan

## Goal

- Add safe network import, export, apply, and factory reset for persistent device configuration.
- Reuse the OTA design principles: staged writes, validation before apply, and atomic activation.
- Keep `opcua_model/src/opcua_model.c` as an OPC UA adapter layer while moving business logic into dedicated services.

## Invariants

- `boards/include/boards/weact_studio_rp2350a_16mb.h` remains the source of board-level flash size facts.
- `ota/cmake/ota_layout.cmake` remains the single source of truth for flash partitioning.
- Firmware image state and configuration state stay separate.
- Compile-time defaults remain available even if both persisted config slots are invalid.
- Certificates, private keys, trust lists, OTA state, runtime diagnostics, and history are not part of the first config import/export scope.

## Target Scope

The first configuration management implementation should cover:

- device name and OPC UA hostname
- network mode and IPv4 settings
- NTP server and timezone offset
- MQTT broker settings already present in `device_profile_config_t`
- watchdog timeout and diagnostics toggles that are intended to be runtime-configurable
- channel configuration already represented through the channel manager

The first implementation should explicitly exclude:

- firmware and bootloader updates
- OPC UA server certificate/private key and trust list provisioning
- audit/history buffers and runtime counters
- one-shot maintenance state such as OTA progress

## Flash Layout Direction

To support safe persistent configuration, reserve two config slots in flash and keep OTA state separate.

Recommended 16 MB layout direction:

- Bootloader: `0x10000000 .. 0x1000FFFF` (`64 KiB`)
- App Bank A: `0x10010000 .. 0x107EFFFF`
- App Bank B: `0x107F0000 .. 0x10FCFFFF`
- Config Slot 0: `0x10FD0000 .. 0x10FDFFFF` (`64 KiB`)
- Config Slot 1: `0x10FE0000 .. 0x10FEFFFF` (`64 KiB`)
- OTA State: `0x10FF0000 .. 0x10FFFFFF` (`64 KiB`)

This keeps configuration in an A/B model similar to firmware staging while preserving a dedicated OTA state sector.

## Persistent Format

Each config slot should contain a small validated header plus payload.

Suggested header shape:

```c
typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint16_t header_version;
    uint16_t payload_format;
    uint32_t schema_version;
    uint32_t generation;
    uint32_t payload_size;
    uint32_t payload_crc32;
    uint32_t flags;
    uint32_t reserved[4];
} config_slot_header_t;
```

Expected semantics:

- `magic`: identifies a valid config slot image, for example `CFG1`
- `header_version`: version of the slot wrapper format
- `payload_format`: start with `json`, leave room for `cbor` later
- `schema_version`: version of the config payload schema
- `generation`: monotonically increasing activation counter
- `payload_size`: byte length of config payload
- `payload_crc32`: integrity check for payload bytes
- `flags`: valid, pending-apply, and reserved bits

Active config selection rules:

1. Read both slots.
2. Validate header, payload bounds, and CRC.
3. Choose the valid slot with the highest `generation`.
4. If neither slot is valid, use compile-time defaults from the firmware profile.

## Payload Format

The first version should use JSON payloads to keep export, debugging, and migration simple.

Payload should contain:

- `schemaVersion`
- `device`
- `network`
- `time`
- `mqtt`
- `diagnostics`
- `opcua`
- `channels`

The JSON structure should mirror persistent parts of `device_profile_config_t` plus channel settings already modeled in the channel layer.

## Runtime Model

Configuration should be applied in three layers:

1. Firmware profile compile-time defaults.
2. Persisted active config overrides from flash.
3. Runtime-only transient changes if they exist later.

This keeps the device bootable even with erased or corrupted configuration slots.

## Service Split

Do not continue growing config business logic inside `opcua_model/src/opcua_model.c`.

Add dedicated services instead:

- `services/include/services/config_wire.h`
- `services/src/config_wire.c` if helper functions are needed
- `services/include/services/config_store_service.h`
- `services/src/config_store_service.c`
- `services/include/services/config_import_service.h`
- `services/src/config_import_service.c`
- `services/include/services/config_validate_service.h`
- `services/src/config_validate_service.c`
- `services/include/services/config_export_service.h`
- `services/src/config_export_service.c`
- `services/include/services/config_reset_service.h`
- `services/src/config_reset_service.c`

Responsibilities:

- `config_store_service`: slot read/select/write/reset primitives
- `config_import_service`: staged upload session and chunk buffering
- `config_validate_service`: schema and business-rule validation
- `config_export_service`: serialize current effective config
- `config_reset_service`: reset scopes and default restoration behavior

`opcua_model/src/opcua_model.c` should only parse OPC UA inputs, call the services, and return method results.

## OPC UA Maintenance API

Add configuration methods under `Device.Maintenance`.

Recommended first API:

- `GetConfigState() -> String`
- `ExportConfig() -> String`
- `BeginConfigImport(size, crc32, schemaVersion) -> Boolean`
- `WriteConfigChunk(ByteString) -> Boolean`
- `ValidateImportedConfig() -> String`
- `ApplyImportedConfig() -> Boolean`
- `AbortConfigImport() -> Boolean`
- `FactoryReset(scope) -> Boolean`
- `Reboot() -> Boolean`

Where:

- `GetConfigState` returns JSON describing active slot, generation, schema version, and any staged import session state.
- `ExportConfig` returns JSON for the currently effective config.
- `Begin/Write/Abort` mirror the already-working OTA upload pattern.
- `ValidateImportedConfig` returns structured validation diagnostics before apply.
- `ApplyImportedConfig` activates the newly imported config atomically.
- `FactoryReset` clears config slots according to a reset scope.

## Validation Rules

Validation should happen before flash activation and be split into layers:

- syntax validation: parse JSON successfully
- schema validation: required fields and supported schema version
- business validation: legal combinations and ranges

Examples of business validation to enforce early:

- DHCP mode must not require a static IPv4 tuple
- static mode must provide valid IP, netmask, and gateway values
- OPC UA port must be within range
- watchdog timeout must remain above a safe minimum
- channel IDs must be unique
- channel timing and threshold values must remain sane

## Factory Reset Semantics

`FactoryReset` should initially operate only on persisted configuration slots.

Suggested reset scopes:

- `runtime_config`
- `network_config`
- `application_config`
- `all_keep_identity`
- `all_keep_identity_and_network`

For the first implementation, it is acceptable to support only `all_keep_identity` and map it to:

- erase both config slots
- reboot or reinitialize config from compile-time defaults
- keep identity, trust list, and firmware images unchanged

## Phases

1. Extend flash layout with `config_slot_0` and `config_slot_1`.
2. Introduce `config_wire` and `config_store_service` with A/B slot selection.
3. Teach startup to load persisted active config over compile-time defaults.
4. Add `config_export_service` and a minimal `ExportConfig` / `GetConfigState` OPC UA surface.
5. Add `config_import_service` with staged chunked upload.
6. Add `config_validate_service` and `ValidateImportedConfig`.
7. Add `ApplyImportedConfig` and atomic generation switching.
8. Add `FactoryReset` for config slots only.
9. Only after that consider larger payload support or OPC UA `FileType` migration.

## Verification Plan

- Unit-test slot selection with empty, single-valid, dual-valid, and corrupt-slot cases.
- Verify that config export matches current effective runtime configuration.
- Import a valid config and confirm it survives reboot.
- Import an invalid config and confirm the active slot does not change.
- Erase both slots and confirm compile-time defaults still boot.
- Run factory reset and confirm defaults return without affecting identity material.

## Open Questions

- Whether `64 KiB` per config slot is enough once channel count grows.
- Whether secrets such as MQTT credentials need a separate encrypted storage path.
- Whether some config fields should apply live without reboot while others require reboot.
- Whether later migration to `OPC UA FileType` is worth the added model complexity for config payload transfer.
