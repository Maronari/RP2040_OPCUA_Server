#!/usr/bin/env python3
"""Post-flash check that the running configuration's channels actually activated.

Manual, credentials-required, live-hardware-only tool -- not part of
run_host_tests.py. Browses the Channels folder in the running address
space and reads every channel's Value, reporting the StatusCode each one
comes back with.

This is the regression surface any change to driver_catalog's resource
claims or to config validation touches: those run for every channel on
every boot and every config apply, so a defect there shows up as a device
that came up on an empty configuration, or with channels present but not
producing Good readings. An anonymous session cannot see this -- the
server denies session activation without credentials -- so this check
cannot be automated without them.

Never pass credentials on the command line where they would land in shell
history; use OPCUA_USERNAME/OPCUA_PASSWORD instead.
"""

from __future__ import annotations

import argparse
import asyncio
import os
import sys
from argparse import Namespace
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import stage_config as opcua_common  # noqa: E402


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server_url")
    parser.add_argument("--channels-node-id", default="ns=1;s=Device.Channels")
    parser.add_argument("--expect-min", type=int, default=1,
                        help="Fail if fewer than this many channels are present")
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
        print("channel-activation: FAIL: set OPCUA_USERNAME and OPCUA_PASSWORD in your own shell first")
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

    good = 0
    bad = 0
    async with client:
        channels = await client.get_node(args.channels_node_id).get_children()
        print(f"channels in the running model: {len(channels)}")
        for channel in channels:
            name = (await channel.read_browse_name()).Name
            value_node = None
            for member in await channel.get_children():
                if (await member.read_browse_name()).Name == "Value":
                    value_node = member
                    break
            if value_node is None:
                print(f"  {name:<28} (no Value node)")
                continue
            data_value = await value_node.read_data_value()
            status = data_value.StatusCode
            if status.is_good():
                good += 1
            else:
                bad += 1
            print(f"  {name:<28} value={data_value.Value.Value!r:<22} status={status.name}")

    print()
    if len(channels) < args.expect_min:
        print(f"channel-activation: FAIL: only {len(channels)} channels present, expected at least {args.expect_min}")
        return 1
    if bad:
        print(f"channel-activation: FAIL: {bad} channel(s) not reporting Good, {good} good")
        return 1
    print(f"channel-activation: PASS ({good}/{good} channels activated and reading Good)")
    return 0


def main() -> int:
    args = parse_args()
    return asyncio.run(run(args))


if __name__ == "__main__":
    raise SystemExit(main())
