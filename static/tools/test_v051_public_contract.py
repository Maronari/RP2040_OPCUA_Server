#!/usr/bin/env python3
"""Regression checks for the immutable v0.5.0 -> v0.5.1 contract delta."""
import copy
import pathlib
import subprocess
import sys
import unittest

import verify_opcua_public_contract as contract


class ContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = pathlib.Path(__file__).resolve().parents[2]
        cls.baseline = contract.public_inventory(contract.aggregate_baseline_source(
            cls.root, contract.EXPECTED_V050_COMMIT))
        # Keep this historical release-delta test pinned to the released
        # v0.5.1 tree. Later additive releases are checked by their own gate.
        cls.current = contract.public_inventory(contract.aggregate_baseline_source(
            cls.root, contract.EXPECTED_V051_COMMIT))

    def test_actual_release_delta(self):
        contract.assert_v051_inventory_delta(self.baseline, self.current)

    def test_removed_or_added_registration_rejected(self):
        for field in ("node_and_browse_calls", "public_attribute_assignments"):
            for operation in ("remove", "duplicate"):
                with self.subTest(field=field, operation=operation):
                    changed = copy.deepcopy(self.current)
                    if operation == "remove":
                        changed[field].pop()
                    else:
                        changed[field].append(changed[field][0])
                    with self.assertRaises(SystemExit):
                        contract.assert_v051_inventory_delta(self.baseline, changed)

    def test_unreviewed_string_changes_rejected(self):
        for operation in ("remove", "add"):
            changed = copy.deepcopy(self.current)
            if operation == "remove":
                changed["string_literals"].pop()
            else:
                changed["string_literals"].append('"unreviewed-node"')
            with self.assertRaises(SystemExit):
                contract.assert_v051_inventory_delta(self.baseline, changed)

    def test_wrong_baseline_rejected_by_cli(self):
        result = subprocess.run([sys.executable, str(pathlib.Path(contract.__file__)),
                                 "--baseline", "v0.4.3", "--allow-v051-additions"],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("v0.5.0@" + contract.EXPECTED_V050_COMMIT, result.stderr)


if __name__ == "__main__":
    unittest.main()
