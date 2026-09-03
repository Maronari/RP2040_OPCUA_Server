#!/usr/bin/env python3
'''Stage an OTA payload into Bank B over OPC UA maintenance methods.'''

import argparse
import asyncio
import getpass
import hashlib
import json
import os
import socket
import sys
import tempfile
import zlib
from pathlib import Path

try:
    from asyncua import Client, ua
    from asyncua.crypto import security_policies
    from asyncua.crypto.cert_gen import setup_self_signed_certificate
    from cryptography.x509.oid import ExtendedKeyUsageOID
except ImportError:
    Client = None
    ua = None
    security_policies = None
    setup_self_signed_certificate = None
    ExtendedKeyUsageOID = None

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


def require_asyncua():
    if Client is None or ua is None or security_policies is None or setup_self_signed_certificate is None:
        raise RuntimeError(
            'asyncua library not installed\n'
            r'Install with: C:\Python312\python.exe -m pip install --user asyncua'
        )


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
    parser.add_argument(
        '--resume-active',
        action='store_true',
        help=(
            'Resume an active upload only when device size, CRC, version and offset '
            'match the selected manifest'
        ),
    )
    parser.add_argument('--status-only', action='store_true',
                        help='Read authenticated OTA status without loading or staging a payload')
    parser.add_argument('--reboot-only', action='store_true',
                        help='Request an authenticated reboot without loading or staging a payload')
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


def compute_payload_integrity(payload_path):
    crc32 = 0
    sha256 = hashlib.sha256()
    with Path(payload_path).open('rb') as payload_file:
        while True:
            chunk = payload_file.read(64 * 1024)
            if not chunk:
                break
            crc32 = zlib.crc32(chunk, crc32)
            sha256.update(chunk)
    return crc32 & 0xFFFFFFFF, sha256.hexdigest()


def parse_cmake_cache_bool(cache_value):
    if cache_value is None:
        raise ValueError('missing OTA_BUILD_BOOTLOADER in sibling CMakeCache.txt')
    return str(cache_value).strip().upper() in {'1', 'ON', 'TRUE', 'YES', 'Y'}


