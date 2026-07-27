#!/usr/bin/env python3
"""Read-only verification of the embedded OPC UA DI/AMB information model."""

import argparse
import asyncio
import getpass
import os
import sys

import stage_config as opcua_common


DI_URI = "http://opcfoundation.org/UA/DI/"
AMB_URI = "http://opcfoundation.org/UA/AMB/"


class ModelAudit:
    def __init__(self, client, ua, di_ns, amb_ns):
        self.client = client
        self.ua = ua
        self.di_ns = di_ns
        self.amb_ns = amb_ns
        self.passed = 0
        self.failures = []

    def ok(self, description):
        self.passed += 1
        print(f"[ OK ] {description}")

    def fail(self, description, detail):
        message = f"{description}: {detail}"
        self.failures.append(message)
        print(f"[FAIL] {message}")

    async def check_node(self, node_id, browse_name=None, node_class=None):
        node = self.client.get_node(node_id)
        try:
            actual_class = await node.read_node_class()
            actual_name = await node.read_browse_name()
        except Exception as exc:
            self.fail(f"Node {node_id} exists", f"{type(exc).__name__}: {exc}")
            return None

        if browse_name is not None and actual_name.Name != browse_name:
            self.fail(f"Node {node_id} BrowseName",
                      f"expected {browse_name!r}, got {actual_name.Name!r}")
        elif node_class is not None and actual_class != node_class:
            self.fail(f"Node {node_id} NodeClass",
                      f"expected {node_class.name}, got {actual_class.name}")
        else:
            self.ok(f"{node_id} {actual_name.Name} ({actual_class.name})")
        return node

    async def check_value(self, node_id, expected, description):
        try:
            value = await self.client.get_node(node_id).read_value()
            if value != expected:
                self.fail(description, f"expected {expected!r}, got {value!r}")
            else:
                self.ok(f"{description} = {expected!r}")
        except Exception as exc:
            self.fail(description, f"{type(exc).__name__}: {exc}")

    async def check_type_definition(self, node_id, expected, description):
        try:
            actual = await self.client.get_node(node_id).read_type_definition()
            if actual != self.ua.NodeId.from_string(expected):
                self.fail(description, f"expected {expected}, got {actual.to_string()}")
            else:
                self.ok(f"{description} -> {expected}")
        except Exception as exc:
            self.fail(description, f"{type(exc).__name__}: {exc}")

    async def check_reference(self, source_id, reference_type_id, target_id,
                              description, forward=True):
        direction = (self.ua.BrowseDirection.Forward if forward
                     else self.ua.BrowseDirection.Inverse)
        try:
            refs = await self.client.get_node(source_id).get_references(
                refs=self.ua.NodeId.from_string(reference_type_id),
                direction=direction)
            targets = [ref.NodeId.to_string() for ref in refs]
            if target_id not in targets:
                self.fail(description, f"target {target_id} not in {targets}")
            else:
                self.ok(f"{description} -> {target_id}")
        except Exception as exc:
            self.fail(description, f"{type(exc).__name__}: {exc}")

    async def check_modelling_rule(self, node_id, rule_id, description):
        await self.check_reference(node_id, "i=37", rule_id, description)

    async def check_child_value(self, parent_id, path, expected, description):
        try:
            node = self.client.get_node(parent_id)
            child = await node.get_child(path)
            value = await child.read_value()
            if value != expected:
                self.fail(description, f"expected {expected!r}, got {value!r}")
            else:
                self.ok(f"{description} = {expected!r}")
            return child
        except Exception as exc:
            self.fail(description, f"{type(exc).__name__}: {exc}")
            return None


