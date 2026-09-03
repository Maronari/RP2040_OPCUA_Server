# Contributor Instructions

`ROADMAP.md` is the authoritative implementation plan for this repository.
Read it before planning or changing code.

If this file, the applicable release plan, and `ROADMAP.md` disagree, stop
before editing and report the conflict. Do not modify `ROADMAP.md` while
implementing or releasing a version. Roadmap changes require a separate,
explicit roadmap-maintenance task. Do not modify this file or the applicable
release plan unless the task explicitly includes instruction or release-plan
maintenance.

## Scope and release discipline

- Work only on the first unreleased roadmap version. The current target is
  **v0.5.0 — Transactional Runtime Configuration**.
- Do not pull features, persistence, refactors, or abstractions from later
  releases into the current release. Do not change release scope or `ROADMAP.md` unless explicitly requested. If the requested work is outside the first unreleased release, report it and stop before implementation.
- Stop when the current release acceptance criteria are met. Do not add
  speculative APIs or companion features.
- Do not implement any item explicitly deferred beyond v1.0.0.
- Preserve existing public NodeIds, BrowseNames, method signatures,
  configuration fields, and behavior unless the roadmap includes a documented
  deprecation and migration path.
- When releasing a version, report the roadmap checklist status and release
  commit SHA in the final report without modifying `ROADMAP.md`.

## Required workflow

Before editing:

1. Inspect the affected implementation, public headers, configuration schema,
   stable NodeIds, CMake integration, and verification scripts.
2. Search for existing functionality before adding a new service, helper, or
   abstraction. This repository already contains channel scaffolding, role-based
   security, Events/A&C, Methods, HistoryRead, PubSub/MQTT, persistent
   configuration, OTA staging, certificate/trust management, PSRAM allocation,
   FULL Namespace 0, DI, and AMB support.
3. Write a file-level implementation plan limited to the current roadmap
   version. Identify public-contract and resource-limit effects.
4. Preserve unrelated working-tree changes. Never overwrite or clean files that
   are outside the task.

## Subagent workflow

Project-scoped subagents are defined in `.codex/agents`. Use them for concrete,
bounded work that matches their role; do not invent agent names or delegate the
same responsibility to multiple agents.

- `code-mapper` is the read-only exploration agent. Use it before non-trivial
  implementation to map files, symbols, state owners, mutation sites, NodeIds,
  FreeRTOS flow, and affected build/test paths.
- `embedded-systems` owns bounded firmware/service/driver implementation,
  synchronization, memory placement, timing, Flash/OTA, and hardware-facing
  failure behavior.
- `iot-engineer` owns OPC UA adapters and contracts, DI/AMB semantics,
  authorization, events, HistoryRead, PubSub/MQTT, interoperability, and
  asyncua/open62541 verification.
- `build-engineer` owns CMake/Pico SDK integration, version propagation,
  reference-target builds, firmware artifacts, size reporting, and scoped CI
  gate work.
- `code-reviewer` is the read-only independent reviewer. Use it after
  implementation and focused verification to review the complete task diff.

The primary agent owns task scope, the implementation plan, integration,
conflict resolution, final verification, and the user-facing report. Subagents
must read and follow this file, `ROADMAP.md`, and the applicable release plan in
`docs/releases/`.

Delegation rules:

1. Give every subagent a concrete deliverable, file/symbol boundary, and explicit
   read-only or write scope.
2. Parallelize only independent work. Do not let two write-capable agents edit
   the same file or subsystem concurrently.
3. Use `code-mapper` findings as evidence for the implementation plan; do not
   treat likely paths as confirmed.
4. Keep service implementation and OPC UA adapter work separate where possible:
   `embedded-systems` owns service/native code and `iot-engineer` owns model and
   verification integration.
5. Use `build-engineer` after the initial implementation exists, unless the task
   is itself a build-system diagnosis.
6. Use `code-reviewer` only after the integrated diff and focused tests are
   ready. Resolve its actionable findings and rerun affected checks.
