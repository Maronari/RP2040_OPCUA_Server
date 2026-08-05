#!/usr/bin/env python3
"""Runtime verification for the v0.3.3 AMB maintenance workflow.

The script is deliberately assertion-driven: a server that merely accepts a
connection is not a passing workflow implementation.  It requires four user
identities because the maintenance Methods are Service-role operations.
"""

import argparse
import asyncio
from datetime import datetime, timedelta, timezone
from importlib.metadata import PackageNotFoundError, version
import json
import math
import os
import sys

import stage_config as opcua_common


AMB_URI = "http://opcfoundation.org/UA/AMB/"
MAINTENANCE_NODE_ID = "ns=1;s=Device.Maintenance"
CONDITION_ID = "ns=1;s=Device.DeviceHealthAlarms.MaintenanceRequired"
REQUIRED_PYTHON = (3, 13)
REQUIRED_ASYNCUA_VERSION = "2.0.1"
UA_UNIX_EPOCH_100NS = 116444736000000000
HUNDRED_NS_PER_SECOND = 10_000_000
METHODS = {
    "PlanMaintenance": "ns=1;s=Device.Maintenance.PlanMaintenance",
    "RescheduleMaintenance": "ns=1;s=Device.Maintenance.RescheduleMaintenance",
    "StartMaintenance": "ns=1;s=Device.Maintenance.StartMaintenance",
    "FinishMaintenance": "ns=1;s=Device.Maintenance.FinishMaintenance",
    "ResetMaintenanceJournal": "ns=1;s=Device.Maintenance.ResetMaintenanceJournal",
    "ExportMaintenanceJournal": "ns=1;s=Device.Maintenance.ExportMaintenanceJournal",
}
FIELDS = ("PlannedDate", "EstimatedDowntime", "MaintenanceMethod",
          "ConfigurationChanged", "MaintenanceSupplier",
          "QualificationOfPersonnel", "PartsOfAssetReplaced",
          "PartsOfAssetServiced")


class Audit:
    def __init__(self):
        self.failures = []
        self.passed = 0

    def check(self, condition, description, category="assertion"):
        context = f"[{category}] {description}"
        if condition:
            self.passed += 1
            print(f"[ OK ] {context}")
        else:
            self.failures.append(context)
            print(f"[FAIL] {context}")

    def equal(self, actual, expected, description, category="assertion"):
        self.check(actual == expected,
                   f"{description}: expected {expected!r}, got {actual!r}", category)

    def report(self):
        print(f"Checks passed: {self.passed}; failed: {len(self.failures)}")
        if self.failures:
            print("Failures:")
            for failure in self.failures:
                print(f"  - {failure}")


class EventCollector:
    """Small asyncua subscription handler retaining only workflow events."""
    def __init__(self):
        self.events = []
        self.audits = []

    def event_notification(self, event):
        operation = getattr(event, "Operation", None)
        if operation in METHODS:
            self.events.append(operation)
        subject = getattr(event, "Subject", None)
        details = getattr(event, "Details", None)
        result = getattr(event, "Result", None)
        if subject in METHODS:
            self.audits.append((subject, result, details))


def node_id(name):
    return f"ns=1;s=Device.DeviceHealthAlarms.MaintenanceRequired.{name}"


def status_matches(actual, expected):
    if actual == expected:
        return True
    actual_value = getattr(actual, "value", actual)
    expected_value = getattr(expected, "value", expected)
    return actual_value == expected_value or str(expected) in str(actual)


def unwrap_scalar_result(result):
    if isinstance(result, (tuple, list)):
        if len(result) != 1:
            raise ValueError(f"expected exactly one method output, got {len(result)}")
        return result[0]
    return result


async def read_child_value(parent, browse_path):
    child = await parent.get_child(browse_path)
    return await child.read_value()


async def read_input_arguments(method, ua):
    """InputArguments is optional on zero-input Methods in OPC UA."""
    try:
        return await read_child_value(method, ["0:InputArguments"])
    except Exception as exc:
        if status_matches(getattr(exc, "code", None), ua.StatusCodes.BadNoMatch):
            return []
        raise


async def call_maintenance_method(client, ua, name, *inputs):
    maintenance = client.get_node(MAINTENANCE_NODE_ID)
    method_id = ua.NodeId.from_string(METHODS[name])
    return await maintenance.call_method(method_id, *inputs)


async def safe_disconnect(client, audit, label):
    if client is None:
        return
    try:
        await client.disconnect()
    except Exception as exc:
        audit.check(False, f"{label}: {type(exc).__name__}: {exc}", "cleanup")


async def safe_delete(subscription, audit, label):
    if subscription is None:
        return
    try:
        await subscription.delete()
    except Exception as exc:
        audit.check(False, f"{label}: {type(exc).__name__}: {exc}", "cleanup")


