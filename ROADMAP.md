# RP2 Industrial OPC UA Device — Development Roadmap

> **Status:** authoritative implementation plan for Codex and human contributors.
>
> **Current baseline:** `v0.4.2`, commit `568dfd4e2a77c2db6fca4cd9a65e6279624928d6`.
>
> **Repository:** `Maronari/RP2040_OPCUA_Server`.

This file defines the implementation order from the `v0.4.2` baseline to
`v1.0.0`. Codex must follow the versions sequentially and must not pull work
from a later version into the current one unless this file is deliberately
updated first.

## 1. Product target

The project is evolving from a temperature-oriented embedded OPC UA server into a reusable industrial device platform for RP2350-class MCUs:

```text
physical drivers / field protocols
              ↓
       channel runtime
              ↓
configuration / history / alarms / maintenance / security / OTA
              ↓
       OPC UA information model
              ↓
clients, SCADA, CMMS, historians and MQTT brokers
```

The `v1.0.0` target is a resource-bounded, testable and documented RP2350 IIoT node with:

- a generic channel and driver model;
- deterministic OPC UA NodeIds and a documented information model;
- DI/AMB asset, health and maintenance semantics;
- secure configuration, identity and trust lifecycle;
- signed and recoverable OTA;
- bounded local history and alarms;
- Modbus retrofit capability;
- reproducible benchmarks and interoperability evidence.

OPC UA remains the primary northbound interface. It must be an adapter over service state, not the owner of business logic.

## 2. Rules Codex must follow

1. **Work on one version only.** Implement the first unreleased version in this file. Do not start later-version features.
2. **Inspect before editing.** Before changing code, inspect the current implementation, public headers, configuration schema, NodeIds and verification scripts.
3. **Do not duplicate existing functionality.** The repository already contains channel scaffolding, security roles, Events/A&C, OPC UA Methods, HistoryRead support, OPC UA PubSub over MQTT, persistent configuration, OTA staging, certificate/trust management, PSRAM allocation, FULL Namespace 0, DI and AMB.
4. **Services own state.** OPC UA callbacks call service APIs. They must not implement maintenance, configuration, alarm, security or OTA state machines directly.
5. **Drivers own hardware.** Code under `services/` must not directly configure ADC, GPIO, SPI, I2C or other peripherals.
6. **No unbounded allocation.** All queues, logs, continuation points, sessions and temporary buffers require explicit limits and deterministic failure behavior.
7. **Preserve compatibility deliberately.** Existing NodeIds, configuration fields and methods may only be removed after a documented deprecation and migration path.
8. **One source of version truth.** Runtime firmware revision, OTA manifest, exported configuration, build artifacts, release tag and documentation must report the same version.
9. **Tests are part of the feature.** A feature is incomplete until positive, negative, reboot/power-loss where applicable, and resource-limit tests exist.
10. **No fake-green CI.** Required checks must fail the workflow on failure. `continue-on-error`, unconditional `|| true`, or tests that only start an emulator without asserting behavior are not acceptable for release gates.
11. **Report release status separately.** When a version is released, report its acceptance checklist status and release commit SHA in the final report without modifying this file. Updating roadmap checklists or release SHAs requires a separate, explicit roadmap-maintenance task.
12. **Stop at acceptance.** Do not add speculative companion specifications, cloud APIs, REST, GDS, PubSub UDP/TSN, full historian behavior or fleet management unless a later roadmap revision explicitly schedules them.
13. **Drivers are firmware, configuration is data.** Through `v1.0.0`, every native sensor, actuator and field-protocol driver must be statically compiled into the firmware image. Configuration tools may select and parameterize an installed driver by stable `driver_id`, but must not upload, link, interpret or execute new driver code on the device.

## 3. Definition of done for every release

Every version below must satisfy all of these conditions:

- [ ] One `Debug` build succeeds for the reference RP2350 + W5500 board.
- [ ] No new compiler warnings in first-party code.
- [ ] Required CI checks are blocking.
- [ ] ELF section sizes and UF2/BIN sizes are stored as CI artifacts.
- [ ] Internal SRAM, PSRAM and task stack impact is measured for changed runtime features.
- [ ] Public NodeIds and configuration-schema changes are documented.
- [ ] Runtime-reported firmware version equals the Git tag.
- [ ] README, changelog/release notes and relevant tools are updated.
- [ ] New code has bounded failure behavior and does not silently fall back to unsafe defaults.
- [ ] UaExpert plus an automated open62541/asyncua client test pass for affected OPC UA behavior.

## 4. Historical baseline

### `v0.1` — layered embedded server baseline

Tag commit: `f15ba645d99f6441e09953920b09d10fe6131c86`.

The release established the current layered repository shape:

- `drivers`, `services`, `opcua_model` and `app/basic_profile`;
- RP2/WIZnet networking under FreeRTOS/lwIP;
- temperature acquisition;
- network and time services;
- diagnostics, watchdog and configuration services;
- temperature alarms and local history;
- writable OPC UA nodes and timezone configuration;
- standard OPC UA historizing for the temperature value.

The implementation was still centered on a single temperature signal, and `opcua_model.c` already contained most server/model behavior.

### `v0.2` — security, events and channel scaffolding

Tag commit: `3ec41223f1c32c82a746a61647952a1eacd936cf`.

Compared with `v0.1`, this release added:

- `telemetry_channel_t`, `telemetry_driver_vtbl_t`, `channel_config_t` and `channel_manager`;
- secure user authentication, roles, provisioning and access checks;
- OPC UA Methods;
- custom Events and audit bridging;
- optional Alarms & Conditions support;
- role-aware sessions;
- OPC UA PubSub transport over MQTT;
- embedded memory/concurrency hardening.

