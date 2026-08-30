# Changelog

This file tracks the firmware release line through `v0.5.0`. Historical tag
SHAs below are resolved from peeled Git tags, so annotated tags are reported by
their target commit SHA rather than the tag-object SHA.

## v0.5.0

- Release metadata: synchronizes the repository current-release instructions
  and the authoritative firmware/runtime/OTA version source to `0.5.0` before
  hardware verification. The version-integrity gate now checks the optional
  OTA payload manifest only when the current CMake configuration enables
  `OTA_BUILD_BOOTLOADER`; this prevents a stale artifact from a previous build
  mode causing a false failure, while still requiring the manifest when enabled.
  OTA staging now independently rejects a local build manifest unless the
  sibling CMake cache enables bootloader artifacts and its version, board,
  platform and Flash origins match the current generated layout.
- Configuration transaction pipeline: restructures `config_service_apply()`/
  `config_service_replace_and_save()` into a real validate -> persist staged
  -> stop -> activate -> verify (driver init plus at least one valid
  per-channel read, each bounded by an explicit timeout) -> commit/rollback
  transaction, fixing the previous activate-before-persist ordering. Adds an
  explicit `SemaphoreHandle_t` mutex around driver activation and ensures a
  transaction that activates the first channel on a channel-less device
  correctly creates/hands off to `SensorTask` (previously boot-only-gated on
  `channel_manager_count() > 0`; `SensorTask` is now always created at boot).
- Flash backend: replaces the two-slot generation config store with a
  staged/running/last-known-good transaction backend
  (`drivers/config_transaction_backend.c`) inside the existing 128 KiB
  configuration budget — no new Flash region, no OTA-bank shrink. Follows
  the maintenance journal's commit-page/CRC physical-read-back
  classification pattern, one physical copy per role. See
  `docs/flash-storage.md`.
- Schema versioning: gives `schema_version` real discriminating meaning with
  a migration-function table shared by the Flash-binary decode path, JSON
  export, and JSON import (previously a single hardcoded literal duplicated
  across three call sites); rejects an incompatible/future schema version
  with a diagnostic.
- Validation: splits the missing-driver condition out of a combined
  generic-error check into its own dedicated result code and collects every
  missing `driver_id` in a rejected candidate configuration, not just the
  first, via an additive (non-breaking) API extension.
- OPC UA server restart: consumes the previously-computed
  `model_config_requires_server_reboot()` diagnostic to trigger an actual
  controlled full server restart, synchronously from within the same
  transaction commit step, reusing the exact cold-boot address-space
  construction codepath. A transaction that does not require a restart
  leaves existing sessions/subscriptions untouched.
- Presets and compatibility: adds three runtime configuration presets
  (`sensor_node`, `remote_io` and `dev_benchmark`) applied through the normal transaction pipeline via new
  `Device.Maintenance.ListConfigPresets`/`ApplyConfigPreset` methods, and
  exposes firmware/configuration compatibility data (firmware profile,
  hardware revision, configuration schema version, supported drivers and
  their capabilities/resources) through a new `Device.Compatibility`
  subtree.
- **In addition to `ROADMAP.md`'s literal v0.5.0 scope**: per-channel
  history/alarm state cleanup on channel delete/modify. Deleting a channel,
  or modifying it with a different driver/value type (same `channel_id`),
  now zeroes that channel's Flash-backed history ring and alarm condition
  instance/acknowledgement state, treated uniformly as delete+recreate. This
  requirement was not derived from `ROADMAP.md` text; it was identified
  during v0.5.0 planning because a reused `channel_id` could otherwise
  surface a stranger's stale history/alarm state. It is a conscious,
  documented tradeoff, not an implicit byproduct: discarding alarm
  acknowledgement history on every qualifying reconfiguration has no
  retention/audit-trail guarantee, which industrial alarm-management
  practice (e.g. ISA-18.2) often expects independent of point
  reconfiguration; this project does not currently claim that compliance
  bar. See `docs/releases/v0.5.0.md` scope decision 7 and
  `docs/channel_model.md`.
