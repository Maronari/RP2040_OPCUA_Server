#!/usr/bin/env python3
"""Export, validate, and apply trust lists over OPC UA maintenance methods."""

import argparse
import asyncio
import getpass
import json
import os
import socket
import sys
import tempfile
import zlib
from pathlib import Path

Client = None
ua = None
security_policies = None
setup_self_signed_certificate = None
ExtendedKeyUsageOID = None


def load_asyncua():
    global Client, ua, security_policies, setup_self_signed_certificate, ExtendedKeyUsageOID

    if Client is not None:
        return

    try:
        from asyncua import Client as imported_client, ua as imported_ua
        from asyncua.crypto import security_policies as imported_security_policies
        from asyncua.crypto.cert_gen import setup_self_signed_certificate as imported_setup_certificate
        from cryptography.x509.oid import ExtendedKeyUsageOID as imported_extended_key_usage_oid
    except ImportError:
        print('ERROR: asyncua library not installed')
        print(r'Install with: C:\Python312\python.exe -m pip install --user asyncua')
        sys.exit(1)

    Client = imported_client
    ua = imported_ua
    security_policies = imported_security_policies
    setup_self_signed_certificate = imported_setup_certificate
    ExtendedKeyUsageOID = imported_extended_key_usage_oid


DEFAULT_CHUNK_SIZE = 4096
DEFAULT_SCHEMA_VERSION = 1
DEFAULT_EXPORT_PATH = Path('build/trust_list_export.pem')
DEFAULT_AUTO_CLIENT_CERT_DIR = 'rp2040_opcua_trust'
LEGACY_AUTO_CLIENT_CERT_DIRS = (
    'rp2040_opcua_client',
    'rp2040_opcua_ota',
    'rp2040_opcua_config',
)
SECURITY_NODE_ID = 'ns=1;s=Device.Maintenance.Security'
SYSTEM_NODE_ID = 'ns=1;s=Device.Maintenance.System'
METHOD_GET_TRUST_STATE = 'ns=1;s=Device.Maintenance.Security.GetTrustState'
METHOD_EXPORT_TRUST_LIST = 'ns=1;s=Device.Maintenance.Security.ExportTrustList'
METHOD_BEGIN_TRUST_LIST_IMPORT = 'ns=1;s=Device.Maintenance.Security.BeginTrustListImport'
METHOD_WRITE_TRUST_LIST_CHUNK = 'ns=1;s=Device.Maintenance.Security.WriteTrustListChunk'
METHOD_ABORT_TRUST_LIST_IMPORT = 'ns=1;s=Device.Maintenance.Security.AbortTrustListImport'
METHOD_VALIDATE_IMPORTED_TRUST_LIST = 'ns=1;s=Device.Maintenance.Security.ValidateImportedTrustList'
METHOD_APPLY_IMPORTED_TRUST_LIST = 'ns=1;s=Device.Maintenance.Security.ApplyImportedTrustList'
METHOD_REBOOT = 'ns=1;s=Device.Maintenance.Reboot'


def build_common_parser():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('server_url', help='OPC UA endpoint, e.g. opc.tcp://192.168.1.100:4840')
    parser.add_argument('--username', default=os.getenv('OPCUA_USERNAME'), help='Username')
    parser.add_argument('--password', default=os.getenv('OPCUA_PASSWORD'), help='Password')
    parser.add_argument('--timeout', type=float, default=30.0, help='Client timeout in seconds')
    parser.add_argument('--client-cert', help='Client certificate for secure endpoint login')
    parser.add_argument('--client-key', help='Client private key for secure endpoint login')
    parser.add_argument('--server-cert', help='Server certificate path')
    parser.add_argument('--security-policy', default='Basic256Sha256', choices=['Basic256Sha256'])
    parser.add_argument('--security-mode', default='SignAndEncrypt', choices=['Sign', 'SignAndEncrypt'])
    parser.add_argument('--no-auto-client-cert', action='store_true')
    return parser