The channel layer was introduced as migration scaffolding. Temperature, alarm, history and large parts of the information model remained special-cased.

### `v0.3` — persistent platform services and RP2350/PSRAM

Tag commit: `aa31cfee7b7093acd44c7c45048c87df2a8c3c4d`.

Compared with `v0.2`, this release added:

- persistent, validated, staged/running configuration;
- OPC UA configuration import/export/reset/apply operations;
- configured channel definitions and a driver catalog;
- internal-temperature, ADC-voltage and GPIO-input channel sources;
- onboarding and identity provisioning;
- certificate, trust-list and expiry lifecycle services;
- staged OTA, bootloader, flash layout and tooling;
- RP2350 board support and PSRAM allocator;
- FULL Namespace 0 execution from PSRAM;
- reproducible submodule patching;
- CI build, size reporting, static analysis and preliminary Renode execution;
- operational tooling for config, trust, identity, OTA and NodeSet export.

This release changed the project from a demo server into a platform prototype, but several subsystems remained incomplete or insufficiently verified.

### `v0.3.1` — DI/AMB asset and maintenance model

Firmware baseline tag commit: `a91bf946f2b605a1aa9bef8e591bfa95e608803f`.

Compared with `v0.3`, this release added:

- DI Device Health semantics and corresponding conditions;
- AMB asset discovery and aliases;
- hierarchical and operational locations;
- AMB maintenance state machine and maintenance properties;
- `verify_amb_model.py` for read-only structural verification;
- Nodeset injector extensions required by DI/AMB.

The historical firmware source baseline for this section remains `v0.3.1`.

## 5. Current technical constraints

The roadmap below is based on these concrete issues remaining at the `v0.3.2`
baseline:

1. `opcua_model` builds from one large `opcua_model.c`; identity, channels, security, events, configuration, OTA, PubSub and DI/AMB are tightly coupled.
2. `CONFIG_SERVICE_MAX_CHANNELS` is `4`, while `CHANNEL_MANAGER_MAX_CHANNELS` is `16`.
3. `alarm_service` is a singleton hard-coded to `Temperature1`.
4. `history_service` uses one global ring, stores only `float`, retains a borrowed `const char *channel_id`, and scans the ring inside a critical section.
5. `sensor_manager` still exposes a legacy temperature snapshot and chooses a “primary” channel.
6. `driver_catalog.c` is under `services/` but directly accesses ADC and GPIO hardware.
7. The generic value enum includes 64-bit integers and strings, while current conversion/polling paths do not fully support them.
8. Raw channel pointers are returned by registry APIs, so cross-task ownership and synchronization are not explicit.
9. Version metadata now derives from the v0.3.2 CMake source, but release-wide
   version and artifact consistency remains a required gate.
10. README still presents the project as an RP2040 firmware skeleton although the reference build is RP2350 + PSRAM.
11. CI static/security/emulator checks are non-blocking; the Renode job loads an RP2040 platform while the default firmware target is RP2350 and does not assert expected boot behavior.
12. OTA integrity is CRC-based, not cryptographically signed. The application clears the ready-to-boot state immediately after startup rather than after a health-confirmation window.
13. The watchdog task kicks independently of subsystem health, so a deadlocked service can coexist with a healthy watchdog task.
14. AMB maintenance has a volatile service-owned method workflow, but no
    persistent journal or Historical Event Access.

### 5.1 Driver and configurator deployment policy

This policy is mandatory through `v1.0.0` and must be treated as an architectural constraint rather than an implementation suggestion.

- The complete human-facing sensor/module catalog belongs in the desktop configurator or other host tooling. It may contain names, wiring diagrams, supported interfaces, default units, channel templates, register maps and the stable `driver_id` required by each entry.
- The RP2350 firmware contains only the platform core plus a compile-time-selected set of native drivers. A device configuration may reference only drivers present in that exact firmware image.
- The configurator sends configuration data, never executable driver code. It must not send native objects, relocatable modules, scripts, bytecode or dynamically loadable plugins for execution on the MCU.
- Before generating or applying configuration, the configurator must read device capabilities, including at least firmware version, hardware revision/profile, configuration-schema version, `SupportedDrivers`, driver capabilities and available hardware resources.
- Firmware must reject an unknown or unavailable `driver_id` deterministically before changing running state. The error must identify the missing driver and leave the previous known-good configuration active.
- Runtime contexts, task state, channel objects and driver buffers are created only for drivers used by the running configuration. Compiled but inactive drivers may consume external Flash, but must not reserve permanent SRAM/PSRAM proportional to the number of available drivers.
- Driver inclusion must be controlled at build time. Disabled drivers must not remain reachable through a global registry and must be removable from the final ELF by source selection and section garbage collection.
- Initial firmware driver packs are:
  - `base`: internal temperature, generic ADC, GPIO input and GPIO output;
  - `environmental`: `base` plus selected environmental sensors;
  - `motion`: `base` plus selected IMU/vibration sensors;
  - `industrial`: `base` plus industrial analog frontends and Modbus adapters;
  - `development`: all drivers intentionally enabled for integration and test builds.
- A firmware profile change is a firmware update, not a configuration transaction. Before signed OTA is complete it is installed through the normal development/release flashing process; after `v0.6.0` it may be installed as a compatible signed OTA image.
- Dynamic native-driver loading is explicitly deferred beyond `v1.0.0`. Any future reconsideration requires a documented ABI, signature verification, compatibility model, resource isolation and fault-containment design.

## 6. Version roadmap

---

## `v0.3.2` — Maintenance Workflow Core