- Fix found during implementation: `alarm_service`'s condition-slot
  allocator (`g_condition_count`) never reclaimed a freed slot, so a naive
  per-channel clear would have permanently shrunk available alarm capacity
  by one per delete/modify cycle on the same channel, eventually exhausting
  `ALARM_SERVICE_MAX_CONDITIONS` (16) even though live channel occupancy
  never exceeds `PLATFORM_CHANNEL_LIMIT`. `alarm_service_clear_channel()`
  fixes the allocator itself (reclaims the first freed slot below the
  cursor) rather than adding a setter on top of the bug.
- Accepted residual risk: no physical hardware power-cut test was performed
  against the configuration transaction pipeline — this implementation
  environment had no access to the reference RP2350 board or any other
  hardware. Coverage is three layers of deterministic, automatable host-test
  fault injection (Flash-backend-level, `config_service.c`-level, and an
  end-to-end real-chain-plus-simulated-reboot suite), which found no
  correctness bug in the frozen commit-ordering guarantee or the
  recovery-state-to-boot-decision mapping. The specific gap host simulation
  cannot catch: real Flash write-alignment/timing failure modes that only
  manifest against physical QSPI hardware. See
  `docs/known_limits.md#v050-accepted-residual-risks`.
- Found while documenting this release (not fixed in this release — flagged
  for follow-up): a device that has run v0.4.0-v0.4.3 firmware does not have
  its configuration migrated in place when it upgrades to v0.5.0 firmware.
  The old two-slot format's header magic and the new transaction backend's
  header/commit-page magic and offsets are unrelated, so the new backend
  does not recognize old-format bytes as a valid record, and the existing
  legacy-importer fallback targets an even older, pre-config-store format,
  not the v0.4.x two-slot layout. The device boots on compiled defaults
  after such an upgrade instead of preserving its prior configuration. See
  `docs/known_limits.md#v050-accepted-residual-risks`.
- Verification: 35/35 deterministic host-test suites
  (`static/tools/run_host_tests.py`) and first-party cppcheck
  (`static/tools/run_first_party_cppcheck.py`) both pass on the integrated
  v0.5.0 tree.
- Release plan: `docs/releases/v0.5.0.md`.

## v0.4.3

- History: replaces the single global `float` ring with a per-channel,
  typed PSRAM ring plus a bounded, non-transactional Flash-backed mirror
  that survives reboot/power loss. `HistoryRead` is generalized to every
  historizing channel with bounded continuation points (16, 30s expiry)
  and a 128-sample page cap; chunked scans never hold one lock across a
  full multi-channel pass, and a channel identity hash was widened to
  32-bit before the on-flash record format froze.
- Alarms: replaces the singleton alarm status with one independent
  condition instance per alarm-eligible channel, supporting
  high/high-high, low/low-low, hysteresis, debounce, discrete boolean
  state, and a distinct invalid/stale-quality fault state.
  Acknowledgement state is independent per condition; transitions remain
  live-event only (no Flash journal — that is v0.8.1 scope).
- Fixes found during hardware verification: a within-call generation race
  in chunked `HistoryRead`, and two pre-existing gaps unrelated to this
  release's own scope — standard `Acknowledge`/`ConditionRefresh` were
  completely uncallable (missing `HasComponent` references in the
  compiled namespace-zero nodeset), and the task stack high-watermark
  diagnostic nodes always returned `BadNotFound`.
- Verification: `static/tools/verify_v043.py`, a real asyncua
  protocol-level verifier run against the reference RP2350 board — 67/67
  checks covering mixed-type HistoryRead, pagination, continuation-point
  exhaustion/release/expiry/foreign-session/generation invalidation, two
  simultaneous independent conditions, and standard Acknowledge/
  ConditionRefresh. See `docs/releases/v0.4.3-verification-report.md`.
- Release plan: `docs/releases/v0.4.3.md`.

## v0.4.2

- OPC UA model: reflects every startup registry channel through one generic,
  typed builder with deterministic channel-ID-derived NodeIds, coherent quality
  and source timestamps, standard `EUInformation`/`Range` metadata, and bounded
  rollback after partial construction failure.
- Runtime ownership: exposes copied driver/source identity and routes authorized
  writes through a generation-safe channel-manager transaction rather than a
  driver call from the OPC UA adapter.
- Compatibility: retains every v0.4.1 channel subtree, including the deprecated
  `Device.Channels.Temperature1` view of the same registry-owned sample.
  Multi-channel history and alarms remain deferred to v0.4.3.
