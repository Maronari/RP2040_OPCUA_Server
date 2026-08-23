#!/usr/bin/env python3
"""Local asyncua client/server integration for the v0.4.2 channel verifier."""

from __future__ import annotations

import argparse
import asyncio
from contextlib import suppress
import pathlib
import socket
import sys
import unittest

from asyncua import Server, ua

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import stage_config as opcua_common  # noqa: E402
from verify_channel_model import (  # noqa: E402
    CHANNEL_NAMESPACE_URI,
    FIELD_TYPES,
    channel_node_id,
    verify,
)


def free_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


async def add_variable(parent, nodeid: ua.NodeId, browse_name: str, value, variant_type, *,
                       writable: bool = False, datatype_nodeid: ua.NodeId | None = None):
    node = await parent.add_variable(
        nodeid,
        ua.QualifiedName(browse_name, nodeid.NamespaceIndex),
        ua.Variant(value, variant_type),
        datatype=datatype_nodeid,
    )
    if writable:
        await node.set_writable()
    return node


async def build_channel(server: Server, channels_node, channel_ns: int, channel_id: str, *,
                        value, variant_type, writable: bool) -> None:
    channel_node = await channels_node.add_object(
        ua.NodeId(channel_node_id(channel_id), channel_ns),
        ua.QualifiedName(channel_id, channel_ns),
    )
    datatype = {
        ua.VariantType.Double: ua.NodeId(ua.ObjectIds.Double),
        ua.VariantType.UInt32: ua.NodeId(ua.ObjectIds.UInt32),
    }[variant_type]
    value_node = await add_variable(
        channel_node,
        ua.NodeId(channel_node_id(channel_id, "Value"), channel_ns),
        "Value",
        value,
        variant_type,
        writable=writable,
        datatype_nodeid=datatype,
    )
    for field, expected_type in FIELD_TYPES.items():
        field_variant = {
            "String": ("metadata", ua.VariantType.String),
            "UInt32": (1, ua.VariantType.UInt32),
            "Boolean": (field == "Writable" and writable, ua.VariantType.Boolean),
        }[expected_type]
        await add_variable(
            channel_node,
            ua.NodeId(channel_node_id(channel_id, field), channel_ns),
            field,
            field_variant[0],
            field_variant[1],
            datatype_nodeid={
                "String": ua.NodeId(ua.ObjectIds.String),
                "UInt32": ua.NodeId(ua.ObjectIds.UInt32),
                "Boolean": ua.NodeId(ua.ObjectIds.Boolean),
            }[expected_type],
        )
    with suppress(Exception):
        await value_node.write_value(ua.Variant(value, variant_type))


class VerifyChannelModelE2E(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.endpoint = f"opc.tcp://127.0.0.1:{free_tcp_port()}/"
        self.server = Server()
        await self.server.init()
        self.server.set_endpoint(self.endpoint)
        self.server.set_server_name("v0.4.2 verifier e2e")

        objects = self.server.nodes.objects
        device = await objects.add_object(ua.NodeId("Device", 1), ua.QualifiedName("Device", 1))
        identification = await device.add_object(
            ua.NodeId("Device.Identification", 1),
            ua.QualifiedName("Identification", 1),
        )
        diagnostics = await device.add_object(
            ua.NodeId("Device.Diagnostics", 1),
            ua.QualifiedName("Diagnostics", 1),
        )
        psram = await diagnostics.add_object(
            ua.NodeId("Device.Diagnostics.PSRAM", 1),
            ua.QualifiedName("PSRAM", 1),
        )
        task = await diagnostics.add_object(
            ua.NodeId("Device.Diagnostics.Task", 1),
            ua.QualifiedName("Task", 1),
        )
        for name, value in (
            ("DeviceName", "RP2 Basic Device"),
            ("Model", "VerifierFixture"),
            ("SerialNumber", "fixture-001"),
            ("HardwareRevision", "hw-fixture"),
            ("SoftwareRevision", "0.4.2"),
            ("GitHash", "fixture"),
            ("MacAddress", "00:11:22:33:44:55"),
        ):
            await add_variable(
                identification,
                ua.NodeId(f"Device.Identification.{name}", 1),
                name,
                value,
                ua.VariantType.String,
                datatype_nodeid=ua.NodeId(ua.ObjectIds.String),
            )
        for name, value in (
            ("Ready", True),
            ("TotalBytes", 1024),
            ("UsedBytes", 512),
            ("FreeBytes", 512),
            ("LargestFreeBlockBytes", 256),
        ):
            await add_variable(
                psram,
                ua.NodeId(f"Device.Diagnostics.PSRAM.{name}", 1),
                name,
                value,
                ua.VariantType.Boolean if isinstance(value, bool) else ua.VariantType.UInt32,
                datatype_nodeid=ua.NodeId(
                    ua.ObjectIds.Boolean if isinstance(value, bool) else ua.ObjectIds.UInt32
                ),
            )
        for task_name in ("OPC", "SPI", "TEMP"):
            task_node = await task.add_object(
                ua.NodeId(f"Device.Diagnostics.Task.{task_name}", 1),
                ua.QualifiedName(task_name, 1),
            )
            await add_variable(
                task_node,
                ua.NodeId(f"Device.Diagnostics.Task.{task_name}.StackHighWatermark", 1),
                "StackHighWatermark",
                128,
                ua.VariantType.UInt32,
                datatype_nodeid=ua.NodeId(ua.ObjectIds.UInt32),
            )

        channels = await device.add_object(
            ua.NodeId("Device.Channels", 1),
            ua.QualifiedName("Channels", 1),
        )
        channel_ns = await self.server.register_namespace(CHANNEL_NAMESPACE_URI)
        await build_channel(
            self.server,
            channels,
            channel_ns,
            "channel.analog",
            value=12.5,
            variant_type=ua.VariantType.Double,
            writable=False,
        )
        await build_channel(
            self.server,
            channels,
            channel_ns,
            "channel.loopback",
            value=7,
            variant_type=ua.VariantType.UInt32,
            writable=True,
        )

        async def noop_security(client, args):
            await client.set_security_string("")
            return None

        self._orig_configure = opcua_common.configure_client_security
        opcua_common.configure_client_security = noop_security
        await self.server.start()

    async def asyncTearDown(self) -> None:
        await self.server.stop()
        opcua_common.configure_client_security = self._orig_configure

    async def test_live_client_verifier_covers_mixed_types_and_writes(self) -> None:
        args = argparse.Namespace(
            endpoint=self.endpoint,
            username=None,
            password=None,
            timeout=5.0,
            client_cert=None,
            client_key=None,
            server_cert=None,
            security_policy="Basic256Sha256",
            security_mode="SignAndEncrypt",
            no_auto_client_cert=True,
            expect_channel=["channel.analog", "channel.loopback"],
            expect_value_type=[
                ("channel.analog", "Double"),
                ("channel.loopback", "UInt32"),
            ],
            require_mixed_value_types=True,
            expect_writable_channel=["channel.loopback"],
            expected_software_revision="0.4.2",
            expected_device_name="RP2 Basic Device",
            expect_legacy_temperature=None,
            verify_history=False,
            verify_read_only_write=True,
            verify_wrong_type_write=True,
            second_username=None,
            second_password="",
            verify_unauthorized_writable_write=False,
        )
        await verify(args)


if __name__ == "__main__":
    unittest.main()