### Goal

Turn the existing AMB maintenance model into a usable, service-owned workflow without adding persistent history yet.

### Required implementation

- [ ] Add `maintenance_service.[ch]` as the sole owner of maintenance runtime state.
- [ ] Define a bounded `maintenance_activity_t` with operation ID, planned date, downtime estimate, method, supplier/personnel/parts fields, configuration-changed flag, actor and result fields.
- [ ] Implement OPC UA Methods:
  - `PlanMaintenance`;
  - `RescheduleMaintenance`;
  - `StartMaintenance`;
  - `FinishMaintenance`.
- [ ] Enforce the standard AMB state-machine transitions:
  - `Planned -> Executing`;
  - `Executing -> Finished`;
  - `Finished -> Planned`.
- [ ] Treat rescheduling as an update while remaining in `Planned`; it is not a state transition.
- [ ] Do not invent `Cancelled`, `Failed` or `Aborted` states in the standard AMB state machine. A future vendor extension requires an explicit roadmap change.
- [ ] Move all updates of `CurrentState`, `CurrentState.Id`, planned fields and condition metadata behind one atomic maintenance-service transaction.
- [ ] Add the optional AMB fields already identified by the model:
  - `MaintenanceSupplier`;
  - `QualificationOfPersonnel`;
  - `PartsOfAssetReplaced`;
  - `PartsOfAssetServiced`.
- [ ] Emit a live OPC UA event after each successful transition.
- [ ] Return deterministic status codes for invalid arguments, unauthorized calls and illegal transitions.
- [ ] Fix release-version generation so the firmware, OTA metadata and config export report `0.3.2` from one source.

### Tests and acceptance

- [ ] Extend `verify_amb_model.py` or add `verify_maintenance_workflow.py` to call all methods.
- [ ] Test every allowed transition and every forbidden transition.
- [ ] Test role authorization for viewer/operator/service/admin accounts.
- [ ] Subscribe with two simultaneous clients and verify exactly one transition event per successful call.
- [ ] Verify no model callback directly mutates maintenance state outside `maintenance_service`.
- [ ] UaExpert shows synchronized state text and state NodeId after every transition.

### Result after release

The device supports a standards-aligned, remotely callable AMB maintenance workflow, but maintenance state is still volatile across reset.

### Out of scope

Flash journal, Historical Event Access, new AMB states and a general event historian.

---

## `v0.3.3` — Maintenance Journal and Condition Semantics

### Goal

Make maintenance activity power-loss tolerant and operationally meaningful to CMMS/SCADA clients.

### Required implementation

- [ ] Add an append-only, fixed-capacity maintenance journal in QSPI Flash.
- [ ] Each record contains sequence number, schema version, operation ID, timestamp, transition, actor, maintenance method, supplier/personnel references, description, result, serviced/replaced parts and configuration-changed flag.
- [ ] Use CRC plus a commit marker so an interrupted write cannot be mistaken for a valid record.
- [ ] Restore the latest valid maintenance state after reboot.
- [ ] Add bounded read/export methods independent of the physical on-flash representation.
- [ ] Add admin-only journal-clear/reset behavior with an audit event.
- [ ] Define and document the policy for `ActiveState`, `Retain`, `Severity` and `Message`.
- [ ] Emit Audit Events for method calls and state changes.
- [ ] Ensure `ConfigurationChanged` describes the maintenance result, not every state transition.
- [ ] Track erase count and journal utilization in diagnostics.

### Tests and acceptance

- [ ] Power loss before payload, during payload, before commit marker and after commit marker.
- [ ] Ring wrap-around and full-journal behavior.
- [ ] Corrupt-last-record recovery.
- [ ] State restoration after reboot.
- [ ] Multi-client event subscriptions during journal writes.
- [ ] Flash wear calculation documented for the configured record rate and capacity.

### Result after release

Maintenance operations survive reboot and produce a verifiable audit trail suitable for later Historical Event Access.

### Out of scope

OPC UA Part 11 Historical Event Access and arbitrary event-filter execution.

---

## `v0.3.4` — 0.3 Stabilization and Release Integrity

### Goal

Freeze the `0.3` feature set and create a trustworthy baseline before architectural refactoring.

### Required implementation

- [ ] Rename/rewrite README positioning for RP2350 + PSRAM and the actual platform capabilities.
- [ ] Add `CHANGELOG.md` with tag SHAs and feature summaries for `v0.1`, `v0.2`, `v0.3`, `v0.3.1`, `v0.3.2`, `v0.3.3` and `v0.3.4`.
- [ ] Make version metadata derive from one CMake variable or generated version file.
- [ ] Replace non-blocking CI placeholders with real release gates.
- [ ] Run static analysis on first-party `drivers`, `services`, `opcua_model`, `ota`, `bootloader` and `app` code, excluding generated/open62541 amalgamation noise where appropriate.
- [ ] Replace the current Renode smoke test with either:
  - an asserted emulator test for a supported target; or
  - a clearly optional non-gating experiment plus blocking host/service tests.
- [ ] Add host-testable unit tests for configuration validation, channel registry, maintenance transitions, journal parsing and OTA state-record parsing.
- [ ] Save size reports and generated config/OTA manifests as CI artifacts.
- [ ] Document known limits: channels, users, sessions, subscriptions, monitored items, history records, journal records and OTA chunk size.

### Tests and acceptance

- [ ] CI fails on a deliberately broken host test.
- [ ] CI fails when no UF2 is produced.
- [ ] CI verifies runtime/build version consistency.
- [ ] All verification tools have nonzero exit status on failure.
- [ ] No new OPC UA features are added in this version.

