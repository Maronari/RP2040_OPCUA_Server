#!/usr/bin/env python3
"""
Export OPC UA nodes from a live server into a NodeSet2 XML file.

Examples:
  python export_nodeset.py opc.tcp://192.168.1.100:4840 1
  python export_nodeset.py opc.tcp://192.168.1.100:4840 1 --username admin
  python export_nodeset.py opc.tcp://192.168.1.100:4840 1 --username admin --password secret
"""

import argparse
import asyncio
import getpass
import os
import socket
import sys
import tempfile
from datetime import datetime
from pathlib import Path

try:
    from asyncua import Client
    from asyncua import ua
    from asyncua.common.xmlexporter import XmlExporter
    from asyncua.crypto import security_policies
    from asyncua.crypto.cert_gen import setup_self_signed_certificate
    from cryptography.x509.oid import ExtendedKeyUsageOID
except ImportError:
    print("ERROR: asyncua library not installed")
    print("Install with: C:\\Python312\\python.exe -m pip install --user asyncua")
    sys.exit(1)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Export a namespace from an OPC UA server into NodeSet2 XML."
    )
    parser.add_argument("server_url", help="OPC UA endpoint, e.g. opc.tcp://192.168.1.100:4840")
    parser.add_argument(
        "namespace_index",
        nargs="?",
        type=int,
        default=1,
        help="Namespace index to export (default: 1)",
    )
    parser.add_argument(
        "--username",
        default=os.getenv("OPCUA_USERNAME"),
        help="Username for authenticated session",
    )
    parser.add_argument(
        "--password",
        default=os.getenv("OPCUA_PASSWORD"),
        help="Password for authenticated session",
    )
    parser.add_argument("--output", help="Output XML path")
    parser.add_argument(
        "--timeout",
        type=float,
        default=30.0,
        help="Client timeout in seconds (default: 30)",
    )
    parser.add_argument(
        "--max-depth",
        type=int,
        default=64,
        help="Maximum browse depth from Root (default: 64)",
    )
    parser.add_argument(
        "--export-values",
        action="store_true",
        help="Also export variable values when possible",
    )
    parser.add_argument(
        "--client-cert",
        help="Path to client certificate for secure endpoint login (.der or .pem)",
    )
    parser.add_argument(
        "--client-key",
        help="Path to client private key for secure endpoint login (.pem or .der)",
    )
    parser.add_argument(
        "--server-cert",
        help="Path to server certificate (.der or .pem). Defaults to static/certs/server.crt.der if present.",
    )
    parser.add_argument(
        "--security-policy",
        default="Basic256Sha256",
        choices=["Basic256Sha256"],
        help="Security policy for authenticated export (default: Basic256Sha256)",
    )
    parser.add_argument(
        "--security-mode",
        default="SignAndEncrypt",
        choices=["Sign", "SignAndEncrypt"],
        help="Security mode for authenticated export (default: SignAndEncrypt)",
    )
    parser.add_argument(
        "--no-auto-client-cert",
        action="store_true",
        help="Do not auto-generate a temporary client certificate when username auth is used.",
    )
    return parser.parse_args()


def make_output_path(namespace_index, output_path=None):
    if output_path:
        return output_path

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    return f"exported_ns{namespace_index}_{timestamp}.xml"


def node_label(node):
    return node.nodeid.to_string()


def resolve_default_server_cert():
    candidate = Path(__file__).resolve().parent.parent / "certs" / "server.crt.der"
    return candidate if candidate.is_file() else None


async def ensure_client_certificate(cert_path, key_path, app_uri):
    cert_path.parent.mkdir(parents=True, exist_ok=True)
    key_path.parent.mkdir(parents=True, exist_ok=True)
    host_name = socket.gethostname() or "localhost"
    await setup_self_signed_certificate(
        key_path,
        cert_path,
        app_uri=app_uri,
        host_name=host_name,
        cert_use=[ExtendedKeyUsageOID.CLIENT_AUTH],
        subject_attrs={"organizationName": "RP2040_OPCUA_Server"},
    )


