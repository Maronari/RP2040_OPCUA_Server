#!/usr/bin/env python3
'''Stage an OTA payload into Bank B over OPC UA maintenance methods.'''

import argparse
import asyncio
import getpass
import json
import os
import socket
import sys
import tempfile
from pathlib import Path

try:
    from asyncua import Client, ua
    from asyncua.crypto import security_policies
    from asyncua.crypto.cert_gen import setup_self_signed_certificate
    from cryptography.x509.oid import ExtendedKeyUsageOID
except ImportError:
    print('ERROR: asyncua library not installed')
    print(r'Install with: C:\Python312\python.exe -m pip install --user asyncua')
    sys.exit(1)

DEFAULT_MANIFEST_PATH = Path('build/ota_artifacts/ota_payload.json')
DEFAULT_CHUNK_SIZE = 4096
FIRMWARE_NODE_ID = 'ns=1;s=Device.Maintenance.Firmware'
SYSTEM_NODE_ID = 'ns=1;s=Device.Maintenance.System'
METHOD_GET_STATUS = 'ns=1;s=Device.Maintenance.GetFirmwareUpdateStatus'
METHOD_BEGIN = 'ns=1;s=Device.Maintenance.BeginFirmwareUpdate'
METHOD_WRITE = 'ns=1;s=Device.Maintenance.WriteFirmwareChunk'
METHOD_FINALIZE = 'ns=1;s=Device.Maintenance.FinalizeFirmwareUpdate'
METHOD_ABORT = 'ns=1;s=Device.Maintenance.AbortFirmwareUpdate'
METHOD_REBOOT = 'ns=1;s=Device.Maintenance.Reboot'


def parse_args():
    parser = argparse.ArgumentParser(
        description='Stage build/ota_artifacts/ota_payload.bin into Bank B over OPC UA.'
    )
    parser.add_argument('server_url', help='OPC UA endpoint, e.g. opc.tcp://192.168.1.100:4840')
    parser.add_argument(
        '--manifest',
        default=str(DEFAULT_MANIFEST_PATH),
        help='Path to ota_payload.json (default: build/ota_artifacts/ota_payload.json)',
    )
    parser.add_argument('--payload', help='Override payload file path')
    parser.add_argument('--chunk-size', type=int, default=DEFAULT_CHUNK_SIZE)
    parser.add_argument('--version-number', type=int, help='Override manifest version_number')
    parser.add_argument('--username', default=os.getenv('OPCUA_USERNAME'), help='Username')
    parser.add_argument('--password', default=os.getenv('OPCUA_PASSWORD'), help='Password')
    parser.add_argument('--timeout', type=float, default=30.0, help='Client timeout in seconds')
    parser.add_argument('--client-cert', help='Client certificate for secure endpoint login')
    parser.add_argument('--client-key', help='Client private key for secure endpoint login')
    parser.add_argument('--server-cert', help='Server certificate path')
    parser.add_argument('--security-policy', default='Basic256Sha256', choices=['Basic256Sha256'])
    parser.add_argument('--security-mode', default='SignAndEncrypt', choices=['Sign', 'SignAndEncrypt'])
    parser.add_argument('--no-auto-client-cert', action='store_true')
    parser.add_argument('--skip-pre-status', action='store_true')
    parser.add_argument('--skip-post-status', action='store_true')
    parser.add_argument('--reboot-after-finalize', action='store_true')
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


async def configure_client_security(client, args):
    if not args.username:
        return

    client_cert = Path(args.client_cert) if args.client_cert else None
    client_key = Path(args.client_key) if args.client_key else None
    server_cert = Path(args.server_cert) if args.server_cert else resolve_default_server_cert()

    if (client_cert is None) != (client_key is None):
        raise ValueError('Use both --client-cert and --client-key together')

    if client_cert is None and not args.no_auto_client_cert:
        base_dir = Path(tempfile.gettempdir()) / 'rp2040_opcua_ota'
        client_cert = base_dir / 'client_cert.der'
        client_key = base_dir / 'client_key.pem'
        await ensure_client_certificate(
            client_cert,
            client_key,
            getattr(client, 'application_uri', 'urn:freeopcua:client'),
        )
        print(f'Using temporary client certificate: {client_cert}')

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


def parse_u32(value, field_name):
    parsed = int(value, 0) if isinstance(value, str) else int(value)
    if parsed < 0 or parsed > 0xFFFFFFFF:
        raise ValueError(f'Manifest field {field_name} is out of uint32 range: {parsed}')
    return parsed


def load_manifest(manifest_path, payload_override=None, version_override=None):
    manifest_path = Path(manifest_path).resolve()
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    payload_path = (
        Path(payload_override).resolve()
        if payload_override
        else (manifest_path.parent / manifest['payload_file']).resolve()
    )
    image_size = parse_u32(manifest['payload_size_bytes'], 'payload_size_bytes')
    image_crc32 = parse_u32(manifest['payload_crc32'], 'payload_crc32')
    version_number = parse_u32(
        version_override if version_override is not None else manifest['version_number'],
        'version_number',
    )
    payload_size = payload_path.stat().st_size
    if payload_size != image_size:
        raise ValueError(
            f'Payload size mismatch: manifest says {image_size} bytes, file has {payload_size} bytes'
        )
    return {
        'manifest_path': manifest_path,
        'payload_path': payload_path,
        'image_size': image_size,
        'image_crc32': image_crc32,
        'version_number': version_number,
        'firmware_version': manifest.get('firmware_version', 'unknown'),
        'git_hash': manifest.get('git_hash', 'unknown'),
    }


def unwrap_scalar_result(result):
    if isinstance(result, (list, tuple)):
        if len(result) != 1:
            raise ValueError(f'Expected a single method output, got {len(result)} items')
        return result[0]
    return result


