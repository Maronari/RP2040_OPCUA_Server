#!/usr/bin/env python3
"""Executable T0 proof for the proposed recoverable OTA flash layout.

This is deliberately a host-side model, not firmware and not a signature
implementation.  It fixes the layout arithmetic and exercises the order of
destructive operations with small synthetic flash slots.  A production T1/T2
implementation must still authenticate SHA-256/ECDSA P-256 manifests, use the
real flash backend, and reproduce this state machine's guarantees.

The frozen T0 policy chooses restoration of B after a reset during pre-launch
C->A copying, rather than resuming C. The release plan permits this conservative
choice. T2 must preserve it; a changed resume policy requires renewed design
review rather than an implicit extra installation attempt.

The proposed 16 MiB geometry is:

    bootloader [0x000000, 0x040000)
    A (Bank-A linked execution) [0x040000, 0x548000)
    B (immutable authenticated fallback) [0x548000, 0xa50000)
    C (authenticated staged candidate) [0xa50000, 0xf58000)
    preserved legacy tail [0xf58000, 0x1000000)

Each slot reserves its final 4096-byte sector for its manifest, so its maximum
payload is 0x507000 bytes.  The model stores payload bytes independently from
the manifest sector; an Image's digest is a stand-in for native authentication.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import sys
from dataclasses import dataclass
from enum import Enum
from typing import Iterable


FLASH_SIZE = 0x1000000
ERASE_SIZE = 0x1000
BOOT_END = 0x40000
SLOT_SIZE = 0x508000
MANIFEST_SIZE = ERASE_SIZE
IMAGE_CAPACITY = SLOT_SIZE - MANIFEST_SIZE
SLOT_A = (BOOT_END, BOOT_END + SLOT_SIZE)
SLOT_B = (SLOT_A[1], SLOT_A[1] + SLOT_SIZE)
SLOT_C = (SLOT_B[1], SLOT_B[1] + SLOT_SIZE)
PRESERVED_TAIL_START = SLOT_C[1]
PRESERVED_TAIL_END = FLASH_SIZE
STATE_SECTORS = 2
MAX_STATE_SEQUENCE = (1 << 64) - 1
MAX_VERSION = (1 << 32) - 1

PRESERVED_REGION_OFFSETS = {
    "maintenance_journal": (0xF58000, 0x8000),
    "history": (0xF60000, 0x20000),
    "config_slot_0": (0xF80000, 0x10000),
    "config_slot_1": (0xF90000, 0x10000),
    "security_identity_slot_0": (0xFA0000, 0x10000),
    "security_identity_slot_1": (0xFB0000, 0x10000),
    "security_trust_slot_0": (0xFC0000, 0x10000),
    "security_trust_slot_1": (0xFD0000, 0x10000),
    "security_state": (0xFE0000, 0x10000),
    "state": (0xFF0000, 0x10000),
}


class DesignError(ValueError):
    """The proposed geometry or a modeled boot transaction is invalid."""


class PowerLoss(RuntimeError):
    """Injected reset: the model stops at one destructive-operation boundary."""


class Phase(str, Enum):
    STABLE = "stable"
    BACKUP_READY = "backup-ready"
    CANDIDATE_LAUNCHED = "candidate-launched"


def require(condition: bool, message: str) -> None:
    """A proof invariant which still runs under ``python -O``."""
    if not condition:
        raise DesignError(message)


def valid_uint(value: object, maximum: int) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= maximum


def valid_digest(value: object) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(character in "0123456789abcdef" for character in value)


@dataclass(frozen=True)
class Image:
    """Authenticated image abstraction used only by this host proof."""

    name: str
    payload: bytes
    version: int
    authenticated: bool = True

    @property
    def digest(self) -> str:
        return hashlib.sha256(self.payload).hexdigest()

    def valid(self) -> bool:
        return self.authenticated and 0 < len(self.payload) <= IMAGE_CAPACITY


@dataclass(frozen=True)
class State:
    sequence: int
    phase: Phase
    fallback_digest: str
    fallback_version: int
    candidate_digest: str | None
    candidate_version: int | None
    minimum_version: int

    def valid(self) -> bool:
        if not valid_uint(self.sequence, MAX_STATE_SEQUENCE) \
                or not valid_uint(self.fallback_version, MAX_VERSION) \
                or not valid_uint(self.minimum_version, MAX_VERSION) \
                or not valid_digest(self.fallback_digest) or not isinstance(self.phase, Phase):
            return False
        candidate_is_absent = self.candidate_digest is None and self.candidate_version is None
        candidate_is_present = valid_digest(self.candidate_digest) and valid_uint(self.candidate_version, MAX_VERSION)
        if not (candidate_is_absent or candidate_is_present):
            return False
        # Confirmation commits the confirmed identity and floor together. A
        # lower floor would misclassify changed same-version content as newer.
        if self.fallback_version != self.minimum_version:
            return False
        if self.phase == Phase.STABLE:
            return candidate_is_absent
        return candidate_is_present


class FaultPlan:
    """Interrupt one global boundary occurrence; every operation is bounded.

    The ordinal, rather than a reusable label, matters: the second
    ``state-commit`` is a distinct fault site from the first one.
    """

    def __init__(self, occurrence: int | None = None) -> None:
        if occurrence is not None and (not valid_uint(occurrence, MAX_STATE_SEQUENCE) or occurrence == 0):
            raise DesignError("fault occurrence must be a positive integer")
        self.occurrence = occurrence
        self.seen: list[str] = []

    def hit(self, boundary: str) -> None:
        ordinal = len(self.seen) + 1
        site = f"{ordinal}:{boundary}"
        self.seen.append(site)
        if self.occurrence == ordinal:
            raise PowerLoss(site)


class RecoveryModel:
    """Small-slot model of the T0 transaction; no real flash or crypto calls."""

    def __init__(self, known_good: Image, candidate: Image | None = None) -> None:
        if not known_good.valid():
            raise DesignError("known-good image must be authenticated and within the payload capacity")
        if candidate is not None and not candidate.valid():
            raise DesignError("candidate image must be authenticated and within the payload capacity")
        self.slots: dict[str, Image | None] = {"A": known_good, "B": known_good, "C": candidate}
        self.slot_bytes: dict[str, list[bytes | None]] = {
            "A": self._payload_sectors(known_good.payload),
            "B": self._payload_sectors(known_good.payload),
            "C": self._payload_sectors(candidate.payload) if candidate is not None else [],
        }
        initial = State(0, Phase.STABLE, known_good.digest, known_good.version,
                        None, None, known_good.version)
        self.state_sectors: list[State | None] = [initial, None]

    def clone(self) -> "RecoveryModel":
        return copy.deepcopy(self)

    @staticmethod
    def _payload_sectors(payload: bytes) -> list[bytes]:
        return [payload[index:index + 16] for index in range(0, len(payload), 16)]

    def _selected_state(self) -> tuple[int, State]:
        valid = [(index, record) for index, record in enumerate(self.state_sectors)
                 if record is not None and record.valid()]
        if not valid:
            raise DesignError("both persistent state sectors are invalid; recovery must stay in bootloader")
        highest_sequence = max(record.sequence for _index, record in valid)
        selected = [(index, record) for index, record in valid if record.sequence == highest_sequence]
        if any(record != selected[0][1] for _index, record in selected[1:]):
            raise DesignError("persistent state sectors contain an equal-sequence conflict")
        return selected[0]

    def state(self) -> State:
        return self._selected_state()[1]

    def _commit(self, proposed: State, fault: FaultPlan) -> None:
        """Write a new record into the other state sector without erasing the prior one.

        ``state-erase`` and ``state-program`` model a reset before commit: the prior
        valid record remains selected.  ``state-commit`` is after the valid record
        becomes readable.  This is the dual-sector survival property T2 must retain.
        """
        if not proposed.valid():
            raise DesignError("refusing to commit invalid persistent state")
        selected_sector, current = self._selected_state()
        require(proposed.sequence == current.sequence + 1,
                "persistent state sequence must advance exactly once")
        target = 1 - selected_sector
        fault.hit("state-erase")
        self.state_sectors[target] = None
        fault.hit("state-program")
        self.state_sectors[target] = proposed
        fault.hit("state-commit")

    @staticmethod
    def _same_image(left: Image | None, right: Image | None) -> bool:
        return left is not None and right is not None and left.valid() and right.valid() \
            and left.digest == right.digest and left.version == right.version

    def _slot_material_valid(self, slot: str) -> bool:
        image = self.slots[slot]
        if image is None or not image.valid():
            return False
        sectors = self.slot_bytes[slot]
        return all(isinstance(sector, bytes) for sector in sectors) \
            and b"".join(sector for sector in sectors if sector is not None) == image.payload

    def _slot_matches_image(self, slot: str, image: Image | None) -> bool:
        return self._same_image(self.slots[slot], image) and self._slot_material_valid(slot)

    def _copy(self, source: str, destination: str, fault: FaultPlan, label: str) -> None:
        image = self.slots[source]
        if image is None or not self._slot_material_valid(source):
            raise DesignError(f"{source} is not an authenticated bootable image")
        # Each small synthetic sector is erased, programmed, and read back before
        # it is promoted.  An interrupted destination contains only the sectors
        # reached so far and has no Image metadata, so it cannot become bootable.
        self.slots[destination] = None
        self.slot_bytes[destination] = [None] * len(self.slot_bytes[source])
        for sector, source_bytes in enumerate(self.slot_bytes[source]):
            require(isinstance(source_bytes, bytes), f"{source} has an unreadable sector")
            fault.hit(f"{label}-erase-{sector}")
            self.slot_bytes[destination][sector] = None
            fault.hit(f"{label}-program-{sector}")
            self.slot_bytes[destination][sector] = source_bytes
            fault.hit(f"{label}-verify-{sector}")
            require(self.slot_bytes[destination][sector] == source_bytes,
                    f"{label} sector {sector} readback differs from source")
        self.slots[destination] = image
        require(self._slot_matches_image(destination, image),
                f"{label} payload readback verification failed")

    def _fallback_matches_state(self, state: State) -> bool:
        image = self.slots["B"]
        return self._slot_material_valid("B") and image is not None \
            and image.digest == state.fallback_digest and image.version == state.fallback_version

    def _version_allowed(self, candidate: Image, state: State) -> bool:
        if candidate.version > state.minimum_version:
            return True
        # Digest equality here abstracts equality of the eventual complete signed
        # identity (manifest bytes/key-id/signature coverage), not payload-only
        # equality.  T1 must implement that exact identity comparison.
        return candidate.version == state.minimum_version and candidate.digest == state.fallback_digest

    def start_activation(self, fault: FaultPlan | None = None) -> str:
        """Create B before damaging A, then copy C to A and consume the one launch."""
        fault = fault or FaultPlan()
        state = self.state()
        if state.phase != Phase.STABLE:
            raise DesignError(f"activation is illegal in persistent phase {state.phase.value}")
        candidate = self.slots["C"]
        active = self.slots["A"]
        if candidate is None or not candidate.valid():
            raise DesignError("candidate is absent or unauthenticated")
        if active is None or not self._slot_material_valid("A"):
            raise DesignError("active A image is invalid; do not overwrite it")
        if active.digest != state.fallback_digest or active.version != state.fallback_version:
            raise DesignError("active A differs from the pinned confirmed image; do not bless it as fallback")
        if not self._version_allowed(candidate, state):
            raise DesignError("candidate violates the minimum-version/same-content policy")

        # B may be refreshed only while A still contains the authenticated active image.
        self._copy("A", "B", fault, "backup")
        backup = self.slots["B"]
        require(backup is not None and self._slot_material_valid("B"),
                "backup copy did not produce readable authenticated content")
        backup_ready = State(state.sequence + 1, Phase.BACKUP_READY, backup.digest, backup.version,
                             candidate.digest, candidate.version, state.minimum_version)
        self._commit(backup_ready, fault)

        # From this point B is immutable until confirmation or recovery.
        self._copy("C", "A", fault, "candidate")
        launched = State(backup_ready.sequence + 1, Phase.CANDIDATE_LAUNCHED,
                         backup.digest, backup.version, candidate.digest, candidate.version,
                         state.minimum_version)
        self._commit(launched, fault)  # Persistent consumption before branch to A.
        return "launch-candidate-once"

    def boot_after_reset(self, fault: FaultPlan | None = None) -> str:
        """Recover once per invocation.  No loop/retry can launch a candidate twice."""
        fault = fault or FaultPlan()
        state = self.state()
        if state.phase == Phase.STABLE:
            active = self.slots["A"]
            if self._slot_material_valid("A") and active is not None and active.digest == state.fallback_digest \
                    and active.version == state.fallback_version:
                return "boot-known-good"
            if not self._fallback_matches_state(state):
                raise DesignError("A and B are invalid; remain in bootloader recovery")
            self._copy("B", "A", fault, "restore")
            return "restored-known-good"
        if not self._fallback_matches_state(state):
            raise DesignError("fallback is invalid; remain in bootloader recovery")
        # BACKUP_READY means C->A was interrupted before launch consumption;
        # CANDIDATE_LAUNCHED means a reset/hang consumed its one chance.  Both
        # restore B and never branch to C or retry it.
        self._copy("B", "A", fault, "restore")
        restored = State(state.sequence + 1, Phase.STABLE, state.fallback_digest,
                         state.fallback_version, None, None, state.minimum_version)
        self._commit(restored, fault)
        return "restored-known-good"

    def confirm_candidate(self, fault: FaultPlan | None = None) -> str:
        """Atomically advance the version floor only for the launched candidate."""
        fault = fault or FaultPlan()
        state = self.state()
        if state.phase != Phase.CANDIDATE_LAUNCHED:
            raise DesignError("confirmation is illegal without a consumed candidate launch")
        active = self.slots["A"]
        if active is None or not self._slot_material_valid("A") or active.digest != state.candidate_digest \
                or active.version != state.candidate_version:
            raise DesignError("candidate A image no longer matches the launch record")
        confirmed = State(state.sequence + 1, Phase.STABLE, active.digest, active.version,
                          None, None, max(state.minimum_version, active.version))
        self._commit(confirmed, fault)
        return "candidate-confirmed"


def proposed_layout() -> dict:
    return {
        "flash_size_bytes": FLASH_SIZE,
        "erase_size_bytes": ERASE_SIZE,
        "bootloader": {"offset": 0, "size": BOOT_END},
        "slots": {
            name: {"offset": begin, "size": end - begin, "manifest_size": MANIFEST_SIZE,
                   "payload_capacity": IMAGE_CAPACITY}
            for name, (begin, end) in {"A": SLOT_A, "B": SLOT_B, "C": SLOT_C}.items()
        },
        "preserved_tail": {"offset": PRESERVED_TAIL_START,
                           "size": PRESERVED_TAIL_END - PRESERVED_TAIL_START},
        "state_sectors_required": STATE_SECTORS,
    }


def validate_layout(layout_path: pathlib.Path) -> dict:
    """Validate that a generated current layout keeps all existing tail regions intact."""
    try:
        document = json.loads(layout_path.read_text(encoding="utf-8"))
        flash_size = document["flash"]["size_bytes"]
        erase_size = document["constraints"]["erase_size_bytes"]
        regions = document["regions"]
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise DesignError(f"invalid generated layout JSON: {error}") from error
    if not isinstance(document, dict) or not isinstance(document.get("flash"), dict) \
            or not isinstance(document.get("constraints"), dict) or not isinstance(regions, dict):
        raise DesignError("generated layout JSON has malformed object fields")
    if not valid_uint(flash_size, MAX_STATE_SEQUENCE) or not valid_uint(erase_size, MAX_STATE_SEQUENCE):
        raise DesignError("generated layout flash/erase sizes must be integer values")
    if flash_size != FLASH_SIZE or erase_size != ERASE_SIZE:
        raise DesignError("generated layout does not match the 16 MiB / 4096-byte-sector T0 geometry")
    for name, (expected_offset, expected_size) in PRESERVED_REGION_OFFSETS.items():
        region = regions.get(name)
        offset = region.get("offset_bytes") if isinstance(region, dict) else None
        size = region.get("size_bytes") if isinstance(region, dict) else None
        if not valid_uint(offset, FLASH_SIZE) or not valid_uint(size, FLASH_SIZE) \
                or offset != expected_offset or size != expected_size:
            raise DesignError(f"preserved region {name} differs from the required current layout")
    require(SLOT_A[0] == BOOT_END and SLOT_C[1] == PRESERVED_TAIL_START,
            "internal proposed geometry is inconsistent")
    return {"path": str(layout_path), "board": document.get("board"),
            "preserved_tail_verified": True, "tail_start": PRESERVED_TAIL_START,
            "tail_end": PRESERVED_TAIL_END}


def validate_image(image_path: pathlib.Path) -> dict:
    try:
        size = image_path.stat().st_size
    except OSError as error:
        raise DesignError(f"cannot inspect image: {error}") from error
    if size <= 0 or size > IMAGE_CAPACITY:
        raise DesignError(f"image size {size} is outside 1..{IMAGE_CAPACITY} payload bytes")
    digest = hashlib.sha256(image_path.read_bytes()).hexdigest()
    return {"path": str(image_path), "size_bytes": size, "payload_capacity": IMAGE_CAPACITY,
            "headroom_bytes": IMAGE_CAPACITY - size, "sha256": digest,
            "cryptographically_authenticated": False,
            "note": "capacity/hash evidence only; T0 does not validate a signed manifest"}


def _assert_bootable(model: RecoveryModel, expected: Image) -> None:
    if not (model._slot_matches_image("A", expected) or model._slot_matches_image("B", expected)):
        raise DesignError("known-good content was lost from both A and B")


def _require_result(actual: str, expected: str) -> None:
    require(actual == expected, f"expected {expected}, received {actual}")


def _inject(model: RecoveryModel, operation, occurrence: int) -> None:
    fault = FaultPlan(occurrence)
    try:
        operation(fault)
    except PowerLoss:
        require(len(fault.seen) == occurrence,
                f"fault occurrence {occurrence} was not reached")
        return
    raise DesignError(f"fault occurrence {occurrence} did not interrupt the operation")


def run_fault_proof() -> dict:
    """Exhaust every modeled erase/program/state boundary for short multi-sector images."""
    old = Image("old", b"old-image-spans-three", 7)
    new = Image("new", b"new-image-spans-three", 8)
    baseline = RecoveryModel(old, new)
    clean = baseline.clone()
    _require_result(clean.start_activation(), "launch-candidate-once")
    _require_result(clean.confirm_candidate(), "candidate-confirmed")
    require(clean.state().minimum_version == 8, "confirmation did not advance version floor")
    _require_result(clean.boot_after_reset(), "boot-known-good")

    # Discover every bounded activation boundary, then re-run from a pristine
    # state with a reset at that boundary.  One recovery call may itself be
    # interrupted; the next invocation is still bounded and retains B.
    trace_plan = FaultPlan()
    baseline.clone().start_activation(trace_plan)
    activation_boundaries = trace_plan.seen
    injected = 0
    for occurrence in range(1, len(activation_boundaries) + 1):
        model = baseline.clone()
        _inject(model, model.start_activation, occurrence)
        injected += 1
        _assert_bootable(model, old)
        # A reset after a pre-launch interruption may choose a one-pass restore.
        for _ in range(2):
            try:
                model.boot_after_reset()
                break
            except PowerLoss:
                continue
        _assert_bootable(model, old)

    # Once launch is consumed, any reset restores B and does not attempt C.
    consumed = baseline.clone()
    consumed.start_activation()
    _require_result(consumed.boot_after_reset(), "restored-known-good")
    require(consumed._slot_matches_image("A", old), "candidate reset did not restore old bytes")

    # A reset during recovery remains recoverable on the next bounded
    # invocation.  B is never a destination in this phase.
    launched = baseline.clone()
    launched.start_activation()
    recovery_trace = FaultPlan()
    launched.clone().boot_after_reset(recovery_trace)
    for occurrence in range(1, len(recovery_trace.seen) + 1):
        model = launched.clone()
        _inject(model, model.boot_after_reset, occurrence)
        injected += 1
        _assert_bootable(model, old)
        require(model.boot_after_reset() in ("restored-known-good", "boot-known-good"),
                "interrupted recovery did not return to a bounded boot result")
        _assert_bootable(model, old)

    # Repeated reset during recovery is still a sequence of bounded invocations;
    # each restart uses B as source and cannot consume or launch C.
    repeated = launched.clone()
    for occurrence in (1, len(recovery_trace.seen) - 1):
        _inject(repeated, repeated.boot_after_reset, occurrence)
        _assert_bootable(repeated, old)
    _require_result(repeated.boot_after_reset(), "restored-known-good")
    _assert_bootable(repeated, old)

    # Confirmation either leaves the candidate unconfirmed (and hence causes a
    # fallback after reset) or has atomically made it the new known-good/floor.
    confirmation_trace = FaultPlan()
    clean_launch = baseline.clone()
    clean_launch.start_activation()
    clean_launch.confirm_candidate(confirmation_trace)
    for occurrence in range(1, len(confirmation_trace.seen) + 1):
        model = baseline.clone()
        model.start_activation()
        _inject(model, model.confirm_candidate, occurrence)
        injected += 1
        state = model.state()
        if state.phase != Phase.STABLE:
            _require_result(model.boot_after_reset(), "restored-known-good")
            _assert_bootable(model, old)
        else:
            require(state.minimum_version == new.version, "confirmed floor was not retained")
            _require_result(model.boot_after_reset(), "boot-known-good")
            _assert_bootable(model, new)

    # Model sensitivity: the checker must reject the classic unsafe algorithm
    # that erases A before an authenticated B backup exists.
    unsafe = baseline.clone()
    unsafe.slots["B"] = None
    unsafe.slot_bytes["B"] = []
    unsafe.slots["A"] = None
    unsafe.slot_bytes["A"] = []
    try:
        _assert_bootable(unsafe, old)
    except DesignError:
        unsafe_overwrite_detected = True
    else:
        raise AssertionError("proof did not detect overwrite-before-backup")

    # Invalid candidates, state and version policy are deterministic rejects.
    for invalid in (Image("bad-auth", b"x", 9, False), Image("empty", b"", 9)):
        try:
            RecoveryModel(old, invalid)
        except DesignError:
            pass
        else:
            raise AssertionError("invalid image was accepted")
    bad_state = baseline.clone()
    bad_state.state_sectors = [None, None]
    try:
        bad_state.boot_after_reset()
    except DesignError:
        pass
    else:
        raise AssertionError("invalid redundant state was accepted")
    same_changed = RecoveryModel(old, Image("changed", b"changed-same-version", 7))
    try:
        same_changed.start_activation()
    except DesignError:
        pass
    else:
        raise AssertionError("same-version changed content was accepted")
    same_identical = RecoveryModel(old, Image("same", old.payload, 7))
    _require_result(same_identical.start_activation(), "launch-candidate-once")

    return {"status": "PASS", "synthetic_sector_bytes": 16,
            "fault_boundaries_exercised": injected,
            "unsafe_overwrite_before_backup_detected": unsafe_overwrite_detected,
            "candidate_attempts": 1,
            "recovery_invocation_copy_passes": 1,
            "fault_sites": {
                "activation": activation_boundaries,
                "recovery": recovery_trace.seen,
                "confirmation": confirmation_trace.seen,
            }}


def parse_args(argv: Iterable[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--layout", type=pathlib.Path,
                        help="generated current ota_layout.json; validates preserved tail")
    parser.add_argument("--image", type=pathlib.Path,
                        help="existing BIN to report capacity/headroom evidence")
    parser.add_argument("--output", type=pathlib.Path,
                        help="write JSON report (otherwise print it)")
    parser.add_argument("--skip-fault-proof", action="store_true",
                        help="only validate layout/image inputs; intended for report inspection")
    return parser.parse_args(argv)


def main(argv: Iterable[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        report = {
            "design": "T0 executable recovery/layout proof",
            "layout": proposed_layout(),
            "limits": {
                "model_work_per_copy_invocation": "one finite slot copy; no retry loop",
                "candidate_launches_per_activation": 1,
                "state_commit_sectors": STATE_SECTORS,
            },
            "conservative_model_policy": (
                "A pre-launch interruption restores B instead of resuming C installation; "
                "This is the frozen T0 choice; T2 must preserve it."
            ),
            "abstraction_limits": [
                "Synthetic slot bytes and injected boundaries are not physical QSPI power-loss tests.",
                "The two State objects model selection only; they do not prove the proposed 1024-byte record/marker-page torn-write encoding.",
                "Image.authenticated is a stand-in; this proof does not parse manifests or verify SHA-256/ECDSA P-256.",
                "Same-version digest equality abstracts complete signed identity metadata; it is not a payload-only production identity check.",
                "This proof does not establish RP2350 execution/linker, watchdog, timing, SRAM/PSRAM, W5500/lwIP, or configuration rollback behavior.",
            ],
        }
        if args.layout:
            report["generated_layout_validation"] = validate_layout(args.layout)
        if args.image:
            report["image_capacity_evidence"] = validate_image(args.image)
        if not args.skip_fault_proof:
            report["fault_proof"] = run_fault_proof()
        encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(encoded, encoding="utf-8")
        else:
            print(encoded, end="")
        return 0
    except (DesignError, AssertionError, PowerLoss) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
