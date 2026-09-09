#!/usr/bin/env python3
"""Bounded hardware check that OPC UA HistoryRead returns real data for a channel.

Manual, credentials-required, live-hardware-only tool -- not part of
run_host_tests.py. Written for MEM-02 (docs/releases/v0.5.1-memory-experiment.md):
history_service_record_channel_sample() used to permanently mark a
channel's ring FLASH_APPEND_FAILED on a single transient Flash-append
failure, with nothing ever clearing it back to READY, so HistoryRead
failed closed forever afterward. Never pass credentials on the command
line where they would land in shell history; use OPCUA_USERNAME/
OPCUA_PASSWORD instead.
"""

from __future__ import annotations

import argparse
import asyncio
import datetime
import os
import sys
from argparse import Namespace
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import stage_config as opcua_common  # noqa: E402


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server_url")
    parser.add_argument("--node-id", default="ns=1;s=Device.Channels.Temperature1.Value")
    parser.add_argument("--calls", type=int, default=3)
    parser.add_argument("--lookback-hours", type=float, default=24.0)
    parser.add_argument("--num-values", type=int, default=128)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--security-policy", default="Basic256Sha256", choices=["Basic256Sha256"])
    parser.add_argument("--security-mode", default="SignAndEncrypt", choices=["Sign", "SignAndEncrypt"])
    parser.add_argument("--client-cert")
    parser.add_argument("--client-key")
    parser.add_argument("--server-cert")
    parser.add_argument("--no-auto-client-cert", action="store_true")
    return parser.parse_args()


async def run(args: argparse.Namespace) -> int:
    username = os.getenv("OPCUA_USERNAME")
    password = os.getenv("OPCUA_PASSWORD")
    if not username or not password:
        print("history-read: FAIL: set OPCUA_USERNAME and OPCUA_PASSWORD in your own shell first")
        return 2
    if args.calls < 1:
        print("history-read: FAIL: --calls must be >= 1")
        return 2

    opcua_common.load_asyncua()
    client = opcua_common.Client(url=args.server_url, timeout=args.timeout)
    security_args = Namespace(
        username=username,
        client_cert=args.client_cert,
        client_key=args.client_key,
        server_cert=args.server_cert,
        no_auto_client_cert=args.no_auto_client_cert,
        security_policy=args.security_policy,
        security_mode=args.security_mode,
    )
    await opcua_common.configure_client_security(client, security_args)
    client.set_user(username)
    client.set_password(password)

    failures = 0
    async with client:
        node = client.get_node(args.node_id)
        now = datetime.datetime.now(datetime.timezone.utc)
        start = now - datetime.timedelta(hours=args.lookback_hours)
        end = now + datetime.timedelta(seconds=1)
        for attempt in range(1, args.calls + 1):
            try:
                history = await node.read_raw_history(
                    starttime=start, endtime=end, numvalues=args.num_values
                )
                print(f"call {attempt}: GOOD, {len(history)} samples returned")
            except Exception as exc:  # noqa: BLE001 - report every outcome, not just success
                failures += 1
                print(f"call {attempt}: FAILED -- {type(exc).__name__}: {exc}")

    if failures:
        print(f"history-read: FAIL: {failures}/{args.calls} calls did not return GOOD")
        return 1
    print(f"history-read: PASS ({args.calls}/{args.calls} calls returned GOOD)")
    return 0


def main() -> int:
    args = parse_args()
    return asyncio.run(run(args))


if __name__ == "__main__":
    raise SystemExit(main())
