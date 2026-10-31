#!/usr/bin/env python3
"""Executable T0 budget/decision model. Never used by production firmware."""
from __future__ import annotations

import argparse
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
POLICY = ROOT / 'docs/releases/v0.6.0-t0-policy.json'
U64_MAX = (1 << 64) - 1


def require(value: bool, message: str) -> None:
    if not value:
        raise ValueError(message)


def integer(value: object, name: str) -> int:
    require(type(value) is int and 0 <= value <= U64_MAX, f'invalid integer: {name}')
    return value


def validate(policy: dict) -> dict:
    require(type(policy['schema']) is int and policy['schema'] == 1, 'unknown policy schema')
    t, b, r, o = (policy[name] for name in
                   ('timing_ms', 'bounds', 'resource_limits_bytes', 'observations'))
    for section in (t, b, r, o):
        for name, value in section.items():
            integer(value, name)
    for name in ('production_policy_id', 'development_override_policy_id', 'fixture_policy_id'):
        require(0 < integer(policy[name], name) <= 0xffffffff, 'invalid policy ID')
    require(len({policy[name] for name in ('production_policy_id',
                'development_override_policy_id', 'fixture_policy_id')}) == 3, 'policy ID collision')
    require(policy['candidate_attempts'] == 1 and b['copy_retries_in_boot'] == 0,
            'unexpected automatic retry policy')
    require(b['copy_passes_per_transaction'] == 3 and b['image_verifications_per_transaction'] == 8,
            'recovery work accounting changed')
    require(b['trusted_keys'] == 2 and b['manifest_bytes'] == 320 and
            b['transfer_chunk_bytes'] == 4096 and b['status_json_bytes'] == 512,
            'fixed transport/key capacity changed')
    require(t['logical_confirmation_cutoff'] == t['readiness_cutoff'] +
            t['sustained_ready'] + t['commit_reserve'], 'confirmation cutoff arithmetic')
    require(0 < t['commit_operation_budget'] < t['commit_reserve'], 'no commit margin')
    require(0 < t['sample_period'] < t['progress_freshness'] < t['watchdog_period'],
            'invalid sample/freshness/watchdog ordering')
    require(t['progress_freshness'] < t['sustained_ready'], 'sustained window too short')
    require(0 < t['handoff_adoption_budget'] < t['watchdog_period'], 'handoff gap unbounded')
    require(t['nominal_hardware_reset_ceiling'] == t['logical_confirmation_cutoff'] +
            t['watchdog_period'], 'reset ceiling arithmetic')
    require(max(o['startup_with_dhcp_us'], o['startup_without_ipv4_us']) +
            1000 * t['handoff_adoption_budget'] < 1000 * t['readiness_cutoff'],
            'startup observation exceeds selected readiness budget')
    require(b['payload_bytes'] == 0x507000 and b['sector_bytes'] == 4096 and
            b['manifest_sectors'] == 1, 'layout contract drift')
    require(b['payload_bytes'] % b['sector_bytes'] == 0, 'unaligned payload')
    sectors = b['payload_bytes'] // b['sector_bytes'] + b['manifest_sectors']
    observed_step_us = o['programmed_erase_and_verify_us'] + o['program_16_pages_us'] + o['readback_us']
    require(observed_step_us < 1000 * t['sector_copy_step_budget'], 'no Flash step margin')
    # Integer arithmetic: projection is not a measured full-image maximum.
    projected_hash_us = (o['cpu_copy_sha256_us'] * b['payload_bytes'] +
                         o['hashed_image_bytes'] - 1) // o['hashed_image_bytes']
    require(projected_hash_us + o['ecdsa_positive_us'] <
            1000 * t['image_verification_budget'], 'no verification margin')
    require(0 < t['crypto_nonpreemptible_budget'] < t['watchdog_period'] and
            o['ecdsa_positive_us'] < 1000 * t['crypto_nonpreemptible_budget'], 'crypto step budget')
    copy_pass_ms = sectors * t['sector_copy_step_budget']
    loader_ms = (copy_pass_ms * b['copy_passes_per_transaction'] +
                 t['image_verification_budget'] * b['image_verifications_per_transaction'] +
                 t['metadata_total_budget'])
    require(loader_ms <= t['loader_transaction_budget'], 'loader transaction exceeds budget')
    require(t['loader_transaction_budget'] + t['nominal_hardware_reset_ceiling'] <=
            t['transaction_including_failed_candidate_budget'], 'transaction exceeds budget')
    require(o['primitive_link_bootloader_bin_bytes'] < r['bootloader_flash'] == 262144,
            'primitive link does not fit loader reservation')
    require(r['crypto_arena'] >= 65536 and r['copy_buffer'] == b['sector_bytes'], 'scratch budget')
    require(r['bootloader_stack'] > o['ram_probe_msp_extent_bytes'], 'no isolated stack margin')
    require(r['crypto_arena'] + r['copy_buffer'] + 2048 < r['bootloader_static_data'],
            'loader data budget cannot contain known buffers')
    require(r['bootloader_static_data'] + r['bootloader_stack'] <= o['rp2350_sram_bytes'],
            'loader SRAM reservation does not fit')
    require(max(o['reference_static_sram_bytes'], o['industrial_static_sram_bytes']) +
            r['application_additional_static'] <= o['rp2350_sram_bytes'], 'static SRAM does not fit')
    require(policy['required_owners'] == ['scheduler', 'configuration', 'storage', 'network_stack', 'opcua'],
            'health owner contract drift')
    require(policy['progress_owners'] == ['network_stack', 'opcua'], 'progress owner contract drift')
    require(policy['readiness_excludes'] == ['ethernet_link', 'dhcp_lease', 'external_client', 'ntp', 'usb_console'],
            'external dependency entered health policy')
    require(policy['production_requires_explicit_board_revision'] is True, 'implicit revision forbidden')
    require(policy['reference_board_revision'] is None, 'unverified board revision introduced')
    require(policy['integrated_timing_validated'] is False and
            policy['integrated_resource_validated'] is False, 'model cannot certify integrated evidence')
    status_bytes = status_json_upper_bound(policy)
    require(status_bytes <= b['status_json_bytes'], 'legacy plus signed JSON fields exceed capacity')
    return {'status': 'pass', 'scope': 'T0 design model only', 'sectors_per_copy': sectors,
            'copy_pass_budget_ms': copy_pass_ms, 'loader_arithmetic_ms': loader_ms,
            'projected_hash_us': projected_hash_us, 'observed_sector_step_us': observed_step_us,
            'status_json_upper_bound_including_nul': status_bytes,
            'integrated_timing_validated': False, 'integrated_resource_validated': False}


