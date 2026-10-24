#!/usr/bin/env python3
"""Assert the v0.5.2 OPC UA resource-diagnostic contract on a live device.

This is a manual, credentialed, live-device verifier.  It never changes
configuration or firmware state.  ``--exercise-capacity`` is deliberately
opt-in and opens at most two additional service sessions before closing them
again; it checks counter observation and cleanup, not a destructive
production-limit exhaustion.

Credentials belong in the environment, never in command history:
``OPCUA_{SERVICE,ADMIN,VIEWER,OPERATOR}_{USERNAME,PASSWORD}``.  For backwards
compatibility the service pair falls back to ``OPCUA_USERNAME`` and
``OPCUA_PASSWORD``.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import os
import platform
import sys
import time
from importlib import metadata
from argparse import Namespace
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent))
import stage_config as opcua_common  # noqa: E402


DIAGNOSTICS_NODE_ID = "ns=1;s=Device.Diagnostics"
OPCUA_ROOT_NODE_ID = "ns=1;s=Device.Diagnostics.OpcUa"
RESOURCE_GROUPS = (
    "Sessions",
    "SecureChannels",
    "Subscriptions",
    "MonitoredItems",
    "PublishRequests",
    "EventQueue",
)
RESOURCE_MEMBERS = ("Current", "Peak", "Capacity")
REQUIRED_GOOD_GROUPS = frozenset((
    "Sessions", "SecureChannels", "Subscriptions", "MonitoredItems",
    "PublishRequests", "EventQueue",
))
RESOURCE_COUNTERS = (
    "RejectedRequests",
    "RejectedRequestsWrapCount",
    "EventQueueDrops",
    "EventQueueDropsWrapCount",
)
REQUIRED_GOOD_COUNTERS = frozenset(RESOURCE_COUNTERS)
RESOURCE_NODE_IDS = tuple(
    f"ns=1;s=Device.Diagnostics.OpcUa.{group}.{member}"
    for group in RESOURCE_GROUPS
    for member in RESOURCE_MEMBERS
) + tuple(
    f"ns=1;s=Device.Diagnostics.OpcUa.{counter}"
    for counter in RESOURCE_COUNTERS
)
EXPECTED_ROOT_CHILDREN = frozenset((*RESOURCE_GROUPS, *RESOURCE_COUNTERS))
WRITE_PROBE_NODE_ID = "ns=1;s=Device.Diagnostics.OpcUa.Sessions.Capacity"

# The evidence is a bounded account of this live verifier, not a benchmark
# sample.  It deliberately records no endpoint authority, identity, path or
# exception text because any of those can contain credentials.
EVIDENCE_SCHEMA_VERSION = 1
EVIDENCE_MAX_BYTES = 64 * 1024
EVIDENCE_ROLE_LIMIT = 5


def _redacted_server(server_url: str) -> dict[str, Any]:
    """Describe transport configuration without retaining endpoint credentials."""
    parsed = urlsplit(server_url)
    try:
        port = parsed.port
    except ValueError:
        port = None
    return {
        "scheme": parsed.scheme or "unknown",
        "authority": "redacted",
        "port": port,
    }


def _package_version(package: str) -> str:
    try:
        return metadata.version(package)
    except metadata.PackageNotFoundError:
        return "unavailable"


def new_evidence(args: argparse.Namespace) -> dict[str, Any]:
    """Create the fixed, credential-free v0.5.2 live-verifier evidence shape."""
    return {
        "schemaVersion": EVIDENCE_SCHEMA_VERSION,
        "kind": "v052-opcua-resource-verification",
        "result": {"state": "not-run"},
        "profile": {
            "id": "v052-resource-diagnostics-live",
            "capacityExerciseEnabled": bool(args.exercise_capacity),
            "extraServiceClients": args.stress_extra_clients if args.exercise_capacity else 0,
            "snapshotSettleMs": int(args.snapshot_settle * 1000),
            "requestTimeoutMs": int(args.timeout * 1000),
        },
        "server": _redacted_server(args.server_url),
        "security": {
            "policy": args.security_policy,
            "mode": args.security_mode,
            "clientCertificateConfigured": bool(args.client_cert),
            "serverCertificateConfigured": bool(args.server_cert),
            "autoClientCertificateDisabled": bool(args.no_auto_client_cert),
        },
        "roles": [],
        "checks": {
            "browse": {"state": "not-run", "roleCount": 0},
            "directRead": {
                "state": "not-run",
                "nodeCountPerRole": len(RESOURCE_NODE_IDS),
                "roleCount": 0,
            },
            "write": {
                "state": "not-run",
                "nodeCountPerRole": 1,
                "roleCount": 0,
                "deniedRoleCount": 0,
                "probeNode": WRITE_PROBE_NODE_ID,
            },
            "capacity": {
                "state": "not-run",
                "extraServiceClients": 0,
            },
        },
        "runtime": {
            "tool": Path(__file__).name,
            "toolVersion": EVIDENCE_SCHEMA_VERSION,
            "python": platform.python_version(),
            "asyncua": _package_version("asyncua"),
        },
        "elapsedMonotonicMs": 0,
    }


def record_role(evidence: dict[str, Any] | None, role: str, *, privileged: bool,
                state: str, write_state: str | None = None,
                anonymousOutcome: str | None = None) -> None:
    """Record one of the five fixed authorization checks without unbounded data."""
    if evidence is None:
        return
    roles = evidence["roles"]
    if len(roles) >= EVIDENCE_ROLE_LIMIT:
        raise VerificationError("evidence role capacity exceeded")
    write_state = state if write_state is None else write_state
    entry: dict[str, Any] = {
        "role": role,
        "authorization": "privileged" if privileged else "unprivileged",
        "browse": state,
        "directRead": state,
        "write": write_state,
    }
    if anonymousOutcome is not None:
        entry["anonymousOutcome"] = anonymousOutcome
    roles.append(entry)
    for check_name in ("browse", "directRead"):
        check = evidence["checks"][check_name]
        check["roleCount"] = sum(
            item["browse" if check_name == "browse" else "directRead"] != "not-run"
            for item in roles)
        if state == "failed":
            check["state"] = "failed"
        elif state == "measured" and check["state"] != "failed":
            check["state"] = "measured"

    write = evidence["checks"]["write"]
    write["roleCount"] = sum(item["write"] != "not-run" for item in roles)
    write["deniedRoleCount"] = sum(item["write"] == "measured" for item in roles)
    if write_state == "failed":
        write["state"] = "failed"
    elif write["state"] != "failed" and write["deniedRoleCount"]:
        write["state"] = "measured"


def mark_capacity(evidence: dict[str, Any] | None, state: str,
                  extra_service_clients: int = 0) -> None:
    if evidence is not None:
        evidence["checks"]["capacity"] = {
            "state": state,
            "extraServiceClients": extra_service_clients,
        }


def finalize_evidence(evidence: dict[str, Any], *, state: str,
                      elapsed_ms: int, error: BaseException | None = None) -> None:
    """Finish a report without serializing exception text or credentials."""
    evidence["result"] = {"state": state}
    if error is not None:
        evidence["result"]["errorType"] = type(error).__name__
    evidence["elapsedMonotonicMs"] = max(0, int(elapsed_ms))


def format_cli_failure(error: BaseException) -> str:
    """Return a useful failure line without serializing third-party exception text."""
    detail = f": {error}" if isinstance(error, VerificationError) else ""
    return f"v0.5.2 OPC UA resource verification: FAIL: {type(error).__name__}{detail}"


def _evidence_payload(evidence: dict[str, Any]) -> bytes:
    payload = (json.dumps(evidence, sort_keys=True, indent=2, ensure_ascii=True,
                          allow_nan=False) + "\n").encode("utf-8")
    if len(payload) > EVIDENCE_MAX_BYTES:
        raise ValueError(
            f"evidence exceeds bounded output limit ({len(payload)} > {EVIDENCE_MAX_BYTES} bytes)")
    return payload


def write_evidence(path: Path, evidence: dict[str, Any]) -> None:
    """Atomically write a bounded deterministic report to an existing directory."""
    output = Path(path)
    if output.exists() and output.is_dir():
        raise ValueError("--output must name a file, not a directory")
    if not output.parent.is_dir():
        raise ValueError("--output parent directory does not exist")
    payload = _evidence_payload(evidence)
    temporary = output.with_name(f".{output.name}.tmp")
    try:
        temporary.write_bytes(payload)
        os.replace(temporary, output)
    finally:
        if temporary.exists():
            temporary.unlink()


class VerificationError(RuntimeError):
    """A server response violates the frozen v0.5.2 public contract."""


def status_name(status: Any) -> str:
    """Return a stable asyncua status spelling without depending on repr()."""
    name = getattr(status, "name", None)
    return str(name if name is not None else status)


def status_is(status: Any, expected: str) -> bool:
    return status_name(status) == expected or expected in str(status)


def status_is_good(status: Any) -> bool:
    predicate = getattr(status, "is_good", None)
    return bool(predicate()) if callable(predicate) else status_is(status, "Good")


def supported_status(status: Any) -> bool:
    return status_is_good(status) or status_is(status, "BadNotSupported")


def resource_member_node_id(group: str, member: str) -> str:
    if group not in RESOURCE_GROUPS or member not in RESOURCE_MEMBERS:
        raise ValueError("unknown v0.5.2 resource member")
    return f"ns=1;s=Device.Diagnostics.OpcUa.{group}.{member}"


def resource_counter_node_id(counter: str) -> str:
    if counter not in RESOURCE_COUNTERS:
        raise ValueError("unknown v0.5.2 resource counter")
    return f"ns=1;s=Device.Diagnostics.OpcUa.{counter}"


def require_u64(data_value: Any, node_id: str) -> int:
    """Validate a Good resource value without treating unavailable data as 0."""
    variant = getattr(data_value, "Value", None)
    value = getattr(variant, "Value", None)
    variant_type = getattr(variant, "VariantType", None)
    type_name = getattr(variant_type, "name", str(variant_type))
    if type_name != "UInt64":
        raise VerificationError(f"{node_id}: expected UInt64, got {type_name}")
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise VerificationError(f"{node_id}: expected non-negative UInt64 value, got {value!r}")
    return value


async def make_client(args: argparse.Namespace, username: str | None,
                      password: str | None, *, anonymous: bool = False):
    """Create a secure client for a role, without altering common arguments."""
    client = opcua_common.Client(
        url=args.server_url,
        timeout=args.timeout,
        watchdog_intervall=args.timeout,
    )
    # The shared helper only configures a secure channel when its Namespace has
    # a username.  For an anonymous identity use service solely as the helper's
    # secure-channel trigger; do not install that UserName token on the client.
    security_username = args.service_username if anonymous else username
    security_args = Namespace(**vars(args), username=security_username)
    try:
        await opcua_common.configure_client_security(client, security_args)
        if not anonymous:
            if not username:
                raise ValueError("authenticated role requires a username")
            client.set_user(username)
            client.set_password(password or "")
        await client.connect()
        return client
    except Exception:
        try:
            await client.disconnect()
        except Exception:  # noqa: BLE001 - original connection error is useful
            pass
        raise


async def browse_names(node: Any) -> set[str]:
    return {
        (await child.read_browse_name()).Name
        for child in await node.get_children()
    }


async def read_resource_data_values(client: Any) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for node_id in RESOURCE_NODE_IDS:
        result[node_id] = await client.get_node(node_id).read_data_value(
            raise_on_bad_status=False)
    return result


def validate_resource_values(values: dict[str, Any]) -> dict[str, int | None]:
    """Check quality/type/order for a privileged coherent resource snapshot."""
    if set(values) != set(RESOURCE_NODE_IDS):
        missing = sorted(set(RESOURCE_NODE_IDS).difference(values))
        unexpected = sorted(set(values).difference(RESOURCE_NODE_IDS))
        raise VerificationError(f"resource read set mismatch: missing={missing}, unexpected={unexpected}")

    decoded: dict[str, int | None] = {}
    for node_id, data_value in values.items():
        status = data_value.StatusCode
        if not supported_status(status):
            raise VerificationError(
                f"{node_id}: expected Good or BadNotSupported, got {status_name(status)}")
        if not status_is_good(status) and getattr(data_value.Value, "Value", None) is not None:
            raise VerificationError(f"{node_id}: unavailable metric carried a numeric value")
        decoded[node_id] = require_u64(data_value, node_id) if status_is_good(status) else None

    for group in REQUIRED_GOOD_GROUPS:
        for member in RESOURCE_MEMBERS:
            node_id = resource_member_node_id(group, member)
            if decoded[node_id] is None:
                raise VerificationError(f"{node_id}: required lifecycle gauge is unavailable")
    for counter in REQUIRED_GOOD_COUNTERS:
        node_id = resource_counter_node_id(counter)
        if decoded[node_id] is None:
            raise VerificationError(f"{node_id}: required resource counter is unavailable")

    for group in RESOURCE_GROUPS:
        current = decoded[resource_member_node_id(group, "Current")]
        peak = decoded[resource_member_node_id(group, "Peak")]
        capacity = decoded[resource_member_node_id(group, "Capacity")]
        if current is not None and peak is not None and capacity is not None:
            if not current <= peak or current > capacity or (group != "EventQueue" and peak > capacity):
                raise VerificationError(
                    f"{group}: invalid Current/Peak/Capacity: {current}/{peak}/{capacity}")
    return decoded


async def verify_privileged_role(client: Any, role: str) -> dict[str, int | None]:
    diagnostics_children = await browse_names(client.get_node(DIAGNOSTICS_NODE_ID))
    if "OpcUa" not in diagnostics_children:
        raise VerificationError(f"{role}: Device.Diagnostics does not expose OpcUa")

    root = client.get_node(OPCUA_ROOT_NODE_ID)
    root_children = await browse_names(root)
    if root_children != EXPECTED_ROOT_CHILDREN:
        raise VerificationError(
            f"{role}: resource root children mismatch: "
            f"expected {sorted(EXPECTED_ROOT_CHILDREN)}, got {sorted(root_children)}")
    for group in RESOURCE_GROUPS:
        children = await browse_names(client.get_node(
            f"ns=1;s=Device.Diagnostics.OpcUa.{group}"))
        if children != set(RESOURCE_MEMBERS):
            raise VerificationError(
                f"{role}: {group} children mismatch: expected {list(RESOURCE_MEMBERS)}, "
                f"got {sorted(children)}")

    decoded = validate_resource_values(await read_resource_data_values(client))
    good = sum(value is not None for value in decoded.values())
    unavailable = len(decoded) - good
    print(f"[ OK ] {role}: Browse/Read 22 resource nodes (Good={good}, BadNotSupported={unavailable})")
    return decoded


def access_denied_text(value: Any) -> bool:
    return status_is(value, "BadUserAccessDenied")


def read_only_write_denied_text(value: Any) -> bool:
    return (status_is(value, "BadNotWritable") or
            status_is(value, "BadUserAccessDenied"))


async def verify_read_only_write_denied(client: Any, role: str) -> None:
    """Probe one UInt64 diagnostic write and require rejection before mutation."""
    if opcua_common.ua is None:
        raise VerificationError("asyncua types were not loaded before write probe")
    try:
        await client.get_node(WRITE_PROBE_NODE_ID).write_value(
            opcua_common.ua.Variant(0, opcua_common.ua.VariantType.UInt64))
    except Exception as exc:
        if read_only_write_denied_text(exc):
            print(f"[ OK ] {role}: read-only diagnostic write denied")
            return
        raise VerificationError(f"{role}: read-only diagnostic write returned unexpected status") from exc
    raise VerificationError(f"{role}: read-only diagnostic write unexpectedly succeeded")


async def verify_unprivileged_role(client: Any, role: str) -> None:
    """Require hidden Browse references and denied direct Read for every node."""
    diagnostics_children = await browse_names(client.get_node(DIAGNOSTICS_NODE_ID))
    if "OpcUa" in diagnostics_children:
        raise VerificationError(f"{role}: OpcUa diagnostics folder was visible in Browse")

    try:
        root_children = await browse_names(client.get_node(OPCUA_ROOT_NODE_ID))
    except Exception as exc:  # direct Browse denial is valid and preferred
        if not access_denied_text(exc):
            raise VerificationError(
                f"{role}: direct Browse returned an unexpected {type(exc).__name__} status"
            ) from exc
    else:
        if root_children:
            raise VerificationError(f"{role}: direct Browse exposed {sorted(root_children)}")

    failures: list[str] = []
    for node_id in RESOURCE_NODE_IDS:
        try:
            value = await client.get_node(node_id).read_data_value(raise_on_bad_status=False)
        except Exception as exc:  # a service-level access denial is equivalent
            if not access_denied_text(exc):
                failures.append(f"{node_id}: unexpected {type(exc).__name__} status")
            continue
        if not access_denied_text(value.StatusCode):
            failures.append(f"{node_id}: got {status_name(value.StatusCode)}")
    if failures:
        raise VerificationError(
            f"{role}: direct reads must return BadUserAccessDenied: " + "; ".join(failures))
    print(f"[ OK ] {role}: Browse hidden and 22 direct reads denied")


async def verify_anonymous(args: argparse.Namespace) -> str:
    """Verify anonymous denial, including servers that reject activation first."""
    try:
        client = await make_client(args, None, None, anonymous=True)
    except Exception as exc:
        # This implementation may reject an anonymous UserIdentityToken during
        # activation.  That is stricter than a direct read denial and is a
        # valid anonymous access outcome; retain the exact evidence in output.
        print(f"[ OK ] anonymous: session activation denied ({type(exc).__name__})")
        return "activation-denied"
    try:
        await verify_unprivileged_role(client, "anonymous")
        await verify_read_only_write_denied(client, "anonymous")
        return "direct-read-and-write-denied"
    finally:
        await client.disconnect()


async def verify_role(args: argparse.Namespace, role: str,
                      username: str, password: str, privileged: bool) -> None:
    client = await make_client(args, username, password)
    try:
        if privileged:
            await verify_privileged_role(client, role)
        else:
            await verify_unprivileged_role(client, role)
        await verify_read_only_write_denied(client, role)
    finally:
        await client.disconnect()


async def exercise_capacity(args: argparse.Namespace, baseline: dict[str, int | None]) -> None:
    """Bounded multi-client observation with deterministic cleanup."""
    if args.stress_extra_clients < 1 or args.stress_extra_clients > 2:
        raise ValueError("--stress-extra-clients must be in 1..2")
    primary = await make_client(args, args.service_username, args.service_password)
    extras: list[Any] = []
    loaded: dict[str, int | None] | None = None
    try:
        for _ in range(args.stress_extra_clients):
            extras.append(await make_client(args, args.service_username, args.service_password))
        await asyncio.sleep(args.snapshot_settle)
        loaded = validate_resource_values(await read_resource_data_values(primary))
        for group in ("Sessions", "SecureChannels"):
            before = baseline[resource_member_node_id(group, "Current")]
            during = loaded[resource_member_node_id(group, "Current")]
            if before is not None and during is not None and during < before + args.stress_extra_clients:
                raise VerificationError(
                    f"{group}: expected at least {args.stress_extra_clients} extra client(s), "
                    f"baseline={before}, during={during}")
        print(f"[ OK ] capacity exercise: {args.stress_extra_clients} additional service clients observed")
    finally:
        for client in reversed(extras):
            await client.disconnect()
        await asyncio.sleep(args.snapshot_settle)
        try:
            if loaded is not None:
                cleaned = validate_resource_values(await read_resource_data_values(primary))
                for group in ("Sessions", "SecureChannels"):
                    during = loaded[resource_member_node_id(group, "Current")]
                    after = cleaned[resource_member_node_id(group, "Current")]
                    if during is not None and after is not None and after >= during:
                        raise VerificationError(
                            f"{group}: client cleanup did not reduce Current ({during} -> {after})")
                print("[ OK ] capacity exercise: extra client sessions cleaned up")
        finally:
            await primary.disconnect()


def required_role_credentials(args: argparse.Namespace) -> tuple[tuple[str, str, str], ...]:
    roles = (
        ("service", args.service_username, args.service_password),
        ("admin", args.admin_username, args.admin_password),
        ("viewer", args.viewer_username, args.viewer_password),
        ("operator", args.operator_username, args.operator_password),
    )
    missing = [role for role, username, password in roles if not username or password is None]
    if missing:
        raise ValueError(
            "missing credentials for " + ", ".join(missing) +
            "; set OPCUA_<ROLE>_USERNAME and OPCUA_<ROLE>_PASSWORD")
    return roles


async def run(args: argparse.Namespace, evidence: dict[str, Any] | None = None) -> int:
    opcua_common.load_asyncua()
    roles = required_role_credentials(args)
    for role, username, password in roles[:2]:
        try:
            await verify_role(args, role, username, password, privileged=True)
        except Exception:
            record_role(evidence, role, privileged=True, state="failed")
            raise
        record_role(evidence, role, privileged=True, state="measured")
    for role, username, password in roles[2:]:
        try:
            await verify_role(args, role, username, password, privileged=False)
        except Exception:
            record_role(evidence, role, privileged=False, state="failed")
            raise
        record_role(evidence, role, privileged=False, state="measured")
    try:
        anonymous_outcome = await verify_anonymous(args)
    except Exception:
        record_role(evidence, "anonymous", privileged=False, state="failed")
        raise
    if anonymous_outcome == "activation-denied":
        record_role(evidence, "anonymous", privileged=False, state="not-run",
                    write_state="not-run", anonymousOutcome=anonymous_outcome)
    else:
        record_role(evidence, "anonymous", privileged=False, state="measured",
                    anonymousOutcome=anonymous_outcome)

    if args.exercise_capacity:
        try:
            client = await make_client(args, args.service_username, args.service_password)
            try:
                baseline = await verify_privileged_role(client, "service baseline")
            finally:
                await client.disconnect()
            await exercise_capacity(args, baseline)
        except Exception:
            mark_capacity(evidence, "failed", args.stress_extra_clients)
            raise
        mark_capacity(evidence, "measured", args.stress_extra_clients)
    return 0


async def execute(args: argparse.Namespace, verifier=run) -> int:
    """Run the live verifier and persist requested failure evidence before exit."""
    evidence = new_evidence(args)
    started_ns = time.monotonic_ns()
    exit_code = 1
    error: BaseException | None = None
    try:
        exit_code = await verifier(args, evidence)
        if exit_code == 0:
            finalize_evidence(evidence, state="passed",
                              elapsed_ms=(time.monotonic_ns() - started_ns) // 1_000_000)
        else:
            finalize_evidence(evidence, state="failed",
                              elapsed_ms=(time.monotonic_ns() - started_ns) // 1_000_000)
    except BaseException as exc:  # caller maps KeyboardInterrupt to its defined exit status
        error = exc
        exit_code = 130 if isinstance(exc, KeyboardInterrupt) else 1
        finalize_evidence(evidence, state="failed",
                          elapsed_ms=(time.monotonic_ns() - started_ns) // 1_000_000,
                          error=exc)
    if args.output is not None:
        try:
            write_evidence(args.output, evidence)
        except Exception as output_error:  # evidence persistence is required once requested
            print(f"v0.5.2 OPC UA resource evidence: FAIL: {type(output_error).__name__}: {output_error}")
            return 1
    if error is not None:
        raise error
    if exit_code == 0:
        print("v0.5.2 OPC UA resource verification: PASS")
    return exit_code


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server_url", help="secure OPC UA endpoint")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--client-cert")
    parser.add_argument("--client-key")
    parser.add_argument("--server-cert")
    parser.add_argument("--security-policy", default="Basic256Sha256", choices=["Basic256Sha256"])
    parser.add_argument("--security-mode", default="SignAndEncrypt", choices=["Sign", "SignAndEncrypt"])
    parser.add_argument("--no-auto-client-cert", action="store_true")
    parser.add_argument("--service-username", default=os.getenv("OPCUA_SERVICE_USERNAME", os.getenv("OPCUA_USERNAME")))
    parser.add_argument("--service-password", default=os.getenv("OPCUA_SERVICE_PASSWORD", os.getenv("OPCUA_PASSWORD")))
    parser.add_argument("--admin-username", default=os.getenv("OPCUA_ADMIN_USERNAME"))
    parser.add_argument("--admin-password", default=os.getenv("OPCUA_ADMIN_PASSWORD"))
    parser.add_argument("--viewer-username", default=os.getenv("OPCUA_VIEWER_USERNAME"))
    parser.add_argument("--viewer-password", default=os.getenv("OPCUA_VIEWER_PASSWORD"))
    parser.add_argument("--operator-username", default=os.getenv("OPCUA_OPERATOR_USERNAME"))
    parser.add_argument("--operator-password", default=os.getenv("OPCUA_OPERATOR_PASSWORD"))
    parser.add_argument("--exercise-capacity", action="store_true",
                        help="open and clean up a bounded multi-client session exercise")
    parser.add_argument("--output", type=Path,
                        help="write bounded credential-free JSON evidence after the run")
    parser.add_argument("--stress-extra-clients", type=int, default=2,
                        help="additional service sessions when exercising capacity (1..2)")
    parser.add_argument("--snapshot-settle", type=float, default=1.2,
                        help="seconds to wait for the one-second coherent snapshot (>= 1.0)")
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    if not math.isfinite(args.snapshot_settle) or args.snapshot_settle < 1.0:
        parser.error("--snapshot-settle must be at least one snapshot cadence (1.0 second)")
    if args.stress_extra_clients < 1 or args.stress_extra_clients > 2:
        parser.error("--stress-extra-clients must be in 1..2")
    return args


def main() -> int:
    try:
        return asyncio.run(execute(parse_args()))
    except KeyboardInterrupt:
        return 130
    except Exception as exc:  # noqa: BLE001 - CLI must turn assertion failures into nonzero exits
        print(format_cli_failure(exc))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
