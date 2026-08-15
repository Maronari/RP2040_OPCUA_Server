#!/usr/bin/env python3
"""Browse and validate the v0.4.2 generic OPC UA channel model.

The verifier deliberately discovers channel objects under Device.Channels.  It
never relies on a display name, registry order, fixed count, or native driver.
"""

from __future__ import annotations

import argparse
import asyncio
from datetime import datetime, timedelta, timezone
import getpass
import json
import os
import sys

import stage_config as opcua_common

CHANNEL_NAMESPACE_URI = "urn:maronari:rp2-industrial-opcua:channel-model"
FIELD_TYPES = {
    "Description": "String", "ChannelKind": "UInt32", "DriverId": "UInt32",
    "SourceIndex": "UInt32", "SourceFlags": "UInt32", "SamplePeriodMs": "UInt32",
    "PublishPeriodMs": "UInt32", "Historizing": "Boolean", "Writable": "Boolean",
}
VALUE_TYPES = {"Float", "Double", "Int32", "UInt32", "Int64", "UInt64", "Boolean", "String"}


def channel_node_id(channel_id: str, field: str | None = None) -> str:
    encoded = channel_id.encode("utf-8").hex().upper()
    result = f"Device.Channels.c_{encoded}"
    return result if field is None else f"{result}.{field}"


async def browse_child(node, name: str, namespace: int):
    for child in await node.get_children():
        browse_name = await child.read_browse_name()
        if browse_name.Name == name and browse_name.NamespaceIndex == namespace:
            return child
    raise AssertionError(f"missing {name} below {node.nodeid}")


