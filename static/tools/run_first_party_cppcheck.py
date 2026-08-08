#!/usr/bin/env python3
"""Run blocking cppcheck over first-party sources only."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile


FIRST_PARTY_DIRS = (
    "drivers",
    "services",
    "opcua_model",
    "ota",
    "bootloader",
    "app",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run cppcheck against first-party code.")
    parser.add_argument("--build-dir", required=True, type=pathlib.Path)
    parser.add_argument("--source-dir", default=pathlib.Path(__file__).resolve().parents[2], type=pathlib.Path)
    parser.add_argument("--xml-output", required=True, type=pathlib.Path)
    parser.add_argument("--text-output", required=True, type=pathlib.Path)
    parser.add_argument(
        "--cppcheck",
        default="cppcheck",
        help="cppcheck executable name or path (default: cppcheck)",
    )
    return parser.parse_args()


def filtered_compile_database(
    source_dir: pathlib.Path, build_dir: pathlib.Path
) -> list[dict[str, object]]:
    compile_commands_path = build_dir / "compile_commands.json"
    if not compile_commands_path.exists():
        raise SystemExit(f"cppcheck-runner: missing {compile_commands_path}")

    entries = json.loads(compile_commands_path.read_text(encoding="utf-8"))
    first_party_roots = tuple((source_dir / item).resolve() for item in FIRST_PARTY_DIRS)
    filtered: list[dict[str, object]] = []
    for entry in entries:
        source_path = pathlib.Path(str(entry.get("file", ""))).resolve()
        if "generated" in source_path.parts:
            continue
        if any(source_path.is_relative_to(root) for root in first_party_roots):
            filtered.append(entry)
    return filtered


def main() -> int:
    args = parse_args()
    source_dir = args.source_dir.resolve()
    build_dir = args.build_dir.resolve()
    xml_output = args.xml_output.resolve()
    text_output = args.text_output.resolve()

    xml_output.parent.mkdir(parents=True, exist_ok=True)
    text_output.parent.mkdir(parents=True, exist_ok=True)

    entries = filtered_compile_database(source_dir, build_dir)
    if not entries:
        raise SystemExit("cppcheck-runner: no first-party compile commands found")

    with tempfile.TemporaryDirectory(prefix="rp2-cppcheck-") as temp_dir:
        database_path = pathlib.Path(temp_dir) / "compile_commands.json"
        database_path.write_text(json.dumps(entries), encoding="utf-8")
        command = [
            args.cppcheck,
            f"--project={database_path}",
            "--enable=warning,performance,portability",
            "--std=c11",
            "--check-level=exhaustive",
            "--inline-suppr",
            "--quiet",
            "--error-exitcode=1",
            "--suppress=missingInclude",
            "--suppress=missingIncludeSystem",
            "--xml",
            "--xml-version=2",
        ]

        result = subprocess.run(command, capture_output=True, text=True, check=False)
    xml_output.write_text(result.stderr, encoding="utf-8")
    text_output.write_text(
        "Command:\n"
        + " ".join(command)
        + "\n\nstdout:\n"
        + result.stdout
        + "\n\nstderr:\n"
        + result.stderr,
        encoding="utf-8",
    )

    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        return result.returncode

    print("cppcheck: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
