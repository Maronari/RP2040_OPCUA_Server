# RP2350 OPC UA Device Platform

This repository contains firmware for an RP2350-based
industrial OPC UA device with PSRAM and W5500 Ethernet. The reference target is
`waveshare_core2350b_16mb_psram8mb`; the default application target is
`device_profile_basic`.

The current implementation target is `v0.5.0`. It restructures runtime
configuration into a real validate -> persist-staged -> stop -> activate ->
verify -> commit/rollback transaction with a staged/running/last-known-good
Flash backend and a real configuration schema version, adds exhaustive
missing-driver reporting, consumes the reboot-required diagnostic to trigger
an actual controlled OPC UA server restart, clears per-channel history/alarm
state on channel delete/modify, and adds runtime configuration presets plus
firmware/configuration compatibility data on top of the per-channel
history/alarms released in `v0.4.3`, while preserving unrelated public
contracts.

## What The Firmware Already Implements

- Layered firmware split into `drivers`, `services`, `opcua_model`, `ota`, and
  `app`.
- RP2350 board support with external PSRAM and W5500 Ethernet.
- FreeRTOS + lwIP based OPC UA server runtime using open62541.
- Persistent staged/running configuration with export/import/reset flows.
- Role-aware authentication, trust-list management, identity rotation, and
  security-state persistence.
- DI/AMB-oriented asset and maintenance model with a persisted maintenance
  journal from `v0.3.3`.
- OTA flash layout, OTA state storage, and CRC-checked staged update plumbing.
- Build-time generated configuration export and OTA/layout manifests.
- A bounded, manager-owned multi-channel runtime with coherent typed snapshots,
  Unix-microsecond source timestamps, explicit driver lifecycle/capabilities,
  and deterministic hardware-resource conflict rejection.
- A discovery-based generic channel address space with the deterministic NodeId,
  metadata, timestamp, quality, and write contract documented in
  [docs/channel_model.md](docs/channel_model.md).
- Per-channel history (typed PSRAM ring plus a Flash-backed ring surviving
  reboot/power loss) exposed through a generalized OPC UA `HistoryRead` with
  bounded continuation points.
- Per-channel Alarms & Conditions (high/high-high/low/low-low with
  hysteresis and debounce for numeric channels, a discrete state for Boolean
  channels, and a quality-fault state that never overwrites the last
  good-quality alarm state) with independent per-condition acknowledgement.
- A real transaction pipeline for runtime configuration (validate -> persist
  staged -> stop -> activate -> verify -> commit/rollback) backed by a
  staged/running/last-known-good Flash region, a real configuration schema
  version with migration functions (Flash-binary and JSON import/export),
  and exhaustive missing-driver reporting.
- A controlled full OPC UA server restart, synchronously triggered by any
  transaction that changes the running model, and per-channel history/alarm
  state cleanup on channel delete/modify.
- Runtime configuration presets (one reference preset, `sensor_node`, proven
  against the full transaction pipeline) and firmware/configuration
  compatibility data (profile, hardware revision, schema version, supported
  drivers/capabilities) exposed through the management model.

## Current Boundaries

- This is not a production security baseline yet. OTA payload integrity is
  still CRC based; signed OTA and rollback-health confirmation are scheduled for
  later roadmap releases.
- Alarm history is not persisted to Flash (live OPC UA events only); OPC UA
  Part 11 Historical Event Access is scheduled for v0.8.1. A structural
  configuration transaction always triggers a full controlled OPC UA server
  restart rather than live, in-place namespace mutation — a deliberate,
  permanent architectural choice (see `docs/channel_model.md`), not deferred
  work.
- No physical hardware power-cut test has been performed against the v0.5.0
  configuration transaction pipeline (host-test fault injection only); a
  device that has run v0.4.0-v0.4.3 firmware does not have its configuration
  migrated in place when it upgrades to v0.5.0. See
  `docs/known_limits.md#v050-accepted-residual-risks`.