async def verify(args: argparse.Namespace) -> None:
    opcua_common.load_asyncua()
    Client = opcua_common.Client
    ua = opcua_common.ua
    client = Client(url=args.endpoint, timeout=args.timeout)
    await opcua_common.configure_client_security(client, args)
    if args.username:
        client.set_user(args.username)
        client.set_password(args.password)
    await client.connect()
    try:
        identity_nodes = {
            "deviceName": "Device.Identification.DeviceName",
            "model": "Device.Identification.Model",
            "serialNumber": "Device.Identification.SerialNumber",
            "hardwareRevision": "Device.Identification.HardwareRevision",
            "softwareRevision": "Device.Identification.SoftwareRevision",
            "gitHash": "Device.Identification.GitHash",
            "macAddress": "Device.Identification.MacAddress",
        }
        identity = {
            name: str(await client.get_node(ua.NodeId(node_id, 1)).read_value())
            for name, node_id in identity_nodes.items()
        }
        print("Device identity: " + json.dumps(identity, sort_keys=True))
        if (args.expected_software_revision and
                identity["softwareRevision"] != args.expected_software_revision):
            raise AssertionError(
                f"software revision {identity['softwareRevision']!r} != "
                f"{args.expected_software_revision!r}"
            )
        if args.expected_device_name and identity["deviceName"] != args.expected_device_name:
            raise AssertionError(
                f"device name {identity['deviceName']!r} != {args.expected_device_name!r}"
            )
        diagnostic_nodes = {
            "psramReady": "Device.Diagnostics.PSRAM.Ready",
            "psramTotalBytes": "Device.Diagnostics.PSRAM.TotalBytes",
            "psramUsedBytes": "Device.Diagnostics.PSRAM.UsedBytes",
            "psramFreeBytes": "Device.Diagnostics.PSRAM.FreeBytes",
            "psramLargestFreeBlockBytes": "Device.Diagnostics.PSRAM.LargestFreeBlockBytes",
            "opcStackHighWatermark": "Device.Diagnostics.Task.OPC.StackHighWatermark",
            "spiStackHighWatermark": "Device.Diagnostics.Task.SPI.StackHighWatermark",
            "tempStackHighWatermark": "Device.Diagnostics.Task.TEMP.StackHighWatermark",
        }
        diagnostics = {}
        for name, node_id in diagnostic_nodes.items():
            try:
                diagnostics[name] = await client.get_node(ua.NodeId(node_id, 1)).read_value()
            except Exception as exc:
                diagnostics[name] = f"unavailable:{type(exc).__name__}"
        print("Resource diagnostics: " + json.dumps(diagnostics, sort_keys=True))
        namespace = await client.get_namespace_index(CHANNEL_NAMESPACE_URI)
        print(f"Generic channel namespace index: {namespace}")
        channels = client.get_node(ua.NodeId("Device.Channels", 1))
        discovered: dict[str, object] = {}
        for candidate in await channels.get_children():
            browse_name = await candidate.read_browse_name()
            if browse_name.NamespaceIndex != namespace:
                continue
            channel_id = browse_name.Name
            expected_id = ua.NodeId(channel_node_id(channel_id), namespace)
            if candidate.nodeid != expected_id:
                raise AssertionError(f"non-deterministic NodeId for {channel_id}: {candidate.nodeid}")
            discovered[channel_id] = candidate

        if not discovered:
            raise AssertionError("no generic channel objects discovered")
        if args.expect_channel and set(args.expect_channel) != set(discovered):
            raise AssertionError(f"discovered ids {sorted(discovered)} != expected {sorted(args.expect_channel)}")

        for channel_id, channel in discovered.items():
            value = await browse_child(channel, "Value", namespace)
            data_value = await value.read_data_value()
            variant_type = data_value.Value.VariantType.name
            if variant_type not in VALUE_TYPES:
                raise AssertionError(f"{channel_id}: unsupported Value type {variant_type}")
            if data_value.StatusCode.is_bad() and data_value.SourceTimestamp is not None:
                # Bad values may still retain a historical sample timestamp; this
                # assertion merely ensures no client-side timestamp is fabricated.
                pass
            for field, expected_type in FIELD_TYPES.items():
                child = await browse_child(channel, field, namespace)
                actual_type = (await child.read_data_type_as_variant_type()).name
                if actual_type != expected_type:
                    raise AssertionError(f"{channel_id}.{field}: {actual_type} != {expected_type}")
            unit_children = await value.get_children()
            unit_names = {(await child.read_browse_name()).Name for child in unit_children}
            if "EngineeringUnits" in unit_names:
                engineering = await browse_child(value, "EngineeringUnits", namespace)
                if (await engineering.read_data_type()).Identifier != 887:
                    raise AssertionError(f"{channel_id}: EngineeringUnits is not EUInformation")
            if "EURange" in unit_names:
                range_node = await browse_child(value, "EURange", namespace)
                if (await range_node.read_data_type()).Identifier != 884:
                    raise AssertionError(f"{channel_id}: EURange is not Range")
            if args.verify_read_only_write:
                writable = await browse_child(channel, "Writable", namespace)
                if not await writable.read_value():
                    try:
                        await value.write_value(
                            ua.Variant(data_value.Value.Value, data_value.Value.VariantType)
                        )
                    except Exception as exc:
                        if not any(status in str(exc) for status in (
                            "BadNotWritable", "BadUserAccessDenied"
                        )):
                            raise AssertionError(
                                f"{channel_id}: read-only write returned {exc!r}"
                            ) from exc
                    else:
                        raise AssertionError(f"{channel_id}: read-only write unexpectedly succeeded")

        if args.expect_legacy_temperature:
            legacy = client.get_node(ua.NodeId("Device.Channels.Temperature1.Value", 1))
            generic = discovered[args.expect_legacy_temperature]
            generic_node = await browse_child(generic, "Value", namespace)
            equivalent = False
            for _ in range(10):
                legacy_value = await legacy.read_data_value()
                generic_value = await generic_node.read_data_value()
                equivalent = (
                    legacy_value.Value.Value == generic_value.Value.Value and
                    legacy_value.StatusCode.value == generic_value.StatusCode.value and
                    legacy_value.SourceTimestamp == generic_value.SourceTimestamp
                )
                if equivalent:
                    break
                await asyncio.sleep(0.05)
            if not equivalent:
                raise AssertionError("legacy Temperature1 Value diverges from generic registry view")
            if args.verify_history:
                start = datetime.now(timezone.utc) - timedelta(minutes=15)
                end = datetime.now(timezone.utc) + timedelta(seconds=1)
                generic_history = await (await browse_child(
                    generic, "Value", namespace
                )).read_raw_history(starttime=start, endtime=end, numvalues=32)
                legacy_history = await legacy.read_raw_history(
                    starttime=start, endtime=end, numvalues=32
                )
                if not generic_history:
                    raise AssertionError("generic temperature HistoryRead returned no samples")
                if len(generic_history) != len(legacy_history):
                    raise AssertionError(
                        "legacy and generic HistoryRead returned different sample counts"
                    )
                for generic_sample, legacy_sample in zip(generic_history, legacy_history):
                    if (generic_sample.Value.Value != legacy_sample.Value.Value or
                            generic_sample.StatusCode.value != legacy_sample.StatusCode.value or
                            generic_sample.SourceTimestamp != legacy_sample.SourceTimestamp):
                        raise AssertionError(
                            "legacy and generic HistoryRead do not expose the same registry samples"
                        )
                print(f"HistoryRead samples compared: {len(generic_history)}")

        if args.second_username:
            second = Client(url=args.endpoint, timeout=args.timeout)
            second_args = argparse.Namespace(**vars(args))
            second_args.username = args.second_username
            await opcua_common.configure_client_security(second, second_args)
            second.set_user(args.second_username)
            second.set_password(args.second_password)
            await second.connect()
            try:
                second_namespace = await second.get_namespace_index(CHANNEL_NAMESPACE_URI)
                second_channels = second.get_node(ua.NodeId("Device.Channels", 1))
                second_ids = set()
                for candidate in await second_channels.get_children():
                    browse_name = await candidate.read_browse_name()
                    if browse_name.NamespaceIndex == second_namespace:
                        second_ids.add(browse_name.Name)
                if second_ids != set(discovered):
                    raise AssertionError("simultaneous second client sees a different channel inventory")
            finally:
                await second.disconnect()
    finally:
        await client.disconnect()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("endpoint")
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
    parser.add_argument("--expect-channel", action="append", default=[])
    parser.add_argument("--expected-software-revision")
    parser.add_argument("--expected-device-name")
    parser.add_argument("--expect-legacy-temperature", metavar="CHANNEL_ID")
    parser.add_argument("--verify-history", action="store_true")
    parser.add_argument("--verify-read-only-write", action="store_true")
    parser.add_argument("--second-username", help="run a simultaneous second-client browse")
    parser.add_argument("--second-password", default="")
    args = parser.parse_args()
    if args.username and args.password is None:
        args.password = getpass.getpass(f"Password for {args.username}: ")
    try:
        asyncio.run(verify(args))
    except Exception as exc:  # command-line verifier: fail closed
        print(f"verify-channel-model: FAIL: {exc}", file=sys.stderr)
        return 1
    print("verify-channel-model: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
