#!/usr/bin/env python3
"""Create an ephemeral, public-only signed T1 verifier fixture header."""

from __future__ import annotations

import argparse
import hashlib
import pathlib

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature


def c_bytes(name: str, value: bytes) -> str:
    body = ", ".join(f"0x{byte:02x}" for byte in value)
    return f"static const unsigned char {name}[{len(value)}] = {{{body}}};\n"


def field(value: str) -> bytes:
    encoded = value.encode("ascii")
    if not 0 < len(encoded) < 32:
        raise ValueError(value)
    return encoded + b"\0" * (32 - len(encoded))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    image_bytes = 5271552
    digest = hashlib.sha256()
    for offset in range(0, image_bytes, 4096):
        digest.update(bytes(((offset + index) * 19 + 7) & 0xFF
                            for index in range(min(4096, image_bytes - offset))))
    private_key = ec.generate_private_key(ec.SECP256R1())
    manifest = bytearray(320)
    manifest[:8] = b"RP2OTA06"
    manifest[8:10] = (1).to_bytes(2, "little")
    manifest[10:12] = (256).to_bytes(2, "little")
    manifest[12:16] = (320).to_bytes(4, "little")
    manifest[16:20] = (1).to_bytes(4, "little")
    manifest[20:24] = (7).to_bytes(4, "little")
    manifest[24:28] = image_bytes.to_bytes(4, "little")
    manifest[28:32] = (1).to_bytes(4, "little")
    manifest[32:36] = (0x10040000).to_bytes(4, "little")
    manifest[36:40] = (1).to_bytes(4, "little")
    manifest[40:44] = (9).to_bytes(4, "little")
    manifest[44:48] = (1).to_bytes(4, "little")
    manifest[48:80] = field("waveshare_core2350b_16m8m")
    manifest[80:112] = field("device_profile_basic")
    manifest[112:144] = field("base")
    manifest[144:176] = digest.digest()
    r, s = decode_dss_signature(private_key.sign(bytes(manifest[:256]), ec.ECDSA(hashes.SHA256())))
    order = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
    if s > order // 2:
        s = order - s
    manifest[256:288] = r.to_bytes(32, "big")
    manifest[288:320] = s.to_bytes(32, "big")
    numbers = private_key.public_key().public_numbers()
    public_key = b"\x04" + numbers.x.to_bytes(32, "big") + numbers.y.to_bytes(32, "big")
    content = "#ifndef V060_OTA_VERIFIER_FIXTURE_H\n#define V060_OTA_VERIFIER_FIXTURE_H\n"
    content += f"#define V060_FIXTURE_IMAGE_BYTES {image_bytes}u\n"
    content += c_bytes("v060_fixture_manifest", bytes(manifest))
    content += c_bytes("v060_fixture_public_key", public_key)
    content += "#endif\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(content, encoding="ascii")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
