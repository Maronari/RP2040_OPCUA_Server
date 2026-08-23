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

BROWSE_REFERENCES_PER_PAGE = 32
MAX_BROWSE_PAGES_PER_NODE = 256
READ_VALUES_PER_REQUEST = 64
CLIENT_APPLICATION_URI = "urn:rp2040-opcua:verification-client"

try:
    from asyncua import Client
    from asyncua import ua
    from asyncua.common.node import Node
    from asyncua.common.xmlexporter import XmlExporter
    from asyncua.crypto import security_policies
    from asyncua.crypto.cert_gen import setup_self_signed_certificate
    from cryptography.x509.oid import ExtendedKeyUsageOID
except ImportError:
    print("ERROR: asyncua library not installed")
    print("Install with: C:\\Python312\\python.exe -m pip install --user asyncua")
    sys.exit(1)


COMMON_EXPORT_ATTRIBUTES = (
    ua.AttributeIds.BrowseName,
    ua.AttributeIds.DisplayName,
    ua.AttributeIds.Description,
)

CLASS_EXPORT_ATTRIBUTES = {
    ua.NodeClass.Object: (ua.AttributeIds.EventNotifier,),
    ua.NodeClass.Variable: (
        ua.AttributeIds.DataType,
        ua.AttributeIds.ValueRank,
        ua.AttributeIds.ArrayDimensions,
        ua.AttributeIds.AccessLevel,
        ua.AttributeIds.UserAccessLevel,
        ua.AttributeIds.MinimumSamplingInterval,
        ua.AttributeIds.Historizing,
    ),
    ua.NodeClass.Method: (
        ua.AttributeIds.Executable,
        ua.AttributeIds.UserExecutable,
    ),
    ua.NodeClass.ObjectType: (ua.AttributeIds.IsAbstract,),
    ua.NodeClass.VariableType: (
        ua.AttributeIds.DataType,
        ua.AttributeIds.ValueRank,
        ua.AttributeIds.ArrayDimensions,
        ua.AttributeIds.IsAbstract,
    ),
    ua.NodeClass.ReferenceType: (ua.AttributeIds.InverseName,),
    ua.NodeClass.DataType: (ua.AttributeIds.DataTypeDefinition,),
}


def bad_attribute_data_value():
    status = ua.StatusCode(ua.StatusCodes.BadAttributeIdInvalid)
    try:
        return ua.DataValue(StatusCode=status)
    except TypeError:
        return ua.DataValue(StatusCode_=status)


class CachedExportNode(Node):
    def __init__(self, source, attributes, references):
        super().__init__(source.session, source.nodeid)
        self._export_attributes = attributes
        self._export_references = references

    async def read_attribute(self, attr, indexrange=None, raise_on_bad_status=True):
        del indexrange
        result = self._export_attributes.get(attr)
        if result is None:
            result = bad_attribute_data_value()
        if raise_on_bad_status:
            result.StatusCode.check()
        return result

    async def get_references(
        self,
        refs=ua.ObjectIds.References,
        direction=ua.BrowseDirection.Both,
        nodeclassmask=ua.NodeClass.Unspecified,
        includesubtypes=True,
        result_mask=ua.BrowseResultMask.All,
    ):
        del nodeclassmask, includesubtypes, result_mask
        if refs == ua.ObjectIds.References and direction == ua.BrowseDirection.Both:
            return self._export_references
        if refs == ua.ObjectIds.HierarchicalReferences and direction == ua.BrowseDirection.Inverse:
            hierarchical_types = {
                ua.ObjectIds.Organizes,
                ua.ObjectIds.HasEventSource,
                ua.ObjectIds.HasSubtype,
                ua.ObjectIds.HasProperty,
                ua.ObjectIds.HasComponent,
                ua.ObjectIds.HasNotifier,
                ua.ObjectIds.HasOrderedComponent,
            }
            return [
                reference for reference in self._export_references
                if not reference.IsForward
                and reference.ReferenceTypeId.NamespaceIndex == 0
                and reference.ReferenceTypeId.Identifier in hierarchical_types
            ]
        raise RuntimeError(f"Unsupported cached reference query for {node_label(self)}")