### Result after release

`v0.3.4` becomes the reproducible, documented baseline against which all later memory, latency and behavior regressions are measured.

---

## `v0.4.0` — OPC UA Model Modularization

### Goal

Break up the monolithic model implementation without changing the externally visible address space.

### Required implementation

- [ ] Introduce an internal `opcua_model_context_t` containing server handles, namespace indices, module state and bounded scratch storage.
- [ ] Split `opcua_model.c` into focused modules, at minimum:
  - model core/server lifecycle;
  - channel model;
  - identity/DI/AMB model;
  - maintenance model;
  - configuration methods;
  - security and audit model;
  - alarms/events model;
  - history backend;
  - OTA model;
  - PubSub/MQTT model;
  - diagnostics model.
- [ ] Keep service APIs independent of open62541 types where practical.
- [ ] Centralize NodeId constants and namespace lookup.
- [ ] Centralize access-control policy mapping.
- [ ] Remove duplicate browse-path and event-field helper logic.
- [ ] Preserve every public NodeId, BrowseName, Method signature and event type from `v0.3.4`.

### Tests and acceptance

- [ ] Existing config, security, OTA, history and AMB verification scripts pass unchanged.
- [ ] Exported NodeSet/public-node inventory matches the `v0.3.4` baseline except for explicitly documented corrections.
- [ ] Binary-size change caused only by refactoring stays within an agreed small tolerance.
- [ ] Each model module can be disabled at build time only through explicit feature options; no implicit source-order dependencies.

### Result after release

The OPC UA layer is maintainable and feature-isolated, enabling later generic channel work without editing a single giant translation unit.

### Out of scope

New NodeIds, dynamic runtime reconfiguration and new companion specifications.

---

## `v0.4.1` — Channel Runtime Hardening

### Goal

Complete the generic data plane that already exists in scaffold form.

### Required implementation

- [ ] Replace the separate `CONFIG_SERVICE_MAX_CHANNELS` and `CHANNEL_MANAGER_MAX_CHANNELS` values with one compile-time platform limit; default reference limit: `16`.
- [ ] Define ownership for channel IDs, names, units and descriptions. Runtime state must not depend on borrowed pointers whose lifetime is ambiguous.
- [ ] Replace raw mutable channel-pointer access with explicit snapshot, update and iteration APIs.
- [ ] Make registry operations thread-safe without holding global critical sections during long operations.
- [ ] Support every advertised value type, or reject unsupported types during configuration validation. Silent fallback to `float` is forbidden.
- [ ] Introduce a 64-bit source timestamp representation with documented epoch and resolution.
- [ ] Expand the driver lifecycle to include capabilities and, where relevant, `start`, `stop`, `read`, `write`, `configure` and `self_test` operations.
- [ ] Move ADC/GPIO hardware implementation out of `services/driver_catalog.c` into `drivers/`.
- [ ] Add resource-claim validation for ADC channels, GPIO pins and shared peripherals.
- [ ] Remove the legacy “primary channel” concept and temperature-only snapshot from active runtime paths.
- [ ] Keep a compatibility adapter only where required by public `v0.3` behavior; mark it deprecated.

### Tests and acceptance

- [ ] Run mixed configurations with 1, 4 and 16 channels.
- [ ] Include analog, boolean, signed/unsigned integer and writable loopback test channels.
- [ ] Concurrent sampling and OPC UA reads produce coherent snapshots.
- [ ] Invalid driver/type/source combinations are rejected before activation.
- [ ] Adding a mock driver requires no change to the OPC UA model.

### Result after release

The firmware has a type-safe, resource-bounded, multi-channel runtime independent of temperature and OPC UA.

---

## `v0.4.2` — Generic OPC UA Channel Information Model

### Goal

Build the public channel address space from the channel registry instead of special-casing signals.

### Required implementation

- [ ] Create one OPC UA channel object for every configured runtime channel at server startup.
- [ ] Use deterministic NodeIds derived from validated channel IDs.
- [ ] Expose typed `Value` with correct `StatusCode` and source timestamp.
- [ ] Expose channel metadata: description, engineering unit, range, kind, driver/source identifier, sample period, publish period, historizing state and writable capability.
- [ ] Use standard data types such as `EUInformation` and `Range` where appropriate rather than vendor strings.
- [ ] Route writes only through writable channel/driver APIs and role checks.
- [ ] Generate channel nodes with bounded temporary allocation and deterministic rollback after a partial build failure.
- [ ] Keep the legacy temperature NodeId as a compatibility alias or reference for the deprecation period; do not maintain a second independent value.
- [ ] Add a channel-model verification tool that discovers channels rather than assuming `Temperature1`.

### Tests and acceptance

- [ ] UaExpert and automated clients correctly browse and read mixed channel types.
- [ ] Source timestamp and quality propagate without being regenerated by the model layer.
- [ ] Writable and read-only channels enforce access correctly.
- [ ] NodeIds remain stable across reboot with the same configuration.
- [ ] No channel-specific creation function is added to model core.

### Result after release

The OPC UA address space becomes a generic reflection of the configured data plane.

---

## `v0.4.3` — Multi-Channel History and Alarm Engine

### Goal

Remove the remaining temperature/singleton assumptions from history and alarms.

### Required implementation

