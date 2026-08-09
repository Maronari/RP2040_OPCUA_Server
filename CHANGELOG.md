# Changelog

This file tracks the firmware release line through `v0.4.1`. Historical tag
SHAs below are resolved from peeled Git tags, so annotated tags are reported by
their target commit SHA rather than the tag-object SHA.

## v0.4.1 (Unreleased)

- Scope: harden the bounded generic channel runtime, driver lifecycle,
  ownership, timestamps and resource validation without adding the v0.4.2 OPC
  UA channel model.
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