async def call_bool_method(object_node, method_id, *args):
    result = await object_node.call_method(ua.NodeId.from_string(method_id), *args)
    return bool(unwrap_scalar_result(result))


async def get_status(maintenance_node):
    result = await maintenance_node.call_method(ua.NodeId.from_string(METHOD_GET_STATUS))
    return json.loads(str(unwrap_scalar_result(result)))


async def request_reboot(maintenance_node):
    result = await maintenance_node.call_method(ua.NodeId.from_string(METHOD_REBOOT))
    return bool(unwrap_scalar_result(result))


def print_status(prefix, status):
    print(f'{prefix}: {json.dumps(status, indent=2, sort_keys=True)}')


def print_progress(sent_bytes, total_bytes):
    percent = (100.0 * sent_bytes / total_bytes) if total_bytes else 100.0
    print(f'\rUploaded {sent_bytes}/{total_bytes} bytes ({percent:5.1f}%)', end='', flush=True)


async def stage_payload(client, args, payload_info):
    firmware = client.get_node(ua.NodeId.from_string(FIRMWARE_NODE_ID))
    system = client.get_node(ua.NodeId.from_string(SYSTEM_NODE_ID))
    upload_started = False

    if not args.skip_pre_status:
        status_before = await get_status(firmware)
        print_status('Pre-upload status', status_before)
        max_chunk_bytes = int(status_before.get('maxChunkBytes', DEFAULT_CHUNK_SIZE))
        if args.chunk_size > max_chunk_bytes:
            raise ValueError(
                f'Requested chunk size {args.chunk_size} exceeds device limit {max_chunk_bytes}'
            )

    begin_ok = await call_bool_method(
        firmware,
        METHOD_BEGIN,
        ua.Variant(payload_info['image_size'], ua.VariantType.UInt32),
        ua.Variant(payload_info['image_crc32'], ua.VariantType.UInt32),
        ua.Variant(payload_info['version_number'], ua.VariantType.UInt32),
    )
    if not begin_ok:
        print_status('Begin rejected', await get_status(firmware))
        raise RuntimeError('BeginFirmwareUpdate returned false')

    upload_started = True
    sent_bytes = 0
    payload_path = payload_info['payload_path']
    image_size = payload_info['image_size']
    image_crc32 = payload_info['image_crc32']
    version_number = payload_info['version_number']
    print(
        f'Staging {payload_path.name}: size={image_size} bytes, '
        f'crc32=0x{image_crc32:08x}, version={version_number}'
    )

    try:
        with payload_path.open('rb') as payload_file:
            while True:
                chunk = payload_file.read(args.chunk_size)
                if not chunk:
                    break

                write_ok = await call_bool_method(
                    firmware,
                    METHOD_WRITE,
                    ua.Variant(chunk, ua.VariantType.ByteString),
                )
                if not write_ok:
                    print()
                    print_status('Write rejected', await get_status(firmware))
                    raise RuntimeError(f'WriteFirmwareChunk returned false at offset {sent_bytes}')

                sent_bytes += len(chunk)
                print_progress(sent_bytes, image_size)

        print()
        finalize_ok = await call_bool_method(firmware, METHOD_FINALIZE)
        if not finalize_ok:
            print_status('Finalize rejected', await get_status(firmware))
            raise RuntimeError('FinalizeFirmwareUpdate returned false')
    except Exception:
        if upload_started:
            try:
                abort_ok = await call_bool_method(firmware, METHOD_ABORT)
                print(f'AbortFirmwareUpdate returned: {abort_ok}')
            except Exception as abort_exc:
                print(f'AbortFirmwareUpdate failed: {abort_exc}')
        raise

    if not args.skip_post_status:
        status_after = await get_status(firmware)
        print_status('Post-upload status', status_after)
        if status_after.get('otaState') != 'STAGED_IN_BANK_B':
            raise RuntimeError('Upload finished but otaState is not STAGED_IN_BANK_B')

    if args.reboot_after_finalize:
        reboot_ok = await request_reboot(system)
        if not reboot_ok:
            raise RuntimeError('Reboot returned false after successful finalize')
        print('Reboot requested')


async def main_async():
    args = parse_args()
    if args.chunk_size <= 0 or args.chunk_size > DEFAULT_CHUNK_SIZE:
        raise ValueError('--chunk-size must be between 1 and 4096 bytes')
    if args.username and args.password is None:
        args.password = getpass.getpass(f'Password for {args.username}: ')

    payload_info = load_manifest(args.manifest, args.payload, args.version_number)
    manifest_path = payload_info['manifest_path']
    payload_path = payload_info['payload_path']
    firmware_version = payload_info['firmware_version']
    git_hash = payload_info['git_hash']
    version_number = payload_info['version_number']
    image_crc32 = payload_info['image_crc32']
    print(f'Manifest: {manifest_path}')
    print(f'Payload:  {payload_path}')
    print(f'Image:    fw={firmware_version} git={git_hash} version={version_number} crc32=0x{image_crc32:08x}')

    client = Client(url=args.server_url, timeout=args.timeout)
    await configure_client_security(client, args)
    if args.username:
        client.set_user(args.username)
        client.set_password(args.password or '')
        print(f'Using authenticated session for user: {args.username}')

    connected = False
    try:
        print(f'Connecting to {args.server_url}...')
        await client.connect()
        connected = True
        print('Connected')
        await stage_payload(client, args, payload_info)
        print('OTA payload staged successfully')
    finally:
        if connected:
            await client.disconnect()
            print('Disconnected')


def main():
    try:
        asyncio.run(main_async())
    except Exception as exc:
        print(f'ERROR: {exc}')
        sys.exit(1)


if __name__ == '__main__':
    main()