async def verify_model(args):
    opcua_common.load_asyncua()
    ua = opcua_common.ua
    client = opcua_common.Client(url=args.server_url, timeout=args.timeout)
    await opcua_common.configure_client_security(client, args)
    if args.username:
        client.set_user(args.username)
        client.set_password(args.password or "")

    print(f"Connecting to {args.server_url}...")
    await client.connect()
    try:
        namespaces = await client.get_namespace_array()
        if DI_URI not in namespaces or AMB_URI not in namespaces:
            missing = [uri for uri in (DI_URI, AMB_URI) if uri not in namespaces]
            raise RuntimeError(f"Required namespaces are missing: {missing}")
        di_ns = namespaces.index(DI_URI)
        amb_ns = namespaces.index(AMB_URI)
        print(f"DI namespace={di_ns}, AMB namespace={amb_ns}")
        audit = ModelAudit(client, ua, di_ns, amb_ns)

        # DI and AMB type declarations.
        await audit.check_node(f"ns={di_ns};i=1005", "FunctionalGroupType", ua.NodeClass.ObjectType)
        await audit.check_node(f"ns={di_ns};i=6244", "DeviceHealthEnumeration", ua.NodeClass.DataType)
        await audit.check_node(f"ns={di_ns};i=15739", "MaintenanceRequiredAlarmType", ua.NodeClass.ObjectType)
        await audit.check_node(f"ns={amb_ns};i=1012", "IMaintenanceEventType", ua.NodeClass.ObjectType)
        await audit.check_node(f"ns={amb_ns};i=1013", "MaintenanceEventStateMachineType", ua.NodeClass.ObjectType)
        await audit.check_reference(f"ns={di_ns};i=15739", "i=17603",
                                    f"ns={amb_ns};i=1012",
                                    "MaintenanceRequiredAlarmType HasInterface IMaintenanceEventType")

        # MaintenanceMethodEnum.
        enum_node = await audit.check_node(f"ns={amb_ns};i=6029", "EnumValues", ua.NodeClass.Variable)
        if enum_node is not None:
            try:
                values = await enum_node.read_value()
                actual = [(int(v.Value), v.DisplayName.Text) for v in values]
                expected = [(0, "Local"), (1, "Remote")]
                if actual == expected:
                    audit.ok("MaintenanceMethodEnum values are Local=0, Remote=1")
                else:
                    audit.fail("MaintenanceMethodEnum values", f"expected {expected}, got {actual}")
            except Exception as exc:
                audit.fail("MaintenanceMethodEnum values", f"{type(exc).__name__}: {exc}")

        # State machine states, numbers and transitions.
        states = ((5006, "Planned", 6021, 1),
                  (5007, "Executing", 6022, 2),
                  (5008, "Finished", 6023, 3))
        for state_id, name, number_id, number in states:
            await audit.check_node(f"ns={amb_ns};i={state_id}", name, ua.NodeClass.Object)
            await audit.check_value(f"ns={amb_ns};i={number_id}", number,
                                    f"{name}.StateNumber")
            await audit.check_modelling_rule(f"ns={amb_ns};i={number_id}", "i=78",
                                             f"{name}.StateNumber is Mandatory")

        transitions = (
            (5009, "FromPlannedToExecuting", 6024, 1, 5006, 5007),
            (5010, "FromExecutingToFinished", 6025, 2, 5007, 5008),
            (5011, "FromFinishedToPlanned", 6026, 3, 5008, 5006),
        )
        for trans_id, name, number_id, number, from_id, to_id in transitions:
            node_id = f"ns={amb_ns};i={trans_id}"
            await audit.check_node(node_id, name, ua.NodeClass.Object)
            await audit.check_value(f"ns={amb_ns};i={number_id}", number,
                                    f"{name}.TransitionNumber")
            await audit.check_modelling_rule(node_id, "i=78", f"{name} is Mandatory")
            await audit.check_reference(node_id, "i=51", f"ns={amb_ns};i={from_id}",
                                        f"{name}.FromState")
            await audit.check_reference(node_id, "i=52", f"ns={amb_ns};i={to_id}",
                                        f"{name}.ToState")

        optional_fields = ((6035, "PlannedDate"), (6036, "EstimatedDowntime"),
                           (6041, "MaintenanceMethod"), (6042, "ConfigurationChanged"))
        for node_id, name in optional_fields:
            await audit.check_node(f"ns={amb_ns};i={node_id}", name, ua.NodeClass.Variable)
            await audit.check_modelling_rule(f"ns={amb_ns};i={node_id}", "i=80",
                                             f"IMaintenanceEventType.{name} is Optional")

        # Asset discovery, locations and standard-facing device groups.
        for node_id, name in ((5002, "Assets"),
                              (5003, "AssetsByProductInstanceUri"),
                              (5004, "AssetsByAssetId"),
                              (5021, "HierarchicalLocations"),
                              (5022, "OperationalLocations")):
            await audit.check_node(f"ns={amb_ns};i={node_id}", name, ua.NodeClass.Object)
        await audit.check_node("ns=1;s=Device.Capabilities", "Capabilities", ua.NodeClass.Object)
        await audit.check_node("ns=1;s=Device.Requirements", "Requirements", ua.NodeClass.Object)
        await audit.check_type_definition("ns=1;s=Device.Identification",
                                          f"ns={di_ns};i=1005",
                                          "Identification uses DI FunctionalGroupType")

        # Runtime maintenance condition.
        condition_id = "ns=1;s=Device.DeviceHealthAlarms.MaintenanceRequired"
        await audit.check_node(condition_id, "MaintenanceRequired", ua.NodeClass.Object)
        await audit.check_type_definition(condition_id, f"ns={di_ns};i=15739",
                                          "MaintenanceRequired condition type")
        await audit.check_child_value(condition_id,
                                      [f"{amb_ns}:MaintenanceState", "0:CurrentState"],
                                      ua.LocalizedText("Finished", "en-US"),
                                      "MaintenanceRequired.CurrentState")
        state_id_node = await audit.check_child_value(
            condition_id,
            [f"{amb_ns}:MaintenanceState", "0:CurrentState", "0:Id"],
            ua.NodeId(5008, amb_ns), "MaintenanceRequired.CurrentState.Id")
        del state_id_node
        for suffix, expected in (("PlannedDate", None),
                                 ("EstimatedDowntime", 0.0),
                                 ("MaintenanceMethod", 0),
                                 ("ConfigurationChanged", False)):
            node_id = f"ns=1;s=Device.DeviceHealthAlarms.MaintenanceRequired.{suffix}"
            node = await audit.check_node(node_id, suffix, ua.NodeClass.Variable)
            if node is not None and expected is not None:
                await audit.check_value(node_id, expected, f"MaintenanceRequired.{suffix}")
        for suffix in ("MaintenanceSupplier",
                       "QualificationOfPersonnel",
                       "PartsOfAssetReplaced",
                       "PartsOfAssetServiced"):
            node_id = f"ns=1;s=Device.DeviceHealthAlarms.MaintenanceRequired.{suffix}"
            await audit.check_node(node_id, suffix, ua.NodeClass.Variable)
        await audit.check_child_value(condition_id, ["0:ConditionClassId"],
                                      ua.NodeId(1016, amb_ns),
                                      "MaintenanceRequired.ConditionClassId")

        print()
        print(f"Checks passed: {audit.passed}")
        print(f"Checks failed: {len(audit.failures)}")
        if audit.failures:
            print("Failures:")
            for failure in audit.failures:
                print(f"  - {failure}")
            return 1
        print("DI/AMB model verification succeeded")
        return 0
    finally:
        await client.disconnect()


def parse_args():
    parser = argparse.ArgumentParser(
        description="Verify the embedded OPC UA DI/AMB model without modifying the server")
    parser.add_argument("server_url", help="OPC UA endpoint URL")
    parser.add_argument("--username", default=os.getenv("OPCUA_USERNAME"))
    parser.add_argument("--password", default=os.getenv("OPCUA_PASSWORD"))
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--client-cert")
    parser.add_argument("--client-key")
    parser.add_argument("--server-cert")
    parser.add_argument("--security-policy", default="Basic256Sha256",
                        choices=["Basic256Sha256"])
    parser.add_argument("--security-mode", default="SignAndEncrypt",
                        choices=["Sign", "SignAndEncrypt"])
    parser.add_argument("--no-auto-client-cert", action="store_true")
    return parser.parse_args()


def main():
    args = parse_args()
    if args.username and args.password is None:
        args.password = getpass.getpass(f"Password for {args.username}: ")
    try:
        raise SystemExit(asyncio.run(verify_model(args)))
    except KeyboardInterrupt:
        raise SystemExit(130)
    except Exception as exc:
        print(f"ERROR: {type(exc).__name__}: {exc}")
        raise SystemExit(1)


if __name__ == "__main__":
    main()
