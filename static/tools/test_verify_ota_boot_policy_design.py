#!/usr/bin/env python3
"""Positive, negative and boundary checks for the T0 policy decision model."""
import copy
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

import verify_ota_boot_policy_design as model


class BootPolicyTests(unittest.TestCase):
    def setUp(self):
        self.policy = json.loads(model.POLICY.read_text(encoding='utf-8'))

    def snapshots(self, now=0):
        return {owner: {'state': 'ok', 'observed_ms': now, 'generation': 1}
                for owner in self.policy['required_owners']}

    def test_actual_policy_arithmetic(self):
        report = model.validate(self.policy)
        self.assertEqual(report['sectors_per_copy'], 1288)
        self.assertEqual(report['copy_pass_budget_ms'], 193200)
        self.assertEqual(report['loader_arithmetic_ms'], 647600)
        self.assertLess(report['projected_hash_us'], 6000000)
        self.assertFalse(report['integrated_timing_validated'])
        self.assertLessEqual(report['status_json_upper_bound_including_nul'], 512)

    def test_json_bound_preserves_legacy_keys_and_unavailable_progress(self):
        self.assertEqual(model.status_json_upper_bound(self.policy), 508)
        changed = copy.deepcopy(self.policy)
        changed['bounds']['status_json_bytes'] = 500
        with self.assertRaises(ValueError):
            model.validate(changed)

    def test_selected_limits_fit_and_exceed_observations(self):
        for section, name, invalid in (
            ('timing_ms', 'readiness_cutoff', 20000),
            ('timing_ms', 'sector_copy_step_budget', 10),
            ('timing_ms', 'image_verification_budget', 100),
            ('timing_ms', 'loader_transaction_budget', 1000),
            ('timing_ms', 'progress_freshness', 4000),
            ('timing_ms', 'handoff_adoption_budget', 5000),
            ('resource_limits_bytes', 'crypto_arena', 256),
            ('resource_limits_bytes', 'bootloader_stack', 1000),
            ('resource_limits_bytes', 'application_additional_static', 4000),
            ('bounds', 'payload_bytes', 5271553),
            ('bounds', 'copy_retries_in_boot', 3),
        ):
            with self.subTest(name=name):
                changed = copy.deepcopy(self.policy)
                changed[section][name] = invalid
                with self.assertRaises(ValueError):
                    model.validate(changed)

    def test_no_provisioning_or_integration_claim_from_a_model(self):
        for name, invalid in (('reference_board_revision', 1),
                              ('integrated_timing_validated', True),
                              ('integrated_resource_validated', True),
                              ('production_requires_explicit_board_revision', False),
                              ('candidate_attempts', 3)):
            changed = copy.deepcopy(self.policy)
            changed[name] = invalid
            with self.assertRaises(ValueError):
                model.validate(changed)

    def test_policy_ids_cannot_alias_fixture_or_override(self):
        changed = copy.deepcopy(self.policy)
        changed['production_policy_id'] = changed['fixture_policy_id']
        with self.assertRaises(ValueError):
            model.validate(changed)

    def test_native_boundary_has_one_policy_identifier(self):
        text = (model.ROOT / 'docs/releases/v0.6.0-t0-native-contract.md').read_text(encoding='utf-8')
        self.assertEqual(text.count('uint32_t policy_id;'), 1)
        self.assertNotIn('uint32_t confirmation_policy_id;', text)
        for policy_id in (self.policy['production_policy_id'], self.policy['development_override_policy_id']):
            self.assertIn(f'0x{policy_id:08x}', text)

    def test_first_ready_does_not_confirm(self):
        self.assertEqual(model.decision(self.policy, 23000, None, self.snapshots(23000)),
                         'start_sustained_window')
        self.assertEqual(model.decision(self.policy, 25999, 23000, self.snapshots(25999)),
                         'wait_sustained_window')
        self.assertEqual(model.decision(self.policy, 26000, 23000, self.snapshots(26000)),
                         'eligible_for_durable_confirmation')

    def test_missing_init_is_unavailable_until_cutoff(self):
        for owner in self.policy['required_owners']:
            snapshots = self.snapshots(31000)
            snapshots[owner]['state'] = 'unavailable'
            self.assertEqual(model.decision(self.policy, 31000, None, snapshots), 'wait_readiness')
            self.assertEqual(model.decision(self.policy, 32000, None, snapshots),
                             'restore_health_timeout')

    def test_failures_and_lost_health_never_extend_deadline(self):
        for owner in self.policy['required_owners']:
            snapshots = self.snapshots(25000)
            snapshots[owner]['state'] = 'failed'
            self.assertEqual(model.decision(self.policy, 25000, None, snapshots), 'restore_health_failed')
            snapshots[owner]['state'] = 'unavailable'
            self.assertEqual(model.decision(self.policy, 25000, 23000, snapshots), 'restore_health_failed')

    def test_stalled_progress_rejected_even_with_latched_ready_state(self):
        for owner in self.policy['progress_owners']:
            snapshots = self.snapshots(26000)
            snapshots[owner]['observed_ms'] = 23000
            self.assertEqual(model.decision(self.policy, 26000, 23000, snapshots),
                             'restore_health_failed')
        # Configuration/storage success is latched: idle storage need not write
        # data or fabricate a heartbeat to remain a usable initialized store.
        snapshots = self.snapshots(26000)
        snapshots['storage']['observed_ms'] = 0
        self.assertEqual(model.decision(self.policy, 26000, 23000, snapshots),
                         'eligible_for_durable_confirmation')

    def test_commit_reserve_and_absolute_cutoff(self):
        self.assertEqual(model.decision(self.policy, 35000, 32000, self.snapshots(35000)),
                         'eligible_for_durable_confirmation')
        for now in (35500, 36000, 40000):
            self.assertEqual(model.decision(self.policy, now, 32000, self.snapshots(now)),
                             'restore_health_timeout')
        self.assertEqual(model.decision(self.policy, 32001, None, self.snapshots(32001)),
                         'restore_health_timeout')
        self.assertEqual(model.decision(self.policy, 35400, 32400, self.snapshots(35400)),
                         'restore_health_timeout')

    def test_future_missing_and_invalid_observations_fail_closed(self):
        for now in (-1, True, 1 << 64):
            with self.assertRaises(ValueError):
                model.decision(self.policy, now, None, self.snapshots())
        for field, invalid in (('observed_ms', 26001), ('generation', 0), ('state', 'unknown')):
            snapshots = self.snapshots(26000)
            snapshots['opcua'][field] = invalid
            with self.assertRaises(ValueError):
                model.decision(self.policy, 26000, 23000, snapshots)
        snapshots = self.snapshots()
        del snapshots['storage']
        with self.assertRaises(ValueError):
            model.decision(self.policy, 0, None, snapshots)

    def test_every_ready_owner_requires_a_valid_observation(self):
        for owner in self.policy['required_owners']:
            for field in ('observed_ms', 'generation'):
                for invalid in (None, True, False, -1, 0.5, '1', 1 << 64):
                    with self.subTest(owner=owner, field=field, invalid=invalid):
                        snapshots = self.snapshots(26000)
                        snapshots[owner][field] = invalid
                        with self.assertRaises(ValueError):
                            model.decision(self.policy, 26000, 23000, snapshots)
                snapshots = self.snapshots(26000)
                del snapshots[owner][field]
                with self.assertRaises(ValueError):
                    model.decision(self.policy, 26000, 23000, snapshots)
            snapshots = self.snapshots(26000)
            snapshots[owner]['observed_ms'] = 26001
            with self.assertRaises(ValueError):
                model.decision(self.policy, 26000, 23000, snapshots)
            snapshots[owner]['observed_ms'] = 23000
            snapshots[owner]['generation'] = 0
            with self.assertRaises(ValueError):
                model.decision(self.policy, 26000, 23000, snapshots)

    def test_no_external_network_or_console_readiness_owner(self):
        for forbidden in ('dhcp_lease', 'ethernet_link', 'external_client', 'usb_console'):
            changed = copy.deepcopy(self.policy)
            changed['required_owners'].append(forbidden)
            with self.assertRaises(ValueError):
                model.validate(changed)

    def test_cli_failure_is_nonzero_even_with_optimization(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'invalid.json'
            self.policy['bounds']['copy_retries_in_boot'] = 3
            path.write_text(json.dumps(self.policy), encoding='utf-8')
            result = subprocess.run([sys.executable, '-O', str(pathlib.Path(model.__file__)),
                                     '--policy', str(path)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('FAIL', result.stdout)


if __name__ == '__main__':
    unittest.main()
