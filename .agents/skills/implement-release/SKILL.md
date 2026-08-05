---

name: implement-release
description: Implement the first unreleased repository version end-to-end using AGENTS.md, ROADMAP.md, the matching release execution plan, project subagents, automated verification, independent review, and release-status reporting.
----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

# Implement release

Implement the first unreleased version of this repository as one coordinated
release workflow.

## Governing documents

Read, in this order:

1. `AGENTS.md`
2. `ROADMAP.md`
3. The matching release execution plan:
   `docs/releases/<version>.md`
4. Relevant source files, public headers, generated-model inputs, build files,
   verification tools, and documentation

Determine `<version>` from the first unreleased version in `ROADMAP.md`.

Verify that the version number and title in the release execution plan match
the first unreleased roadmap version.

If `AGENTS.md`, `ROADMAP.md`, and the release execution plan disagree, stop
before editing and report the conflict.

Do not modify:

* `ROADMAP.md`;
* `AGENTS.md`;
* the applicable release execution plan;

unless the user explicitly requested instruction, roadmap, or release-plan
maintenance.

## Scope

Treat the first unreleased roadmap version as the complete implementation
boundary.

Do not:

* implement work assigned to later releases;
* introduce speculative APIs or abstractions;
* weaken acceptance criteria;
* change existing public contracts without explicit release authorization;
* perform unrelated cleanup or refactoring;
* overwrite unrelated working-tree changes.

Complete as much of the current release as the available environment permits.

## Initial repository checks

Before delegating or editing:

1. Inspect the working tree and current branch.
2. Identify unrelated modified or untracked files.
3. Read the complete current release plan.
4. Map the release tasks and their dependencies.
5. Identify required generated files, toolchains, submodules, hardware, clients,
   and external tools.
6. Identify which verification can and cannot be performed in the current
   environment.

Do not clean, reset, stash, overwrite, or discard unrelated user changes.

## Subagent orchestration

Use only project-scoped agents defined in `.codex/agents/`.

Do not invent agent names.

Give every subagent:

* one concrete responsibility;
* an explicit deliverable;
* a file or subsystem boundary;
* an explicit read-only or write scope;
* the applicable release-plan task identifiers.

The primary agent owns:

* release scope;
* the integrated implementation plan;
* architecture decisions;
* conflict resolution;
* integration;
* final verification;
* final reporting.

Subagent output is advisory until the primary agent validates it against source
code, generated models, build output, or executed tests.

### Analysis phase

Spawn the mandatory read-only analysis agents required by the release plan.

For the current maintenance release, normally begin with `code-mapper`.

Ask it to return:

* relevant files and symbols;
* code and state-flow paths;
* mutable-state owners;
* all mutation sites;
* public compatibility surfaces;
* build and verification integration;
* unresolved unknowns with supporting evidence.

Wait for all required read-only analysis agents before producing the integrated
implementation plan.

Do not begin code changes while a mandatory pre-coding analysis task remains
incomplete.

### Design phase

Spawn the design agents assigned by the release plan.

For v0.3.2 these normally include:

* `embedded-systems` for bounded native service state, synchronization,
  transaction semantics, resource constraints, and the open62541-free service
  API;
* `iot-engineer` for OPC UA method contracts, authorization, status-code
  mapping, DI/AMB semantics, model synchronization, events, and automated client
  verification.

Design work may run in parallel only when responsibilities do not overlap.

Wait for the required design results.

Reconcile all findings into one file-level implementation plan containing:

* files to create or modify;
* ownership boundaries;
* public API changes;
* state and lifetime semantics;
* synchronization;
* argument bounds;
* error mappings;
* event behavior;
* build integration;
* verification;
* resource-impact checks.

Resolve contradictions before editing.

If an unresolved contradiction affects a public contract, state ownership,
safety, resource bounds, or release scope, stop and report it.

### Implementation phase

Freeze the agreed service API and public OPC UA contracts before launching
parallel write tasks.

Assign disjoint write scopes.

For v0.3.2, prefer:

* `embedded-systems`:

  * `services/include/services/maintenance_service.h`
  * `services/src/maintenance_service.c`
  * explicitly assigned service-local files

* `iot-engineer`:

  * explicitly assigned `opcua_model/**` files
  * the dedicated maintenance runtime verification script
  * explicitly assigned OPC UA documentation

* `build-engineer`:

  * explicitly assigned CMake files
  * version-generation files
  * artifact or scoped CI files

Never allow two agents to edit the same file or subsystem concurrently.

A write-capable subagent must not change another agent's agreed public interface
without returning the issue to the primary agent.

Wait for all write-capable agents before integration.

Inspect every returned diff before accepting it.

