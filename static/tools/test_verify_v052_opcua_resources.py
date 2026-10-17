#!/usr/bin/env python3
"""Offline contract tests for verify_v052_opcua_resources.py."""

import asyncio
import contextlib
import io
import json
import tempfile
from pathlib import Path
from types import SimpleNamespace

import verify_v052_opcua_resources as subject


class Status:
    def __init__(self, name):
        self.name = name

    def is_good(self):
        return self.name == "Good"


def data_value(value, status="Good", variant_type="UInt64"):
    return SimpleNamespace(
        StatusCode=Status(status),
        Value=SimpleNamespace(Value=value, VariantType=SimpleNamespace(name=variant_type)),
    )


assert len(subject.RESOURCE_NODE_IDS) == 22
assert len(set(subject.RESOURCE_NODE_IDS)) == 22
assert subject.resource_member_node_id("Sessions", "Peak").endswith("Sessions.Peak")
assert subject.resource_counter_node_id("EventQueueDrops").endswith("EventQueueDrops")
assert subject.read_only_write_denied_text(Status("BadNotWritable"))
assert subject.read_only_write_denied_text(Status("BadUserAccessDenied"))
assert not subject.read_only_write_denied_text(Status("BadTypeMismatch"))

try:
    subject.resource_member_node_id("Unknown", "Peak")
except ValueError:
    pass
else:
    raise AssertionError("unknown group must fail")

values = {node_id: data_value(0) for node_id in subject.RESOURCE_NODE_IDS}
for group in subject.RESOURCE_GROUPS:
    values[subject.resource_member_node_id(group, "Current")] = data_value(1)
    values[subject.resource_member_node_id(group, "Peak")] = data_value(2)
    values[subject.resource_member_node_id(group, "Capacity")] = data_value(3)
decoded = subject.validate_resource_values(values)
assert decoded[subject.resource_member_node_id("Sessions", "Capacity")] == 3

shrunk_event_capacity = dict(values)
shrunk_event_capacity[subject.resource_member_node_id("EventQueue", "Current")] = data_value(0)
shrunk_event_capacity[subject.resource_member_node_id("EventQueue", "Peak")] = data_value(4)
shrunk_event_capacity[subject.resource_member_node_id("EventQueue", "Capacity")] = data_value(0)
assert subject.validate_resource_values(shrunk_event_capacity)[
    subject.resource_member_node_id("EventQueue", "Peak")] == 4

unsupported = dict(values)
unsupported[subject.resource_counter_node_id("RejectedRequests")] = data_value(None, "BadNotSupported")
try:
    subject.validate_resource_values(unsupported)
except subject.VerificationError as exc:
    assert "required resource counter" in str(exc)
else:
    raise AssertionError("required rejection counter must not be unavailable")

missing_required = dict(values)
missing_required[subject.resource_member_node_id("Sessions", "Current")] = data_value(None, "BadNotSupported")
try:
    subject.validate_resource_values(missing_required)
except subject.VerificationError as exc:
    assert "required lifecycle gauge" in str(exc)
else:
    raise AssertionError("required lifecycle gauge must not be unavailable")

fabricated_zero = dict(values)
fabricated_zero[subject.resource_counter_node_id("RejectedRequests")] = data_value(0, "BadNotSupported")
try:
    subject.validate_resource_values(fabricated_zero)
except subject.VerificationError as exc:
    assert "carried a numeric value" in str(exc)
else:
    raise AssertionError("unsupported metric must not carry numeric zero")

inverted = dict(values)
inverted[subject.resource_member_node_id("Sessions", "Current")] = data_value(4)
try:
    subject.validate_resource_values(inverted)
except subject.VerificationError as exc:
    assert "invalid Current/Peak/Capacity" in str(exc)
else:
    raise AssertionError("inverted occupancy must fail")

wrong_type = dict(values)
wrong_type[subject.resource_counter_node_id("RejectedRequests")] = data_value(1, variant_type="UInt32")
try:
    subject.validate_resource_values(wrong_type)
except subject.VerificationError as exc:
    assert "UInt64" in str(exc)
else:
    raise AssertionError("wrong type must fail")