def verify_runtime(audit):
    python_version = ".".join(map(str, sys.version_info[:3]))
    audit.check(sys.version_info[:2] == REQUIRED_PYTHON,
                f"Python {python_version}; required {REQUIRED_PYTHON[0]}.{REQUIRED_PYTHON[1]}",
                "environment")
    try:
        installed_asyncua = version("asyncua")
    except PackageNotFoundError:
        installed_asyncua = None
    audit.equal(installed_asyncua, REQUIRED_ASYNCUA_VERSION,
                "asyncua distribution version", "environment")
    return not audit.failures


def format_ua_datetime_100ns(value):
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"expected signed 100 ns integer, got {value!r}")
    seconds, fraction = divmod(value - UA_UNIX_EPOCH_100NS, HUNDRED_NS_PER_SECOND)
    instant = datetime(1970, 1, 1, tzinfo=timezone.utc) + timedelta(seconds=seconds)
    return f"{instant:%Y-%m-%dT%H:%M:%S}.{fraction:07d}Z"


def report_export_timestamps(record, audit, page_index, record_index):
    prefix = f"export page {page_index} record {record_index}"
    decoded = {}
    for field in ("committedAt", "plannedDate"):
        try:
            decoded[field] = format_ua_datetime_100ns(record[field])
            print(f"[INFO] [timestamp] {prefix} {field}={record[field]} ({decoded[field]})")
            audit.check(True, f"{prefix} {field} decodes deterministically", "timestamp")
        except (KeyError, OverflowError, ValueError) as exc:
            audit.check(False, f"{prefix} {field}: {type(exc).__name__}: {exc}", "timestamp")
    if len(decoded) == 2:
        audit.check(record["committedAt"] > UA_UNIX_EPOCH_100NS,
                    f"{prefix} committedAt is a post-Unix-epoch UTC timestamp", "timestamp")


async def client_for(args, username, password):
    client = opcua_common.Client(url=args.server_url, timeout=args.timeout)
    # The shared security helper keys certificate setup from ``args.username``.
    # Keep role credentials isolated instead of mutating the common Namespace.
    role_args = argparse.Namespace(**vars(args), username=username)
    await opcua_common.configure_client_security(client, role_args)
    client.set_user(username)
    client.set_password(password or "")
    await client.connect()
    return client


async def read_snapshot(client, ua, amb_ns):
    condition = client.get_node(CONDITION_ID)
    text = await read_child_value(condition, [f"{amb_ns}:MaintenanceState", "0:CurrentState"])
    state_id = await read_child_value(
        condition, [f"{amb_ns}:MaintenanceState", "0:CurrentState", "0:Id"])
    values = {name: await client.get_node(node_id(name)).read_value() for name in FIELDS}
    return {"text": text, "id": state_id, "fields": values}


async def read_condition_semantics(client):
    condition = client.get_node(CONDITION_ID)
    active = await read_child_value(condition, ["0:ActiveState", "0:Id"])
    return {
        "active": active,
        "retain": await read_child_value(condition, ["0:Retain"]),
        "severity": await read_child_value(condition, ["0:Severity"]),
        "message": await read_child_value(condition, ["0:Message"]),
        "time": await read_child_value(condition, ["0:Time"]),
        "receive_time": await read_child_value(condition, ["0:ReceiveTime"]),
    }


async def expect_condition(client, audit, active, retain, severity, message, label):
    condition = await read_condition_semantics(client)
    audit.equal(condition["active"], active, f"{label}: ActiveState")
    audit.equal(condition["retain"], retain, f"{label}: Retain")
    audit.equal(condition["severity"], severity, f"{label}: Severity")
    audit.equal(condition["message"].Text, message, f"{label}: Message")
    audit.check(condition["time"] is not None and condition["receive_time"] is not None,
                f"{label}: condition timestamps available")


async def call_expect(client, ua, audit, name, inputs, expected_status):
    actual = None
    result = None
    exception_context = ""
    try:
        result = await call_maintenance_method(client, ua, name, *inputs)
        actual = ua.StatusCodes.Good
    except Exception as exc:
        actual = getattr(exc, "code", None)
        if actual is None:
            actual = str(exc)
        exception_context = f"; exception={type(exc).__name__}: {exc}"
    expected_text = str(expected_status)
    audit.check(status_matches(actual, expected_status),
                f"object={MAINTENANCE_NODE_ID}, method={METHODS[name]}, input_count={len(inputs)}, "
                f"expected={expected_text}, actual={actual!r}{exception_context}", "method-status")
    if status_matches(expected_status, ua.StatusCodes.Good) and status_matches(actual, ua.StatusCodes.Good):
        try:
            audit.equal(bool(unwrap_scalar_result(result)), True,
                        f"{name} Success output", "method-output")
        except Exception as exc:
            audit.check(False, f"{name}: {type(exc).__name__}: {exc}", "method-output")


