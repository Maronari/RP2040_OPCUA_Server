#!/usr/bin/env python3
"""Emit CI's explicit driver hardware-qualification label (ROADMAP.md:607).

Prints a human-readable label to stdout (a CI step can append this to
$GITHUB_STEP_SUMMARY) and writes the same facts to a JSON file for the
build's evidence artifacts. This does not perform any new verification --
it labels the build against the compile/mock-validation report that
already exists (docs/releases/v0.5.1-driver-verification.md), and fails
loudly if that report is missing so the label can never silently drift
away from a real document.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys


INDUSTRIAL_LABEL = (
    "Industrial-board drivers in this build (FIRMWARE_DRIVER_PACK={pack}) "
    "are compile/mock validated, not hardware qualified. "
    "RP2350_INDUSTRIAL_MVP_V1 does not exist as assembled hardware; no "
    "electrical, timing, calibration or real-device correctness is "
    "claimed. See {report} for the compile/mock-validation report and the "
    "consolidated unconfirmed-hardware-facts table."
)

NO_INDUSTRIAL_LABEL = (
    "FIRMWARE_DRIVER_PACK={pack} does not include the industrial-only "
    "drivers, so no hardware-qualification label applies to this build."
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--driver-pack",
        required=True,
        help="The FIRMWARE_DRIVER_PACK value this build was configured with (e.g. industrial, base).",
    )
    parser.add_argument(
        "--report",
        default="docs/releases/v0.5.1-driver-verification.md",
        help="Path to the compile/mock-validation report the label points at.",
    )
    parser.add_argument(
        "--output",
        help="Optional path to write the label as JSON evidence.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    industrial_packs = {"industrial"}
    is_industrial = args.driver_pack in industrial_packs

    if is_industrial:
        report_path = pathlib.Path(args.report)
        if not report_path.is_file():
            print(
                f"error: FIRMWARE_DRIVER_PACK={args.driver_pack} requires a "
                f"compile/mock-validation report at {args.report}, but it "
                "does not exist -- the hardware-qualification label cannot "
                "be issued without a real document behind it.",
                file=sys.stderr,
            )
            return 1
        label = INDUSTRIAL_LABEL.format(pack=args.driver_pack, report=args.report)
        qualification_status = "compile/mock validated, not hardware qualified"
    else:
        label = NO_INDUSTRIAL_LABEL.format(pack=args.driver_pack)
        qualification_status = "not applicable"

    print(label)

    if args.output:
        output_path = pathlib.Path(args.output)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(
            json.dumps(
                {
                    "driver_pack": args.driver_pack,
                    "qualification_status": qualification_status,
                    "report_path": args.report if is_industrial else None,
                    "label": label,
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
