#!/usr/bin/env python3
"""Build and run T1's real-mbedTLS verifier vectors with an ephemeral test key."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import subprocess
import sys
import tempfile

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature


def fixed_id(value: str) -> bytes:
    encoded = value.encode("ascii")
    assert 0 < len(encoded) < 32
    return encoded + b"\0" * (32 - len(encoded))


def make_fixture(directory: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path, pathlib.Path]:
    image = bytes((index * 19 + 7) & 0xFF for index in range(9001))
    key = ec.generate_private_key(ec.SECP256R1())
    manifest = bytearray(320)
    manifest[:8] = b"RP2OTA06"
    manifest[8:10] = (1).to_bytes(2, "little")
    manifest[10:12] = (256).to_bytes(2, "little")
    manifest[12:16] = (320).to_bytes(4, "little")
    manifest[16:20] = (1).to_bytes(4, "little")
    manifest[20:24] = (7).to_bytes(4, "little")
    manifest[24:28] = len(image).to_bytes(4, "little")
    manifest[28:32] = (1).to_bytes(4, "little")
    manifest[32:36] = (0x10040000).to_bytes(4, "little")
    manifest[36:40] = (1).to_bytes(4, "little")
    manifest[40:44] = (9).to_bytes(4, "little")
    manifest[44:48] = (1).to_bytes(4, "little")
    manifest[48:80] = fixed_id("waveshare_core2350b_16m8m")
    manifest[80:112] = fixed_id("device_profile_basic")
    manifest[112:144] = fixed_id("base")
    manifest[144:176] = hashlib.sha256(image).digest()
    signature = key.sign(bytes(manifest[:256]), ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(signature)
    order = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
    if s > order // 2:
        s = order - s
    manifest[256:288] = r.to_bytes(32, "big")
    manifest[288:320] = s.to_bytes(32, "big")
    public_numbers = key.public_key().public_numbers()
    public = b"\x04" + public_numbers.x.to_bytes(32, "big") + public_numbers.y.to_bytes(32, "big")
    manifest_path = directory / "manifest.bin"
    image_path = directory / "image.bin"
    public_path = directory / "public.bin"
    manifest_path.write_bytes(manifest)
    image_path.write_bytes(image)
    public_path.write_bytes(public)
    return manifest_path, image_path, public_path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--nm", default="nm")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    mbedtls = root / "libraries" / "pico-sdk" / "lib" / "mbedtls"
    verifier = root / "ota" / "src" / "ota_verifier.c"
    harness = root / "static" / "tests" / "ota_verifier" / "ota_verifier_test.c"
    sources = [
        "bignum.c", "bignum_core.c", "bignum_mod.c", "bignum_mod_raw.c", "constant_time.c",
        "ecp.c", "ecp_curves.c", "ecp_curves_new.c", "ecdsa.c", "sha256.c", "platform_util.c",
        "asn1parse.c", "asn1write.c",
    ]
    with tempfile.TemporaryDirectory(prefix="ota-verifier-") as temporary:
        temporary_path = pathlib.Path(temporary)
        namespace = temporary_path / "ota_crypto_namespace.h"
        subprocess.run([sys.executable, str(root / "static" / "tools" / "generate_ota_crypto_namespace.py"),
                        "--mbedtls-root", str(mbedtls), "--output", str(namespace)], check=True)
        executable = temporary_path / "ota_verifier_test.exe"
        command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-DNDEBUG",
                   f"-I{root / 'ota' / 'include'}", f"-I{root / 'ota' / 'src'}", f"-I{mbedtls / 'include'}",
                   "-DMBEDTLS_CONFIG_FILE=\"ota/ota_verifier_mbedtls_config.h\"", "-include", str(namespace),
                   str(verifier), str(harness)]
        command.extend(str(mbedtls / "library" / source) for source in sources)
        command.extend(["-o", str(executable)])
        subprocess.run(command, check=True)
        symbols = subprocess.run([args.nm, "-g", str(executable)], check=True,
                                 text=True, capture_output=True).stdout
        symbol_names = {line.split()[-1] for line in symbols.splitlines() if line.split()}
        if "ota_private_mbedtls_ecdsa_verify" not in symbol_names:
            raise RuntimeError("private ECDSA verifier symbol is absent")
        leaked = sorted(name for name in symbol_names if name.startswith("mbedtls_"))
        if leaked:
            raise RuntimeError(f"ordinary mbedTLS symbol leaked from verifier harness: {leaked}")
        manifest, image, public = make_fixture(temporary_path)
        subprocess.run([str(executable), str(manifest), str(image), str(public)], check=True)
    print("OTA verifier host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