async def export_page(client, ua, token=""):
    result = await call_maintenance_method(
        client, ua, "ExportMaintenanceJournal", ua.Variant(token, ua.VariantType.String))
    return json.loads(unwrap_scalar_result(result))


def state_expectation(ua, amb_ns, name, numeric):
    return ua.LocalizedText(name, "en-US"), ua.NodeId(numeric, amb_ns)


async def check_public_contract(client, ua, audit, amb_ns):
    expected = {
        "PlanMaintenance": (("OperationId", ua.VariantType.String),
                             ("PlannedDate", ua.VariantType.DateTime),
                             ("EstimatedDowntime", ua.VariantType.Double),
                             ("MaintenanceMethod", ua.VariantType.Int32),
                             ("MaintenanceSupplier", ua.VariantType.String),
                             ("QualificationOfPersonnel", ua.VariantType.String)),
        "RescheduleMaintenance": (("PlannedDate", ua.VariantType.DateTime),
                                   ("EstimatedDowntime", ua.VariantType.Double),
                                   ("MaintenanceMethod", ua.VariantType.Int32),
                                   ("MaintenanceSupplier", ua.VariantType.String),
                                   ("QualificationOfPersonnel", ua.VariantType.String)),
        "StartMaintenance": (),
        "FinishMaintenance": (("PartsOfAssetServiced", ua.VariantType.String),
                              ("PartsOfAssetReplaced", ua.VariantType.String),
                              ("ConfigurationChanged", ua.VariantType.Boolean),
                              ("Result", ua.VariantType.String)),
    }
    for name, arguments in expected.items():
        method = client.get_node(METHODS[name])
        browse = await method.read_browse_name()
        audit.equal(browse.Name, name, f"BrowseName {name}", "public-contract")
        input_args = await read_input_arguments(method, ua)
        audit.equal(len(input_args), len(arguments), f"{name} input count", "public-contract")
        for actual, wanted in zip(input_args, arguments):
            audit.equal(actual.Name, wanted[0], f"{name} argument name", "public-contract")
            audit.equal(actual.DataType.Identifier, int(wanted[1]),
                        f"{name}.{wanted[0]} builtin type", "public-contract")
        outputs = await read_child_value(method, ["0:OutputArguments"])
        audit.check(len(outputs) == 1 and outputs[0].Name == "Success" and
                    outputs[0].DataType.Identifier == int(ua.VariantType.Boolean),
                    f"{name} has Success:Boolean output", "public-contract")

    reset = client.get_node(METHODS["ResetMaintenanceJournal"])
    reset_browse = await reset.read_browse_name()
    audit.equal(reset_browse.Name, "ResetMaintenanceJournal",
                "BrowseName ResetMaintenanceJournal", "public-contract")
    reset_inputs = await read_input_arguments(reset, ua)
    reset_outputs = await read_child_value(reset, ["0:OutputArguments"])
    audit.equal(len(reset_inputs), 0, "ResetMaintenanceJournal input count", "public-contract")
    audit.check(len(reset_outputs) == 1 and reset_outputs[0].Name == "Success" and
                    reset_outputs[0].DataType.Identifier == int(ua.VariantType.Boolean),
                "ResetMaintenanceJournal has Success:Boolean output", "public-contract")
    export = client.get_node(METHODS["ExportMaintenanceJournal"])
    export_browse = await export.read_browse_name()
    export_inputs = await read_input_arguments(export, ua)
    export_outputs = await read_child_value(export, ["0:OutputArguments"])
    audit.equal(export_browse.Name, "ExportMaintenanceJournal",
                "BrowseName ExportMaintenanceJournal", "public-contract")
    audit.check(len(export_inputs) == 1 and export_inputs[0].Name == "ContinuationToken" and
                export_inputs[0].DataType.Identifier == int(ua.VariantType.String),
                "ExportMaintenanceJournal ContinuationToken:String", "public-contract")
    audit.check(len(export_outputs) == 1 and export_outputs[0].Name == "Result" and
                export_outputs[0].DataType.Identifier == int(ua.VariantType.String),
                "ExportMaintenanceJournal Result:String output", "public-contract")

    types = {"MaintenanceSupplier": (3003, -1),
             "QualificationOfPersonnel": (3003, -1),
             "PartsOfAssetReplaced": (3003, 1),
             "PartsOfAssetServiced": (3003, 1)}
    for name, (type_id, rank) in types.items():
        node = client.get_node(node_id(name))
        attrs = await node.read_attributes([ua.AttributeIds.DataType, ua.AttributeIds.ValueRank,
                                            ua.AttributeIds.AccessLevel])
        audit.equal(attrs[0].Value.Value, ua.NodeId(type_id, amb_ns), f"{name} data type",
                    "public-contract")
        audit.equal(attrs[1].Value.Value, rank, f"{name} value rank", "public-contract")
        audit.check(not (attrs[2].Value.Value & 0x02), f"{name} is not client writable",
                    "public-contract")


