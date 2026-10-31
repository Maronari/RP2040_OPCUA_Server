#!/usr/bin/env python3
"""Prove startup console normalization cannot conceal public model drift."""
import pathlib
import unittest

import verify_opcua_public_contract as contract


class StartupContractTests(unittest.TestCase):
    def test_only_exact_reviewed_console_statements_are_normalized(self):
        old = b'printf("[OPC UA]\\tWaiting for IPv4 before server startup\\n");'
        new = b'printf("[OPC UA]\\tNo IPv4 address at server creation; using wildcard endpoint\\n");'
        failure = b'printf("[OPC UA]\\tNetwork manager is not initialized\\n");'
        self.assertEqual(contract.canonicalize_v060_startup_console(new), old)
        self.assertEqual(contract.canonicalize_v060_startup_console(failure), b'')
        unknown = new.replace(b'wildcard', b'unreviewed')
        self.assertEqual(contract.canonicalize_v060_startup_console(unknown), unknown)

    def test_public_values_and_registrations_are_never_normalized(self):
        public = (
            b'UA_LOCALIZEDTEXT("en", "[OPC UA]\\tNetwork manager is not initialized\\n");'
            b'UA_NODEID_STRING(1, "OfflineStartup");'
            b'attr.accessLevel = UA_ACCESSLEVELMASK_READ;'
        )
        self.assertEqual(contract.canonicalize_v060_startup_console(public), public)
        baseline = contract.public_inventory(public)
        for before, after in ((b'OfflineStartup', b'ChangedNode'),
                              (b'UA_ACCESSLEVELMASK_READ', b'UA_ACCESSLEVELMASK_WRITE'),
                              (b'Network manager', b'Changed public text')):
            changed = contract.canonicalize_v060_startup_console(public.replace(before, after))
            self.assertNotEqual(contract.public_inventory(changed), baseline)

    def test_current_public_inventory_matches_v052(self):
        root = pathlib.Path(__file__).resolve().parents[2]
        baseline = contract.public_inventory(contract.aggregate_baseline_source(
            root, '2a0bcd8027b37e176e3264f739ee70cfb105e64b'))
        current = contract.public_inventory(contract.aggregate_current_source(root))
        self.assertEqual(current, baseline)
        self.assertEqual(current['sha256'], contract.EXPECTED_V052_PUBLIC_INVENTORY_SHA256)


if __name__ == '__main__':
    unittest.main()