- The built-in `adc_voltage` channel driver is limited to protected low-voltage
  RP2350 ADC use and is not the industrial `AI0..AI3` implementation from
  `HARDWARE_MVP.md`; industrial analog inputs require the dedicated external
  ADC/frontend driver scheduled by the later hardware-driver work.
- The release baseline is RP2350 + PSRAM + W5500. The older RP2040-oriented
  README positioning is obsolete.

## Repository Layout

```text
.
+-- app/                    # Device-profile targets and generated exports
+-- bootloader/             # OTA bootloader target
+-- cmake/                  # Authoritative build/version inputs
+-- drivers/                # Hardware-facing drivers
+-- ota/                    # OTA layout, state, tooling, and artifacts
+-- opcua_model/            # Modular OPC UA address-space and callback adapters
+-- services/               # Runtime/business services
+-- static/tests/           # Host-test sources
+-- static/tools/           # Verification, staging, and release scripts
+-- .github/workflows/      # Blocking CI jobs
+L-- libraries/             # Pinned third-party sources and SDKs
```

## Reference Build

The authoritative local reference build for this release is the Debug RP2350 +
W5500 workflow from `AGENTS.md`:

```powershell
C:\msys64\mingw64\bin\cmake.EXE -DCMAKE_BUILD_TYPE:STRING=Debug -DPICO_BOARD:STRING=waveshare_core2350b_16mb_psram8mb -DOTA_ROM_PRIMARY_APP:STRING=ON -DOTA_BUILD_BOOTLOADER:STRING=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE -DCMAKE_C_COMPILER:FILEPATH=D:\Programms\ST\STM32CubeCLT_1.18.0\GNU-tools-for-STM32\bin\arm-none-eabi-gcc.exe -DCMAKE_CXX_COMPILER:FILEPATH=D:\Programms\ST\STM32CubeCLT_1.18.0\GNU-tools-for-STM32\bin\arm-none-eabi-g++.exe --no-warn-unused-cli -S E:/Projects/RP2040_OPCUA_Server -B e:/Projects/RP2040_OPCUA_Server/build -G Ninja
C:\msys64\mingw64\bin\cmake.EXE --build e:/Projects/RP2040_OPCUA_Server/build --config Debug --target all --
```

For Linux CI, the matching workflow configures the same board and build type
with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` and builds the default `all` target.

## Release Artifacts

The `v0.4.2` release workflow treats these as required artifacts for the
reference Debug build:

- `device_profile_basic.elf`
- `device_profile_basic.uf2`
- `device_profile_basic.bin`
- `build/config_export.json`
- `build/generated/ota_layout/ota_layout.json`
- `build/reports/size.txt`
- `build/reports/sections.txt`
- `build/reports/version_integrity.json`
- `build/reports/opcua_public_contract.json`
- `build/reports/cppcheck.xml`
- `build/reports/cppcheck.txt`

The version verifier checks that the authoritative CMake version, CMake cache,
firmware compile definition, generated config export, generated OTA/layout
manifest, and release-artifact evidence agree. Tag builds must use
`vX.Y.Z == X.Y.Z`.

## Verification

The blocking `v0.4.2` CI split is:

- `host`: the deterministic host/static suite runner plus its explicit injected
  failure proof, the blocking `v0.4.1` public OPC UA inventory comparison, and
  the generic channel-model contract checks.
- `firmware`: reference Debug configure/build, size reporting, version
  verification, and positive/negative artifact checks.
- `analysis`: first-party cppcheck over `drivers`, `services`, `opcua_model`,
  `ota`, `bootloader`, and `app`, excluding generated/vendor noise.

Renode is not a required release gate for this board. Until an asserted RP2350
emulation target exists, emulator work remains an explicit manual experiment.

## Versioning

`cmake/firmware_version.cmake` is the only editable firmware version source.
Runtime firmware revision, CMake project version,
generated config export, OTA/layout metadata, and release verification all
derive from that file.

## Release History

See [CHANGELOG.md](CHANGELOG.md) for the release history and peeled tag commit
SHAs.