def plan_inputs(ua):
    return (ua.Variant("maintenance-001", ua.VariantType.String),
            ua.Variant(ua.DateTime.now(), ua.VariantType.DateTime),
            ua.Variant(3600.0, ua.VariantType.Double),
            ua.Variant(0, ua.VariantType.Int32),
            ua.Variant("Vendor", ua.VariantType.String),
            ua.Variant("Qualified technician", ua.VariantType.String))


async def verify_roles(args, ua, audit):
    for role, expected in (("viewer", ua.StatusCodes.BadUserAccessDenied),
                           ("operator", ua.StatusCodes.BadUserAccessDenied)):
        username = getattr(args, f"{role}_username")
        password = getattr(args, f"{role}_password")
        client = None
        try:
            client = await client_for(args, username, password)
            namespaces = await client.get_namespace_array()
            before = await read_snapshot(client, ua, namespaces.index(AMB_URI))
            for method, inputs in (("PlanMaintenance", plan_inputs(ua)),
                                   ("RescheduleMaintenance", plan_inputs(ua)[1:]),
                                   ("StartMaintenance", ()),
                                   ("FinishMaintenance", finish_inputs(ua))):
                await call_expect(client, ua, audit, method, inputs, expected)
                audit.equal(await read_snapshot(client, ua, namespaces.index(AMB_URI)), before,
                            f"{role} {method} rejection leaves snapshot unchanged")
            await call_expect(client, ua, audit, "ResetMaintenanceJournal", (), expected)
            audit.equal(await read_snapshot(client, ua, namespaces.index(AMB_URI)), before,
                        f"{role} ResetMaintenanceJournal rejection leaves snapshot unchanged")
            await call_expect(client, ua, audit, "ExportMaintenanceJournal",
                              (ua.Variant("", ua.VariantType.String),), expected)
        finally:
            await safe_disconnect(client, audit, f"{role} client disconnect")


def finish_inputs(ua, serviced=("filter",), replaced=("seal",)):
    return (ua.Variant(list(serviced), ua.VariantType.String),
            ua.Variant(list(replaced), ua.VariantType.String),
            ua.Variant(True, ua.VariantType.Boolean),
            ua.Variant("completed", ua.VariantType.String))


async def reject_unchanged(client, ua, audit, amb_ns, method, inputs, expected, label):
    before = await read_snapshot(client, ua, amb_ns)
    await call_expect(client, ua, audit, method, inputs, expected)
    audit.equal(await read_snapshot(client, ua, amb_ns), before,
                f"{label}: unchanged after rejection")


async def verify_rejection_matrix(client, ua, audit, amb_ns):
    # Finished: only Plan is legal.
    for method, inputs in (("RescheduleMaintenance", plan_inputs(ua)[1:]),
                           ("StartMaintenance", ()),
                           ("FinishMaintenance", finish_inputs(ua))):
        await reject_unchanged(client, ua, audit, amb_ns, method, inputs,
                               ua.StatusCodes.BadStateNotActive, f"Finished/{method}")
    # open62541 performs canonical Call arity validation before the adapter.
    # Supplied values that violate the project constraints remain
    # BadInvalidArgument; out-of-range values use BadOutOfRange.
    await reject_unchanged(client, ua, audit, amb_ns, "PlanMaintenance", (),
                           ua.StatusCodes.BadArgumentsMissing, "Plan missing arguments")
    await reject_unchanged(client, ua, audit, amb_ns, "PlanMaintenance",
                           plan_inputs(ua) + (ua.Variant("extra", ua.VariantType.String),),
                           ua.StatusCodes.BadTooManyArguments, "Plan extra argument")
    await reject_unchanged(client, ua, audit, amb_ns, "StartMaintenance",
                           (ua.Variant("extra", ua.VariantType.String),),
                           ua.StatusCodes.BadTooManyArguments, "Start extra argument")
    await reject_unchanged(client, ua, audit, amb_ns, "ExportMaintenanceJournal", (),
                           ua.StatusCodes.BadArgumentsMissing, "Export missing argument")
    await reject_unchanged(
        client, ua, audit, amb_ns, "ExportMaintenanceJournal",
        (ua.Variant("", ua.VariantType.String), ua.Variant("extra", ua.VariantType.String)),
        ua.StatusCodes.BadTooManyArguments, "Export extra argument")
    wrong = list(plan_inputs(ua)); wrong[2] = ua.Variant("wrong", ua.VariantType.String)
    await reject_unchanged(client, ua, audit, amb_ns, "PlanMaintenance", tuple(wrong),
                           ua.StatusCodes.BadInvalidArgument, "Plan wrong type")
    oversize = list(plan_inputs(ua)); oversize[0] = ua.Variant("x" * 48, ua.VariantType.String)
    await reject_unchanged(client, ua, audit, amb_ns, "PlanMaintenance", tuple(oversize),
                           ua.StatusCodes.BadInvalidArgument, "Plan oversized OperationId")
    for index, label in ((4, "supplier"), (5, "personnel")):
        oversized_plan = list(plan_inputs(ua))
        oversized_plan[index] = ua.Variant("x" * 48, ua.VariantType.String)
        await reject_unchanged(
            client, ua, audit, amb_ns, "PlanMaintenance", tuple(oversized_plan),
            ua.StatusCodes.BadInvalidArgument, f"Plan oversized {label}")
    oversized_finish = list(finish_inputs(ua))
    oversized_finish[3] = ua.Variant("x" * 96, ua.VariantType.String)
    await reject_unchanged(
        client, ua, audit, amb_ns, "FinishMaintenance", tuple(oversized_finish),
        ua.StatusCodes.BadInvalidArgument, "Finish oversized result")
    oversized_finish = list(finish_inputs(ua))
    oversized_finish[0] = ua.Variant(["x" * 48], ua.VariantType.String)
    await reject_unchanged(
        client, ua, audit, amb_ns, "FinishMaintenance", tuple(oversized_finish),
        ua.StatusCodes.BadInvalidArgument, "Finish oversized part")


