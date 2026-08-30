#!/usr/bin/env python3
"""Export, validate, and apply device configuration over OPC UA."""

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
x509 = None
ExtensionOID = None


def load_asyncua():
    global Client, ua, security_policies, setup_self_signed_certificate
    global ExtendedKeyUsageOID, x509, ExtensionOID

    if Client is not None:
        return

    try:
        from asyncua import Client as imported_client, ua as imported_ua
        from asyncua.crypto import security_policies as imported_security_policies
        from asyncua.crypto.cert_gen import setup_self_signed_certificate as imported_setup_certificate
        from cryptography import x509 as imported_x509
        from cryptography.x509.oid import (
            ExtendedKeyUsageOID as imported_extended_key_usage_oid,
            ExtensionOID as imported_extension_oid,
        )
    except ImportError:
        print('ERROR: asyncua library not installed')
        print(r'Install with: C:\Python312\python.exe -m pip install --user asyncua')
        sys.exit(1)

    Client = imported_client
    ua = imported_ua
    security_policies = imported_security_policies
    setup_self_signed_certificate = imported_setup_certificate
    ExtendedKeyUsageOID = imported_extended_key_usage_oid
    x509 = imported_x509
    ExtensionOID = imported_extension_oid

DEFAULT_CHUNK_SIZE = 4096
DEFAULT_EXPORT_PATH = Path('build/config_export.json')
DEFAULT_AUTO_CLIENT_CERT_DIR = 'rp2040_opcua_client'
CLIENT_APPLICATION_URI = 'urn:rp2040-opcua:verification-client'
CONFIG_SCHEMA_VERSION = 1
CONFIG_MAX_CHANNELS = 16
CONFIGURATION_NODE_ID = 'ns=1;s=Device.Maintenance.Configuration'
SYSTEM_NODE_ID = 'ns=1;s=Device.Maintenance.System'
METHOD_GET_CONFIG_STATE = 'ns=1;s=Device.Maintenance.GetConfigState'
METHOD_EXPORT_CONFIG = 'ns=1;s=Device.Maintenance.ExportConfig'
METHOD_BEGIN_CONFIG_IMPORT = 'ns=1;s=Device.Maintenance.BeginConfigImport'
METHOD_WRITE_CONFIG_CHUNK = 'ns=1;s=Device.Maintenance.WriteConfigChunk'
METHOD_ABORT_CONFIG_IMPORT = 'ns=1;s=Device.Maintenance.AbortConfigImport'
METHOD_VALIDATE_IMPORTED_CONFIG = 'ns=1;s=Device.Maintenance.ValidateImportedConfig'
METHOD_APPLY_IMPORTED_CONFIG = 'ns=1;s=Device.Maintenance.ApplyImportedConfig'
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
        description='Export and import device configuration over OPC UA maintenance methods.'
    )
    subparsers = parser.add_subparsers(dest='command', required=True)

    status_parser = subparsers.add_parser('status', parents=[common], help='Read GetConfigState()')
    status_parser.set_defaults(handler=run_status)

    export_parser = subparsers.add_parser('export', parents=[common], help='Read ExportConfig()')
    export_parser.add_argument(
        '--output',
        default=str(DEFAULT_EXPORT_PATH),
        help='Write exported config to this file (default: build/config_export.json)',
    )
    export_parser.add_argument('--stdout', action='store_true', help='Print config JSON to stdout')
    export_parser.set_defaults(handler=run_export)

    import_parser = subparsers.add_parser('import', parents=[common], help='Upload and apply config JSON')
    import_parser.add_argument('config_path', help='Path to config JSON file')
    import_parser.add_argument('--chunk-size', type=int, default=DEFAULT_CHUNK_SIZE)
    import_parser.add_argument('--schema-version', type=int, help='Override schemaVersion for BeginConfigImport')
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
        '--write-canonical',
        help='Optional path to write the exact canonical JSON payload that is sent to the device',
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