- Verification: adds model discovery, all built-in channel types, deterministic
  NodeId, metadata, write authorization, rollback, legacy-equivalence, and
  bounded lwIP listener-capacity recovery gates.
- Release plan: `docs/releases/v0.4.2.md`.

## v0.4.1

Initial release commit before the review-hardening follow-up:
`669ab94c349777d0b635d66e5377fcb64fd4fa92`.

- Runtime: one 1-16 compile-time platform limit, manager-owned metadata and
  slots, coherent snapshot/iteration APIs, generation-checked driver
  operations, and a Unix-microsecond source timestamp contract.
- Drivers: explicit capabilities and lifecycle results, native ADC/GPIO
  hardware ownership under `drivers/`, pre-activation resource claims,
  reverse-order activation rollback, and build-time driver exclusion.
- Compatibility: deterministic migration of the four-channel v0.4.0 persisted
  payload, a stateless deprecated temperature projection, and an empty public
  OPC UA contract delta.
- Verification: mixed 1/4/16-channel, capacity, type/driver, timestamp,
  concurrency, lifecycle, rollback, migration and public-contract gates.
- Hardening follow-up: configuration apply now commits only after synchronous
  runtime activation, non-float historizing is rejected, history scans use a
  task mutex, HistoryRead scratch lives in bounded PSRAM, and driver handover
  quiesces the old runtime before starting the candidate.
- Release plan: `docs/releases/v0.4.1.md`.

## v0.4.0

- Scope: decomposed the OPC UA adapter into focused, independently compiled
  modules backed by one bounded internal model context.
- Compatibility: preserves the v0.3.4 NodeIds, BrowseNames, methods, events,
  access behavior, configuration schema, and service contracts.
- Verification: the source-derived public contract is pinned to v0.3.4 and the
  module source manifest is checked for untracked public UA registrations.
- Peeled tag commit: `8a454945aaccc3d381a2e889e90b53569415b705`.

## v0.3.4

- Peeled tag commit: `3ff4689e403fc806122cbafb509cc14cc4487bfb`.
- Scope: authoritative `0.3.4` version source, blocking CI gates, first-party
  static analysis, release artifact verification, and updated project
  positioning/documentation.

## v0.3.3

- Peeled tag commit: `19bf3d24e80e217f960b251654c323eacfcf1c0f`
- Summary:
  - Added the persisted maintenance journal and recovery path.
  - Added journal/condition verification tooling and focused Flash checks.
  - Established the `v0.3.3` implementation baseline used by `v0.3.4`.

## v0.3.2

- Peeled tag commit: `f9336396f1102002eeb28bc7241c05de268d72aa`
- Summary:
  - Introduced the service-owned AMB maintenance workflow.
  - Added maintenance methods, state transitions, and deterministic status
    handling.
  - Unified firmware/config/OTA version reporting around the `0.3.2` CMake
    source.

## v0.3.1

- Peeled tag commit: `a91bf946f2b605a1aa9bef8e591bfa95e608803f`
- Summary:
  - Added DI Device Health semantics and AMB asset modeling.
  - Added maintenance state machine properties and companion-model support.
  - Added structural verification tooling for the AMB model.

## v0.3

- Peeled tag commit: `aa31cfee7b7093acd44c7c45048c87df2a8c3c4d`
- Summary:
  - Added persistent configuration, OTA staging/layout, trust and identity
    lifecycle services, and RP2350 + PSRAM support.
  - Added FULL Namespace 0 execution from PSRAM and reproducible submodule
    patching.
  - Added preliminary CI build, size reporting, static analysis, and Renode
    experimentation.

## v0.2

- Peeled tag commit: `3ec41223f1c32c82a746a61647952a1eacd936cf`
- Summary:
  - Added channel scaffolding, authentication/roles, OPC UA Methods, and custom
    Events/A&C plumbing.
  - Added role-aware sessions and OPC UA PubSub over MQTT.
  - Hardened embedded memory and concurrency behavior.

## v0.1

- Peeled tag commit: `f15ba645d99f6441e09953920b09d10fe6131c86`
- Summary:
  - Established the layered firmware structure around drivers, services,
    `opcua_model`, and the basic profile app.
  - Added RP2/WIZnet networking, temperature acquisition, diagnostics,
    watchdog, time/network services, and writable OPC UA nodes.
  - Added local history and temperature alarms for the initial server baseline.