async def verify_export_reset(client, admin, ua, audit, amb_ns):
    """Assert bounded service-owned logical paging and reset invalidation."""
    export_audits = EventCollector()
    export_subscription = None
    try:
        export_subscription = await client.create_subscription(100, export_audits)
        await export_subscription.subscribe_events(client.get_node(MAINTENANCE_NODE_ID))
        await call_expect(client, ua, audit, "ExportMaintenanceJournal",
                          (ua.Variant("not-a-token", ua.VariantType.String),),
                          ua.StatusCodes.BadContinuationPointInvalid)
        await asyncio.sleep(0.5)
        audit.check(any(subject == "ExportMaintenanceJournal" and result is False
                        for subject, result, _ in export_audits.audits),
                    "invalid export token produces rejected AuditEvent fields", "audit")
        first = await export_page(client, ua)
        audit.check(len(first["records"]) <= 4, "export page contains at most four records", "resource")
        for record_index, record in enumerate(first["records"]):
            for field in ("epoch", "sequence", "operation", "transition", "state", "revision",
                          "committedAt", "plannedDate", "estimatedDowntime", "method",
                          "configurationChanged", "operationId", "supplier", "personnel", "actor",
                          "result", "partsServicedCount", "partsServiced", "partsReplacedCount",
                          "partsReplaced"):
                audit.check(field in record, f"logical export record includes {field}", "export")
            report_export_timestamps(record, audit, 0, record_index)
        token = first["continuationToken"]
        audit.check(first["more"] == bool(token), "export more/token agreement", "export")
        if token:
            await asyncio.sleep(31.0)
            await call_expect(client, ua, audit, "ExportMaintenanceJournal",
                              (ua.Variant(token, ua.VariantType.String),),
                              ua.StatusCodes.BadContinuationPointInvalid)
            await asyncio.sleep(0.5)
            audit.check(any(subject == "ExportMaintenanceJournal" and result is False
                            for subject, result, _ in export_audits.audits),
                        "expired export token is auditable", "audit")

        pages = [await export_page(client, ua) for _ in range(4)]
        active_tokens = [page["continuationToken"] for page in pages if page["more"]]
        if len(active_tokens) == 4:
            await call_expect(client, ua, audit, "ExportMaintenanceJournal",
                              (ua.Variant("", ua.VariantType.String),),
                              ua.StatusCodes.BadNoContinuationPoints)
            await asyncio.sleep(0.5)
            audit.check(sum(subject == "ExportMaintenanceJournal" and result is False
                            for subject, result, _ in export_audits.audits) <= 4,
                        "export rejection audit limiter bounds invalid/expired/fifth calls", "resource")
        else:
            audit.check(False, "journal has enough records to exercise fifth continuation", "resource")

        before = await read_snapshot(admin, ua, amb_ns)
        await call_expect(admin, ua, audit, "ResetMaintenanceJournal", (), ua.StatusCodes.Good)
        audit.equal(await read_snapshot(admin, ua, amb_ns), before,
                    "admin logical reset preserves workflow snapshot", "reset")
        for stale in active_tokens:
            await call_expect(client, ua, audit, "ExportMaintenanceJournal",
                              (ua.Variant(stale, ua.VariantType.String),),
                              ua.StatusCodes.BadContinuationPointInvalid)
    finally:
        await safe_delete(export_subscription, audit, "export subscription delete")


