#!/usr/bin/env python3
"""Compare a running device's capability model and address space with the build it was flashed from.

Manual, credentials-required, live-hardware-only tool -- not part of
run_host_tests.py (its pure comparison logic is covered by
test_verify_live_capabilities.py). Closes the "live capability/NodeSet
comparison" gate of docs/releases/v0.5.1-review-fixes.md.

Expectations come from the build directory, never from the device:

- FirmwareProfile == the build's FIRMWARE_DRIVER_PACK;
- HardwareRevision and Identification.SoftwareRevision == config_export.json;
- SupportedDrivers == exactly the catalog entries whose
  FIRMWARE_ENABLE_NATIVE_*_DRIVER flag is ON in CMakeCache.txt (parsed from
  services/src/driver_catalog.c), each descriptor's id and capabilities ==
  static/host_descriptors/driver_descriptors.json.

It also walks the live ns=1 address space from Device, writes a canonical
inventory (NodeId, NodeClass, BrowseName) as evidence, and lists every static
"Device.*" NodeId literal in opcua_model/src that the device does not expose.
Those are reported for review rather than failed: some literals are browse
prefixes, method paths or compiled out by open62541 feature flags.

Credentials come only from OPCUA_USERNAME/OPCUA_PASSWORD in the environment,
never from the command line.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import sys
from argparse import Namespace
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

CATALOG_SOURCE = ROOT / "services" / "src" / "driver_catalog.c"
DESCRIPTORS = ROOT / "static" / "host_descriptors" / "driver_descriptors.json"
MODEL_SOURCES = ROOT / "opcua_model" / "src"

# Live descriptors spell capabilities as the firmware's JSON names; the host
# descriptor file uses camelCase for one of them.
CAPABILITY_ALIASES = {"selfTest": "self_test"}

MAX_INVENTORY_NODES = 4000


# ---------------------------------------------------------------- expectations

def read_cache(build_dir: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in (build_dir / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"^([A-Za-z0-9_]+):[A-Z_]+=(.*)$", line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def catalog_flags(source: str) -> dict[str, str]:
    """Map each catalog entry's .name to the #if flag guarding it, inside g_driver_catalog."""
    start = source.index("g_driver_catalog[")
    body = source[start:source.index("};", start)]
    names: dict[str, str] = {}
    flag = None
    for line in body.splitlines():
        guard = re.match(r"\s*#if\s+(FIRMWARE_ENABLE_NATIVE_\w+)", line)
        if guard:
            flag = guard.group(1)
            continue
        if re.match(r"\s*#endif", line):
            flag = None
            continue
        name = re.search(r'\.name\s*=\s*"([^"]+)"', line)
        if name:
            if flag is None:
                raise ValueError(f"catalog entry {name.group(1)!r} is not guarded by a FIRMWARE_ENABLE flag")
            names[name.group(1)] = flag
    if not names:
        raise ValueError("no catalog entries found in driver_catalog.c")
    return names


def expected_from_build(build_dir: Path, catalog_source: str, descriptors: dict) -> dict:
    cache = read_cache(build_dir)
    export = json.loads((build_dir / "config_export.json").read_text(encoding="utf-8"))["device"]
    enabled = sorted(name for name, flag in catalog_flags(catalog_source).items() if cache.get(flag) == "ON")
    by_name = {entry["name"]: entry for entry in descriptors["drivers"]}
    drivers = {}
    for name in enabled:
        if name not in by_name:
            raise ValueError(f"compiled driver {name!r} has no entry in driver_descriptors.json")
        drivers[name] = {
            "id": by_name[name]["driverId"],
            "capabilities": sorted(CAPABILITY_ALIASES.get(c, c) for c in by_name[name]["capabilities"]),
        }
    return {
        "firmwareProfile": cache["FIRMWARE_DRIVER_PACK"],
        "hardwareRevision": export["hwRevision"],
        "softwareRevision": export["fwRevision"],
        "drivers": drivers,
    }


