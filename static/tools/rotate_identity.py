#!/usr/bin/env python3
"""Stage and apply device identity rotation over OPC UA maintenance methods."""

import argparse
import asyncio
import getpass
import json
import os
import socket
import struct
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
DEFAULT_AUTO_CLIENT_CERT_DIR = 'rp2040_opcua_trust'
LEGACY_AUTO_CLIENT_CERT_DIRS = (
    'rp2040_opcua_client',
    'rp2040_opcua_ota',
    'rp2040_opcua_config',
)
IDENTITY_PACKAGE_MAGIC = 0x31444953
SECURITY_NODE_ID = 'ns=1;s=Device.Maintenance.Security'
SYSTEM_NODE_ID = 'ns=1;s=Device.Maintenance.System'
METHOD_GET_CERTIFICATE_STATE = 'ns=1;s=Device.Maintenance.Security.GetCertificateState'
METHOD_EXPORT_CERTIFICATE_METADATA = 'ns=1;s=Device.Maintenance.Security.ExportCertificateMetadata'
METHOD_BEGIN_IDENTITY_ROTATION = 'ns=1;s=Device.Maintenance.Security.BeginIdentityRotation'
METHOD_WRITE_IDENTITY_ROTATION_CHUNK = 'ns=1;s=Device.Maintenance.Security.WriteIdentityRotationChunk'
METHOD_ABORT_IDENTITY_ROTATION = 'ns=1;s=Device.Maintenance.Security.AbortIdentityRotation'
METHOD_VALIDATE_STAGED_IDENTITY = 'ns=1;s=Device.Maintenance.Security.ValidateStagedIdentity'
METHOD_APPLY_STAGED_IDENTITY = 'ns=1;s=Device.Maintenance.Security.ApplyStagedIdentity'
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
        description='Inspect and rotate the device identity over OPC UA maintenance methods.'
    )
    subparsers = parser.add_subparsers(dest='command', required=True)

    status_parser = subparsers.add_parser('status', parents=[common], help='Read GetCertificateState()')
    status_parser.set_defaults(handler=run_status)

    metadata_parser = subparsers.add_parser(
        'metadata', parents=[common], help='Read ExportCertificateMetadata()'
    )
    metadata_parser.set_defaults(handler=run_metadata)

    import_parser = subparsers.add_parser(
        'import', parents=[common], help='Upload and apply a new certificate/private-key package'
    )
    import_parser.add_argument('certificate_path', help='Path to certificate file (DER or PEM)')
    import_parser.add_argument('private_key_path', help='Path to private key file (DER or PEM)')
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
    import_parser.add_argument(
        '--write-package',
        help='Optional path to write the exact binary SID1 package that is sent to the device',
    )
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


async def call_bool_method(object_node, method_id, *args):
    result = await object_node.call_method(ua.NodeId.from_string(method_id), *args)
    return bool(unwrap_scalar_result(result))


async def call_string_method(object_node, method_id, *args):
    result = await object_node.call_method(ua.NodeId.from_string(method_id), *args)
    return str(unwrap_scalar_result(result))


async def get_certificate_state(security_node):
    return json.loads(await call_string_method(security_node, METHOD_GET_CERTIFICATE_STATE))


async def export_certificate_metadata(security_node):
    return json.loads(await call_string_method(security_node, METHOD_EXPORT_CERTIFICATE_METADATA))


async def validate_staged_identity(security_node):
    return json.loads(await call_string_method(security_node, METHOD_VALIDATE_STAGED_IDENTITY))


async def request_reboot(system_node):
    return await call_bool_method(system_node, METHOD_REBOOT)


def print_json(prefix, payload):
    print(f'{prefix}: {json.dumps(payload, indent=2, sort_keys=False)}')


def print_progress(sent_bytes, total_bytes):
    percent = (100.0 * sent_bytes / total_bytes) if total_bytes else 100.0
    print(f'\rUploaded {sent_bytes}/{total_bytes} bytes ({percent:5.1f}%)', end='', flush=True)


def build_identity_package(certificate_bytes, private_key_bytes, schema_version):
    if not certificate_bytes:
        raise ValueError('Certificate file is empty')
    if not private_key_bytes:
        raise ValueError('Private key file is empty')
    if len(certificate_bytes) > 0xFFFFFFFF or len(private_key_bytes) > 0xFFFFFFFF:
        raise ValueError('Certificate or private key is too large for the SID1 package format')

    header = struct.pack(
        '<IIIII',
        IDENTITY_PACKAGE_MAGIC,
        int(schema_version),
        len(certificate_bytes),
        len(private_key_bytes),
        0,
    )
    return header + certificate_bytes + private_key_bytes