7. Inspect and validate every subagent result before integrating or reporting
   it. Subagent claims are not verification evidence unless commands or code
   locations support them.
8. Subagent work does not expand authorization or roadmap scope. Preserve
   unrelated user changes shared through the working tree.

For v0.5.0 implementation and verification, keep assignments aligned with
`docs/releases/v0.5.0.md`. The configuration transaction chain is serialized:
`embedded-systems` owns the service/native transaction, driver lifecycle and
Flash-backend behavior; `iot-engineer` owns the OPC UA restart adapter,
compatibility model, configuration Methods and automated client verification;
`build-engineer` owns version propagation, build/artifact, size and Flash-layout
checks; and `code-reviewer` performs the final independent review. The isolated
missing-driver validation work may run independently only where the release plan
explicitly permits it.

## Architecture boundaries

- Services own business and runtime state. OPC UA callbacks are adapters: they
  validate and convert arguments, call service APIs, map service results to
  deterministic OPC UA status codes, and update/publish the model from returned
  state. They must not contain maintenance, configuration, alarm, security, or
  OTA state machines.
- Drivers own hardware. Code under `services/` must not configure or access ADC,
  GPIO, SPI, I2C, Flash, or other peripherals directly.
- Keep service public APIs independent of open62541 types where practical.
- Native drivers are statically compiled firmware. Configuration may select an
  installed driver by stable `driver_id`, but must never upload or execute
  driver code, scripts, bytecode, or dynamically loadable modules.
- Build-time driver selection must permit disabled drivers to be removed from
  the ELF. Inactive compiled drivers must not reserve permanent runtime memory.
- Do not duplicate state. Use one service-owned transaction for related
  maintenance state, state NodeId/text, planned fields, and condition metadata.

## Embedded-system rules

- Bound every queue, ring, journal, continuation point, session table,
  subscription table, log, string, and temporary buffer explicitly.
- Define deterministic behavior at capacity and on invalid input, allocation
  failure, partial initialization, timeout, and rollback. Never silently fall
  back to unsafe defaults.
- Avoid long critical sections and blocking unrelated channels or clients.
- Make cross-task ownership and synchronization explicit; do not retain borrowed
  pointers unless their lifetime is guaranteed and documented.
- Measure SRAM, PSRAM, task-stack, ELF-section, UF2, and BIN impact for changed
  runtime features.
- Use a single version source for runtime firmware revision, OTA metadata,
  configuration export, artifacts, tags, and documentation.

## v0.5.0 constraints

- Treat tag `v0.4.3` and commit
  `dec08ba22e835e8037f9fd5342dcc63ddc03f5ac` as the immutable implementation
  and compatibility baseline.
- Follow `docs/releases/v0.5.0.md`. The transaction order is validate ->
  persist staged -> stop -> activate -> verify at least one valid read per
  new/changed channel -> commit or rollback. `config_service` remains the sole
  transaction owner and `sensor_manager` remains the driver-lifecycle executor.
- Keep staged, running and last-known-good configuration in distinct,
  power-loss-recoverable roles inside the existing bounded configuration Flash
  region. Recovery must expose a complete old or new configuration, never a
  mixture.
- Treat configuration schema version as real persisted/imported data. Migrate
  supported older forms and reject incompatible or future schemas
  deterministically.
- Report every missing driver and resource conflict in a rejected candidate;
  do not stop at the first missing `driver_id`.
- A transaction that changes the running information model triggers a
  controlled full OPC UA server restart. Do not implement live namespace
  mutation. A transaction that does not require restart must leave sessions
  and subscriptions intact.
- Deleting a channel, or modifying its driver/value type under the same stable
  channel ID, clears its history ring and alarm condition/acknowledgement state
  as documented in the release plan and changelog.
- Expose the three runtime configuration presets and bounded compatibility data
  required by the release plan. Do not confuse runtime presets with compile-time
  driver packs.
