# Changelog

This file tracks the firmware release line through `v0.5.1`, plus the
in-progress `v0.5.2` line. Historical tag SHAs below are resolved from peeled
Git tags, so annotated tags are reported by their target commit SHA rather than
the tag-object SHA.

## v0.5.2 (unreleased)

- Development preparation: selects v0.5.2 as the first unreleased roadmap
  version, updates contributor instructions and adds the scoped
  `docs/releases/v0.5.2.md` implementation plan.
- Sets the authoritative firmware/runtime/OTA/config-export version source
  to `0.5.2` and begins the bounded resource-instrumentation vertical: static
  ELF/map reporting, coherent Heap/PSRAM/task snapshots, and service/admin-only
  OPC UA sessions, secure-channel, subscription, monitored-item, Publish,
  event-queue, ServiceFault rejection and event-drop diagnostics. The
  reference-board baseline records passing secure 1-channel and clean
  16-channel, four-client, 10 Hz `test-none` corners; the maximum profile
  completed all 1,200 fixed-rate Reads without a timeout or missed arrival.
  The other 190 cells are explicit `not-run` results.
  Live T2/T3 role, quality and session/SecureChannel capacity evidence is
  recorded. Broader hardware coverage, UaExpert evidence and release acceptance
  remain pending.
- Adds reproducible test-only synthetic firmware profiles for 1/4/8/16
  channels at 1/10 Hz, declared 1/2/4-client load, independent Events/history
  switches and exact secure versus plaintext-test endpoints. Benchmark images
  ignore persisted configuration, reject configuration mutations, preserve the
  physical `dev_benchmark` preset and emit deterministic profile/export metadata.
- Adds the T5 read-only benchmark client with exact profile verification,
  bounded latency/error reports and host tests, plus benchmark-only PSRAM
  sampling capture controlled through serial. Each completed load repetition
  also records and validates the frozen 22-node resource snapshot through an
  existing client before cleanup. Benchmark timing samples use bounded 32-bit
  microsecond storage internally while keeping public values 64-bit, reducing
  the maximum-profile static SRAM footprint by 800 bytes. Imported device
  timing evidence remains explicitly separate from the current client run.
  Cold startup, successful synthetic Write, correlated event delivery,
  UaExpert and clean secure-profile release evidence remain open.
- Adds the T2 test-only reference-board exhaustion profile, with bounded
  100-cycle Heap and PSRAM fill/fail/free runners, payload verification,
  exact allocation-failure accounting, baseline and stabilized-low-water
  checks, bounded USB JSON, host validation, and a blocking CI firmware build.
  The reference board completed all 200 cycles in 569,778 ms with exact
  counters, verified payloads, stable low-water values and complete baseline
  recovery. Raw JSON, provenance and static-size evidence are recorded; this
  result is reference-board validation only.

## v0.5.1