def load_json_file(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def load_cmake_cache(cache_path):
    cache = {}
    for raw_line in Path(cache_path).read_text(encoding='utf-8').splitlines():
        line = raw_line.strip()
        if not line or line.startswith('#') or line.startswith('//'):
            continue
        if '=' not in line:
            continue
        key, value = line.split('=', 1)
        key = key.split(':', 1)[0]
        cache[key] = value
    return cache


def parse_manifest_u32_field(manifest, field_name):
    if field_name not in manifest:
        raise ValueError(f'Manifest is missing required field {field_name}')
    return parse_u32(manifest[field_name], field_name)


def validate_local_build_manifest(manifest_path, manifest, version_number):
    manifest_path = Path(manifest_path).resolve()
    if manifest_path.parent.name != 'ota_artifacts':
        return

    build_dir = manifest_path.parent.parent
    cache_path = build_dir / 'CMakeCache.txt'
    layout_path = build_dir / 'generated' / 'ota_layout' / 'ota_layout.json'
    is_default_manifest = manifest_path == DEFAULT_MANIFEST_PATH.resolve()
    has_local_build_metadata = cache_path.exists() or layout_path.exists()
    if not is_default_manifest and not has_local_build_metadata:
        return

    if not cache_path.is_file():
        raise ValueError(
            f'Local build OTA manifest requires sibling CMakeCache.txt: {cache_path}'
        )
    if not layout_path.is_file():
        raise ValueError(
            f'Local build OTA manifest requires sibling ota_layout.json: {layout_path}'
        )

    cache = load_cmake_cache(cache_path)
    if not parse_cmake_cache_bool(cache.get('OTA_BUILD_BOOTLOADER')):
        raise ValueError(
            'Local build OTA manifest is unusable because sibling CMakeCache.txt '
            'has OTA_BUILD_BOOTLOADER=OFF'
        )

    layout = load_json_file(layout_path)
    cache_layout_fields = {
        'firmware_version': (
            str(layout['firmware']['version']),
            str(cache.get('RP2_FIRMWARE_VERSION', '')),
        ),
        'version_number': (
            int(layout['firmware']['version_number']),
            parse_u32(cache.get('RP2_FIRMWARE_VERSION_NUMBER', ''), 'RP2_FIRMWARE_VERSION_NUMBER'),
        ),
        'board': (str(layout['board']), str(cache.get('PICO_BOARD', ''))),
        'platform': (str(layout['platform']), str(cache.get('PICO_PLATFORM', ''))),
    }
    for field_name, (layout_value, cache_value) in cache_layout_fields.items():
        if cache_value != layout_value:
            raise ValueError(
                f'Local build OTA layout {field_name} mismatch: '
                f'layout={layout_value!r} CMakeCache={cache_value!r}'
            )

    expected_fields = {
        'firmware_version': str(layout['firmware']['version']),
        'version_number': int(layout['firmware']['version_number']),
        'board': str(layout['board']),
        'platform': str(layout['platform']),
        'execution_origin': parse_u32(layout['regions']['bank_a']['origin'], 'execution_origin'),
        'staging_offset': parse_u32(layout['regions']['bank_b']['offset_bytes'], 'staging_offset'),
    }

    actual_fields = {
        'firmware_version': str(manifest.get('firmware_version', '')),
        'version_number': version_number,
        'board': str(manifest.get('board', '')),
        'platform': str(manifest.get('platform', '')),
        'execution_origin': parse_manifest_u32_field(manifest, 'execution_origin'),
        'staging_offset': parse_manifest_u32_field(manifest, 'staging_offset'),
    }

    for field_name, expected_value in expected_fields.items():
        actual_value = actual_fields[field_name]
        if actual_value != expected_value:
            raise ValueError(
                f'Local build OTA manifest {field_name} mismatch: '
                f'manifest={actual_value!r} layout={expected_value!r}'
            )


def load_manifest(manifest_path, payload_override=None, version_override=None):
    manifest_path = Path(manifest_path).resolve()
    manifest = load_json_file(manifest_path)
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
    validate_local_build_manifest(manifest_path, manifest, version_number)
    payload_size = payload_path.stat().st_size
    if payload_size != image_size:
        raise ValueError(
            f'Payload size mismatch: manifest says {image_size} bytes, file has {payload_size} bytes'
        )
    payload_crc32, payload_sha256 = compute_payload_integrity(payload_path)
    if payload_crc32 != image_crc32:
        raise ValueError(
            f'Payload CRC32 mismatch: manifest says 0x{image_crc32:08x}, '
            f'file has 0x{payload_crc32:08x}'
        )
    manifest_sha256 = str(manifest.get('payload_sha256', '')).strip().lower()
    if manifest_sha256 and payload_sha256 != manifest_sha256:
        raise ValueError(
            f'Payload SHA256 mismatch: manifest says {manifest_sha256}, file has {payload_sha256}'
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


def resolve_resume_offset(status, payload_info, chunk_size):
    if not status.get('sessionActive', False):
        return None

    expected = {
        'expectedImageSize': payload_info['image_size'],
        'expectedImageCrc32': payload_info['image_crc32'],
        'versionNumber': payload_info['version_number'],
    }
    for field, expected_value in expected.items():
        actual_value = parse_u32(status.get(field, -1), field)
        if actual_value != expected_value:
            raise RuntimeError(
                f'Active OTA session {field} mismatch: '
                f'device={actual_value} manifest={expected_value}'
            )

    offset = parse_u32(status.get('bytesWritten', -1), 'bytesWritten')
    if offset > payload_info['image_size']:
        raise RuntimeError('Active OTA session offset exceeds the selected payload size')
    if offset != payload_info['image_size'] and offset % chunk_size != 0:
        raise RuntimeError(
            f'Active OTA session offset {offset} is not aligned to chunk size {chunk_size}'
        )
    remaining = parse_u32(status.get('remainingBytes', -1), 'remainingBytes')
    if remaining != payload_info['image_size'] - offset:
        raise RuntimeError('Active OTA session remaining byte count is inconsistent')
    return offset


async def stage_payload(client, args, payload_info):
    firmware = client.get_node(ua.NodeId.from_string(FIRMWARE_NODE_ID))
    system = client.get_node(ua.NodeId.from_string(SYSTEM_NODE_ID))
    upload_started = False
    sent_bytes = 0
    status_before = None

    if not args.skip_pre_status:
        status_before = await get_status(firmware)
        print_status('Pre-upload status', status_before)
        max_chunk_bytes = int(status_before.get('maxChunkBytes', DEFAULT_CHUNK_SIZE))
        if args.chunk_size > max_chunk_bytes:
            raise ValueError(
                f'Requested chunk size {args.chunk_size} exceeds device limit {max_chunk_bytes}'
            )

    if args.resume_active:
        if status_before is None:
            raise ValueError('--resume-active cannot be combined with --skip-pre-status')
        resume_offset = resolve_resume_offset(status_before, payload_info, args.chunk_size)
        if resume_offset is None:
            raise RuntimeError('--resume-active requested but the device has no active OTA session')
        sent_bytes = resume_offset
        upload_started = True
        print(f'Resuming matching active OTA session at offset {sent_bytes}')
    else:
        if status_before is not None and status_before.get('sessionActive', False):
            raise RuntimeError(
                'Device already has an active OTA session; abort it or rerun with --resume-active'
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
            payload_file.seek(sent_bytes)
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
            if args.resume_active:
                print(
                    'AbortFirmwareUpdate was not requested after resume failure; '
                    'inspect device status before retrying --resume-active'
                )
            else:
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
    require_asyncua()
    args = parse_args()
    if args.chunk_size <= 0 or args.chunk_size > DEFAULT_CHUNK_SIZE:
        raise ValueError('--chunk-size must be between 1 and 4096 bytes')
    if args.username and args.password is None:
        args.password = getpass.getpass(f'Password for {args.username}: ')

    if args.status_only or args.reboot_only:
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
            if args.status_only:
                firmware = client.get_node(ua.NodeId.from_string(FIRMWARE_NODE_ID))
                print_status('OTA status', await get_status(firmware))
            else:
                system = client.get_node(ua.NodeId.from_string(SYSTEM_NODE_ID))
                if not await request_reboot(system):
                    raise RuntimeError('Reboot returned false')
                print('Reboot requested')
        finally:
            if connected:
                await client.disconnect()
                print('Disconnected')
        return

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


