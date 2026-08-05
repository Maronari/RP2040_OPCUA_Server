#!/usr/bin/env python3
"""Runtime verification for the v0.3.2 AMB maintenance workflow.

The script is deliberately assertion-driven: a server that merely accepts a
connection is not a passing workflow implementation.  It requires four user
identities because the maintenance Methods are Service-role operations.
"""

import argparse
import asyncio
import math
import os
import sys

import stage_config as opcua_common


AMB_URI = "http://opcfoundation.org/UA/AMB/"
CONDITION_ID = "ns=1;s=Device.DeviceHealthAlarms.MaintenanceRequired"
METHODS = {
    "PlanMaintenance": "ns=1;s=Device.Maintenance.PlanMaintenance",
    "RescheduleMaintenance": "ns=1;s=Device.Maintenance.RescheduleMaintenance",
    "StartMaintenance": "ns=1;s=Device.Maintenance.StartMaintenance",
    "FinishMaintenance": "ns=1;s=Device.Maintenance.FinishMaintenance",
}
FIELDS = ("PlannedDate", "EstimatedDowntime", "MaintenanceMethod",
          "ConfigurationChanged", "MaintenanceSupplier",
          "QualificationOfPersonnel", "PartsOfAssetReplaced",
          "PartsOfAssetServiced")


class Audit:
    def __init__(self):
        self.failures = []
        self.passed = 0

    def check(self, condition, description):
        if condition:
            self.passed += 1
            print(f"[ OK ] {description}")
        else:
            self.failures.append(description)
            print(f"[FAIL] {description}")

    def equal(self, actual, expected, description):
        self.check(actual == expected,
                   f"{description}: expected {expected!r}, got {actual!r}")


class EventCollector:
    """Small asyncua subscription handler retaining only workflow events."""
    def __init__(self):
        self.events = []

    def event_notification(self, event):
        operation = getattr(event, "Operation", None)
        if operation in METHODS:
            self.events.append(operation)


def node_id(name):
    return f"ns=1;s=Device.DeviceHealthAlarms.MaintenanceRequired.{name}"


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
    text = await condition.get_child([f"{amb_ns}:MaintenanceState", "0:CurrentState"]).read_value()
    state_id = await condition.get_child(
        [f"{amb_ns}:MaintenanceState", "0:CurrentState", "0:Id"]).read_value()
    values = {name: await client.get_node(node_id(name)).read_value() for name in FIELDS}
    return {"text": text, "id": state_id, "fields": values}


async def call_expect(client, ua, audit, name, inputs, expected_status):
    try:
        await client.get_node(METHODS[name]).call_method(*inputs)
        actual = ua.StatusCodes.Good
    except Exception as exc:  # asyncua exposes UA status in the exception text
        actual = getattr(exc, "code", None)
        if actual is None:
            actual = str(exc)
    expected_text = str(expected_status)
    audit.check(actual == expected_status or expected_text in str(actual),
                f"{name} status is {expected_text}, got {actual!r}")


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
        audit.equal(browse.Name, name, f"BrowseName {name}")
        input_args = await method.get_child(["0:InputArguments"]).read_value()
        audit.equal(len(input_args), len(arguments), f"{name} input count")
        for actual, wanted in zip(input_args, arguments):
            audit.equal(actual.Name, wanted[0], f"{name} argument name")
            audit.equal(actual.DataType.Identifier, int(wanted[1]), f"{name}.{wanted[0]} builtin type")
        outputs = await method.get_child(["0:OutputArguments"]).read_value()
        audit.check(len(outputs) == 1 and outputs[0].Name == "Success" and
                    outputs[0].DataType.Identifier == int(ua.VariantType.Boolean),
                    f"{name} has Success:Boolean output")

    types = {"MaintenanceSupplier": (3003, -1),
             "QualificationOfPersonnel": (3003, -1),
             "PartsOfAssetReplaced": (3003, 1),
             "PartsOfAssetServiced": (3003, 1)}
    for name, (type_id, rank) in types.items():
        node = client.get_node(node_id(name))
        attrs = await node.read_attributes([ua.AttributeIds.DataType, ua.AttributeIds.ValueRank,
                                            ua.AttributeIds.AccessLevel])
        audit.equal(attrs[0].Value.Value, ua.NodeId(type_id, amb_ns), f"{name} data type")
        audit.equal(attrs[1].Value.Value, rank, f"{name} value rank")
        audit.check(not (attrs[2].Value.Value & 0x02), f"{name} is not client writable")


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
        client = await client_for(args, username, password)
        try:
            namespaces = await client.get_namespace_array()
            before = await read_snapshot(client, ua, namespaces.index(AMB_URI))
            for method, inputs in (("PlanMaintenance", plan_inputs(ua)),
                                   ("RescheduleMaintenance", plan_inputs(ua)[1:]),
                                   ("StartMaintenance", ()),
                                   ("FinishMaintenance", finish_inputs(ua))):
                await call_expect(client, ua, audit, method, inputs, expected)
                audit.equal(await read_snapshot(client, ua, namespaces.index(AMB_URI)), before,
                            f"{role} {method} rejection leaves snapshot unchanged")
        finally:
            await client.disconnect()


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
    # Every input class is asserted rather than delegated to client defaults.
    await reject_unchanged(client, ua, audit, amb_ns, "PlanMaintenance", (),
                           ua.StatusCodes.BadInvalidArgument, "Plan missing arguments")
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