def status_json_upper_bound(policy: dict) -> int:
    """Conservative long-form design shape; T4 must test the actual C emitter."""
    n = policy['bounds']['payload_bytes']
    # Even mutually exclusive counters are all maximized here. Null progress
    # is longer than 100; unavailable progress must never be fabricated as zero.
    status = {'sessionActive': False, 'bytesWritten': n, 'expectedImageSize': n,
              'remainingBytes': n, 'expectedImageCrc32': '0xffffffff',
              'versionNumber': 0xffffffff, 'lastResult': 'INVALID_ARGUMENT',
              'maxChunkBytes': 4096, 'otaStateValid': True,
              'otaState': 'COPYING_TO_BANK_A', 'otaImageSize': n,
              'otaImageCrc32': '0xffffffff', 'otaStagedOffset': '0xffffffff',
              'otaStagedSize': '0xffffffff', 'otaBootAttempts': 1,
              'otaLastError': 'INVALID_STAGED_IMAGE', 'phase': 5, 'verify': 15,
              'attemptUsed': False, 'rollback': 15, 'progress': None}
    return len(json.dumps(status, separators=(',', ':')).encode('utf-8')) + 1


def decision(policy: dict, elapsed_ms: int, ready_since_ms: int | None,
             snapshots: dict[str, dict]) -> str:
    """Evaluate an immutable copied snapshot; never persist or feed hardware."""
    t = policy['timing_ms']
    integer(elapsed_ms, 'elapsed')
    require(set(snapshots) == set(policy['required_owners']), 'missing/extra owner')
    if ready_since_ms is not None:
        integer(ready_since_ms, 'ready_since')
        require(ready_since_ms <= elapsed_ms, 'future readiness')
        if ready_since_ms > t['readiness_cutoff']:
            return 'restore_health_timeout'
    ready = True
    for owner, snapshot in snapshots.items():
        require(type(snapshot) is dict and 'state' in snapshot, 'missing owner state')
        require(snapshot['state'] in ('ok', 'unavailable', 'failed'), 'invalid owner state')
        if snapshot['state'] == 'failed':
            return 'restore_health_failed'
        if snapshot['state'] != 'ok':
            ready = False
        if snapshot['state'] == 'ok':
            require('observed_ms' in snapshot and 'generation' in snapshot,
                    'missing ready owner observation')
            observed = integer(snapshot['observed_ms'], 'observed_ms')
            require(observed <= elapsed_ms, 'future owner observation')
            integer(snapshot['generation'], 'generation')
            require(snapshot['generation'] > 0, 'missing progress generation')
            if owner in policy['progress_owners'] and elapsed_ms - observed > t['progress_freshness']:
                if ready_since_ms is not None:
                    return 'restore_health_failed'
                ready = False
    if elapsed_ms >= t['logical_confirmation_cutoff']:
        return 'restore_health_timeout'
    if not ready:
        if ready_since_ms is not None:
            return 'restore_health_failed'
        return 'restore_health_timeout' if elapsed_ms >= t['readiness_cutoff'] else 'wait_readiness'
    if ready_since_ms is None:
        return 'restore_health_timeout' if elapsed_ms > t['readiness_cutoff'] else 'start_sustained_window'
    if elapsed_ms - ready_since_ms < t['sustained_ready']:
        return 'wait_sustained_window'
    if elapsed_ms + t['commit_operation_budget'] >= t['logical_confirmation_cutoff']:
        return 'restore_health_timeout'
    return 'eligible_for_durable_confirmation'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--policy', type=pathlib.Path, default=POLICY)
    parser.add_argument('--output', type=pathlib.Path)
    args = parser.parse_args()
    try:
        require(args.policy.stat().st_size <= 16384, 'policy exceeds bound')
        result = validate(json.loads(args.policy.read_text(encoding='utf-8')))
        encoded = json.dumps(result, indent=2) + '\n'
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(encoded, encoding='utf-8')
        print(encoded, end='')
    except (ValueError, KeyError, TypeError, OSError, ZeroDivisionError) as error:
        print(f'OTA boot policy design: FAIL: {error}')
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