async def verify_event_cardinality(client, admin, ua, audit, amb_ns):
    """Check both subscribers for every transition and representative rejection."""
    first_events, second_events = EventCollector(), EventCollector()
    first_sub = None
    second_sub = None

    async def counts():
        await asyncio.sleep(0.5)
        return (len(first_events.events), len(second_events.events))

    async def expect_no_event(method, inputs, status, label):
        before = await counts()
        await reject_unchanged(client, ua, audit, amb_ns, method, inputs, status, label)
        audit.equal(await counts(), before, f"{label}: no matching event")

    async def expect_transition(method, inputs):
        before_first = first_events.events.count(method)
        before_second = second_events.events.count(method)
        await call_expect(client, ua, audit, method, inputs, ua.StatusCodes.Good)
        await asyncio.sleep(0.5)
        audit.equal(first_events.events.count(method), before_first + 1,
                    f"first subscriber receives exactly one {method} event")
        audit.equal(second_events.events.count(method), before_second + 1,
                    f"second subscriber receives exactly one {method} event")

    try:
        first_sub = await client.create_subscription(100, first_events)
        second_sub = await admin.create_subscription(100, second_events)
        await first_sub.subscribe_events(client.get_node(MAINTENANCE_NODE_ID))
        await second_sub.subscribe_events(admin.get_node(MAINTENANCE_NODE_ID))

        # Entry state is Planned. Reschedule succeeds but is not a transition.
        before = await counts()
        await call_expect(client, ua, audit, "RescheduleMaintenance",
                          plan_inputs(ua)[1:], ua.StatusCodes.Good)
        audit.equal(await counts(), before, "RescheduleMaintenance emits no event")
        await expect_no_event("PlanMaintenance", plan_inputs(ua),
                              ua.StatusCodes.BadStateNotActive, "Planned/Plan event")
        audit_before_first = len(first_events.audits)
        audit_before_second = len(second_events.audits)
        for _ in range(6):
            await call_expect(client, ua, audit, "PlanMaintenance", plan_inputs(ua),
                              ua.StatusCodes.BadStateNotActive)
        await asyncio.sleep(0.5)
        audit.check(len(first_events.audits) - audit_before_first <= 4,
                    "per-session rejected audit burst is limited to four")
        audit.equal(len(first_events.audits) - audit_before_first,
                    len(second_events.audits) - audit_before_second,
                    "two subscribers receive equal rejected-audit cardinality")
        await expect_no_event("FinishMaintenance", finish_inputs(ua),
                              ua.StatusCodes.BadStateNotActive, "Planned/Finish event")
        await expect_transition("StartMaintenance", ())
        audit.check(any(subject == "StartMaintenance" and result is True
                        for subject, result, _ in first_events.audits),
                    "accepted maintenance call produces AuditEvent fields")

        await expect_no_event("PlanMaintenance", plan_inputs(ua),
                              ua.StatusCodes.BadStateNotActive, "Executing/Plan event")
        await expect_no_event("RescheduleMaintenance", plan_inputs(ua)[1:],
                              ua.StatusCodes.BadStateNotActive, "Executing/Reschedule event")
        await expect_no_event("StartMaintenance", (),
                              ua.StatusCodes.BadStateNotActive, "Executing/Start event")
        await expect_transition("FinishMaintenance", finish_inputs(ua))

        await expect_no_event("RescheduleMaintenance", plan_inputs(ua)[1:],
                              ua.StatusCodes.BadStateNotActive, "Finished/Reschedule event")
        await expect_no_event("StartMaintenance", (),
                              ua.StatusCodes.BadStateNotActive, "Finished/Start event")
        await expect_no_event("FinishMaintenance", finish_inputs(ua),
                              ua.StatusCodes.BadStateNotActive, "Finished/Finish event")
        await expect_transition("PlanMaintenance", plan_inputs(ua))
    finally:
        await safe_delete(first_sub, audit, "first event-cardinality subscription delete")
        await safe_delete(second_sub, audit, "second event-cardinality subscription delete")