- [ ] Replace the one global `float` ring with typed, owned records and per-channel retention policy.
- [ ] Store bounded per-channel history in a PSRAM ring with a bounded, non-transactional Flash-backed mirror for persistence across reboot/power loss. Single supported profile (`device_profile_basic`); no compile-time RAM-only or Flash-optional backend selection in this release.
- [ ] Do not scan the complete history ring while interrupts/scheduling are globally locked.
- [ ] Generalize OPC UA `HistoryRead` to every historized channel.
- [ ] Implement bounded continuation points and maximum return counts.
- [ ] Replace singleton `alarm_service_status_t` with independent per-channel alarm instances.
- [ ] Support, at minimum:
  - numeric high and high-high alarms;
  - configurable hysteresis;
  - debounce/delay;
  - invalid/stale-quality state;
  - discrete state alarm for boolean channels.
- [ ] Create stable condition instances per alarm-capable channel.
- [ ] Preserve acknowledgement state independently per condition.
- [ ] Record alarm transitions through the common event/journal abstraction.

### Tests and acceptance

- [ ] Simultaneous alarms on multiple channels remain independent.
- [ ] History supports mixed value types and concurrent readers.
- [ ] Continuation-point exhaustion returns a bounded OPC UA error without heap runaway.
- [ ] Sampling jitter remains within the benchmark budget during HistoryRead and event bursts.

### Result after release

History and alarms become reusable channel services rather than temperature features.

---

## `v0.5.0` — Transactional Runtime Configuration

### Goal

Make one firmware image safely reconfigurable for multiple device profiles.

### Required implementation

- [ ] Add an explicit configuration schema version and migration functions.
- [ ] Preserve staged/running separation and add a last-known-good configuration.
- [ ] Validate cross-channel hardware resource conflicts before commit.
- [ ] Apply configuration as a transaction:
  - validate;
  - persist staged copy;
  - stop affected drivers;
  - activate a new registry/model generation;
  - verify activation;
  - commit or rollback.
- [ ] Use a controlled OPC UA server restart if safe dynamic node deletion/recreation cannot be proven. Do not implement fragile live mutation merely to avoid a short reconnect.
- [ ] Recover correctly if power is lost during stage, persist, activation or commit.
- [ ] Version configuration import/export and reject incompatible schemas with diagnostics.
- [ ] Expose firmware/configuration compatibility data through the management model: firmware profile, hardware revision, configuration-schema version, `SupportedDrivers`, per-driver capabilities and available hardware resources.
- [ ] Reject configurations that reference unavailable drivers before staged configuration can become running; preserve the previous known-good configuration and report every missing `driver_id`.
- [ ] Add profile presets: sensor node, remote I/O and development/benchmark profile. These runtime configuration presets are distinct from compile-time firmware driver packs.

### Tests and acceptance

- [ ] Apply valid channel additions, removals and modifications.
- [ ] Roll back after driver initialization failure and namespace build failure.
- [ ] Reboot after every transaction phase and recover either old or new complete configuration, never a mixture.
- [ ] Existing clients receive a documented disconnect/reconnect behavior during model restart.

### Result after release

A single binary can safely become different device profiles through a versioned, recoverable configuration transaction.

---

## `v0.5.1` — Driver SDK and Reference Drivers

### Goal

Make third-party sensor/actuator integration a documented extension point.

### Required implementation

- [ ] Freeze the pre-1.0 driver API candidate.
- [ ] Keep all native drivers statically linked into firmware; configuration and host tools may instantiate them only by stable `driver_id`.
- [ ] Add one compile-time enable/disable option per driver or driver family, and generate the runtime driver catalog only from enabled targets.
- [ ] Ensure disabled drivers and their private dependencies are absent from the final ELF/UF2 rather than merely hidden from the catalog.
- [ ] Define firmware driver-pack manifests for `base`, `environmental`, `motion`, `industrial` and `development`, including profile ID, included driver IDs, hardware compatibility and resource budget.
- [ ] Expose `SupportedDrivers` and `DriverCapabilities` through a bounded OPC UA capability model usable by the configurator before configuration upload.
- [ ] Define a versioned host-side sensor/module descriptor format that maps catalog entries to required `driver_id`, connection constraints, default channel templates, units and wiring information. This descriptor data remains host-side and is not executable firmware code.
- [ ] Add driver capability descriptors: readable, writable, polling, interrupt-driven, self-test, calibration and required hardware resources.
- [ ] Provide reference drivers:
  - internal temperature;
  - ADC voltage;
  - GPIO input;
  - GPIO output/writable channel;
  - synthetic benchmark source;
  - loopback/mock source for host tests.
- [ ] Add driver error classification and quality mapping.
- [ ] Add retry/backoff and stale-data policy without blocking unrelated channels.
- [ ] Document how to add a driver, reserve resources, define configuration and expose channels.
- [ ] Provide a minimal standalone example driver outside model code.

### Tests and acceptance

- [ ] A new mock driver can be added without editing OPC UA modules or core channel manager logic.
- [ ] Build two firmware profiles and verify that a disabled driver's code/symbols are absent from the smaller ELF and that the reported `SupportedDrivers` set exactly matches the linked catalog.
- [ ] Verify that compiled but inactive drivers allocate no per-instance runtime context, task, channel buffer or permanent SRAM/PSRAM block.
- [ ] A configuration referencing a driver absent from the installed profile is rejected without altering the running configuration.
- [ ] A failing/slow driver does not stop other channels.
- [ ] GPIO write authorization and readback are verified.
- [ ] Driver self-test results contribute to Device Health.

### Result after release

The repository becomes a usable embedded SDK rather than a fixed firmware application.

---

## `v0.5.2` — Resource Instrumentation and Benchmark Suite

### Goal

Create the quantitative evidence required for engineering decisions, diploma work and publication.

### Required implementation

