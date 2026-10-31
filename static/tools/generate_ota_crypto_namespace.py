"""Generate a private namespace for the vendored OTA crypto compilation.

Apply with -include to every verifier and private mbedTLS translation unit.
Renaming types as well as functions keeps declarations and definitions aligned.
The ordinary TLS compilation must never include this header.
"""
import argparse
import pathlib
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mbedtls-root', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    names = set()
    paths = sorted((args.mbedtls_root / 'include' / 'mbedtls').glob('*.h'))
    paths += sorted((args.mbedtls_root / 'library').glob('*.[ch]'))
    if not paths:
        parser.error('vendored mbedTLS sources not found')
    macros = set()
    for path in paths:
        source = path.read_text(encoding='utf-8')
        names.update(re.findall(r'\bmbedtls_[A-Za-z0-9_]+\b', source))
        macros.update(re.findall(r'^\s*#\s*define\s+(mbedtls_[A-Za-z0-9_]+)', source, re.MULTILINE))
    names.difference_update(macros)
    content = '#ifndef OTA_CRYPTO_NAMESPACE_H\n#define OTA_CRYPTO_NAMESPACE_H\n'
    content += ''.join(f'#define {name} ota_private_{name}\n' for name in sorted(names))
    content += '#endif\n'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != content:
        args.output.write_text(content, encoding='utf-8')


if __name__ == '__main__':
    main()