async def configure_client_security(client, args):
    if not args.username:
        return None

    client_cert = Path(args.client_cert) if args.client_cert else None
    client_key = Path(args.client_key) if args.client_key else None
    server_cert = Path(args.server_cert) if args.server_cert else resolve_default_server_cert()

    if (client_cert is None) != (client_key is None):
        raise ValueError("Use both --client-cert and --client-key together")

    if client_cert is None and not args.no_auto_client_cert:
        base_dir = Path(tempfile.gettempdir()) / "rp2040_opcua_export"
        client_cert = base_dir / "client_cert.der"
        client_key = base_dir / "client_key.pem"
        client_app_uri = getattr(client, "application_uri", "urn:freeopcua:client")
        await ensure_client_certificate(client_cert, client_key, client_app_uri)
        print(f"Using temporary client certificate: {client_cert}")

    if client_cert is None or client_key is None:
        raise ValueError(
            "Username authentication requires a secure endpoint. "
            "Provide --client-cert and --client-key, or omit --no-auto-client-cert."
        )

    policy_class = getattr(security_policies, f"SecurityPolicy{args.security_policy}")
    mode = getattr(ua.MessageSecurityMode, args.security_mode)

    await client.set_security(
        policy_class,
        certificate=client_cert,
        private_key=client_key,
        server_certificate=server_cert,
        mode=mode,
    )

    print(f"Using secure channel: {args.security_policy}/{args.security_mode}")
    if server_cert is not None:
        print(f"Using server certificate: {server_cert}")


async def collect_namespace_nodes(client, namespace_index, max_depth):
    """Collect namespace nodes reachable from Root via hierarchical references."""

    namespace_array = await client.get_namespace_array()
    if namespace_index >= len(namespace_array):
        raise ValueError(f"Namespace {namespace_index} does not exist")

    namespace_uri = namespace_array[namespace_index]
    print(f"Namespace URI: {namespace_uri}")
    print("Browsing from Root folder...")

    visited = set()
    exported = {}
    stack = [(client.nodes.root, 0)]

    while stack:
        node, depth = stack.pop()
        key = node_label(node)
        if key in visited:
            continue
        visited.add(key)

        if node.nodeid.NamespaceIndex == namespace_index:
            exported[key] = node

        if depth >= max_depth:
            continue

        try:
            children = await node.get_children(refs=ua.ObjectIds.HierarchicalReferences)
        except Exception as exc:
            print(f"  Warning: could not browse children of {key}: {exc}")
            continue

        for child in reversed(children):
            child_key = node_label(child)
            if child_key not in visited:
                stack.append((child, depth + 1))

    print(f"Found {len(exported)} namespace nodes reachable from Root")
    return namespace_uri, list(exported.values())


async def export_namespace_nodes(client, namespace_index, output_path, max_depth, export_values):
    namespace_uri, nodes = await collect_namespace_nodes(client, namespace_index, max_depth)

    if not nodes:
        print("Warning: no nodes were found in the requested namespace")
        return None

    exporter = XmlExporter(client, export_values=export_values)
    await exporter.build_etree(nodes)
    await exporter.write_xml(output_path, pretty=True)

    print(f"Exported namespace {namespace_index} ({namespace_uri})")
    print(f"Nodeset exported to: {output_path}")
    return output_path


async def main_async():
    args = parse_args()

    if args.username and args.password is None:
        args.password = getpass.getpass(f"Password for {args.username}: ")

    output_path = make_output_path(args.namespace_index, args.output)

    print(f"Connecting to {args.server_url}...")
    client = Client(url=args.server_url, timeout=args.timeout)

    await configure_client_security(client, args)

    if args.username:
        client.set_user(args.username)
        client.set_password(args.password or "")
        print(f"Using authenticated session for user: {args.username}")

    connected = False
    try:
        await client.connect()
        connected = True
        print("Connected!")

        await export_namespace_nodes(
            client,
            args.namespace_index,
            output_path,
            args.max_depth,
            args.export_values,
        )
    except Exception as exc:
        print(f"ERROR: {exc}")
        import traceback

        traceback.print_exc()
        sys.exit(1)
    finally:
        if connected:
            await client.disconnect()
            print("Disconnected")


def main():
    asyncio.run(main_async())


if __name__ == "__main__":
    main()