def load_identity_package(certificate_path, private_key_path, schema_version):
    certificate_source = Path(certificate_path).resolve()
    private_key_source = Path(private_key_path).resolve()
    certificate_bytes = certificate_source.read_bytes()
    private_key_bytes = private_key_source.read_bytes()
    payload_bytes = build_identity_package(certificate_bytes, private_key_bytes, schema_version)
    payload_crc32 = zlib.crc32(payload_bytes) & 0xFFFFFFFF
    return {
        'certificate_source': certificate_source,
        'private_key_source': private_key_source,
        'certificate_size': len(certificate_bytes),
        'private_key_size': len(private_key_bytes),
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
        print_json('Certificate state', await get_certificate_state(security))
    finally:
        await client.disconnect()
        print('Disconnected')


async def run_metadata(args):
    client = await connect_client(args)
    try:
        security = client.get_node(ua.NodeId.from_string(SECURITY_NODE_ID))
        print_json('Certificate metadata', await export_certificate_metadata(security))
    finally:
        await client.disconnect()
        print('Disconnected')


async def run_import(args):
    if args.chunk_size <= 0 or args.chunk_size > DEFAULT_CHUNK_SIZE:
        raise ValueError('--chunk-size must be between 1 and 4096 bytes')

    identity_info = load_identity_package(args.certificate_path, args.private_key_path, args.schema_version)
    if args.write_package:
        package_path = Path(args.write_package).resolve()
        package_path.parent.mkdir(parents=True, exist_ok=True)
        package_path.write_bytes(identity_info['payload_bytes'])
        print(f'Wrote SID1 package to {package_path}')

    print(f"Certificate: {identity_info['certificate_source']}")
    print(f"Private key: {identity_info['private_key_source']}")
    print(
        'Package:    '
        f"size={identity_info['payload_size']} bytes "
        f"crc32=0x{identity_info['payload_crc32']:08x} "
        f"schema={identity_info['schema_version']} "
        f"cert={identity_info['certificate_size']} key={identity_info['private_key_size']}"
    )

    client = await connect_client(args)
    upload_started = False
    keep_session = False
    security = None

    try:
        security = client.get_node(ua.NodeId.from_string(SECURITY_NODE_ID))
        system = client.get_node(ua.NodeId.from_string(SYSTEM_NODE_ID))

        if not args.skip_pre_status:
            status_before = await get_certificate_state(security)
            print_json('Pre-import status', status_before)
            max_chunk_bytes = int(status_before.get('maxChunkBytes', DEFAULT_CHUNK_SIZE))
            if args.chunk_size > max_chunk_bytes:
                raise ValueError(
                    f'Requested chunk size {args.chunk_size} exceeds device limit {max_chunk_bytes}'
                )

        begin_ok = await call_bool_method(
            security,
            METHOD_BEGIN_IDENTITY_ROTATION,
            ua.Variant(identity_info['payload_size'], ua.VariantType.UInt32),
            ua.Variant(identity_info['payload_crc32'], ua.VariantType.UInt32),
            ua.Variant(identity_info['schema_version'], ua.VariantType.UInt32),
        )
        if not begin_ok:
            print_json('Begin rejected', await get_certificate_state(security))
            raise RuntimeError('BeginIdentityRotation returned false')

        upload_started = True
        sent_bytes = 0
        payload_bytes = identity_info['payload_bytes']
        print('Uploading identity rotation package...')
        while sent_bytes < len(payload_bytes):
            chunk = payload_bytes[sent_bytes : sent_bytes + args.chunk_size]
            write_ok = await call_bool_method(
                security,
                METHOD_WRITE_IDENTITY_ROTATION_CHUNK,
                ua.Variant(chunk, ua.VariantType.ByteString),
            )
            if not write_ok:
                print()
                print_json('Write rejected', await get_certificate_state(security))
                raise RuntimeError(f'WriteIdentityRotationChunk returned false at offset {sent_bytes}')
            sent_bytes += len(chunk)
            print_progress(sent_bytes, len(payload_bytes))
        print()

        validation_report = await validate_staged_identity(security)
        print_json('Validation report', validation_report)
        if not bool(validation_report.get('valid', False)):
            keep_session = args.keep_staged
            raise RuntimeError(
                validation_report.get('result') or 'ValidateStagedIdentity returned invalid result'
            )

        if args.validate_only:
            if args.keep_staged:
                keep_session = True
                print('Validation succeeded and the staged identity rotation session was kept active')
            else:
                abort_ok = await call_bool_method(security, METHOD_ABORT_IDENTITY_ROTATION)
                print(f'AbortIdentityRotation returned: {abort_ok}')
                upload_started = False
                print('Validation succeeded; staged identity rotation session was closed')
            return

        apply_ok = await call_bool_method(security, METHOD_APPLY_STAGED_IDENTITY)
        if not apply_ok:
            print_json('Apply failed status', await get_certificate_state(security))
            raise RuntimeError('ApplyStagedIdentity returned false')

        upload_started = False
        print('Identity rotation applied successfully; reboot required for activation')

        if not args.skip_post_status:
            print_json('Post-import status', await get_certificate_state(security))

        if args.reboot_after_apply:
            reboot_ok = await request_reboot(system)
            if not reboot_ok:
                raise RuntimeError('Reboot returned false after successful identity apply')
            print('Reboot requested')
    except Exception:
        if upload_started and not keep_session and security is not None:
            try:
                abort_ok = await call_bool_method(security, METHOD_ABORT_IDENTITY_ROTATION)
                print(f'AbortIdentityRotation returned: {abort_ok}')
            except Exception as abort_exc:
                print(f'AbortIdentityRotation failed: {abort_exc}')
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