def parse_args():
    common = build_common_parser()
    parser = argparse.ArgumentParser(
        description='Export and import trust lists over OPC UA maintenance methods.'
    )
    subparsers = parser.add_subparsers(dest='command', required=True)

    status_parser = subparsers.add_parser('status', parents=[common], help='Read GetTrustState()')
    status_parser.set_defaults(handler=run_status)

    export_parser = subparsers.add_parser('export', parents=[common], help='Read ExportTrustList()')
    export_parser.add_argument(
        '--output',
        default=str(DEFAULT_EXPORT_PATH),
        help='Write exported trust list to this file (default: build/trust_list_export.pem)',
    )
    export_parser.add_argument('--stdout', action='store_true', help='Print trust list to stdout as UTF-8 text')
    export_parser.set_defaults(handler=run_export)

    import_parser = subparsers.add_parser('import', parents=[common], help='Upload and apply a trust list bundle')
    import_parser.add_argument('trust_path', help='Path to trust list PEM/DER bundle')
    import_parser.add_argument('--chunk-size', type=int, default=DEFAULT_CHUNK_SIZE)
    import_parser.add_argument('--schema-version', type=int, default=DEFAULT_SCHEMA_VERSION)
    import_parser.add_argument('--skip-pre-status', action='store_true')
    import_parser.add_argument('--skip-post-status', action='store_true')
    import_parser.add_argument('--validate-only', action='store_true', help='Upload and validate, but do not apply')
    import_parser.add_argument(
        '--keep-staged',
        action='store_true',
        help='Keep the staged import session active after validate-only or failure',
    )
    import_parser.add_argument('--reboot-after-apply', action='store_true')
    import_parser.set_defaults(handler=run_import)

    return parser.parse_args()


def resolve_default_server_cert():
    candidate = Path(__file__).resolve().parent.parent / 'certs' / 'server.crt.der'
    return candidate if candidate.is_file() else None


async def ensure_client_certificate(cert_path, key_path, app_uri):
    cert_path.parent.mkdir(parents=True, exist_ok=True)
    key_path.parent.mkdir(parents=True, exist_ok=True)
    await setup_self_signed_certificate(
        key_path,
        cert_path,
        app_uri=app_uri,
        host_name=socket.gethostname() or 'localhost',
        cert_use=[ExtendedKeyUsageOID.CLIENT_AUTH],
        subject_attrs={'organizationName': 'RP2040_OPCUA_Server'},
    )


def resolve_auto_client_certificate_paths():
    temp_root = Path(tempfile.gettempdir())

    for dirname in LEGACY_AUTO_CLIENT_CERT_DIRS:
        base_dir = temp_root / dirname
        client_cert = base_dir / 'client_cert.der'
        client_key = base_dir / 'client_key.pem'
        if client_cert.is_file() and client_key.is_file():
            return client_cert, client_key

    base_dir = temp_root / DEFAULT_AUTO_CLIENT_CERT_DIR
    return base_dir / 'client_cert.der', base_dir / 'client_key.pem'


async def configure_client_security(client, args):
    if not args.username:
        return

    client_cert = Path(args.client_cert) if args.client_cert else None
    client_key = Path(args.client_key) if args.client_key else None
    server_cert = Path(args.server_cert) if args.server_cert else resolve_default_server_cert()

    if (client_cert is None) != (client_key is None):
        raise ValueError('Use both --client-cert and --client-key together')

    if client_cert is None and not args.no_auto_client_cert:
        client_cert, client_key = resolve_auto_client_certificate_paths()
        if not client_cert.is_file() or not client_key.is_file():
            await ensure_client_certificate(
                client_cert,
                client_key,
                getattr(client, 'application_uri', 'urn:freeopcua:client'),
            )
        print(f'Using auto client certificate: {client_cert}')

    if client_cert is None or client_key is None:
        raise ValueError(
            'Username authentication requires a secure endpoint. '
            'Provide --client-cert and --client-key, or omit --no-auto-client-cert.'
        )

    policy_class = getattr(security_policies, f'SecurityPolicy{args.security_policy}')
    mode = getattr(ua.MessageSecurityMode, args.security_mode)
    await client.set_security(
        policy_class,
        certificate=client_cert,
        private_key=client_key,
        server_certificate=server_cert,
        mode=mode,
    )
    print(f'Using secure channel: {args.security_policy}/{args.security_mode}')
    if server_cert is not None:
        print(f'Using server certificate: {server_cert}')