Released tag commit: `2881230c2ccbb98b8701730c4de930dc95da30fc` (PR #20).
Release confirmed by the owner on 2026-09-15; the tag was resolved locally.

Historical verification gaps and exceptions are recorded in
`docs/releases/v0.5.1-review-fixes.md` ("Roadmap checklist status and
remaining gates"). Plan and history: `docs/releases/v0.5.1.md` (T1-T25).
Every industrial driver below is **compile/mock validated, not hardware
qualified** -- `RP2350_INDUSTRIAL_MVP_V1` has not been assembled.

- Driver SDK: writable reference driver (`gpio_output`, with `write`,
  `configure` and `self_test`), bus-agnostic SPI/I2C/UART transport contracts
  with RP2350 bindings, the `synthetic` example driver and the
  "how to add a driver" guide (`docs/code/adding-a-driver.md`).
- Industrial drivers for `RP2350_INDUSTRIAL_MVP_V1`: `ADS8688` analog inputs
  (AI0..AI3), `AD5422` daisy-chained analog outputs (0-10 V / 4-20 mA, chain
  startup with software reset and `AO_ENABLE_REQ` release, write-only),
  `MAX22190` digital inputs (frozen initialization sequence, ADI CRC-5),
  `IPS2050` protected digital outputs, `UIC` digital input / pulse count /
  frequency / period, `ISOW1432` RS-485 transport, `TPS3851` external
  watchdog, board diagnostics collection and raw `ADC4..ADC6` rail monitors.
  Board bindings and a 16-channel industrial seed configuration.
- Board profiles: `FIRMWARE_DRIVER_PACK` selects `base` (default, reference
  Waveshare Core2350B + W5500) or `industrial`. The pack now fixes the board
  header (`rp2350_industrial_mvp_v1.h`: PSRAM on `GPIO0`) and the W5500 pins
  (`GPIO4..GPIO8`); `static/tools/check_board_profile.py` and
  `static/tools/check_driver_pack_symbols.py` verify both in CI, which builds
  and links both profiles.
- OPC UA / capabilities: `SupportedDrivers` and `DriverCapabilities` report
  every driver's real resource claims; `Device.Compatibility.FirmwareProfile`
  reports the compiled pack; a versioned host-side descriptor format
  (`static/host_descriptors/driver_descriptors.json`).
- Runtime: a channel whose device does not answer (`TIMEOUT`) activates with
  `Bad` quality instead of rolling back the whole configuration; write-only
  channels are exempt from the read-based verify gate; AO writes outside the
  output range fail with `BadOutOfRange` instead of being clamped.
- Fixes found on the reference board: a fourth concurrent OPC UA connection
  no longer hangs (MEM-01), and temperature HistoryRead no longer stays
  unavailable after boot (MEM-02); both confirmed on hardware.
- OPC UA clock (regression present in `v0.5.0`): the open62541 v1.5.7 upgrade
  had dropped the port hook that makes `UA_DateTime_now()` use the
  SNTP-synchronized clock, so event, condition, maintenance and ServerStatus
  times and SecureChannel token `CreatedAt` read 1970 + uptime, and long
  sessions with strict clients such as asyncua could abort at token renewal.
  Restored and guarded by a host test; confirmed on the reference board.
  Until the first SNTP sync after a cold boot, those timestamps are the OPC UA
  null DateTime (1601-01-01), never a boot-relative value.
- Login backoff: lockouts ran on the SNTP wall clock, which reads 0 until the
  first sync, so on a cold boot without NTP one wrong password locked the
  account until the network delivered a time, and SNTP corrections moved
  running lockouts. Lockouts now use the monotonic boot clock.
- Time status: `Device.Time.TimeSynced`/Good was reported even when the RTC
  rejected the SNTP time, and a sync could be lost to a status write-back
  race. Sync is now reported only for a time the RTC accepted and only while
  the RTC stays valid; rejected times count in the sync error counter.
- Clock concurrency: an SNTP correction stops and rewrites the RP2350
  Always-On timer, and a task reading the clock inside that window saw no
  valid time -- a channel sample could get an invalid SourceTimestamp and
  the time status could briefly read Bad (with other task priorities, OPC UA
  timestamps could read null). RTC access is now serialized, and TimeTask no
  longer publishes a clock observation older than the latest SNTP apply.
- SNTP client: started and restarted from TimeTask and from the
  `SyncTimeNow` method without the lwIP core lock, racing the network stack's
  timer and UDP lists, and handed lwIP a server-name pointer into a stack
  frame that lwIP kept re-reading on every request. Now driven under the core
  lock with a persistent name buffer.
- Network bring-up: the W5500 netif, DHCP start and static address/DNS
  configuration ran without the lwIP core lock after the network thread had
  started. They now run under it (boot-time exposure only), and the driver,
  not `network_manager`, applies the static DNS server.
- Resource claims: GPIO-driven drivers claim their physical pins, so a
  generic GPIO channel can no longer alias an industrial signal.
- Breaking: the default pack is `base`; `industrial` images target only the
  industrial board; `FIRMWARE_ENABLE_INDUSTRIAL_W5500_PINS` is removed and
  `PICO_BOARD` must match the pack; the external sensor-I/O drivers
  (`TCAL9539`, board power service) were removed with their hardware
  (2026-09-10); `AD5422` accepts only range codes `0x01`/`0x05` and has no
  `SelfTest`; `ADS8688` accepts only mux inputs 0-3.
- Verified live on the reference board (2026-09-13): capability model and
  `SupportedDrivers` against the build, NodeSet exports against an
  independent address-space walk, HistoryRead and DI/AMB model.
- Recorded release limitations: UaExpert inspection lacks recorded completion;
  UIC quadrature is unimplemented; `environmental`/`motion`/`development`
  packs remain deferred by decision. Industrial hardware qualification is
  deferred under ROADMAP section 5.2. Publication does not turn these into
  passed checks or add them to v0.5.2 scope.

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
- OPC UA server restart, root cause and fix (found on hardware 2026-08-31):
  `UA_Server_run_shutdown()` could hang indefinitely during a controlled
  restart because open62541's `UA_BinaryProtocolManager` network callback
  accepts and fully activates new incoming connections even after being
  told to stop, so any client reconnecting during the shutdown drain window
  keeps the callback's SecureChannel-empty wait from ever completing —
  upstream open62541 behavior (confirmed present in both v1.5.3 and the
  v1.5.7 upgrade this release also carries, not a version-specific
  regression). Fixed with an independent FreeRTOS one-shot timer around the
  call; if it fires, falls back to the existing hardware-watchdog hard
  reboot. Recovers in practice around 27-28s worst case (detection + reboot
  + reconnect) instead of hanging forever, an accepted tradeoff versus a
  fast in-process restart when no client is present. See
  `docs/releases/v0.5.0-hardware-verification.md`.
- Namespace-build-failure rollback: closes the one transaction phase T4/T9's
  original scope left without a recovery path — `model_build_address_space()`
  failing during the restart above now rolls the device back to
  LAST_KNOWN_GOOD (Flash, `config_service`'s own running/staged state, and
  `channel_manager`/driver runtime state via a re-invoked `runtime_apply`,
  all three — an initial version that only restored the first two was
  found and fixed under review) instead of leaving `OpcUaTask` dead with no
  recovery. Two further review findings closed the same day: a genuinely
  fresh device's compiled defaults are now persisted to Flash `RUNNING` at
  first boot (previously RAM-only, so a first-ever transaction had nothing
  to roll back to if it failed) and guaranteed persisted again, retried on
  every transaction attempt, immediately before any transaction commits
  (the first-boot persist alone is necessarily best-effort and could leave
  the same gap open after one transient Flash failure); and a rollback that
  changes Flash but cannot fully reconcile runtime state now forces a
  hardware reboot instead of parking the device indefinitely with nothing
  else ever rebooting it. See `docs/releases/v0.5.0.md`'s "T6/T4 addendum".
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
- Found while documenting this release, then resolved as out of scope
  (2026-09-02), not fixed: a device that has run v0.4.0-v0.4.3 firmware
  does not have its configuration migrated in place when it upgrades to
  v0.5.0 firmware, because the old two-slot format's header magic and the
  new transaction backend's header/commit-page magic and offsets are
  unrelated. On review, this gap was retired rather than closed with an
  importer: this project has no fleet of field devices ever running
  pre-v0.5.0 firmware, and every device is flashed directly with the
  current tagged release rather than upgraded in place from an old image.
  The pre-v0.4.0 legacy single-sector importer this device never actually
  needed (`drivers/flash_storage_driver.c`/`.h`) and the dead v0.4.x
  two-slot format's own on-Flash struct definition were removed entirely
  rather than kept as unreachable code. See
  `docs/known_limits.md#v050-accepted-residual-risks`.
- Verification: 38/38 deterministic host-test suites
  (`static/tools/run_host_tests.py`, including new `config_service_rollback`
  and `config_transaction_reboot_recovery` coverage for the rollback fixes
  above) and first-party cppcheck (`static/tools/run_first_party_cppcheck.py`)
  both pass on the integrated v0.5.0 tree; hardware-verified on the
  reference RP2350 board (see `docs/releases/v0.5.0-hardware-verification.md`).
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
