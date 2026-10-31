#!/usr/bin/env python3
"""Host tests for the T0 executable recoverable-OTA layout proof."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "static" / "tools" / "verify_ota_recovery_design.py"
SPEC = importlib.util.spec_from_file_location("ota_recovery_design", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("could not load ota recovery design model")
MODEL = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODEL
SPEC.loader.exec_module(MODEL)


def current_layout() -> dict:
    regions = {name: {"offset_bytes": offset, "size_bytes": size}
               for name, (offset, size) in MODEL.PRESERVED_REGION_OFFSETS.items()}
    return {"board": "synthetic-reference", "flash": {"size_bytes": MODEL.FLASH_SIZE},
            "constraints": {"erase_size_bytes": MODEL.ERASE_SIZE}, "regions": regions}


class RecoveryDesignTests(unittest.TestCase):
    def test_full_fault_proof(self) -> None:
        report = MODEL.run_fault_proof()
        self.assertEqual("PASS", report["status"])
        self.assertGreater(report["fault_boundaries_exercised"], 10)
        self.assertTrue(report["unsafe_overwrite_before_backup_detected"])

    def test_candidate_reset_never_relaunches(self) -> None:
        old = MODEL.Image("old", b"old-content-spanning-sectors", 5)
        candidate = MODEL.Image("candidate", b"candidate-content-spanning-sectors", 6)
        model = MODEL.RecoveryModel(old, candidate)
        self.assertEqual("launch-candidate-once", model.start_activation())
        self.assertEqual("restored-known-good", model.boot_after_reset())
        self.assertEqual(old, model.slots["A"])
        self.assertEqual("boot-known-good", model.boot_after_reset())

    def test_active_content_must_match_pinned_confirmed_state(self) -> None:
        old = MODEL.Image("old", b"old", 5)
        candidate = MODEL.Image("candidate", b"candidate", 6)
        model = MODEL.RecoveryModel(old, candidate)
        replacement = MODEL.Image("other-signed", b"other", 5)
        model.slots["A"] = replacement
        model.slot_bytes["A"] = model._payload_sectors(replacement.payload)
        with self.assertRaisesRegex(MODEL.DesignError, "pinned confirmed"):
            model.start_activation()

    def test_confirmation_advances_floor_atomically_in_model(self) -> None:
        old = MODEL.Image("old", b"old", 5)
        candidate = MODEL.Image("candidate", b"candidate", 6)
        model = MODEL.RecoveryModel(old, candidate)
        model.start_activation()
        self.assertEqual("candidate-confirmed", model.confirm_candidate())
        self.assertEqual(6, model.state().minimum_version)
        with self.assertRaises(MODEL.DesignError):
            MODEL.RecoveryModel(candidate, MODEL.Image("downgrade", b"old", 5)).start_activation()

    def test_rejects_invalid_images_state_and_same_version_change(self) -> None:
        old = MODEL.Image("old", b"old", 5)
        with self.assertRaises(MODEL.DesignError):
            MODEL.RecoveryModel(old, MODEL.Image("unsigned", b"new", 6, False))
        with self.assertRaises(MODEL.DesignError):
            MODEL.RecoveryModel(old, MODEL.Image("too-large", b"x" * (MODEL.IMAGE_CAPACITY + 1), 6))
        model = MODEL.RecoveryModel(old, MODEL.Image("changed", b"changed", 5))
        with self.assertRaises(MODEL.DesignError):
            model.start_activation()
        model.state_sectors = [None, None]
        with self.assertRaises(MODEL.DesignError):
            model.boot_after_reset()

    def test_confirmed_identity_and_floor_cannot_diverge(self) -> None:
        old = MODEL.Image("confirmed", b"old", 8)
        changed = MODEL.Image("changed", b"different", 8)
        for phase in MODEL.Phase:
            record = MODEL.State(0, phase, old.digest, 8,
                                 None if phase == MODEL.Phase.STABLE else changed.digest,
                                 None if phase == MODEL.Phase.STABLE else changed.version, 7)
            with self.subTest(phase=phase):
                self.assertFalse(record.valid())
                model = MODEL.RecoveryModel(old, changed)
                model.state_sectors = [record, None]
                with self.assertRaisesRegex(MODEL.DesignError, "state sectors are invalid"):
                    model.start_activation()
                self.assertEqual(old, model.slots["A"])
                self.assertEqual(old, model.slots["B"])

    def test_rejects_equal_sequence_conflict_and_both_invalid_images(self) -> None:
        old = MODEL.Image("old", b"old", 5)
        model = MODEL.RecoveryModel(old)
        initial = model.state()
        model.state_sectors[1] = MODEL.State(
            initial.sequence, initial.phase, "0" * 64, initial.fallback_version,
            initial.candidate_digest, initial.candidate_version, initial.minimum_version,
        )
        with self.assertRaisesRegex(MODEL.DesignError, "equal-sequence"):
            model.state()
        model = MODEL.RecoveryModel(old)
        model.slots["A"] = None
        model.slot_bytes["A"] = []
        model.slots["B"] = None
        model.slot_bytes["B"] = []
        with self.assertRaisesRegex(MODEL.DesignError, "A and B are invalid"):
            model.boot_after_reset()

    def test_accepts_identical_same_generation_copies(self) -> None:
        model = MODEL.RecoveryModel(MODEL.Image("old", b"old", 5))
        initial = model.state()
        model.state_sectors[1] = initial
        self.assertEqual(initial, model.state())

    def test_rejects_malformed_state_phase_bounds_and_descriptors(self) -> None:
        old = MODEL.Image("old", b"old", 5)
        valid = MODEL.RecoveryModel(old).state()
        malformed = (
            MODEL.State(MODEL.MAX_STATE_SEQUENCE + 1, MODEL.Phase.STABLE,
                        valid.fallback_digest, 5, None, None, 5),
            MODEL.State(0, "unknown-phase", valid.fallback_digest, 5, None, None, 5),
            MODEL.State(0, MODEL.Phase.BACKUP_READY, valid.fallback_digest, 5,
                        valid.fallback_digest, None, 5),
        )
        for record in malformed:
            model = MODEL.RecoveryModel(old)
            model.state_sectors = [record, None]
            with self.subTest(record=record):
                with self.assertRaisesRegex(MODEL.DesignError, "state sectors are invalid"):
                    model.state()

    def test_partial_sector_copy_is_not_promoted(self) -> None:
        old = MODEL.Image("old", b"old-payload-spans-two", 5)
        candidate = MODEL.Image("candidate", b"candidate-payload-spans-two", 6)
        model = MODEL.RecoveryModel(old, candidate)
        with self.assertRaises(MODEL.PowerLoss):
            model.start_activation(MODEL.FaultPlan(1))
        self.assertIsNone(model.slots["B"])
        self.assertFalse(model._slot_material_valid("B"))
        self.assertTrue(model._slot_material_valid("A"))

    def test_generated_layout_tail_validation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            layout = pathlib.Path(directory) / "ota_layout.json"
            layout.write_text(json.dumps(current_layout()), encoding="utf-8")
            result = MODEL.validate_layout(layout)
            self.assertTrue(result["preserved_tail_verified"])
            broken = current_layout()
            broken["regions"]["history"]["offset_bytes"] += MODEL.ERASE_SIZE
            layout.write_text(json.dumps(broken), encoding="utf-8")
            with self.assertRaises(MODEL.DesignError):
                MODEL.validate_layout(layout)
            malformed = current_layout()
            malformed["flash"]["size_bytes"] = "16777216"
            layout.write_text(json.dumps(malformed), encoding="utf-8")
            with self.assertRaisesRegex(MODEL.DesignError, "integer"):
                MODEL.validate_layout(layout)

    def test_cli_reports_existing_bin_and_fails_oversize(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            layout = root / "ota_layout.json"
            image = root / "firmware.bin"
            report = root / "report.json"
            layout.write_text(json.dumps(current_layout()), encoding="utf-8")
            image.write_bytes(b"firmware")
            subprocess.run([sys.executable, str(MODULE_PATH), "--layout", str(layout),
                            "--image", str(image), "--output", str(report)], check=True)
            document = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(len(b"firmware"), document["image_capacity_evidence"]["size_bytes"])
            image.write_bytes(b"x" * (MODEL.IMAGE_CAPACITY + 1))
            failed = subprocess.run([sys.executable, str(MODULE_PATH), "--image", str(image)],
                                    text=True, capture_output=True)
            self.assertNotEqual(0, failed.returncode)
            self.assertIn("outside", failed.stderr)


if __name__ == "__main__":
    unittest.main()