def unwrap_scalar_result(result):
    if isinstance(result, (list, tuple)):
        if len(result) != 1:
            raise ValueError(f'Expected a single method output, got {len(result)} items')
        return result[0]
    return result


def unwrap_bytestring_result(result):
    value = unwrap_scalar_result(result)
    if value is None:
        return b''
    if isinstance(value, bytes):
        return value
    if isinstance(value, bytearray):
        return bytes(value)
    data = getattr(value, 'data', None)
    if data is not None:
        return bytes(data)
    return bytes(value)


async def call_bool_method(object_node, method_id, *args):
    result = await object_node.call_method(ua.NodeId.from_string(method_id), *args)
    return bool(unwrap_scalar_result(result))


async def call_string_method(object_node, method_id, *args):
    result = await object_node.call_method(ua.NodeId.from_string(method_id), *args)
    return str(unwrap_scalar_result(result))


async def call_bytestring_method(object_node, method_id, *args):
    result = await object_node.call_method(ua.NodeId.from_string(method_id), *args)
    return unwrap_bytestring_result(result)


async def get_trust_state(security_node):
    return json.loads(await call_string_method(security_node, METHOD_GET_TRUST_STATE))


async def export_trust_list(security_node):
    return await call_bytestring_method(security_node, METHOD_EXPORT_TRUST_LIST)


async def validate_imported_trust_list(security_node):
    return json.loads(await call_string_method(security_node, METHOD_VALIDATE_IMPORTED_TRUST_LIST))


async def request_reboot(system_node):
    return await call_bool_method(system_node, METHOD_REBOOT)


def print_json(prefix, payload):
    print(f'{prefix}: {json.dumps(payload, indent=2, sort_keys=False)}')


def print_progress(sent_bytes, total_bytes):
    percent = (100.0 * sent_bytes / total_bytes) if total_bytes else 100.0
    print(f'\rUploaded {sent_bytes}/{total_bytes} bytes ({percent:5.1f}%)', end='', flush=True)


def load_trust_payload(trust_path, schema_version):
    source_path = Path(trust_path).resolve()
    payload_bytes = source_path.read_bytes()
    payload_crc32 = zlib.crc32(payload_bytes) & 0xFFFFFFFF
    return {
        'source_path': source_path,
        'payload_bytes': payload_bytes,
        'payload_size': len(payload_bytes),
        'payload_crc32': payload_crc32,
        'schema_version': int(schema_version),
    }


async def connect_client(args):
    load_asyncua()
    client = Client(url=args.server_url, timeout=args.timeout)
    await configure_client_security(client, args)
    if args.username:
        client.set_user(args.username)
        client.set_password(args.password or '')
        print(f'Using authenticated session for user: {args.username}')
    print(f'Connecting to {args.server_url}...')
    await client.connect()
    print('Connected')
    return client


async def run_status(args):
    client = await connect_client(args)
    try:
        security = client.get_node(ua.NodeId.from_string(SECURITY_NODE_ID))
        print_json('Trust state', await get_trust_state(security))
    finally:
        await client.disconnect()
        print('Disconnected')


async def run_export(args):
    client = await connect_client(args)
    try:
        security = client.get_node(ua.NodeId.from_string(SECURITY_NODE_ID))
        payload = await export_trust_list(security)
        output_path = Path(args.output).resolve()
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(payload)
        print(f'Exported trust list to {output_path} ({len(payload)} bytes)')
        if args.stdout:
            print(payload.decode('utf-8', errors='replace'), end='')
    finally:
        await client.disconnect()
        print('Disconnected')