class CachedXmlExporter(XmlExporter):
    async def add_etree_variable_type(self, node):
        element = await self._add_node_common("UAVariableType", node)
        await self.add_variable_common(node, element, self._export_values)
        abstract = await node.read_attribute(ua.AttributeIds.IsAbstract)
        if abstract.Value.Value:
            element.attrib["IsAbstract"] = "true"


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
        help="Maximum browse depth from the selected browse root (default: 64)",
    )
    parser.add_argument(
        "--start-node",
        help="Optional NodeId to use as the bounded browse root (default: Root)",
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


async def get_bounded_references(
    client,
    node,
    reference_type,
    direction,
):
    description = ua.BrowseDescription()
    description.NodeId = node.nodeid
    description.BrowseDirection = direction
    description.ReferenceTypeId = ua.NodeId(reference_type)
    description.IncludeSubtypes = True
    description.NodeClassMask = ua.NodeClass.Unspecified
    description.ResultMask = ua.BrowseResultMask.All

    parameters = ua.BrowseParameters()
    parameters.View.Timestamp = ua.get_win_epoch()
    parameters.RequestedMaxReferencesPerNode = BROWSE_REFERENCES_PER_PAGE
    parameters.NodesToBrowse.append(description)

    results = await client.uaclient.browse(parameters)
    references = []
    pages = 0
    while results:
        result = results[0]
        result.StatusCode.check()
        references.extend(result.References)
        continuation_point = result.ContinuationPoint
        if not continuation_point:
            return references

        pages += 1
        if pages >= MAX_BROWSE_PAGES_PER_NODE:
            release = ua.BrowseNextParameters()
            release.ReleaseContinuationPoints = True
            release.ContinuationPoints = [continuation_point]
            await client.uaclient.browse_next(release)
            raise RuntimeError(
                f"Browse page limit exceeded for {node_label(node)} "
                f"({MAX_BROWSE_PAGES_PER_NODE} pages)"
            )

        browse_next = ua.BrowseNextParameters()
        browse_next.ReleaseContinuationPoints = False
        browse_next.ContinuationPoints = [continuation_point]
        results = await client.uaclient.browse_next(browse_next)

    return references


async def get_bounded_hierarchical_children(client, node):
    references = await get_bounded_references(
        client,
        node,
        ua.ObjectIds.HierarchicalReferences,
        ua.BrowseDirection.Forward,
    )
    return [client.get_node(reference.NodeId) for reference in references]


async def read_export_attributes(client, reads, attribute_cache):
    for offset in range(0, len(reads), READ_VALUES_PER_REQUEST):
        batch = reads[offset:offset + READ_VALUES_PER_REQUEST]
        parameters = ua.ReadParameters()
        for _, node, attribute in batch:
            item = ua.ReadValueId()
            item.NodeId = node.nodeid
            item.AttributeId = attribute
            parameters.NodesToRead.append(item)
        try:
            results = await client.uaclient.read(parameters)
        except Exception as exc:
            first_node_index, first_node, first_attribute = batch[0]
            last_node_index, last_node, last_attribute = batch[-1]
            del first_node_index, last_node_index
            raise RuntimeError(
                f"Read batch {offset // READ_VALUES_PER_REQUEST + 1} failed: "
                f"{node_label(first_node)}/{first_attribute.name} through "
                f"{node_label(last_node)}/{last_attribute.name}: {exc}"
            ) from exc
        if len(results) != len(batch):
            raise RuntimeError(
                f"Read returned {len(results)} results for {len(batch)} requested attributes"
            )
        for (node_index, _, attribute), result in zip(batch, results):
            attribute_cache[node_index][attribute] = result


async def cache_export_nodes(client, nodes, export_values):
    attribute_cache = [dict() for _ in nodes]
    node_class_reads = [
        (node_index, node, ua.AttributeIds.NodeClass)
        for node_index, node in enumerate(nodes)
    ]
    print(
        f"Reading {len(node_class_reads)} node classes in bounded batches of "
        f"{READ_VALUES_PER_REQUEST}..."
    )
    await read_export_attributes(client, node_class_reads, attribute_cache)

    reads = []
    for node_index, node in enumerate(nodes):
        node_class_value = attribute_cache[node_index][ua.AttributeIds.NodeClass]
        node_class_value.StatusCode.check()
        node_class = node_class_value.Value.Value
        attributes = COMMON_EXPORT_ATTRIBUTES + CLASS_EXPORT_ATTRIBUTES.get(node_class, ())
        if export_values and node_class in (ua.NodeClass.Variable, ua.NodeClass.VariableType):
            attributes += (ua.AttributeIds.Value,)
        reads.extend((node_index, node, attribute) for attribute in attributes)

    print(
        f"Reading {len(reads)} class-specific attributes in bounded batches of "
        f"{READ_VALUES_PER_REQUEST}..."
    )
    await read_export_attributes(client, reads, attribute_cache)

    print("Reading node references with bounded continuation pages...")
    cached_nodes = []
    for node_index, node in enumerate(nodes):
        references = await get_bounded_references(
            client,
            node,
            ua.ObjectIds.References,
            ua.BrowseDirection.Both,
        )
        cached_nodes.append(CachedExportNode(node, attribute_cache[node_index], references))
        if (node_index + 1) % 50 == 0 or node_index + 1 == len(nodes):
            print(f"  Cached {node_index + 1}/{len(nodes)} nodes")

    return cached_nodes


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

    client.application_uri = CLIENT_APPLICATION_URI
    client_cert = Path(args.client_cert) if args.client_cert else None
    client_key = Path(args.client_key) if args.client_key else None
    server_cert = Path(args.server_cert) if args.server_cert else resolve_default_server_cert()

    if (client_cert is None) != (client_key is None):
        raise ValueError("Use both --client-cert and --client-key together")

    if client_cert is None and not args.no_auto_client_cert:
        base_dir = Path(tempfile.gettempdir()) / "rp2040_opcua_export"
        client_cert = base_dir / "client_cert.der"
        client_key = base_dir / "client_key.pem"
        client_app_uri = CLIENT_APPLICATION_URI
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


async def collect_namespace_nodes(client, namespace_index, max_depth, start_node=None):
    """Collect namespace nodes reachable from Root via hierarchical references."""

    namespace_array = await client.get_namespace_array()
    if namespace_index >= len(namespace_array):
        raise ValueError(f"Namespace {namespace_index} does not exist")

    namespace_uri = namespace_array[namespace_index]
    print(f"Namespace URI: {namespace_uri}")
    browse_root = client.get_node(ua.NodeId.from_string(start_node)) if start_node else client.nodes.root
    print(f"Browsing from {node_label(browse_root)}...")

    visited = set()
    exported = {}
    stack = [(browse_root, 0)]

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
            children = await get_bounded_hierarchical_children(client, node)
        except Exception as exc:
            raise RuntimeError(f"Could not browse children of {key}: {exc}") from exc

        for child in reversed(children):
            child_key = node_label(child)
            if child_key not in visited:
                stack.append((child, depth + 1))

    print(f"Found {len(exported)} namespace nodes reachable from Root")
    return namespace_uri, list(exported.values())


async def export_namespace_nodes(client, namespace_index, output_path, max_depth,
                                 export_values, start_node=None):
    namespace_uri, nodes = await collect_namespace_nodes(
        client, namespace_index, max_depth, start_node
    )

    if not nodes:
        print("Warning: no nodes were found in the requested namespace")
        return None

    cached_nodes = await cache_export_nodes(client, nodes, export_values)
    exporter = CachedXmlExporter(client, export_values=export_values)
    await exporter.build_etree(cached_nodes)
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
    client = Client(
        url=args.server_url,
        timeout=args.timeout,
        watchdog_intervall=args.timeout,
    )

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
            args.start_node,
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