- [ ] Report static Flash and SRAM sections from ELF/map files.
- [ ] Report current/minimum FreeRTOS heap, largest free block and allocation failures.
- [ ] Report PSRAM total/current/peak use, largest block and failed allocations.
- [ ] Report per-task stack high-water marks and runtime statistics.
- [ ] Report OPC UA sessions, secure channels, subscriptions, monitored items, publish requests, rejected requests and event queue utilization.
- [ ] Measure startup time, secure-session establishment time, read/write/method latency, HistoryRead latency, event-delivery latency and sampling jitter.
- [ ] Define reproducible benchmark profiles:
  - 1, 4, 8 and 16 channels;
  - REDUCED and FULL Namespace 0 where supported;
  - security off only in test profile versus Basic256Sha256 SignAndEncrypt;
  - 1, 2 and 4 clients;
  - 1 Hz and 10 Hz sampling/subscription load;
  - events and history enabled/disabled.
- [ ] Store machine-readable benchmark JSON plus human-readable summary in CI artifacts.
- [ ] Add regression thresholds for binary size, static SRAM and selected latency/heap metrics.

### Tests and acceptance

- [ ] A baseline report for `v0.5.2` is committed or attached to the release.
- [ ] Benchmark firmware uses synthetic sources so results are reproducible.
- [ ] Measurements distinguish internal SRAM, PSRAM and external Flash.
- [ ] Resource exhaustion produces bounded failure rather than reset or corruption.

### Result after release

The project has defensible scalability and resource-consumption data instead of qualitative claims.

---

## `v0.6.0` — Signed OTA and Recoverable Boot

### Goal

Upgrade the existing CRC-checked OTA prototype to a production-oriented update chain.

### Required implementation

- [ ] Add a signed firmware manifest containing hardware compatibility, firmware-profile ID, included driver-pack/version information, image version, size, hash and signature.
- [ ] Use SHA-256 plus ECDSA P-256 verification through the existing mbedTLS stack unless a documented evaluation selects another supported algorithm.
- [ ] Verify the manifest and image before marking an update bootable.
- [ ] Add anti-rollback policy with an explicit development override.
- [ ] Replace immediate startup acknowledgement with a health-confirmation window.
- [ ] Confirm a new image only after scheduler, configuration, storage, network and OPC UA initialization remain healthy for the configured period.
- [ ] Preserve or provide a bootable fallback image so failure can actually roll back, not merely mark the copied image failed.
- [ ] Make bootloader state transitions power-loss safe.
- [ ] Expose update progress, verification result, boot attempt and rollback reason through OPC UA diagnostics/events.

### Tests and acceptance

- [ ] Reject modified image, modified manifest, wrong hardware target and older forbidden version.
- [ ] Interrupt power during every erase/copy/state-write phase.
- [ ] Force a new image to fail health confirmation and verify rollback.
- [ ] Confirm the previous known-good firmware remains bootable.

### Result after release

Remote update becomes cryptographically authenticated and recoverable from interrupted or unhealthy deployments.

---

## `v0.6.1` — Security and Provisioning Hardening

### Goal

Turn existing security features into a documented device security baseline.

### Required implementation

- [ ] Define a node/method permission matrix for viewer, operator, service and admin roles.
- [ ] Add automated authorization tests for every writable node and method.
- [ ] Disable unsecured endpoints outside an explicitly entered commissioning/development mode.
- [ ] Add commissioning timeout and, where hardware permits, physical-presence or locally provisioned authorization.
- [ ] Make identity and trust-list replacement atomic with rollback to the previous valid set.
- [ ] Add CSR/certificate-renewal workflow or document the exact external provisioning flow.
- [ ] Separate secrets/private keys from normal configuration export.
- [ ] Add signed configuration packages for remote import.
- [ ] Persist security audit records in a bounded journal.
- [ ] Document lockout, password storage, certificate-expiry and recovery policy.
- [ ] Produce a project threat model and security assumptions.

### Tests and acceptance

- [ ] Unauthorized users cannot invoke configuration, security, maintenance or OTA mutations.
- [ ] Interrupted trust/identity rotation leaves one valid identity and trust set.
- [ ] Commissioning mode cannot remain enabled indefinitely by default.
- [ ] Config export contains no private key, password material or authentication salts.

### Result after release

The device has a consistent access-control, provisioning and audit model rather than isolated security functions.

---

## `v0.6.2` — Reliability, Supervision and Fault Injection

### Goal

Make failure detection depend on subsystem health rather than a blindly kicked watchdog.

### Required implementation

- [ ] Add heartbeat/deadline supervision for network, acquisition, time, alarm/history, storage and OPC UA tasks.
- [ ] Kick the hardware watchdog only when required subsystems meet health policy.
- [ ] Record reset reason, failing subsystem, last fault and boot counter persistently.
- [ ] Add controlled degradation when PSRAM, history storage, MQTT or time synchronization is unavailable.
- [ ] Add recovery policies for Ethernet link loss, DHCP loss, broker disconnect and OPC UA client abuse.
- [ ] Add fault injection hooks for allocation failure, Flash error, corrupted config, driver timeout and network flap.
- [ ] Add brownout/power-loss campaigns for configuration, maintenance journal and OTA.

### Tests and acceptance

- [ ] A deadlocked supervised task causes watchdog recovery.
- [ ] A failed optional subsystem does not unnecessarily reboot the device.
- [ ] 72-hour and 7-day soak profiles complete without memory growth or unhandled error.
- [ ] Repeated secure handshakes and malformed requests remain within resource limits.

### Result after release

The device detects, records and recovers from realistic embedded and network faults predictably.

---

## `v0.7.0` — Modbus RTU Channel Adapter

### Goal

Support retrofit acquisition from existing serial industrial equipment.

### Required implementation