- Preserve all unrelated v0.4.3 public NodeIds, BrowseNames, method signatures,
  events, permissions and configuration fields. Do not start later-release
  dynamic driver loading, signed OTA, Historical Event Access, Modbus or
  industrial-board driver work.

## v0.5.0 OTA verification authorization

For v0.5.0 verification only, Codex is authorized to stage and upload a
verified firmware artifact to the user-designated reference test device through
the existing authenticated OTA workflow in `static/tools/stage_ota.py`, finalize
the update and reboot with the explicit `--reboot-after-finalize` option when a
planned transaction recovery or reconnect check requires it.

Before upload, record and verify the endpoint/device identity, board/profile,
artifact version, hash/CRC and current OTA status. Use the matching generated
manifest and payload. Do not target an unidentified or production device,
weaken authentication or integrity checks, alter security/trust/commissioning,
or add/change OTA functionality. Abort staging on failure where the existing
workflow permits it, and record transfer, finalize, reboot, reconnect, health
and recovery/rollback outcomes. If network OTA fails, fall back to direct USB
BOOTSEL flashing and record which path was used. Preserve the known-good
configuration unless a test explicitly requires a documented configuration
change. This authorization expires when v0.5.0 work is complete.

## Build and verification

The reference target is RP2350 + W5500:
`waveshare_core2350b_16mb_psram8mb`. Keep generated output in an out-of-tree
build directory.

Required single Debug build:

```sh
C:\msys64\mingw64\bin\cmake.EXE -DCMAKE_BUILD_TYPE:STRING=Debug -DPICO_BOARD:STRING=waveshare_core2350b_16mb_psram8mb -DOTA_ROM_PRIMARY_APP:STRING=ON -DOTA_BUILD_BOOTLOADER:STRING=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE -DCMAKE_C_COMPILER:FILEPATH=D:\Programms\ST\STM32CubeCLT_1.18.0\GNU-tools-for-STM32\bin\arm-none-eabi-gcc.exe -DCMAKE_CXX_COMPILER:FILEPATH=D:\Programms\ST\STM32CubeCLT_1.18.0\GNU-tools-for-STM32\bin\arm-none-eabi-g++.exe --no-warn-unused-cli -S E:/Projects/RP2040_OPCUA_Server -B e:/Projects/RP2040_OPCUA_Server/build -G Ninja
C:\msys64\mingw64\bin\cmake.EXE --build e:/Projects/RP2040_OPCUA_Server/build --config Debug --target all --
```

For every feature:

- Add positive, negative, authorization, illegal-state, and resource-limit tests.
  Add reboot/power-loss tests when persistence is involved.
- For v0.5.0, run the complete gate in `docs/releases/v0.5.0.md`, including
  schema migration, transaction-phase and Flash power-loss injection,
  driver-init/verification rollback, exhaustive missing-driver/resource
  conflicts, restart/no-restart session behavior, history/alarm cleanup,
  all three presets, version integrity and automated reconnect checks.
- Verification scripts must exit nonzero on failure. CI release gates must
  actually fail; do not use `continue-on-error`, unconditional `|| true`, or
  smoke tests that assert no behavior.
- Complete one Debug build for the reference target with no new first-party
  warnings.
- Confirm affected behavior with an automated open62541/asyncua client and
  UaExpert when public OPC UA behavior is touched. Compare the public contract
  and exported NodeSet/inventory against tag v0.4.3 and document every intended
  addition.
- Record artifact sizes and memory/stack impact. Update public NodeId/schema
  documentation, README, release notes, and tools when affected.
- Report commands actually run and any validation that could not be run. Do not
  claim hardware, UaExpert, or network verification without performing it.

## Definition of done

A change is complete only when its implementation, tests, failure behavior,
resource bounds, documentation, and roadmap acceptance criteria agree. Review
the general definition of done in `ROADMAP.md` in addition to the version-specific
checklist before handing off or releasing.
