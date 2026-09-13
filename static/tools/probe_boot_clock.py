#!/usr/bin/env python3
"""Observe the OPC UA server's clock from power-up, and optionally login backoff before SNTP.

Manual, live-hardware-only tool for the cold-boot window between "network
ready" (OpcUaTask accepts connections) and the first SNTP sync. Run it, then
power-cycle the board (a soft reset keeps the AON timer running and hides the
window).

Clock probe, no credentials: every --interval seconds it opens a
SecurityPolicy None SecureChannel and records the server's SecurityToken
CreatedAt, taken from UA_DateTime_now(). Each sample is classified:

- ``unreachable`` -- no TCP connection yet (booting);
- ``null``        -- 1601-01-01, the OPC UA null DateTime: server up, wall
                     clock not yet set (the documented pre-SNTP behavior);
- ``boot-relative`` -- 1970 + uptime: the port hook to time_service is
                     missing again (the regression fixed in ddc63c5);
- ``utc``         -- a real time; the skew against this PC is reported.

Login backoff check, optional (--login-check): uses OPCUA_USERNAME/
OPCUA_PASSWORD from the environment. As soon as the server answers with a
``null`` clock, it logs in once with a deliberately wrong password, waits
--retry-after seconds (default 1.5 s, beyond the 1 s first lockout), then logs
in with the right one, which must succeed. Firmware that timed lockouts on the
wall clock rejects the second login for as long as SNTP has not synced. The
check is only meaningful while the clock is still ``null``; it reports
INCONCLUSIVE otherwise. Blocking NTP (for example an unreachable NtpServer)
keeps that window open indefinitely.
"""

from __future__ import annotations

import argparse
import asyncio
import datetime
import json
import logging
import os
import sys
import time
from argparse import Namespace
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

NULL_DATETIME_YEAR = 1601
BOOT_RELATIVE_LIMIT = datetime.datetime(2000, 1, 1, tzinfo=datetime.timezone.utc)


def classify(created_at: datetime.datetime | None, local_now: datetime.datetime) -> tuple[str, float | None]:
    """Classify one token CreatedAt; returns (class, skew seconds for utc)."""
    if created_at is None:
        return "unreachable", None
    if created_at.tzinfo is None:
        created_at = created_at.replace(tzinfo=datetime.timezone.utc)
    if created_at.year <= NULL_DATETIME_YEAR:
        return "null", None
    if created_at < BOOT_RELATIVE_LIMIT:
        return "boot-relative", None
    return "utc", (created_at - local_now).total_seconds()


async def token_created_at(url: str, timeout: float):
    from asyncua import Client

    client = Client(url, timeout=timeout)
    await client.connect_socket()
    try:
        await client.send_hello()
        await client.open_secure_channel()
        return client.uaclient.protocol._connection.security_token.CreatedAt
    finally:
        try:
            await client.close_secure_channel()
        except Exception:  # noqa: BLE001 - best-effort teardown of a probe channel
            pass
        client.disconnect_socket()


async def try_login(url: str, username: str, password: str, timeout: float) -> str:
    import stage_config as opcua_common

    opcua_common.load_asyncua()
    client = opcua_common.Client(url=url, timeout=timeout)
    await opcua_common.configure_client_security(client, Namespace(
        username=username, client_cert=None, client_key=None, server_cert=None,
        no_auto_client_cert=False, security_policy="Basic256Sha256", security_mode="SignAndEncrypt"))
    client.set_user(username)
    client.set_password(password)
    try:
        async with client:
            return "accepted"
    except Exception as exc:  # noqa: BLE001 - the status is the result being measured
        return f"rejected ({type(exc).__name__}: {exc})"