Integrate the results and fix interface, build, ownership, or behavioral
inconsistencies in the primary thread.

## Verification

Run focused verification after meaningful implementation changes.

Verification must test behavior, not merely process startup.

Use the exact commands and acceptance criteria defined by the release plan.

For v0.3.2, verify at minimum:

* the required Debug build;
* AMB structural correctness;
* all maintenance methods;
* every allowed state transition;
* every forbidden source state;
* every relevant role;
* missing and malformed arguments;
* boundary and oversized inputs;
* unchanged state after rejection;
* synchronized state text and state NodeId;
* exactly one transition event after success;
* no transition event after rejection;
* two simultaneous subscribed clients;
* nonzero verification-script exit status on assertion failure;
* version consistency across required outputs.

Distinguish:

* static/model verification;
* runtime automated client verification;
* target-hardware verification;
* UaExpert/manual verification.

Do not claim that a command, build, hardware test, network test, UaExpert check,
or resource measurement was performed unless it was actually performed.

If required verification cannot run, report:

* the exact missing dependency or environment;
* which acceptance criteria remain unverified;
* whether the implementation is still suitable as a release candidate.

## Independent review

After implementation is integrated and focused tests pass, spawn
`code-reviewer`.

Give it the complete integrated diff and require review against:

* `AGENTS.md`;
* `ROADMAP.md`;
* the applicable release plan;
* existing public contracts;
* architecture boundaries;
* resource constraints;
* executed verification.

The reviewer must remain read-only.

Require findings ordered by severity with exact file and line evidence.

Resolve all valid critical and high findings.

Resolve medium findings or document a concrete release-scoped justification.

After every fix, rerun the affected verification.

Do not run the independent final review before the integrated diff is ready.

## Release gate

Run the release-plan build and verification gate after review findings are
resolved.

Record:

* exact commands executed;
* exit status and result;
* new first-party warnings;
* produced ELF, UF2, BIN, and other required artifacts;
* ELF section sizes;
* internal SRAM impact;
* PSRAM impact;
* task-stack impact;
* artifact-size deltas;
* version evidence;
* skipped or unavailable checks.

Do not substitute estimates for measurements without labeling them as
estimates.

## Documentation

Update only documentation affected by the implemented behavior.

Do not update roadmap checkboxes, release SHA, or release status while executing
this skill.

Instead, report the roadmap acceptance status in the final response so the user
can perform roadmap maintenance separately.

Do not modify generated or vendored files directly unless the repository
explicitly defines them as editable sources.

## Stop conditions

Stop implementation and report the blocker when:

* governing documents conflict;
* the matching release plan is missing;
* required public contracts cannot be determined;
* safe bounded behavior cannot be defined;
* state ownership or synchronization cannot be made coherent;
* required work depends on a later release;
* unrelated working-tree changes overlap required files and cannot be safely
  preserved;
* implementation would weaken authorization, compatibility, rollback, or
  deterministic failure behavior;
* the release plan defines another explicit stop condition.

Do not modify governing documents merely to bypass a stop condition.

## Completion classification

Classify the result as exactly one of:

* `blocked`;
* `partially implemented`;
* `implementation complete`;
* `release candidate`;
* `release accepted`.

Use:

### Blocked

A stop condition prevented safe implementation.

### Partially implemented

Only part of the release scope was implemented, or required implementation tasks
remain incomplete.

### Implementation complete

All planned code and automated verification available in the environment are
complete, but release-gate or manual acceptance work remains.

### Release candidate

Implementation, automated verification, independent review, required build
artifacts, and documentation are complete. Only explicitly manual or
environment-dependent acceptance checks remain.

### Release accepted

Every required automated, hardware, network, and manual acceptance check,
including UaExpert where required, was actually performed successfully.

Do not classify the release as accepted when any required manual or hardware
check was not performed.

## Final report

Return a final report containing:

1. Completion classification.
2. Release summary.
3. Architecture and state-ownership result.
4. Subagents used and their assigned tasks.
5. Files changed and why.
6. Public API, OPC UA method, NodeId, BrowseName, schema, and version effects.
7. Tests and verification added.
8. Exact commands actually executed and their results.
9. Compiler warnings.
10. ELF, SRAM, PSRAM, task-stack, UF2, and BIN results.
11. Independent-review findings and resolutions.
12. Hardware, UaExpert, network, or external checks not performed.
13. Residual risks and assumptions.
14. Status of every applicable roadmap acceptance criterion.
15. Recommended release commit SHA handling without editing `ROADMAP.md`.

Clearly separate:

* implemented and verified;
* implemented but partially verified;
* blocked;
* deferred because it is outside the current release.