def client_certificate_has_application_uri(cert_path, app_uri):
    try:
        encoded = cert_path.read_bytes()
        try:
            certificate = x509.load_der_x509_certificate(encoded)
        except ValueError:
            certificate = x509.load_pem_x509_certificate(encoded)
        san = certificate.extensions.get_extension_for_oid(
            ExtensionOID.SUBJECT_ALTERNATIVE_NAME
        ).value
        return app_uri in san.get_values_for_type(x509.UniformResourceIdentifier)
    except (OSError, ValueError, x509.ExtensionNotFound):
        return False


def resolve_auto_client_certificate_paths():
    temp_root = Path(tempfile.gettempdir())
    base_dir = temp_root / DEFAULT_AUTO_CLIENT_CERT_DIR
    return base_dir / 'client_cert.der', base_dir / 'client_key.pem'


async def configure_client_security(client, args):
    if not args.username:
        return

    client.application_uri = CLIENT_APPLICATION_URI
    client_cert = Path(args.client_cert) if args.client_cert else None
    client_key = Path(args.client_key) if args.client_key else None
    server_cert = Path(args.server_cert) if args.server_cert else resolve_default_server_cert()

    if (client_cert is None) != (client_key is None):
        raise ValueError('Use both --client-cert and --client-key together')

    if client_cert is None and not args.no_auto_client_cert:
        client_cert, client_key = resolve_auto_client_certificate_paths()
        client_app_uri = CLIENT_APPLICATION_URI
        if (
            not client_cert.is_file()
            or not client_key.is_file()
            or not client_certificate_has_application_uri(client_cert, client_app_uri)
        ):
            await ensure_client_certificate(
                client_cert,
                client_key,
                client_app_uri,
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


async def get_config_state(maintenance_node):
    return json.loads(await call_string_method(maintenance_node, METHOD_GET_CONFIG_STATE))


async def export_config(maintenance_node):
    return json.loads(await call_string_method(maintenance_node, METHOD_EXPORT_CONFIG))


async def validate_imported_config(maintenance_node):
    return json.loads(await call_string_method(maintenance_node, METHOD_VALIDATE_IMPORTED_CONFIG))


async def request_reboot(maintenance_node):
    return await call_bool_method(maintenance_node, METHOD_REBOOT)


def print_json(prefix, payload):
    print(f'{prefix}: {json.dumps(payload, indent=2, sort_keys=False)}')


def print_progress(sent_bytes, total_bytes):
    percent = (100.0 * sent_bytes / total_bytes) if total_bytes else 100.0
    print(f'\rUploaded {sent_bytes}/{total_bytes} bytes ({percent:5.1f}%)', end='', flush=True)


def require_dict(value, path):
    if not isinstance(value, dict):
        raise ValueError(f'{path} must be an object')
    return value


def require_list(value, path):
    if not isinstance(value, list):
        raise ValueError(f'{path} must be an array')
    return value


def require_key(mapping, key, path):
    if key not in mapping:
        raise KeyError(f'missing {path}.{key}')
    return mapping[key]


def coerce_u32(value, path):
    parsed = int(value, 0) if isinstance(value, str) else int(value)
    if parsed < 0 or parsed > 0xFFFFFFFF:
        raise ValueError(f'{path} must fit uint32')
    return parsed


def coerce_u16(value, path):
    parsed = coerce_u32(value, path)
    if parsed > 0xFFFF:
        raise ValueError(f'{path} must fit uint16')
    return parsed


def coerce_i16(value, path):
    parsed = int(value)
    if parsed < -32768 or parsed > 32767:
        raise ValueError(f'{path} must fit int16')
    return parsed


def coerce_i64(value, path):
    parsed = int(value)
    if parsed < -(1 << 63) or parsed > ((1 << 63) - 1):
        raise ValueError(f'{path} must fit int64')
    return parsed


def coerce_bool(value, path):
    if not isinstance(value, bool):
        raise ValueError(f'{path} must be boolean')
    return value


def coerce_string(value, path):
    if not isinstance(value, str):
        raise ValueError(f'{path} must be a string')
    return value


def coerce_number(value, path):
    if isinstance(value, bool):
        raise ValueError(f'{path} must be numeric')
    if not isinstance(value, (int, float)):
        raise ValueError(f'{path} must be numeric')
    return float(value)

def canonicalize_channel(channel, slot):
    channel = require_dict(channel, f'channels[{slot}]')
    channel_slot = coerce_u32(require_key(channel, 'slot', f'channels[{slot}]'), f'channels[{slot}].slot')
    if channel_slot != slot:
        raise ValueError(f'channels[{slot}].slot must equal {slot}')

    return {
        'slot': channel_slot,
        'inUse': coerce_bool(require_key(channel, 'inUse', f'channels[{slot}]'), f'channels[{slot}].inUse'),
        'kind': coerce_u32(require_key(channel, 'kind', f'channels[{slot}]'), f'channels[{slot}].kind'),
        'valueType': coerce_u32(require_key(channel, 'valueType', f'channels[{slot}]'), f'channels[{slot}].valueType'),
        'driverId': coerce_u32(require_key(channel, 'driverId', f'channels[{slot}]'), f'channels[{slot}].driverId'),
        'sourceIndex': coerce_u32(require_key(channel, 'sourceIndex', f'channels[{slot}]'), f'channels[{slot}].sourceIndex'),
        'sourceFlags': coerce_u32(require_key(channel, 'sourceFlags', f'channels[{slot}]'), f'channels[{slot}].sourceFlags'),
        'id': coerce_string(require_key(channel, 'id', f'channels[{slot}]'), f'channels[{slot}].id'),
        'name': coerce_string(require_key(channel, 'name', f'channels[{slot}]'), f'channels[{slot}].name'),
        'description': coerce_string(require_key(channel, 'description', f'channels[{slot}]'), f'channels[{slot}].description'),
        'unit': coerce_string(require_key(channel, 'unit', f'channels[{slot}]'), f'channels[{slot}].unit'),
        'flags': coerce_u32(require_key(channel, 'flags', f'channels[{slot}]'), f'channels[{slot}].flags'),
        'minimum': coerce_number(require_key(channel, 'minimum', f'channels[{slot}]'), f'channels[{slot}].minimum'),
        'maximum': coerce_number(require_key(channel, 'maximum', f'channels[{slot}]'), f'channels[{slot}].maximum'),
        'hasMinimum': coerce_bool(require_key(channel, 'hasMinimum', f'channels[{slot}]'), f'channels[{slot}].hasMinimum'),
        'hasMaximum': coerce_bool(require_key(channel, 'hasMaximum', f'channels[{slot}]'), f'channels[{slot}].hasMaximum'),
        'enabled': coerce_bool(require_key(channel, 'enabled', f'channels[{slot}]'), f'channels[{slot}].enabled'),
        'samplePeriodMs': coerce_u32(require_key(channel, 'samplePeriodMs', f'channels[{slot}]'), f'channels[{slot}].samplePeriodMs'),
        'publishPeriodMs': coerce_u32(require_key(channel, 'publishPeriodMs', f'channels[{slot}]'), f'channels[{slot}].publishPeriodMs'),
        'historyIntervalMs': coerce_u32(require_key(channel, 'historyIntervalMs', f'channels[{slot}]'), f'channels[{slot}].historyIntervalMs'),
        'scale': coerce_number(require_key(channel, 'scale', f'channels[{slot}]'), f'channels[{slot}].scale'),
        'offset': coerce_number(require_key(channel, 'offset', f'channels[{slot}]'), f'channels[{slot}].offset'),
        'warningThreshold': coerce_number(require_key(channel, 'warningThreshold', f'channels[{slot}]'), f'channels[{slot}].warningThreshold'),
        'criticalThreshold': coerce_number(require_key(channel, 'criticalThreshold', f'channels[{slot}]'), f'channels[{slot}].criticalThreshold'),
        'lowThreshold': coerce_number(require_key(channel, 'lowThreshold', f'channels[{slot}]'), f'channels[{slot}].lowThreshold'),
        'lowLowThreshold': coerce_number(require_key(channel, 'lowLowThreshold', f'channels[{slot}]'), f'channels[{slot}].lowLowThreshold'),
        'alarmHysteresis': coerce_number(require_key(channel, 'alarmHysteresis', f'channels[{slot}]'), f'channels[{slot}].alarmHysteresis'),
        'alarmDebounceMs': coerce_u32(require_key(channel, 'alarmDebounceMs', f'channels[{slot}]'), f'channels[{slot}].alarmDebounceMs'),
    }


def canonicalize_config_document(document):
    root = require_dict(document, 'root')
    device = require_dict(require_key(root, 'device', 'root'), 'device')
    network = require_dict(require_key(root, 'network', 'root'), 'network')
    time_config = require_dict(require_key(root, 'time', 'root'), 'time')
    mqtt = require_dict(require_key(root, 'mqtt', 'root'), 'mqtt')
    diagnostics = require_dict(require_key(root, 'diagnostics', 'root'), 'diagnostics')
    opcua = require_dict(require_key(root, 'opcua', 'root'), 'opcua')
    channels_input = require_list(require_key(root, 'channels', 'root'), 'channels')

    channels_by_slot = {}
    for index, channel in enumerate(channels_input):
        channel_obj = require_dict(channel, f'channels[{index}]')
        slot = coerce_u32(require_key(channel_obj, 'slot', f'channels[{index}]'), f'channels[{index}].slot')
        if slot in channels_by_slot:
            raise ValueError(f'duplicate channel slot {slot}')
        channels_by_slot[slot] = channel_obj

    expected_slots = set(range(CONFIG_MAX_CHANNELS))
    if set(channels_by_slot) != expected_slots:
        raise ValueError(f'channels must contain exactly slots 0..{CONFIG_MAX_CHANNELS - 1}')

    canonical = {
        'schemaVersion': coerce_u32(require_key(root, 'schemaVersion', 'root'), 'schemaVersion'),
        'device': {
            'manufacturer': coerce_string(require_key(device, 'manufacturer', 'device'), 'device.manufacturer'),
            'model': coerce_string(require_key(device, 'model', 'device'), 'device.model'),
            'deviceName': coerce_string(require_key(device, 'deviceName', 'device'), 'device.deviceName'),
            'opcuaHostname': coerce_string(require_key(device, 'opcuaHostname', 'device'), 'device.opcuaHostname'),
            'serialNumber': coerce_string(require_key(device, 'serialNumber', 'device'), 'device.serialNumber'),
            'productInstanceUri': coerce_string(require_key(device, 'productInstanceUri', 'device'), 'device.productInstanceUri'),
            'assetId': coerce_string(require_key(device, 'assetId', 'device'), 'device.assetId'),
            'hwRevision': coerce_string(require_key(device, 'hwRevision', 'device'), 'device.hwRevision'),
            'fwRevision': coerce_string(require_key(device, 'fwRevision', 'device'), 'device.fwRevision'),
            'buildTime': coerce_string(require_key(device, 'buildTime', 'device'), 'device.buildTime'),
            'gitHash': coerce_string(require_key(device, 'gitHash', 'device'), 'device.gitHash'),
            'location': coerce_string(require_key(device, 'location', 'device'), 'device.location'),
            'assetClass': coerce_string(require_key(device, 'assetClass', 'device'), 'device.assetClass'),
            'manufactureDateUtc': coerce_i64(require_key(device, 'manufactureDateUtc', 'device'), 'device.manufactureDateUtc'),
            'calibrationDateUtc': coerce_i64(require_key(device, 'calibrationDateUtc', 'device'), 'device.calibrationDateUtc'),
            'hierarchicalLocationPath': coerce_string(
                device.get('hierarchicalLocationPath', require_key(device, 'location', 'device')),
                'device.hierarchicalLocationPath'),
            'operationalLocationPath': coerce_string(
                device.get('operationalLocationPath', ''),
                'device.operationalLocationPath'),
        },
        'network': {
            'mode': coerce_string(require_key(network, 'mode', 'network'), 'network.mode'),
            'mac': coerce_string(require_key(network, 'mac', 'network'), 'network.mac'),
            'staticIp': coerce_string(require_key(network, 'staticIp', 'network'), 'network.staticIp'),
            'staticNetmask': coerce_string(require_key(network, 'staticNetmask', 'network'), 'network.staticNetmask'),
            'staticGateway': coerce_string(require_key(network, 'staticGateway', 'network'), 'network.staticGateway'),
            'staticDns': coerce_string(require_key(network, 'staticDns', 'network'), 'network.staticDns'),
        },
        'time': {
            'ntpServer': coerce_string(require_key(time_config, 'ntpServer', 'time'), 'time.ntpServer'),
            'timezoneOffsetMinutes': coerce_i16(require_key(time_config, 'timezoneOffsetMinutes', 'time'), 'time.timezoneOffsetMinutes'),
        },
        'mqtt': {
            'enabled': coerce_bool(require_key(mqtt, 'enabled', 'mqtt'), 'mqtt.enabled'),
            'useJson': coerce_bool(require_key(mqtt, 'useJson', 'mqtt'), 'mqtt.useJson'),
            'brokerUrl': coerce_string(require_key(mqtt, 'brokerUrl', 'mqtt'), 'mqtt.brokerUrl'),
            'topicRoot': coerce_string(require_key(mqtt, 'topicRoot', 'mqtt'), 'mqtt.topicRoot'),
        },
        'diagnostics': {
            'enableHeapDiagnostics': coerce_bool(require_key(diagnostics, 'enableHeapDiagnostics', 'diagnostics'), 'diagnostics.enableHeapDiagnostics'),
            'enableTaskDiagnostics': coerce_bool(require_key(diagnostics, 'enableTaskDiagnostics', 'diagnostics'), 'diagnostics.enableTaskDiagnostics'),
            'watchdogTimeoutMs': coerce_u32(require_key(diagnostics, 'watchdogTimeoutMs', 'diagnostics'), 'diagnostics.watchdogTimeoutMs'),
        },
        'opcua': {
            'port': coerce_u16(require_key(opcua, 'port', 'opcua'), 'opcua.port'),
            'enableAlarmAckMethod': coerce_bool(require_key(opcua, 'enableAlarmAckMethod', 'opcua'), 'opcua.enableAlarmAckMethod'),
        },
        'channels': [canonicalize_channel(channels_by_slot[slot], slot) for slot in range(CONFIG_MAX_CHANNELS)],
    }

    return canonical


def serialize_canonical_document(document, pretty=False):
    if pretty:
        return json.dumps(document, indent=2, ensure_ascii=True) + '\n'
    return json.dumps(document, separators=(',', ':'), ensure_ascii=True)


def load_and_prepare_config(config_path, schema_override=None):
    source_path = Path(config_path).resolve()
    source_document = json.loads(source_path.read_text(encoding='utf-8'))
    canonical_document = canonicalize_config_document(source_document)
    schema_version = schema_override if schema_override is not None else int(canonical_document['schemaVersion'])
    payload_text = serialize_canonical_document(canonical_document, pretty=False)
    payload_bytes = payload_text.encode('utf-8')
    payload_crc32 = zlib.crc32(payload_bytes) & 0xFFFFFFFF
    return {
        'source_path': source_path,
        'canonical_document': canonical_document,
        'schema_version': schema_version,
        'payload_text': payload_text,
        'payload_bytes': payload_bytes,
        'payload_size': len(payload_bytes),
        'payload_crc32': payload_crc32,
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
        configuration = client.get_node(ua.NodeId.from_string(CONFIGURATION_NODE_ID))
        print_json('Config state', await get_config_state(configuration))
    finally:
        await client.disconnect()
        print('Disconnected')


async def run_export(args):
    client = await connect_client(args)
    try:
        configuration = client.get_node(ua.NodeId.from_string(CONFIGURATION_NODE_ID))
        exported_document = canonicalize_config_document(await export_config(configuration))
        pretty_text = serialize_canonical_document(exported_document, pretty=True)
        output_path = Path(args.output).resolve()
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(pretty_text, encoding='utf-8')
        print(f'Exported config to {output_path}')
        if args.stdout:
            print(pretty_text, end='')
    finally:
        await client.disconnect()
        print('Disconnected')

async def run_import(args):
    if args.chunk_size <= 0 or args.chunk_size > DEFAULT_CHUNK_SIZE:
        raise ValueError('--chunk-size must be between 1 and 4096 bytes')

    config_info = load_and_prepare_config(args.config_path, args.schema_version)
    if args.write_canonical:
        canonical_path = Path(args.write_canonical).resolve()
        canonical_path.parent.mkdir(parents=True, exist_ok=True)
        canonical_path.write_text(config_info['payload_text'], encoding='utf-8')
        print(f'Wrote canonical payload to {canonical_path}')

    print(f"Source:   {config_info['source_path']}")
    print(
        'Payload:  '
        f"size={config_info['payload_size']} bytes "
        f"crc32=0x{config_info['payload_crc32']:08x} "
        f"schema={config_info['schema_version']}"
    )

    client = await connect_client(args)
    upload_started = False
    keep_session = False

    try:
        configuration = client.get_node(ua.NodeId.from_string(CONFIGURATION_NODE_ID))
        system = client.get_node(ua.NodeId.from_string(SYSTEM_NODE_ID))

        if not args.skip_pre_status:
            status_before = await get_config_state(configuration)
            print_json('Pre-import status', status_before)
            max_chunk_bytes = int(status_before.get('importMaxChunkBytes', DEFAULT_CHUNK_SIZE))
            if args.chunk_size > max_chunk_bytes:
                raise ValueError(
                    f'Requested chunk size {args.chunk_size} exceeds device limit {max_chunk_bytes}'
                )

        begin_ok = await call_bool_method(
            configuration,
            METHOD_BEGIN_CONFIG_IMPORT,
            ua.Variant(config_info['payload_size'], ua.VariantType.UInt32),
            ua.Variant(config_info['payload_crc32'], ua.VariantType.UInt32),
            ua.Variant(config_info['schema_version'], ua.VariantType.UInt32),
        )
        if not begin_ok:
            print_json('Begin rejected', await get_config_state(configuration))
            raise RuntimeError('BeginConfigImport returned false')

        upload_started = True
        sent_bytes = 0
        payload_bytes = config_info['payload_bytes']
        print('Uploading canonical JSON payload...')
        while sent_bytes < len(payload_bytes):
            chunk = payload_bytes[sent_bytes : sent_bytes + args.chunk_size]
            write_ok = await call_bool_method(
                configuration,
                METHOD_WRITE_CONFIG_CHUNK,
                ua.Variant(chunk, ua.VariantType.ByteString),
            )
            if not write_ok:
                print()
                print_json('Write rejected', await get_config_state(configuration))
                raise RuntimeError(f'WriteConfigChunk returned false at offset {sent_bytes}')
            sent_bytes += len(chunk)
            print_progress(sent_bytes, len(payload_bytes))
        print()

        validation_report = await validate_imported_config(configuration)
        print_json('Validation report', validation_report)
        if not bool(validation_report.get('valid', False)):
            keep_session = args.keep_staged
            raise RuntimeError(
                validation_report.get('message')
                or validation_report.get('result')
                or 'ValidateImportedConfig returned invalid result'
            )

        if args.validate_only:
            if args.keep_staged:
                keep_session = True
                print('Validation succeeded and the staged import session was kept active')
            else:
                abort_ok = await call_bool_method(configuration, METHOD_ABORT_CONFIG_IMPORT)
                print(f'AbortConfigImport returned: {abort_ok}')
                upload_started = False
                print('Validation succeeded; staged import session was closed')
            return

        apply_ok = await call_bool_method(configuration, METHOD_APPLY_IMPORTED_CONFIG)
        if not apply_ok:
            print_json('Apply failed status', await get_config_state(configuration))
            raise RuntimeError('ApplyImportedConfig returned false')

        upload_started = False
        print('Configuration applied successfully')

        if not args.skip_post_status:
            print_json('Post-import status', await get_config_state(configuration))

        if args.reboot_after_apply:
            reboot_ok = await request_reboot(system)
            if not reboot_ok:
                raise RuntimeError('Reboot returned false after successful config apply')
            print('Reboot requested')
    except Exception:
        if upload_started and not keep_session:
            try:
                abort_ok = await call_bool_method(configuration, METHOD_ABORT_CONFIG_IMPORT)
                print(f'AbortConfigImport returned: {abort_ok}')
            except Exception as abort_exc:
                print(f'AbortConfigImport failed: {abort_exc}')
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