def compare(expected: dict, live: dict) -> list[str]:
    problems: list[str] = []
    for key in ("firmwareProfile", "hardwareRevision", "softwareRevision"):
        if live.get(key) != expected[key]:
            problems.append(f"{key}: device reports {live.get(key)!r}, build expects {expected[key]!r}")
    if not isinstance(live.get("configurationSchemaVersion"), int):
        problems.append(f"configurationSchemaVersion is not an integer: {live.get('configurationSchemaVersion')!r}")

    live_drivers = live.get("drivers", {})
    missing = sorted(set(expected["drivers"]) - set(live_drivers))
    extra = sorted(set(live_drivers) - set(expected["drivers"]))
    if missing:
        problems.append(f"SupportedDrivers missing compiled drivers: {missing}")
    if extra:
        problems.append(f"SupportedDrivers reports drivers this build does not compile: {extra}")
    for name in sorted(set(expected["drivers"]) & set(live_drivers)):
        descriptor = live_drivers[name]
        if not isinstance(descriptor, dict):
            problems.append(f"{name}: descriptor is not valid JSON: {descriptor!r}")
            continue
        if descriptor.get("name") != name:
            problems.append(f"{name}: descriptor name is {descriptor.get('name')!r}")
        if descriptor.get("id") != expected["drivers"][name]["id"]:
            problems.append(f"{name}: id {descriptor.get('id')!r} != descriptor file {expected['drivers'][name]['id']}")
        live_caps = sorted(descriptor.get("capabilities", []))
        if live_caps != expected["drivers"][name]["capabilities"]:
            problems.append(f"{name}: capabilities {live_caps} != descriptor file {expected['drivers'][name]['capabilities']}")
        for claim in descriptor.get("resources", []):
            if claim.get("type") not in {"none", "adc_input", "adc_peripheral", "gpio", "spi_chip_select",
                                         "spi_peripheral", "ad5422_channel", "max22190_channel",
                                         "ips2050_channel", "uic_channel"}:
                problems.append(f"{name}: unknown resource claim type {claim.get('type')!r}")
    return problems


def source_node_literals(model_sources: Path) -> list[str]:
    literals: set[str] = set()
    for path in sorted(model_sources.glob("*.c")):
        literals.update(re.findall(r'"(Device(?:\.[A-Za-z0-9_]+)+)"', path.read_text(encoding="utf-8", errors="replace")))
    return sorted(literals)


# ---------------------------------------------------------------- live device

async def read_live(client, ua) -> tuple[dict, list[dict]]:
    async def value(identifier: str):
        data = await client.get_node(ua.NodeId(identifier, 1)).read_data_value(raise_on_bad_status=False)
        return data.Value.Value if data.StatusCode.is_good() else None

    live = {
        "firmwareProfile": await value("Device.Compatibility.FirmwareProfile"),
        "hardwareRevision": await value("Device.Compatibility.HardwareRevision"),
        "configurationSchemaVersion": await value("Device.Compatibility.ConfigurationSchemaVersion"),
        "softwareRevision": await value("Device.Identification.SoftwareRevision"),
        "drivers": {},
    }
    folder = client.get_node(ua.NodeId("Device.Compatibility.SupportedDrivers", 1))
    for child in await folder.get_children():
        name = (await child.read_browse_name()).Name
        raw = (await child.read_data_value(raise_on_bad_status=False)).Value.Value
        try:
            live["drivers"][name] = json.loads(raw)
        except (TypeError, ValueError):
            live["drivers"][name] = raw

    inventory: dict[str, dict] = {}
    queue = [ua.NodeId("Device", 1)]
    root = await client.get_node(queue[0]).read_browse_name()
    inventory["Device"] = {"nodeClass": "Object", "browseName": root.Name}
    while queue and len(inventory) < MAX_INVENTORY_NODES:
        node_id = queue.pop(0)
        for ref in await client.get_node(node_id).get_children_descriptions():
            child = ref.NodeId
            if child.NamespaceIndex != 1 or not isinstance(child.Identifier, str):
                continue
            if child.Identifier in inventory:
                continue
            inventory[child.Identifier] = {
                "nodeClass": ref.NodeClass.name,
                "browseName": ref.BrowseName.Name,
            }
            queue.append(ua.NodeId(child.Identifier, 1))
    rows = [{"nodeId": key, **inventory[key]} for key in sorted(inventory)]
    return live, rows


