# RP2350 OPC UA Device Platform

This repository contains firmware for an RP2350-based
industrial OPC UA device with PSRAM and W5500 Ethernet. The reference target is
`waveshare_core2350b_16mb_psram8mb`; the default application target is
`device_profile_basic`.

The current implementation target is `v0.4.0`. It modularizes the OPC UA
adapter while preserving the externally visible `v0.3.4` address-space and
behavior contract.

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

## Current Boundaries

- This is not a production security baseline yet. OTA payload integrity is
  still CRC based; signed OTA and rollback-health confirmation are scheduled for
  later roadmap releases.
- The OPC UA adapter is split into focused modules backed by one bounded
  internal context; channel-runtime and service refactors remain assigned to
  later roadmap releases.
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

The `v0.4.0` release workflow treats these as required artifacts for the
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

The blocking `v0.4.0` CI split is:

- `host`: the deterministic host/static suite runner plus its explicit injected
  failure proof and the blocking `v0.3.4` public OPC UA inventory comparison.
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