async def run_import(args):
    if args.chunk_size <= 0 or args.chunk_size > DEFAULT_CHUNK_SIZE:
        raise ValueError('--chunk-size must be between 1 and 4096 bytes')

    trust_info = load_trust_payload(args.trust_path, args.schema_version)
    print(f"Source:   {trust_info['source_path']}")
    print(
        'Payload:  '
        f"size={trust_info['payload_size']} bytes "
        f"crc32=0x{trust_info['payload_crc32']:08x} "
        f"schema={trust_info['schema_version']}"
    )

    client = await connect_client(args)
    upload_started = False
    keep_session = False
    security = None

    try:
        security = client.get_node(ua.NodeId.from_string(SECURITY_NODE_ID))
        system = client.get_node(ua.NodeId.from_string(SYSTEM_NODE_ID))

        if not args.skip_pre_status:
            status_before = await get_trust_state(security)
            print_json('Pre-import status', status_before)
            max_chunk_bytes = int(status_before.get('maxChunkBytes', DEFAULT_CHUNK_SIZE))
            if args.chunk_size > max_chunk_bytes:
                raise ValueError(
                    f'Requested chunk size {args.chunk_size} exceeds device limit {max_chunk_bytes}'
                )

        begin_ok = await call_bool_method(
            security,
            METHOD_BEGIN_TRUST_LIST_IMPORT,
            ua.Variant(trust_info['payload_size'], ua.VariantType.UInt32),
            ua.Variant(trust_info['payload_crc32'], ua.VariantType.UInt32),
            ua.Variant(trust_info['schema_version'], ua.VariantType.UInt32),
        )
        if not begin_ok:
            print_json('Begin rejected', await get_trust_state(security))
            raise RuntimeError('BeginTrustListImport returned false')

        upload_started = True
        sent_bytes = 0
        payload_bytes = trust_info['payload_bytes']
        print('Uploading trust list payload...')
        while sent_bytes < len(payload_bytes):
            chunk = payload_bytes[sent_bytes : sent_bytes + args.chunk_size]
            write_ok = await call_bool_method(
                security,
                METHOD_WRITE_TRUST_LIST_CHUNK,
                ua.Variant(chunk, ua.VariantType.ByteString),
            )
            if not write_ok:
                print()
                print_json('Write rejected', await get_trust_state(security))
                raise RuntimeError(f'WriteTrustListChunk returned false at offset {sent_bytes}')
            sent_bytes += len(chunk)
            print_progress(sent_bytes, len(payload_bytes))
        print()

        validation_report = await validate_imported_trust_list(security)
        print_json('Validation report', validation_report)
        if not bool(validation_report.get('valid', False)):
            keep_session = args.keep_staged
            raise RuntimeError(
                validation_report.get('result') or 'ValidateImportedTrustList returned invalid result'
            )

        if args.validate_only:
            if args.keep_staged:
                keep_session = True
                print('Validation succeeded and the staged import session was kept active')
            else:
                abort_ok = await call_bool_method(security, METHOD_ABORT_TRUST_LIST_IMPORT)
                print(f'AbortTrustListImport returned: {abort_ok}')
                upload_started = False
                print('Validation succeeded; staged import session was closed')
            return

        apply_ok = await call_bool_method(security, METHOD_APPLY_IMPORTED_TRUST_LIST)
        if not apply_ok:
            print_json('Apply failed status', await get_trust_state(security))
            raise RuntimeError('ApplyImportedTrustList returned false')

        upload_started = False
        print('Trust list applied successfully')

        if not args.skip_post_status:
            print_json('Post-import status', await get_trust_state(security))

        if args.reboot_after_apply:
            reboot_ok = await request_reboot(system)
            if not reboot_ok:
                raise RuntimeError('Reboot returned false after successful trust apply')
            print('Reboot requested')
    except Exception:
        if upload_started and not keep_session and security is not None:
            try:
                abort_ok = await call_bool_method(security, METHOD_ABORT_TRUST_LIST_IMPORT)
                print(f'AbortTrustListImport returned: {abort_ok}')
            except Exception as abort_exc:
                print(f'AbortTrustListImport failed: {abort_exc}')
        raise
    finally:
        await client.disconnect()
        print('Disconnected')


def main():
    load_asyncua()
    args = parse_args()
    if args.username and args.password is None:
        args.password = getpass.getpass(f'Password for {args.username}: ')

    try:
        asyncio.run(args.handler(args))
    except Exception as exc:
        detail = str(exc).strip()
        if detail:
            print(f'ERROR: {type(exc).__name__}: {detail}')
        else:
            print(f'ERROR: {type(exc).__name__}')
        sys.exit(1)


if __name__ == '__main__':
    main()