- [ ] Add a Modbus RTU master under the driver/field-protocol layer.
- [ ] Configure UART, baud, parity, stop bits, slave ID, function code, address, width, data type, byte/word order, scale and polling period.
- [ ] Map each configured Modbus value to a normal telemetry channel.
- [ ] Add grouped reads and a bounded polling scheduler.
- [ ] Isolate slow/offline slaves from unrelated channels.
- [ ] Propagate timeout, exception and stale-data state into channel quality and diagnostics.
- [ ] Add per-slave counters and Device Health contribution.

### Tests and acceptance

- [ ] Test normal response, timeout, CRC error, exception response and reconnect.
- [ ] Verify signed/unsigned 16/32-bit and float mappings with byte/word order variants.
- [ ] Existing OPC UA channel, history and alarm code requires no Modbus-specific branch.

### Result after release

The platform becomes an OPC UA retrofit node for Modbus RTU devices.

---

## `v0.7.1` — Modbus TCP Channel Adapter

### Goal

Reuse the Modbus mapping model for Ethernet field devices.

### Required implementation

- [ ] Add Modbus TCP client connections with bounded per-device state.
- [ ] Reuse the RTU register/data-type mapping schema where protocol-independent.
- [ ] Add connection timeout, retry/backoff, transaction-ID handling and stale-quality policy.
- [ ] Prevent Modbus TCP polling from starving OPC UA/lwIP traffic.
- [ ] Add per-device diagnostics and health contribution.

### Tests and acceptance

- [ ] Multiple slaves with one offline device do not block healthy devices.
- [ ] Link flap and server restart recover without reboot.
- [ ] Polling load is included in benchmark profiles.

### Result after release

The same firmware supports serial and Ethernet Modbus equipment through the common channel model.

---

## `v0.7.2` — Computed Channels and Edge Functions

### Goal

Add deterministic local derived values without embedding an unrestricted scripting engine.

### Required implementation

- [ ] Add computed-channel drivers with a bounded dependency graph.
- [ ] Support a fixed, documented set of operations such as linear transform, sum, difference, ratio, min/max, moving average, RMS, delta and rate-of-change.
- [ ] Detect dependency cycles during configuration validation.
- [ ] Propagate worst-input quality and a deterministic output timestamp policy.
- [ ] Bound window storage and computation time.
- [ ] Allow computed values to use normal history/alarm/OPC UA paths.

### Tests and acceptance

- [ ] Cycle and missing-dependency configurations are rejected.
- [ ] RMS/moving-window memory is bounded and reported.
- [ ] A bad/stale source propagates expected output quality.

### Result after release

The node can perform useful edge preprocessing while remaining deterministic and auditable.

---

## `v0.8.0` — OPC UA Interoperability Profile

### Goal

Define and verify the exact OPC UA feature profile the device claims to support.

### Required implementation

- [ ] Publish supported Services, security policies, user-token policies, subscriptions, Events/A&C, HistoryRead, PubSub and companion-model subsets.
- [ ] Publish namespace URIs, stable NodeIds, model version and generated NodeSet/public-node documentation.
- [ ] Build an interoperability matrix for UaExpert, open62541 client, Prosys Browser and at least one SCADA platform.
- [ ] Add repeatable tests for browse/read/write/call/subscription/event/history/security reconnect behavior.
- [ ] Prepare a CTT-oriented profile and run the feasible subset; document unsupported tests rather than masking them.
- [ ] Verify DI/AMB nodes against the imported NodeSet versions.

### Tests and acceptance

- [ ] No undocumented OPC UA feature claim remains.
- [ ] All supported-client matrix entries have a reproducible test procedure.
- [ ] Public NodeId inventory is frozen for the `0.8` line.

### Result after release

The project can state what is interoperable and standards-aligned with evidence rather than screenshots alone.

---

## `v0.8.1` — Historical Event Access

### Goal

Expose the existing maintenance and alarm journals through standard OPC UA Part 11 event history.

### Required implementation

- [ ] Implement `HistoryRead` with `ReadEventDetails` for maintenance and alarm events.
- [ ] Advertise accurate HistoryServerCapabilities.
- [ ] Support a bounded, documented subset of EventFilter select/where clauses.
- [ ] Implement continuation points with strict per-session and global limits.
- [ ] Translate storage records to OPC UA events without coupling the Flash schema to OPC UA field layout.
- [ ] Preserve live-event and historical-event field semantics.

### Tests and acceptance

- [ ] UaExpert/compatible client reads maintenance and alarm history.
- [ ] Unsupported filters return a correct status instead of partial silent behavior.
- [ ] Concurrent live subscriptions and history reads remain within benchmark limits.
- [ ] Reboot does not change already stored event identity/order.

### Result after release

CMMS and SCADA clients can retrieve maintenance/alarm history through a standard interface.

---

## `v0.8.2` — OPC UA PubSub over MQTT Productionization

### Goal

Stabilize the MQTT PubSub feature that already exists instead of introducing another MQTT abstraction.

### Required implementation

- [ ] Define DataSet metadata, field ordering and configuration version behavior.
- [ ] Add broker TLS/authentication configuration.
- [ ] Add bounded reconnect/backoff and offline queue policy.
- [ ] Expose broker state, last publish result, dropped messages and queue utilization.
- [ ] Publish channel quality and timestamps consistently with client/server values.
- [ ] Test interoperability with at least two MQTT brokers and an OPC UA PubSub subscriber implementation.
- [ ] Prevent broker failure from affecting OPC UA client/server availability.

### Tests and acceptance

