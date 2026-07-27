Implement the first unreleased version of this repository.

Read, in this order:

1. `AGENTS.md`
2. `ROADMAP.md`
3. The matching release execution plan under `docs/releases/`
4. Relevant source, public headers, generated-model inputs, verification tools,
   build files, and documentation

Treat the first unreleased roadmap version as the complete task boundary.

Execute the entire release workflow in the matching release plan:

1. Inspect the repository and working tree.
2. Run the required subagents in their assigned modes.
3. Reconcile their findings into one file-level implementation plan.
4. Resolve contradictions before editing.
5. Implement all in-scope tasks.
6. Add and run focused automated verification.
7. Run the independent reviewer.
8. Fix valid review findings.
9. Run all release-gate builds and verification.
10. Update affected documentation, but do not modify `ROADMAP.md`.
11. Produce the required final report.

Do not ask for confirmation between ordinary implementation stages.

Stop rather than guessing when a stop condition in `AGENTS.md` or the release
execution plan applies. Never modify `ROADMAP.md` while executing this prompt.
Do not broaden the release, weaken acceptance criteria, or pull functionality
from later versions.

Use subagents for distinct responsibilities. Read-only subagents stay read-only
where designated, and write-capable subagents follow the release plan's
assigned scope. The primary agent owns all final decisions, implementation,
integration, verification, and reporting.

Do not claim that hardware, UaExpert, target-network, or other manual checks
were performed unless they were actually performed.

Complete as much of the version as the available environment permits. Clearly
distinguish:

* implemented and verified;
* implemented but only partially verified;
* blocked;
* deferred because it is outside the current release.

Report the status of every applicable roadmap acceptance item and any release
commit SHA in the final report. Leave all roadmap checkboxes and text unchanged.