async def run(args: argparse.Namespace) -> int:
    username = os.getenv("OPCUA_USERNAME")
    password = os.getenv("OPCUA_PASSWORD")
    if not username or not password:
        print("live-capabilities: FAIL: set OPCUA_USERNAME and OPCUA_PASSWORD in your own shell first")
        return 2

    build_dir = args.build_dir.resolve()
    expected = expected_from_build(build_dir, CATALOG_SOURCE.read_text(encoding="utf-8"),
                                   json.loads(DESCRIPTORS.read_text(encoding="utf-8")))

    import stage_config as opcua_common  # noqa: E402

    opcua_common.load_asyncua()
    client = opcua_common.Client(url=args.server_url, timeout=args.timeout)
    await opcua_common.configure_client_security(client, Namespace(
        username=username, client_cert=args.client_cert, client_key=args.client_key,
        server_cert=args.server_cert, no_auto_client_cert=args.no_auto_client_cert,
        security_policy=args.security_policy, security_mode=args.security_mode))
    client.set_user(username)
    client.set_password(password)
    async with client:
        live, inventory = await read_live(client, opcua_common.ua)

    problems = compare(expected, live)
    live_ids = {row["nodeId"] for row in inventory}
    unexposed = [literal for literal in source_node_literals(MODEL_SOURCES) if literal not in live_ids]

    evidence = {
        "server": args.server_url,
        "buildDir": str(build_dir),
        "expected": expected,
        "live": live,
        "problems": problems,
        "inventoryNodeCount": len(inventory),
        "inventoryTruncated": len(inventory) >= MAX_INVENTORY_NODES,
        "sourceLiteralsNotExposed": unexposed,
        "inventory": inventory,
    }
    args.evidence.parent.mkdir(parents=True, exist_ok=True)
    args.evidence.write_text(json.dumps(evidence, indent=2, ensure_ascii=False), encoding="utf-8")

    print(f"profile={live['firmwareProfile']} hw={live['hardwareRevision']} sw={live['softwareRevision']} "
          f"schema={live['configurationSchemaVersion']}")
    print(f"SupportedDrivers: {sorted(live['drivers'])}")
    print(f"address space: {len(inventory)} ns=1 nodes under Device"
          + (" (TRUNCATED)" if evidence["inventoryTruncated"] else ""))
    for literal in unexposed:
        print(f"REVIEW: source NodeId literal not exposed: {literal}")
    for problem in problems:
        print(f"MISMATCH: {problem}")
    print(f"evidence: {args.evidence}")
    if problems or evidence["inventoryTruncated"]:
        print("live-capabilities: FAIL")
        return 1
    print("live-capabilities: PASS")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("server_url")
    parser.add_argument("--build-dir", required=True, type=Path, help="Build directory the device was flashed from")
    parser.add_argument("--evidence", type=Path, default=Path("build/live-capabilities.json"))
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--security-policy", default="Basic256Sha256", choices=["Basic256Sha256"])
    parser.add_argument("--security-mode", default="SignAndEncrypt", choices=["Sign", "SignAndEncrypt"])
    parser.add_argument("--client-cert")
    parser.add_argument("--client-key")
    parser.add_argument("--server-cert")
    parser.add_argument("--no-auto-client-cert", action="store_true")
    return parser.parse_args()


def main() -> int:
    return asyncio.run(run(parse_args()))


if __name__ == "__main__":
    sys.exit(main())