async def verify_workflow(args, ua, audit):
    client = None
    try:
        client = await client_for(args, args.service_username, args.service_password)
        namespaces = await client.get_namespace_array()
        audit.check(AMB_URI in namespaces, "AMB namespace is present")
        amb_ns = namespaces.index(AMB_URI)
        await check_public_contract(client, ua, audit, amb_ns)
        await verify_roles(args, ua, audit)

        # The service starts in Finished.  Planning begins the next volatile
        # activity and is intentionally illegal from the other two states.
        planned_text, planned_id = state_expectation(ua, amb_ns, "Planned", 5006)
        executing_text, executing_id = state_expectation(ua, amb_ns, "Executing", 5007)
        finished_text, finished_id = state_expectation(ua, amb_ns, "Finished", 5008)
        snapshot = await read_snapshot(client, ua, amb_ns)
        audit.equal((snapshot["text"], snapshot["id"]), (finished_text, finished_id),
                    "initial Finished state text and Id")
        await expect_condition(client, audit, False, False, 100, "Maintenance finished",
                               "initial recovered Finished condition")
        await verify_rejection_matrix(client, ua, audit, amb_ns)

        # Two simultaneous subscribers must each receive exactly one event for
        # a successful transition.  Filter only the workflow Operation because
        # the existing generic MaintenanceEvent also carries unrelated events.
        admin = None
        first_events, second_events = EventCollector(), EventCollector()
        first_sub = None
        second_sub = None
        try:
            admin = await client_for(args, args.admin_username, args.admin_password)
            first_sub = await client.create_subscription(100, first_events)
            second_sub = await admin.create_subscription(100, second_events)
            await first_sub.subscribe_events(client.get_node(MAINTENANCE_NODE_ID))
            await second_sub.subscribe_events(admin.get_node(MAINTENANCE_NODE_ID))
            await call_expect(client, ua, audit, "PlanMaintenance", plan_inputs(ua), ua.StatusCodes.Good)
            await asyncio.sleep(0.5)
            audit.equal(first_events.events.count("PlanMaintenance"), 1,
                        "first subscriber receives one PlanMaintenance event")
            audit.equal(second_events.events.count("PlanMaintenance"), 1,
                        "second subscriber receives one PlanMaintenance event")
        finally:
            await safe_delete(first_sub, audit, "first planning subscription delete")
            await safe_delete(second_sub, audit, "second planning subscription delete")
            await safe_disconnect(admin, audit, "planning admin client disconnect")

        snapshot = await read_snapshot(client, ua, amb_ns)
        audit.equal((snapshot["text"], snapshot["id"]), (planned_text, planned_id),
                    "PlanMaintenance synchronizes Planned text and Id")
        await expect_condition(client, audit, True, True, 350, "Maintenance planned",
                               "PlanMaintenance condition")

        for method, inputs in (("PlanMaintenance", plan_inputs(ua)),
                               ("FinishMaintenance", finish_inputs(ua))):
            await reject_unchanged(client, ua, audit, amb_ns, method, inputs,
                                   ua.StatusCodes.BadStateNotActive, f"Planned/{method}")
        await reject_unchanged(client, ua, audit, amb_ns, "FinishMaintenance",
                               finish_inputs(ua, ("a", "b", "c", "d", "e")),
                               ua.StatusCodes.BadInvalidArgument, "Finish fifth part")

        await call_expect(client, ua, audit, "StartMaintenance", (), ua.StatusCodes.Good)
        snapshot = await read_snapshot(client, ua, amb_ns)
        audit.equal((snapshot["text"], snapshot["id"]), (executing_text, executing_id),
                    "StartMaintenance synchronizes Executing text and Id")
        await expect_condition(client, audit, True, True, 500, "Maintenance executing",
                               "StartMaintenance condition")

        for method, inputs in (("PlanMaintenance", plan_inputs(ua)),
                               ("RescheduleMaintenance", plan_inputs(ua)[1:]),
                               ("StartMaintenance", ())):
            await reject_unchanged(client, ua, audit, amb_ns, method, inputs,
                                   ua.StatusCodes.BadStateNotActive, f"Executing/{method}")

        finish = finish_inputs(ua)
        await call_expect(client, ua, audit, "FinishMaintenance", finish, ua.StatusCodes.Good)
        snapshot = await read_snapshot(client, ua, amb_ns)
        audit.equal((snapshot["text"], snapshot["id"]), (finished_text, finished_id),
                    "FinishMaintenance synchronizes Finished text and Id")
        await expect_condition(client, audit, False, False, 100, "completed",
                               "FinishMaintenance condition")

        await call_expect(client, ua, audit, "PlanMaintenance", plan_inputs(ua), ua.StatusCodes.Good)
        snapshot = await read_snapshot(client, ua, amb_ns)
        audit.equal((snapshot["text"], snapshot["id"]), (planned_text, planned_id),
                    "second PlanMaintenance synchronizes Planned text and Id")
        reschedule = plan_inputs(ua)[1:]
        before = await read_snapshot(client, ua, amb_ns)
        await call_expect(client, ua, audit, "RescheduleMaintenance", reschedule, ua.StatusCodes.Good)
        after = await read_snapshot(client, ua, amb_ns)
        audit.equal((after["text"], after["id"]), (planned_text, planned_id),
                    "RescheduleMaintenance preserves Planned state")
        audit.check(before["fields"]["PlannedDate"] != after["fields"]["PlannedDate"],
                    "RescheduleMaintenance changes planned data")
        reschedule_events = EventCollector()
        reschedule_subscription = None
        try:
            reschedule_subscription = await client.create_subscription(100, reschedule_events)
            await reschedule_subscription.subscribe_events(
                client.get_node(MAINTENANCE_NODE_ID))
            before_events = reschedule_events.events.count("RescheduleMaintenance")
            await call_expect(client, ua, audit, "RescheduleMaintenance",
                              plan_inputs(ua)[1:], ua.StatusCodes.Good)
            await asyncio.sleep(0.5)
            audit.equal(reschedule_events.events.count("RescheduleMaintenance"),
                        before_events,
                        "RescheduleMaintenance emits no transition event")
        finally:
            await safe_delete(reschedule_subscription, audit, "reschedule subscription delete")

        # Representative malformed values; the runner reports an explicit
        # failure if the server accepts an out-of-contract call.
        before = await read_snapshot(client, ua, amb_ns)
        bad_plan = list(plan_inputs(ua))
        bad_plan[2] = ua.Variant(math.inf, ua.VariantType.Double)
        await call_expect(client, ua, audit, "PlanMaintenance", tuple(bad_plan),
                          ua.StatusCodes.BadOutOfRange)
        audit.equal(await read_snapshot(client, ua, amb_ns), before,
                    "invalid duration leaves snapshot unchanged")
        # Admin has the same permitted contract: complete an independent
        # transition cycle after Service has returned the activity to Planned.
        admin = None
        try:
            admin = await client_for(args, args.admin_username, args.admin_password)
            await call_expect(admin, ua, audit, "StartMaintenance", (), ua.StatusCodes.Good)
            maximum_finish = (
                ua.Variant(["s" * 47] * 4, ua.VariantType.String),
                ua.Variant(["r" * 47] * 4, ua.VariantType.String),
                ua.Variant(True, ua.VariantType.Boolean),
                ua.Variant("x" * 95, ua.VariantType.String))
            await call_expect(admin, ua, audit, "FinishMaintenance",
                              maximum_finish, ua.StatusCodes.Good)
            maximum_plan = list(plan_inputs(ua))
            maximum_plan[0] = ua.Variant("o" * 47, ua.VariantType.String)
            maximum_plan[4] = ua.Variant("s" * 47, ua.VariantType.String)
            maximum_plan[5] = ua.Variant("p" * 47, ua.VariantType.String)
            await call_expect(admin, ua, audit, "PlanMaintenance",
                              tuple(maximum_plan), ua.StatusCodes.Good)
            maximum_reschedule = list(plan_inputs(ua)[1:])
            maximum_reschedule[3] = ua.Variant("s" * 47, ua.VariantType.String)
            maximum_reschedule[4] = ua.Variant("p" * 47, ua.VariantType.String)
            before = await read_snapshot(admin, ua, amb_ns)
            await call_expect(admin, ua, audit, "RescheduleMaintenance",
                              tuple(maximum_reschedule), ua.StatusCodes.Good)
            after = await read_snapshot(admin, ua, amb_ns)
            audit.equal((after["text"], after["id"]), (before["text"], before["id"]),
                        "Admin RescheduleMaintenance preserves Planned state")
            await verify_event_cardinality(client, admin, ua, audit, amb_ns)
            await verify_export_reset(client, admin, ua, audit, amb_ns)
        finally:
            await safe_disconnect(admin, audit, "admin client disconnect")
    finally:
        await safe_disconnect(client, audit, "service client disconnect")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server_url")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--viewer-username", required=True)
    parser.add_argument("--viewer-password", default=os.getenv("OPCUA_VIEWER_PASSWORD", ""))
    parser.add_argument("--operator-username", required=True)
    parser.add_argument("--operator-password", default=os.getenv("OPCUA_OPERATOR_PASSWORD", ""))
    parser.add_argument("--service-username", required=True)
    parser.add_argument("--service-password", default=os.getenv("OPCUA_SERVICE_PASSWORD", ""))
    parser.add_argument("--admin-username", required=True)
    parser.add_argument("--admin-password", default=os.getenv("OPCUA_ADMIN_PASSWORD", ""))
    parser.add_argument("--client-cert")
    parser.add_argument("--client-key")
    parser.add_argument("--server-cert")
    parser.add_argument("--security-policy", default="Basic256Sha256", choices=["Basic256Sha256"])
    parser.add_argument("--security-mode", default="SignAndEncrypt",
                        choices=["Sign", "SignAndEncrypt"])
    parser.add_argument("--no-auto-client-cert", action="store_true",
                        help="Require --client-cert and --client-key instead of generating a temporary certificate")
    return parser.parse_args()


async def main():
    args = parse_args()
    audit = Audit()
    if not verify_runtime(audit):
        audit.report()
        return 1
    try:
        opcua_common.load_asyncua()
        await verify_workflow(args, opcua_common.ua, audit)
    except Exception as exc:
        audit.check(False, f"workflow setup: {type(exc).__name__}: {exc}", "setup")
    audit.report()
    return 1 if audit.failures else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