async def verify_event_cardinality(client, admin, ua, audit, amb_ns):
    """Check both subscribers for every transition and representative rejection."""
    first_events, second_events = EventCollector(), EventCollector()
    first_sub = await client.create_subscription(100, first_events)
    second_sub = await admin.create_subscription(100, second_events)

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
        await first_sub.subscribe_events(client.get_node("ns=1;s=Device.Maintenance"))
        await second_sub.subscribe_events(admin.get_node("ns=1;s=Device.Maintenance"))

        # Entry state is Planned. Reschedule succeeds but is not a transition.
        before = await counts()
        await call_expect(client, ua, audit, "RescheduleMaintenance",
                          plan_inputs(ua)[1:], ua.StatusCodes.Good)
        audit.equal(await counts(), before, "RescheduleMaintenance emits no event")
        await expect_no_event("PlanMaintenance", plan_inputs(ua),
                              ua.StatusCodes.BadStateNotActive, "Planned/Plan event")
        await expect_no_event("FinishMaintenance", finish_inputs(ua),
                              ua.StatusCodes.BadStateNotActive, "Planned/Finish event")
        await expect_transition("StartMaintenance", ())

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
        await first_sub.delete()
        await second_sub.delete()


async def verify_workflow(args, ua, audit):
    client = await client_for(args, args.service_username, args.service_password)
    try:
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
        await verify_rejection_matrix(client, ua, audit, amb_ns)

        # Two simultaneous subscribers must each receive exactly one event for
        # a successful transition.  Filter only the workflow Operation because
        # the existing generic MaintenanceEvent also carries unrelated events.
        admin = await client_for(args, args.admin_username, args.admin_password)
        first_events, second_events = EventCollector(), EventCollector()
        first_sub = await client.create_subscription(100, first_events)
        second_sub = await admin.create_subscription(100, second_events)
        try:
            await first_sub.subscribe_events(client.get_node("ns=1;s=Device.Maintenance"))
            await second_sub.subscribe_events(admin.get_node("ns=1;s=Device.Maintenance"))
            await call_expect(client, ua, audit, "PlanMaintenance", plan_inputs(ua), ua.StatusCodes.Good)
            await asyncio.sleep(0.5)
            audit.equal(first_events.events.count("PlanMaintenance"), 1,
                        "first subscriber receives one PlanMaintenance event")
            audit.equal(second_events.events.count("PlanMaintenance"), 1,
                        "second subscriber receives one PlanMaintenance event")
        finally:
            await first_sub.delete()
            await second_sub.delete()
            await admin.disconnect()

        snapshot = await read_snapshot(client, ua, amb_ns)
        audit.equal((snapshot["text"], snapshot["id"]), (planned_text, planned_id),
                    "PlanMaintenance synchronizes Planned text and Id")

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
        reschedule_subscription = await client.create_subscription(100, reschedule_events)
        try:
            await reschedule_subscription.subscribe_events(
                client.get_node("ns=1;s=Device.Maintenance"))
            before_events = reschedule_events.events.count("RescheduleMaintenance")
            await call_expect(client, ua, audit, "RescheduleMaintenance",
                              plan_inputs(ua)[1:], ua.StatusCodes.Good)
            await asyncio.sleep(0.5)
            audit.equal(reschedule_events.events.count("RescheduleMaintenance"),
                        before_events,
                        "RescheduleMaintenance emits no transition event")
        finally:
            await reschedule_subscription.delete()

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
        admin = await client_for(args, args.admin_username, args.admin_password)
        try:
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
        finally:
            await admin.disconnect()
    finally:
        await client.disconnect()


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
    parser.add_argument("--security-policy", default="Basic256Sha256")
    return parser.parse_args()


async def main():
    args = parse_args()
    opcua_common.load_asyncua()
    audit = Audit()
    try:
        await verify_workflow(args, opcua_common.ua, audit)
    except Exception as exc:
        audit.check(False, f"workflow setup: {type(exc).__name__}: {exc}")
    print(f"Checks passed: {audit.passed}; failed: {len(audit.failures)}")
    return 1 if audit.failures else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