async def run(args: argparse.Namespace) -> int:
    logging.disable(logging.CRITICAL)
    samples = []
    login_result = None
    start = time.monotonic()
    utc_seen = 0
    username = os.getenv("OPCUA_USERNAME")
    password = os.getenv("OPCUA_PASSWORD")
    if args.login_check and (not username or not password):
        print("boot-clock: FAIL: --login-check needs OPCUA_USERNAME and OPCUA_PASSWORD in your own shell")
        return 2

    while time.monotonic() - start < args.duration:
        elapsed = time.monotonic() - start
        now = datetime.datetime.now(datetime.timezone.utc)
        try:
            created = await asyncio.wait_for(token_created_at(args.server_url, args.timeout), args.timeout + 1.0)
        except Exception:  # noqa: BLE001 - unreachable while the board boots
            created = None
        kind, skew = classify(created, now)
        samples.append({"t": round(elapsed, 1), "class": kind, "createdAt": created.isoformat() if created else None,
                        "skewSeconds": None if skew is None else round(skew, 3)})
        print(f"t+{elapsed:6.1f}s {kind:<13} createdAt={created}" + ("" if skew is None else f" skew={skew:+.2f}s"))

        if args.login_check and login_result is None and kind in ("null", "utc"):
            if kind == "utc":
                login_result = {"verdict": "INCONCLUSIVE", "reason": "clock already synchronized at first contact"}
            else:
                wrong = await try_login(args.server_url, username, password + "-wrong", args.timeout)
                await asyncio.sleep(args.retry_after)
                right = await try_login(args.server_url, username, password, args.timeout)
                after = await token_created_at(args.server_url, args.timeout)
                still_null = classify(after, datetime.datetime.now(datetime.timezone.utc))[0] == "null"
                verdict = "PASS" if right == "accepted" else "FAIL"
                if not still_null and verdict == "PASS":
                    verdict = "INCONCLUSIVE"
                login_result = {"verdict": verdict, "wrongPassword": wrong, "rightPasswordAfterSeconds": args.retry_after,
                                "rightPassword": right, "clockStillNull": still_null}
                print(f"login check: wrong -> {wrong}; after {args.retry_after}s right -> {right}; "
                      f"clock still null: {still_null}; {verdict}")

        if kind == "boot-relative":
            break
        if kind == "utc":
            utc_seen += 1
            if utc_seen >= args.utc_samples and (not args.login_check or login_result is not None):
                break
        await asyncio.sleep(args.interval)

    classes = [s["class"] for s in samples]
    first_utc = next((s["t"] for s in samples if s["class"] == "utc"), None)
    first_reachable = next((s["t"] for s in samples if s["class"] != "unreachable"), None)
    summary = {
        "server": args.server_url,
        "firstReachableSeconds": first_reachable,
        "firstUtcSeconds": first_utc,
        "nullWindowSamples": classes.count("null"),
        "bootRelativeSeen": "boot-relative" in classes,
        "login": login_result,
        "samples": samples,
    }
    args.evidence.parent.mkdir(parents=True, exist_ok=True)
    args.evidence.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"first reachable: {first_reachable}s, first UTC: {first_utc}s, null samples: {classes.count('null')}")
    print(f"evidence: {args.evidence}")

    failed = summary["bootRelativeSeen"] or (login_result is not None and login_result["verdict"] == "FAIL")
    if failed:
        print("boot-clock: FAIL")
        return 1
    print("boot-clock: PASS" if first_utc is not None else "boot-clock: NO UTC SEEN (NTP blocked or duration too short)")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("server_url")
    parser.add_argument("--duration", type=float, default=300.0, help="Give up after this many seconds")
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--timeout", type=float, default=4.0)
    parser.add_argument("--utc-samples", type=int, default=3, help="Stop after this many UTC samples")
    parser.add_argument("--login-check", action="store_true")
    parser.add_argument("--retry-after", type=float, default=1.5)
    parser.add_argument("--evidence", type=Path, default=Path("build/boot-clock.json"))
    return parser.parse_args()


def main() -> int:
    return asyncio.run(run(parse_args()))


if __name__ == "__main__":
    sys.exit(main())