- [ ] Broker disconnect/restart recovers without reboot.
- [ ] Queue overflow is observable and bounded.
- [ ] PubSub configuration changes are versioned and transactional.
- [ ] MQTT load is included in resource benchmarks.

### Result after release

Existing OPC UA PubSub/MQTT support becomes a reliable optional northbound transport.

---

## `v0.9.0` — API, Schema and NodeId Freeze

### Goal

Freeze the contracts that will be guaranteed by `v1.0.0`.

### Required implementation

- [ ] Freeze public driver API, channel API and service error model.
- [ ] Freeze configuration schema version `1` and migration policy.
- [ ] Freeze vendor namespace URI and public NodeIds.
- [ ] Document compatibility and deprecation policy.
- [ ] Remove or formally retain deprecated temperature-only compatibility nodes/APIs.
- [ ] Complete driver SDK, configuration, security, OTA, maintenance, Modbus and benchmark documentation.
- [ ] Add examples for custom driver, remote I/O, Modbus gateway, computed channel and secure provisioning.
- [ ] Rename product-facing documentation consistently to an RP2/RP2350 industrial OPC UA device platform.

### Tests and acceptance

- [ ] A clean-room developer can build firmware and add a mock/custom driver using documentation only.
- [ ] Configuration generated by `v0.9.0` round-trips without loss.
- [ ] Public API/NodeId inventory is machine-checked in CI.

### Result after release

The platform contract is frozen; later release candidates focus on defects and optimization only.

---

## `v0.9.1` — Performance and Long-Run Release Candidate

### Goal

Optimize the frozen platform and publish its real operating envelope.

### Required implementation

- [ ] Remove avoidable allocations and copies from hot paths.
- [ ] Define PSRAM placement policy for NodeSet/model objects, history and temporary buffers.
- [ ] Reduce long critical sections and lock contention.
- [ ] Tune lwIP/WIZnet buffers, open62541 limits and task priorities using measured data.
- [ ] Run complete benchmark profiles and compare with `v0.5.2`.
- [ ] Run 7-day soak with security, subscriptions, history, alarms and selected field protocol enabled.
- [ ] Publish tested maximums and recommended operating limits.

### Tests and acceptance

- [ ] No unexplained memory growth.
- [ ] No stack watermark below the documented safety margin.
- [ ] All benchmark regressions are either fixed or explicitly justified.
- [ ] Maximum supported channel/client/subscription limits are enforced, not merely documented.

### Result after release

The project has a measured, stable performance envelope suitable for deployment guidance and scientific comparison.

---

## `v0.9.2` — Production Release Candidate

### Goal

Complete manufacturing, provisioning and release reproducibility.

### Required implementation

- [ ] Define unique serial, MAC, product-instance URI, asset ID and certificate provisioning flow.
- [ ] Add factory-test profile and test-jig protocol/results format.
- [ ] Produce reproducible signed release artifacts: bootloader, application, factory image, manifest, default config, NodeSet/public-node inventory, SBOM and size/benchmark reports.
- [ ] Add release automation that verifies tag, runtime version and artifact manifest consistency.
- [ ] Document flash layout, recovery procedure, factory reset and decommissioning.
- [ ] Audit third-party licenses and pinned dependency revisions.
- [ ] Close all known data-corruption, authentication-bypass, unsafe-OTA and unbounded-allocation defects.

### Tests and acceptance

- [ ] A blank reference board can be provisioned, factory-tested, updated and recovered using documented tooling.
- [ ] Release artifacts can be rebuilt reproducibly from the tag.
- [ ] A complete upgrade test from the last supported pre-1.0 version passes.

### Result after release

The system is feature-complete and operationally ready for the `v1.0.0` declaration.

---

## `v1.0.0` — Stable RP2350 Industrial OPC UA Device Platform

### Goal

Declare the first stable platform contract. No major feature is introduced in this release.

### Release requirements

- [ ] All `v0.9.2` acceptance criteria remain green.
- [ ] Public driver/channel APIs, configuration schema and NodeIds are stable.
- [ ] Security, OTA, maintenance, history, alarms, Modbus and PubSub limits are documented and enforced.
- [ ] Interoperability matrix and benchmark report are published.
- [ ] Upgrade, rollback, factory reset and recovery procedures are validated.
- [ ] No critical or high-severity unresolved defect remains.

### Final product state

`v1.0.0` is a configurable RP2350 IIoT node that can acquire local and Modbus data, expose a deterministic OPC UA model, maintain DI/AMB asset and maintenance semantics, buffer bounded history/events, enforce role-based security, update safely and provide measured resource/performance limits.

## 7. Explicitly deferred beyond `v1.0.0`

Codex must not implement the following before a roadmap revision assigns an exact version:

- full GDS/push-management integration;
- OPC UA PubSub UDP/TSN;
- arbitrary user scripting;
- REST/HTTP management API;
- cloud/fleet-management backend;
- full database-class historian;
- additional companion specifications unrelated to a demonstrated device use case;
- dynamic native/bytecode driver loading or executable driver upload from the configurator;
- high-availability clustering.

## 8. Immediate next action

The next implementation target is
**`v0.4.3 — Multi-Channel History and Alarm Engine`**.

Before coding, Codex must:

1. use tag `v0.4.2` as the immutable implementation and compatibility baseline;
2. follow the file-level execution plan in `docs/releases/v0.4.3.md`;
3. preserve unrelated v0.4.2 public contracts and configuration fields;
4. freeze the per-channel history record, Flash layout, continuation-point and
   alarm-condition contracts before changing implementation code;
5. verify mixed-type history, bounded continuation points, independent alarms,
   persistence and the affected public OPC UA contract;
6. only then start implementation limited to `v0.4.3`.