args = subject.parse_args([
    "opc.tcp://127.0.0.1:4840",
    "--snapshot-settle", "1.0",
    "--stress-extra-clients", "1",
])
assert args.server_url == "opc.tcp://127.0.0.1:4840"


async def passing_verifier(run_args, evidence):
    subject.record_role(evidence, "service", privileged=True, state="measured")
    subject.mark_capacity(evidence, "not-run")
    return 0


async def failing_verifier(run_args, evidence):
    raise RuntimeError("password=do-not-persist")


activation_evidence = subject.new_evidence(args)
subject.record_role(
    activation_evidence,
    "anonymous",
    privileged=False,
    state="not-run",
    write_state="not-run",
    anonymousOutcome="activation-denied",
)
assert activation_evidence["roles"][0]["anonymousOutcome"] == "activation-denied"
assert activation_evidence["checks"]["browse"]["roleCount"] == 0
assert activation_evidence["checks"]["directRead"]["roleCount"] == 0
assert activation_evidence["checks"]["write"]["roleCount"] == 0
assert activation_evidence["checks"]["write"]["deniedRoleCount"] == 0


class EndpointLeakingError(RuntimeError):
    pass


captured = io.StringIO()
with contextlib.redirect_stdout(captured):
    leaked = EndpointLeakingError("opc.tcp://operator:secret@example.invalid:4840")
    print(subject.format_cli_failure(leaked))
assert "secret" not in captured.getvalue()
assert "operator" not in captured.getvalue()


with tempfile.TemporaryDirectory() as temporary_directory:
    temp = Path(temporary_directory)
    output = temp / "resource-evidence.json"
    evidence_args = subject.parse_args([
        "opc.tcp://operator:secret@example.invalid:4840",
        "--output", str(output),
    ])
    assert asyncio.run(subject.execute(evidence_args, passing_verifier)) == 0
    first = output.read_bytes()
    report = json.loads(first)
    assert report["schemaVersion"] == subject.EVIDENCE_SCHEMA_VERSION
    assert report["result"] == {"state": "passed"}
    assert report["server"] == {"authority": "redacted", "port": 4840, "scheme": "opc.tcp"}
    assert report["checks"]["browse"]["state"] == "measured"
    assert report["checks"]["directRead"]["nodeCountPerRole"] == 22
    assert report["checks"]["write"]["state"] == "measured"
    assert report["checks"]["write"]["roleCount"] == 1
    assert report["checks"]["write"]["deniedRoleCount"] == 1
    assert report["roles"][0]["write"] == "measured"
    assert report["checks"]["capacity"]["state"] == "not-run"
    assert report["elapsedMonotonicMs"] >= 0
    assert b"secret" not in first and b"operator" not in first

    # Serialization is stable for the same complete evidence object.
    subject.write_evidence(output, report)
    assert output.read_bytes() == first

    failed_output = temp / "resource-evidence-failed.json"
    failed_args = subject.parse_args([
        "opc.tcp://user:secret@example.invalid:4840",
        "--output", str(failed_output),
    ])
    try:
        asyncio.run(subject.execute(failed_args, failing_verifier))
    except RuntimeError:
        pass
    else:
        raise AssertionError("a verifier failure must remain nonzero")
    failed = failed_output.read_bytes()
    failed_report = json.loads(failed)
    assert failed_report["result"] == {"state": "failed", "errorType": "RuntimeError"}
    assert b"secret" not in failed and b"do-not-persist" not in failed

    try:
        subject.write_evidence(temp, report)
    except ValueError as exc:
        assert "file" in str(exc)
    else:
        raise AssertionError("a directory output target must fail")

    missing_parent = temp / "missing" / "evidence.json"
    try:
        subject.write_evidence(missing_parent, report)
    except ValueError as exc:
        assert "parent" in str(exc)
    else:
        raise AssertionError("a missing output parent must fail")

    too_large = {"payload": "x" * subject.EVIDENCE_MAX_BYTES}
    try:
        subject.write_evidence(temp / "too-large.json", too_large)
    except ValueError as exc:
        assert "bounded output limit" in str(exc)
    else:
        raise AssertionError("oversized evidence must fail")

print("verify_v052_opcua_resources offline tests: PASS")
